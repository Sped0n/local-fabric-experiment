// The form is the only source of truth while editing. Derive the watch list and Lua together.
const list = (v) => (Array.isArray(v) ? v : [v]);
const quote = (s) =>
  '"' +
  s.replace(
    /[\x00-\x1f"\\]/g,
    (c) => "\\" + String(c.charCodeAt(0)).padStart(3, "0"),
  ) +
  '"';
const literal = (v) => (typeof v === "string" ? quote(v) : String(v));
const fail = (msg) => {
  throw Error(msg);
};
function path(p) {
  return {
    device: p.device,
    endpoint: p.endpoint,
    cluster: p.cluster,
    property: p.property,
  };
}
function clockTest(c) {
  const tests = ["v"];
  if (c.weekdays) {
    if (
      !Array.isArray(c.weekdays) ||
      !c.weekdays.length ||
      c.weekdays.some((d) => !Number.isInteger(d) || d < 1 || d > 7) ||
      new Set(c.weekdays).size !== c.weekdays.length
    )
      fail("Invalid weekdays");
    tests.push(
      "(" + c.weekdays.map((d) => "v.weekday == " + d).join(" or ") + ")",
    );
  }
  const minute = "v.hour*60+v.minute";
  if (c.at !== undefined) {
    if (
      !Number.isInteger(c.at) ||
      c.at < 0 ||
      c.at >= 1440 ||
      c.start !== undefined ||
      c.end !== undefined
    )
      fail("Invalid clock time");
    tests.push(minute + " == " + c.at);
  } else if (c.start !== undefined || c.end !== undefined) {
    if (
      !Number.isInteger(c.start) ||
      !Number.isInteger(c.end) ||
      c.start < 0 ||
      c.start >= c.end ||
      c.end > 1440
    )
      fail("Invalid clock window");
    tests.push(minute + " >= " + c.start, minute + " < " + c.end);
  }
  return tests.join(" and ");
}
const inEnum = (v, choices) =>
  !choices?.length || choices.some((x) => (x?.value ?? x) === v);
function validValue(v, s) {
  if (s.type !== "enum" && !inEnum(v, s.enum)) return false;
  switch (s.type) {
    case "boolean":
      return typeof v === "boolean";
    case "string":
      return typeof v === "string";
    case "enum":
      return (
        (typeof v === "string" || Number.isInteger(v)) &&
        (v === s.value || s.enum?.some((x) => (x?.value ?? x) === v))
      );
    case "integer":
    case "number":
      return (
        Number.isFinite(v) &&
        (s.type !== "integer" || Number.isInteger(v)) &&
        !(v < s.min || v > s.max) &&
        (!s.step || Math.abs(v / s.step - Math.round(v / s.step)) < 1e-7)
      );
    default:
      return false;
  }
}
function test(p, properties, changes) {
  if (p.clock === "minute") return clockTest(p);
  const prop = properties.find(
    (x) =>
      x.device === p.device &&
      x.endpoint === p.endpoint &&
      x.cluster === p.cluster &&
      x.property === p.property,
  )?.meta;
  if (!prop || p.type !== prop.type)
    fail("Unknown or changed property: " + p.property);
  if (changes && p.op === "changes") return "true";
  if (p.type === "boolean" && ["true", "false"].includes(p.op))
    return "v == " + p.op;
  if (
    p.type === "boolean" ||
    ![
      "==",
      "!=",
      ...(p.type === "number" ? ["<", "<=", ">", ">="] : []),
    ].includes(p.op) ||
    !validValue(p.value, prop)
  )
    fail("Invalid comparison for " + p.property);
  return (
    "v ~= nil and v " + (p.op === "!=" ? "~=" : p.op) + " " + literal(p.value)
  );
}
function step(s, actions) {
  if (s.delay_s !== undefined) {
    if (!Number.isInteger(s.delay_s) || s.delay_s < 1 || s.delay_s > 86400)
      fail("Delay must be 1–86400 seconds");
    return "automation.delay(" + s.delay_s + ")";
  }
  const hook = typeof s.native === "string";
  const a = actions.find((x) =>
    hook
      ? x.native === s.native
      : !x.native &&
        x.device === s.device &&
        x.endpoint === s.endpoint &&
        x.cluster === s.cluster &&
        x.name === s.name,
  );
  if (!a) fail("Unknown action: " + (s.native ?? s.name));
  const specs = Object.entries(a.meta.arguments ?? {}),
    args = s.arguments ?? {};
  if (
    specs.length !== Object.keys(args).length ||
    specs.some(
      ([k, spec]) =>
        !Object.hasOwn(args, k) ||
        !validValue(args[k], spec) ||
        (hook &&
          spec.type === "string" &&
          new TextEncoder().encode(args[k]).length > 512),
    )
  )
    fail("Invalid arguments for " + (s.native ?? s.name));
  const table =
    "{" +
    specs.map(([k]) => "[" + quote(k) + "]=" + literal(args[k])).join(",") +
    "}";
  return hook
    ? "automation.invoke_local(" + quote(s.native) + "," + table + ")"
    : "matter.invoke_action(" +
        [s.device, s.endpoint, quote(s.cluster), quote(s.name), table].join(
          ",",
        ) +
        ")";
}
export function compileVisual(editor, properties, actions) {
  const inputs = list(editor.when),
    named = Array.isArray(editor.when),
    lines = [];
  if (inputs.length < 1 || inputs.length > 4) fail("Select 1–4 inputs");
  const predicates = inputs.map((p) => test(p, properties, !named));
  const watches = inputs.map((p) =>
    p.clock
      ? { name: p.name, clock: "minute" }
      : named
        ? { name: p.name, ...path(p) }
        : path(p),
  );
  if (named) {
    if (
      inputs.some(
        (p) =>
          typeof p.name !== "string" ||
          !/^[A-Za-z_][A-Za-z0-9_]{0,22}$/.test(p.name),
      ) ||
      new Set(inputs.map((p) => p.name)).size !== inputs.length
    )
      fail("Use unique Lua input names");
    if (
      new Set(watches.map(({ name, ...p }) => JSON.stringify(p))).size !==
      inputs.length
    )
      fail("Duplicate input");
    lines.push("local function ready(s)");
    inputs.forEach((p, i) =>
      lines.push(
        " local v=s[" + quote(p.name) + "]",
        " if not (" + predicates[i] + ") then return false end",
      ),
    );
    lines.push(" return true", "end");
    for (const p of inputs)
      if (p.at !== undefined)
        lines.push(
          "local p=event.previous[" + quote(p.name) + "]",
          "if p.hour*60+p.minute == " + p.at + " then return end",
        );
    lines.push(
      "if ready(event.previous) or not ready(event.current) then return end",
    );
  } else if (editor.when.op !== "changes") {
    lines.push(
      "local function ready(v) return " + predicates[0] + " end",
      "if ready(event.previous) or not ready(event.current) then return end",
    );
  }
  if ((editor.conditions?.length ?? 0) > 4) fail("At most four conditions");
  for (const p of editor.conditions ?? []) {
    const read = p.clock
      ? "automation.local_time()"
      : "matter.get_property(" +
        [p.device, p.endpoint, quote(p.cluster), quote(p.property)].join(",") +
        ")";
    const label = p.clock
      ? "Outside selected local schedule"
      : "#" +
        p.device +
        "/" +
        p.endpoint +
        " " +
        p.cluster +
        "." +
        p.property +
        " " +
        p.op +
        (p.value === undefined ? "" : " " + p.value);
    lines.push(
      "local v=" + read,
      "if not (" +
        test(p, properties, false) +
        ") then return automation.skip(" +
        quote(label) +
        (p.clock ? "" : ", v") +
        ") end",
    );
  }
  const steps = list(editor.action);
  if (!steps.length || steps.length > 8) fail("Select 1–8 actions");
  lines.push(...steps.map((s) => step(s, actions)));
  return {
    when: named ? watches : watches[0],
    source: lines.join("\n"),
    editor,
  };
}
export function savedRule(draft, properties, actions) {
  const rule = { enabled: draft.enabled, title: draft.title };
  if (draft.mode === "raw")
    Object.assign(rule, { when: draft.when, source: draft.source });
  else {
    const editor = { when: draft.when, action: draft.action };
    if (draft.conditions?.length) editor.conditions = draft.conditions;
    Object.assign(rule, compileVisual(editor, properties, actions));
  }
  if (new TextEncoder().encode(JSON.stringify(rule)).length > 3072)
    fail("Rule exceeds 3072 bytes");
  return rule;
}

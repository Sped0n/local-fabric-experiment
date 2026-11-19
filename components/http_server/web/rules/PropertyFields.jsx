import { For, Show } from "solid-js";
import { devices, deviceState } from "../connection.js";

const pathKey = (p) =>
  p?.clock
    ? "clock:" + p.clock
    : p?.delay_s !== undefined
      ? "delay"
      : p?.native
        ? "local:" + p.native
        : p?.device
          ? [p.device, p.endpoint, p.cluster, p.property ?? p.name].join("/")
          : "";
function paths(kind) {
  return devices.flatMap((d) =>
    Object.entries(deviceState[d.device_id]?.endpoints ?? {}).flatMap(
      ([ep, e]) =>
        Object.entries(e.clusters ?? {}).flatMap(([cluster, c]) =>
          Object.entries(c[kind] ?? {}).map(([name, meta]) => ({
            device: d.device_id,
            endpoint: +ep,
            cluster,
            [kind === "properties" ? "property" : "name"]: name,
            meta,
            label: d.name + " · " + ep + " · " + cluster + " / " + name,
          })),
        ),
    ),
  );
}
function Selector(p) {
  return (
    <label>
      {p.label}
      <select
        required={p.required}
        value={p.value}
        onChange={(e) => p.change(e.currentTarget.value)}
      >
        <option value="">Choose…</option>
        <Show when={p.value && !p.options.some((v) => pathKey(v) === p.value)}>
          <option value={p.value}>{p.value} (unavailable)</option>
        </Show>
        <For each={p.options}>
          {(o) => (
            <option value={pathKey(o)} selected={p.value === pathKey(o)}>
              {o.label}
            </option>
          )}
        </For>
      </select>
    </label>
  );
}
const operators = {
  boolean: ["true", "false"],
  enum: ["==", "!="],
  string: ["==", "!="],
  number: [">", "<", ">=", "<=", "==", "!="],
};
function choices(meta) {
  const out = [...(meta.enum ?? [])];
  if (meta.value != null && !out.some((v) => (v?.value ?? v) === meta.value))
    out.push({ value: meta.value, label: meta.label ?? meta.value });
  return out;
}
function makeTest(p, op) {
  if (!p) return undefined;
  const { label, meta, ...path } = p;
  const t = { ...path, type: meta.type, op: op ?? operators[meta.type][0] };
  if (t.op !== "changes" && t.type !== "boolean") {
    const first = choices(meta)[0];
    t.value =
      t.type === "enum"
        ? (first?.value ?? first)
        : t.type === "number"
          ? (meta.value ?? 0)
          : "";
  }
  return t;
}
function TestPredicate(p) {
  const selected = () => p.options.find((v) => pathKey(v) === pathKey(p.test)),
    meta = () => selected()?.meta;
  const labels = () =>
    p.trigger
      ? {
          true: "becomes true",
          false: "becomes false",
          "==": "becomes equal to",
          "!=": "becomes different from",
          ">": "crosses above",
          "<": "crosses below",
          ">=": "reaches or exceeds",
          "<=": "reaches or falls below",
          changes: "changes",
        }
      : {
          true: "is true",
          false: "is false",
          "==": "is equal to",
          "!=": "is not equal to",
        };
  return (
    <>
      <Show when={meta()}>
        <label>
          Operator
          <select
            value={p.test.op}
            onChange={(e) =>
              p.change(makeTest(selected(), e.currentTarget.value))
            }
          >
            <For
              each={[
                ...operators[meta().type],
                ...(p.trigger ? ["changes"] : []),
              ]}
            >
              {(op) => (
                <option value={op} selected={p.test.op === op}>
                  {labels()[op] ?? op}
                </option>
              )}
            </For>
          </select>
        </label>
        <Show when={p.test.op !== "changes" && meta().type !== "boolean"}>
          <label>
            Value {meta().unit}
            <Show
              when={meta().type === "enum"}
              fallback={
                <input
                  required
                  type={meta().type === "number" ? "number" : "text"}
                  step={meta().step}
                  min={meta().min}
                  max={meta().max}
                  value={p.test.value ?? ""}
                  onInput={(e) =>
                    p.change({
                      ...p.test,
                      value:
                        meta().type === "number"
                          ? e.currentTarget.value === ""
                            ? null
                            : e.currentTarget.valueAsNumber
                          : e.currentTarget.value,
                    })
                  }
                />
              }
            >
              <select
                required
                onChange={(e) =>
                  p.change({
                    ...p.test,
                    value: JSON.parse(e.currentTarget.value),
                  })
                }
              >
                <For each={choices(meta())}>
                  {(v) => (
                    <option
                      selected={(v?.value ?? v) === p.test.value}
                      value={JSON.stringify(v?.value ?? v)}
                    >
                      {v?.label ?? String(v)}
                    </option>
                  )}
                </For>
              </select>
            </Show>
          </label>
        </Show>
      </Show>
    </>
  );
}

export { Selector, TestPredicate, makeTest, pathKey, paths };

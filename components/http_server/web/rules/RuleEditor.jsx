import { onSettled, createSignal, createMemo, For, Show } from "solid-js";
import {
  rules,
  setRules,
  localActions,
  mcp,
  attempt,
  setNotice,
} from "../connection.js";
import { parameters } from "../fields.jsx";
import {
  Selector,
  TestPredicate,
  makeTest,
  pathKey,
  paths,
} from "./PropertyFields.jsx";
import { ClockFields, clockOption } from "./ClockFields.jsx";
import { WhenFields } from "./WhenFields.jsx";
import { savedRule } from "./compile.js";
import { Traces } from "./Traces.jsx";
import { ActionSteps } from "./ActionSteps.jsx";

const blankRule = () => ({
  mode: "visual",
  enabled: true,
  conditions: [],
  title: "",
});
export function RuleEditor() {
  const [slot, selectSlot] = createSignal(1),
    [draft, setDraft] = createSignal(blankRule());
  const props = createMemo(() => paths("properties")),
    actions = createMemo(() => [
      ...paths("actions"),
      ...localActions().map((a) => ({
        native: a.name,
        label: a.name,
        meta: a,
      })),
    ]);
  function load(i, rows = rules()) {
    selectSlot(i);
    const r = rows?.[i - 1];
    setDraft(
      !r
        ? blankRule()
        : {
            enabled: r.enabled,
            title: r.title,
            ...(r.editor
              ? { ...r.editor, mode: "visual" }
              : { mode: "raw", when: r.when, source: r.source }),
          },
    );
  }
  onSettled(() => {
    load(1);
  });
  async function reload(i) {
    const rows = await mcp.read("fabric://automation/rules");
    setRules(rows);
    load(i, rows);
  }
  const set = (k, v) => setDraft({ ...draft(), [k]: v });
  const selected = (items, k) => {
    const p = items.find((o) => pathKey(o) === k);
    if (!p) return undefined;
    const { label, meta, ...rest } = p;
    return rest;
  };
  const steps = () =>
    Array.isArray(draft().action) ? draft().action : [draft().action ?? {}];
  const stepOptions = createMemo(() => [
    { delay_s: 1, label: "Delay" },
    ...actions(),
  ]);
  const descriptor = (v) => actions().find((a) => pathKey(a) === pathKey(v));
  async function save(form) {
    const def = { ...draft() };
    if (
      !def.when ||
      (Array.isArray(def.when) && !def.when.length) ||
      (def.mode === "visual" && !def.action)
    )
      throw Error("Select a trigger and action");
    if (def.mode === "visual") {
      const items = steps().map((step, i) => {
        if (step.delay_s !== undefined) return step;
        const desc = descriptor(step);
        if (!desc) throw Error("Select an available action for each step");
        return {
          ...step,
          arguments: parameters(
            form.querySelectorAll("[data-then-step]")[i],
            desc.meta.arguments,
          ),
        };
      });
      if (!items.length) throw Error("Add an action or delay");
      def.action = items.length === 1 ? items[0] : items;
      delete def.source;
    }
    await mcp.call("rule.save", {
      slot: slot(),
      definition: savedRule(def, props(), actions()),
    });
    await reload(slot());
    setNotice("Rule saved on board");
  }
  return (
    <section id="rules" class="rules">
      <h2>Automation rules</h2>
      <label>
        Rule slot
        <select value={slot()} onChange={(e) => load(+e.currentTarget.value)}>
          <For each={(rules() ?? []).map((_, i) => i + 1)}>
            {(i) => (
              <option value={i}>
                {i}: {rules()?.[i - 1]?.title || "Untitled / empty"}
              </option>
            )}
          </For>
        </select>
      </label>
      <form
        onSubmit={(e) => {
          e.preventDefault();
          attempt(() => save(e.currentTarget));
        }}
      >
        <label>
          Title
          <input
            maxLength="64"
            value={draft().title || ""}
            onInput={(e) => set("title", e.currentTarget.value)}
          />
        </label>
        <label>
          Enabled
          <input
            type="checkbox"
            checked={draft().enabled}
            onChange={(e) => set("enabled", e.currentTarget.checked)}
          />
        </label>
        <fieldset class="definition">
          <legend>Rule definition</legend>
          <label>
            Mode
            <select
              value={draft().mode}
              onChange={(e) => {
                const mode = e.currentTarget.value;
                const d = draft();
                if (
                  (d.when || d.action || d.conditions?.length || d.source) &&
                  !confirm(
                    "Clear this draft and switch mode? The saved rule keeps running until you save a replacement.",
                  )
                ) {
                  e.currentTarget.value = d.mode;
                  return;
                }
                setDraft({
                  mode,
                  title: d.title ?? "",
                  enabled: d.enabled,
                  ...(mode === "visual" ? { conditions: [] } : { source: "" }),
                });
              }}
            >
              <option value="visual">Visual</option>
              <option value="raw">Raw Lua</option>
            </select>
          </label>
          <WhenFields
            visual={draft().mode === "visual"}
            when={draft().when}
            options={props()}
            select={(key) => selected(props(), key)}
            change={(v) => set("when", v)}
          />
          <Show when={draft().mode === "raw"}>
            <label>
              Lua body
              <textarea
                required
                spellcheck={false}
                placeholder={
                  Array.isArray(draft().when)
                    ? "-- event.current.<input name>, event.previous.<input name>"
                    : "if not event.current then return end"
                }
                value={draft().source ?? ""}
                onInput={(e) => set("source", e.currentTarget.value)}
              />
            </label>
          </Show>
        </fieldset>
        <Show when={draft().mode === "visual"}>
          <fieldset class="conditions">
            <Show when={draft().conditions?.length}>
              <legend>If all conditions hold</legend>
              <For keyed={false} each={draft().conditions}>
                {(c, i) => {
                  const update = (v) =>
                    set(
                      "conditions",
                      draft().conditions.map((old, j) => (j === i ? v : old)),
                    );
                  return (
                    <fieldset>
                      <Selector
                        required
                        label={i === 0 ? "If input" : "And condition"}
                        options={[clockOption, ...props()]}
                        value={pathKey(c())}
                        change={(k) =>
                          update(
                            k === "clock:minute"
                              ? { clock: "minute", start: 1080, end: 1320 }
                              : (makeTest(
                                  props().find((v) => pathKey(v) === k),
                                ) ?? {}),
                          )
                        }
                      />
                      <Show
                        when={c().clock}
                        fallback={
                          <TestPredicate
                            options={props()}
                            test={c()}
                            change={update}
                          />
                        }
                      >
                        <ClockFields condition input={c()} change={update} />
                      </Show>
                      <button
                        type="button"
                        onClick={() =>
                          set(
                            "conditions",
                            draft().conditions.filter((_, j) => j !== i),
                          )
                        }
                      >
                        Remove condition
                      </button>
                    </fieldset>
                  );
                }}
              </For>
            </Show>
            <button
              type="button"
              disabled={(draft().conditions?.length ?? 0) >= 4}
              onClick={() =>
                set("conditions", [...(draft().conditions ?? []), {}])
              }
            >
              Add condition
            </button>
          </fieldset>
        </Show>
        <Show when={draft().mode === "visual"}>
          <ActionSteps
            steps={steps()}
            options={stepOptions()}
            descriptor={descriptor}
            select={(key) => selected(actions(), key)}
            change={(v) => set("action", v)}
          />
        </Show>

        <button class="primary">Save rule</button>
        <button
          type="button"
          onClick={() =>
            attempt(async () => {
              if (confirm("Delete this rule?")) {
                await mcp.call("rule.remove", { slot: slot() });
                await reload(slot());
              }
            })
          }
        >
          Delete rule
        </button>
        <Show when={rules()?.[slot() - 1]?.editor}>
          <details>
            <summary>View saved rule as Lua</summary>
            <pre>{rules()[slot() - 1].source}</pre>
          </details>
        </Show>
        <small>{rules()?.[slot() - 1]?.diagnostic}</small>
      </form>
      <Traces ruleId={rules()?.[slot() - 1]?.rule_id} />
    </section>
  );
}

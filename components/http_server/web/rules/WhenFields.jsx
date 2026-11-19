import { For, Show } from "solid-js";
import {
  Selector,
  TestPredicate,
  makeTest,
  pathKey,
} from "./PropertyFields.jsx";
import { ClockFields, clockOption } from "./ClockFields.jsx";

function WhenFields(p) {
  const inputs = () => (Array.isArray(p.when) ? p.when : [p.when ?? {}]);
  const compound = () => Array.isArray(p.when);
  // Keep named raw inputs named even after removing a row: saved Lua reads those keys.
  function commit(rows) {
    if (!rows.length) {
      p.change(undefined);
      return;
    }
    if (rows.length === 1 && !rows[0].clock && (p.visual || !compound())) {
      const { name, ...property } = rows[0];
      p.change(property.device ? property : undefined);
      return;
    }
    const used = new Set(rows.map((v) => v.name));
    let n = 1;
    p.change(
      rows.map((v) => {
        if (v.name !== undefined || !pathKey(v)) return v;
        let name;
        if (v.clock && !used.has("clock")) name = "clock";
        else {
          while (used.has("input" + n)) n++;
          name = "input" + n++;
        }
        used.add(name);
        return { ...v, name };
      }),
    );
  }
  const update = (i, v) =>
    commit(inputs().map((old, j) => (j === i ? v : old)));
  const hasChanges = () => inputs().some((v) => v.op === "changes");
  return (
    <fieldset class="when">
      <legend>When</legend>
      <For keyed={false} each={inputs()}>
        {(entry, i) => {
          const change = (v) =>
            update(i, {
              ...v,
              ...(entry().name !== undefined ? { name: entry().name } : {}),
            });
          const options = () => [
            ...(!inputs().some((v, j) => j !== i && v.clock)
              ? [clockOption]
              : []),
            ...p.options.filter(
              (o) =>
                !inputs().some((v, j) => j !== i && pathKey(v) === pathKey(o)),
            ),
          ];
          return (
            <fieldset>
              <Selector
                required
                label={
                  i === 0
                    ? "When input"
                    : p.visual
                      ? "And input"
                      : "Watched input"
                }
                value={pathKey(entry())}
                options={options()}
                change={(key) => {
                  if (key === "clock:minute") {
                    change({
                      clock: "minute",
                      ...(p.visual ? { at: 1080 } : {}),
                    });
                    return;
                  }
                  const property = p.options.find((v) => pathKey(v) === key);
                  change(p.visual ? makeTest(property) : p.select(key));
                }}
              />
              <Show when={p.visual && entry().clock}>
                <ClockFields input={entry()} change={change} />
              </Show>
              <Show when={p.visual && entry().device}>
                <TestPredicate
                  options={p.options}
                  test={entry()}
                  trigger={inputs().length === 1}
                  change={change}
                />
              </Show>
              <Show when={!p.visual && compound() && pathKey(entry())}>
                <label>
                  Lua input name
                  <input
                    required
                    pattern="[A-Za-z_][A-Za-z0-9_]*"
                    maxLength={23}
                    value={entry().name}
                    onInput={(e) =>
                      update(i, { ...entry(), name: e.currentTarget.value })
                    }
                  />
                </label>
              </Show>
              <Show when={inputs().length > 1 || pathKey(entry())}>
                <button
                  type="button"
                  onClick={() => commit(inputs().filter((_, j) => j !== i))}
                >
                  Remove input
                </button>
              </Show>
            </fieldset>
          );
        }}
      </For>
      <button
        type="button"
        disabled={
          inputs().length >= 4 ||
          hasChanges() ||
          inputs().some((v) => !pathKey(v))
        }
        onClick={() => commit([...inputs(), {}])}
      >
        Add input
      </button>
      <Show when={hasChanges()}>
        <small>
          Choose a property test instead of “changes” to combine inputs.
        </small>
      </Show>
      <Show when={p.visual && inputs().length > 1}>
        <small>
          {inputs().some((v) => v.at !== undefined)
            ? "Run at the selected minute if the other When tests are already true."
            : "Run when all When tests become true together."}{" "}
          If conditions are checked only then.
        </small>
      </Show>
      <Show when={!p.visual && compound()}>
        <small>
          Lua runs when any watched value changes. Read values by name, e.g.
          event.current.{inputs().find((v) => v.name)?.name}.
        </small>
      </Show>
    </fieldset>
  );
}

export { WhenFields };

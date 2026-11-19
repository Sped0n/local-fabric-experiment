import { For, Show } from "solid-js";
import { Fields, parameters } from "../fields.jsx";
import { Selector, pathKey } from "./PropertyFields.jsx";

export function ActionSteps(p) {
  const replace = (i, v) =>
    p.change(p.steps.map((old, j) => (j === i ? v : old)));
  return (
    <fieldset>
      <legend>Then, in order</legend>
      <For keyed={false} each={p.steps}>
        {(step, i) => (
          <fieldset data-then-step="">
            <legend>Step {i + 1}</legend>
            <Selector
              label="Action"
              required
              options={p.options}
              value={pathKey(step())}
              change={(key) =>
                replace(
                  i,
                  key === "delay" ? { delay_s: 1 } : (p.select(key) ?? {}),
                )
              }
            />
            <Show when={step().delay_s !== undefined}>
              {" "}
              <label>
                Delay (seconds)
                <input
                  required
                  type="number"
                  min="1"
                  max="86400"
                  step="1"
                  value={step().delay_s}
                  onInput={(e) =>
                    replace(i, {
                      delay_s: e.currentTarget.valueAsNumber,
                    })
                  }
                />
              </label>
            </Show>
            <Show when={p.descriptor(step())}>
              {(a) => (
                <div
                  onInput={(e) =>
                    replace(i, {
                      ...step(),
                      arguments: parameters(
                        e.currentTarget.closest("fieldset"),
                        a().meta.arguments,
                      ),
                    })
                  }
                >
                  <Show when={step().native}>
                    <p>{a().meta.description}</p>
                  </Show>
                  <Fields
                    schema={a().meta.arguments}
                    defaults={step().arguments}
                  />
                </div>
              )}
            </Show>
            <button
              type="button"
              onClick={() => p.change(p.steps.filter((_, j) => j !== i))}
            >
              Remove step
            </button>
          </fieldset>
        )}
      </For>
      <button
        type="button"
        disabled={p.steps.length >= 8}
        onClick={() => p.change([...p.steps, {}])}
      >
        Add step
      </button>
    </fieldset>
  );
}

import { For, Show } from "solid-js";

const entries = (o) => Object.entries(o || {});
function parameters(form, schema) {
  let args = {};
  for (const [k, a] of entries(schema)) {
    const raw = form.elements.namedItem(k).value;
    args[k] =
      a.type === "boolean"
        ? raw === "true"
        : ["integer", "number"].includes(a.type)
          ? Number(raw)
          : raw;
  }
  return args;
}
function Fields(p) {
  return (
    <For each={Object.keys(p.schema || {})}>
      {(k) => {
        const a = () => p.schema[k];
        return (
          <label>
            {k}
            <Show
              when={a().enum || a().type === "boolean"}
              fallback={
                <input
                  name={k}
                  type={
                    ["integer", "number"].includes(a().type) ? "number" : "text"
                  }
                  step={a().step ?? (a().type === "integer" ? 1 : "any")}
                  min={a().min}
                  max={a().max}
                  required
                  value={p.defaults?.[k] ?? a().min ?? ""}
                />
              }
            >
              <select
                name={k}
                value={String(
                  p.defaults?.[k] ??
                    a().enum?.[0]?.value ??
                    a().enum?.[0] ??
                    true,
                )}
              >
                <For each={a().enum || [true, false]}>
                  {(v) => (
                    <option value={String(v?.value ?? v)}>
                      {String(v?.label ?? v)}
                    </option>
                  )}
                </For>
              </select>
            </Show>
          </label>
        );
      }}
    </For>
  );
}

export { Fields, parameters };

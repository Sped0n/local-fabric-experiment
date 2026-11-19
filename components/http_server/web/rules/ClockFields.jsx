import { For, Show } from "solid-js";

const minutes = (s) => {
  const [h, m] = s.split(":").map(Number);
  return h * 60 + m;
};
const hhmm = (m) =>
  [Math.floor(m / 60), m % 60].map((n) => String(n).padStart(2, "0")).join(":");
const weekdays = ["Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"];
const clockOption = { clock: "minute", label: "Local clock · every minute" };
function ClockFields(p) {
  const days = () =>
    p.input.weekdays ??
    (p.input.weekday ? [p.input.weekday] : [1, 2, 3, 4, 5, 6, 7]);
  function toggle(day) {
    const selected = days().includes(day)
      ? days().filter((d) => d !== day)
      : [...days(), day].sort((a, b) => a - b);
    if (!selected.length) return;
    const { weekday, ...rest } = p.input;
    p.change({ ...rest, weekdays: selected });
  }
  function mode(at) {
    const { start, end, at: old, ...rest } = p.input;
    p.change(
      at
        ? { ...rest, at: start ?? 1080 }
        : {
            ...rest,
            start: old ?? 1080,
            end: Math.min(1440, (old ?? 1080) + 60),
          },
    );
  }
  return (
    <>
      <Show when={!p.condition}>
        <label>
          Schedule
          <select
            value={p.input.at !== undefined ? "at" : "during"}
            onChange={(e) => mode(e.currentTarget.value === "at")}
          >
            <option value="at" selected={p.input.at !== undefined}>
              At
            </option>
            <option value="during" selected={p.input.at === undefined}>
              During
            </option>
          </select>
        </label>
      </Show>
      <For each={p.input.at !== undefined ? ["at"] : ["start", "end"]}>
        {(key) => (
          <label>
            {key === "at" ? "Time" : key === "start" ? "From" : "Until"}
            <input
              type="time"
              required
              value={hhmm((p.input[key] ?? (key === "end" ? 1440 : 0)) % 1440)}
              onInput={(e) => {
                const time = e.currentTarget.value;
                if (!time) return;
                p.change({
                  ...(!("at" in p.input) ? { start: 0, end: 1440 } : {}),
                  ...p.input,
                  [key]:
                    key === "end" && time === "00:00" ? 1440 : minutes(time),
                });
              }}
            />
          </label>
        )}
      </For>
      <div role="group" aria-label="Days of week" class="days">
        <span>Days</span>
        {weekdays.map((d, i) => (
          <button
            type="button"
            aria-pressed={days().includes(i + 1) ? "true" : "false"}
            disabled={days().length === 1 && days()[0] === i + 1}
            onClick={() => toggle(i + 1)}
          >
            {d}
          </button>
        ))}
      </div>
    </>
  );
}

export { ClockFields, clockOption };

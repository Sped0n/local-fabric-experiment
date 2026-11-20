import { createEffect, createSignal, For, Show } from "solid-js";
import { mcp } from "../connection.js";

const uri = "fabric://automation/runs";
const stamp = (r) =>
  r.time
    ? new Date(r.time * 1000).toLocaleString()
    : `boot +${Math.floor(r.uptime_ms / 1000)}s`;
function Feed(p) {
  const [snapshot, setSnapshot] = createSignal(null),
    [only, setOnly] = createSignal(false),
    [error, setError] = createSignal("");
  createEffect(
    () => mcp,
    (client) => {
      const stop = new AbortController();
      (async () => {
        while (!stop.signal.aborted) {
          try {
            for await (const _ of client.watch([uri], stop.signal)) {
              const s = await client.read(uri);
              if (stop.signal.aborted) return;
              setSnapshot(s);
              setError("");
            }
          } catch (e) {
            if (stop.signal.aborted) return;
            setError(e.message);
          }
          if (!stop.signal.aborted)
            await new Promise((r) => setTimeout(r, 1000));
        }
      })();
      return () => stop.abort();
    },
  );
  const rows = () =>
    (snapshot()?.finished ?? []).filter((r) => !only() || r.rule_id === p.ruleId);
  return (
    <>
      <div class="flex items-end gap-3">
        <label>
          Show runs
          <select
            value={only() ? "rule" : "all"}
            onChange={(e) => {
              setOnly(e.currentTarget.value === "rule");
            }}
          >
            <option value="all">All rules</option>
            <option value="rule">Selected rule</option>
          </select>
        </label>
        <small>Up to {snapshot()?.capacity ?? "…"} finished runs · RAM only, since restart</small>
      </div>
      <Show when={error()}>
        <p role="status">{error()}</p>
      </Show>
      <ul class="max-h-64 overflow-auto">
        <For
          each={rows()}
          fallback={
            <li>{snapshot() ? "No runs in this view." : "Loading traces…"}</li>
          }
        >
          {(r) => (
            <li class="border-b border-divider py-1">
              <strong>Run #{r.id} · {r.title || `Slot ${r.slot}`} · {r.outcome}</strong> · {r.elapsed_ms} ms · {stamp(r)}
              <div>{r.trigger}</div>
              <Show when={r.reason}><div>{r.reason}</div></Show>
            </li>
          )}
        </For>
      </ul>
    </>
  );
}
export function Traces(p) {
  const [open, setOpen] = createSignal(false);
  return (
    <details onToggle={(e) => setOpen(e.currentTarget.open)}>
      <summary>Automation traces</summary>
      <Show when={open()}>
        <Feed ruleId={p.ruleId} />
      </Show>
    </details>
  );
}

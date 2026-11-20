import { For } from "solid-js";
import { deviceActivity } from "./connection.js";

function value(row) {
  if (row.current == null) return "unknown";
  if (row.cluster === "on_off") return row.current ? "on" : "off";
  if (row.cluster === "occupancy_sensing") return row.current ? "occupied" : "unoccupied";
  return String(row.current) + (row.truncated ? "…" : "") + (row.unit ? " " + row.unit : "");
}
export function DeviceActivity() {
  return (
    <section id="activity">
      <h2>Device activity</h2>
      <p>Recent reported property changes · since restart</p>
      <ol class="max-h-56 overflow-auto">
        <For each={deviceActivity() ?? []} fallback={<li>{deviceActivity() ? "No device transitions yet." : "Loading…"}</li>}>
          {(r) => <li>#{r.device}/{r.endpoint} {r.cluster}.{r.property}: {value(r)} · {r.time ? new Date(r.time * 1000).toLocaleString() : `boot +${Math.floor(r.uptime_ms / 1000)}s`}</li>}
        </For>
      </ol>
    </section>
  );
}

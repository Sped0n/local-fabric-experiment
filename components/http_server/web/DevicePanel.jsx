import { For, Show, createSignal } from "solid-js";
import { devices, deviceState, attempt, operation, mcp } from "./connection.js";
import { Fields, parameters } from "./fields.jsx";

function Device(p) {
  const d = p.device,
    state = () => deviceState[d.device_id];
  // Snapshot only when opening the editor; live reports never rewrite unsaved inputs.
  const [draft, setDraft] = createSignal(null);
  return (
    <article>
      <h3>
        {d.name}{" "}
        <small>
          #{d.device_id} ·{" "}
          {!state() ? "loading" : state().online ? "online" : "offline"}
          {d.room ? " · " + d.room : ""}
        </small>
      </h3>
      <Show
        when={draft()}
        fallback={
          <button
            type="button"
            onClick={() => setDraft({ name: d.name, room: d.room })}
          >
            Edit name &amp; room
          </button>
        }
      >
        {(initial) => {
          return (
            <form
              onSubmit={(e) => {
                e.preventDefault();
                const { name, room } = e.currentTarget.elements;
                const args = {
                  device_id: d.device_id,
                  name: name.value.trim(),
                  room: room.value.trim(),
                };
                attempt(async () => {
                  await mcp.call("device.set_identity", args);
                  setDraft(null);
                });
              }}
            >
              <label>
                Name
                <input
                  name="name"
                  required
                  maxlength="63"
                  value={initial().name}
                />
              </label>
              <label>
                Room
                <input name="room" maxlength="63" value={initial().room} />
              </label>
              <button>Save identity</button>
              <button type="button" onClick={() => setDraft(null)}>
                Cancel
              </button>
            </form>
          );
        }}
      </Show>
      <For each={Object.keys(state()?.endpoints ?? {})}>
        {(ep) => (
          <For each={Object.keys(state()?.endpoints?.[ep]?.clusters || {})}>
            {(key) => {
              const cluster = () => state()?.endpoints?.[ep]?.clusters?.[key];
              return (
                <fieldset>
                  <legend>
                    {key} · endpoint {ep}
                  </legend>
                  <div class="values">
                    <For each={Object.entries(cluster()?.properties ?? {})}>
                      {([k, v]) => (
                        <span class="value">
                          {k}:{" "}
                          <strong>
                            {String(v.label ?? v.value ?? "unknown")}
                          </strong>{" "}
                          {v.unit}
                        </span>
                      )}
                    </For>
                  </div>
                  <For each={Object.keys(cluster()?.actions || {})}>
                    {(a) => (
                      <form
                        onSubmit={(e) => {
                          e.preventDefault();
                          const args = parameters(
                            e.currentTarget,
                            cluster().actions[a].arguments,
                          );
                          attempt(() =>
                            operation(
                              "device.action",
                              {
                                device_id: d.device_id,
                                endpoint_id: +ep,
                                cluster: key,
                                action: a,
                                arguments: args,
                              },
                              "control",
                            ),
                          );
                        }}
                      >
                        <Fields schema={cluster().actions[a].arguments} />
                        <button disabled={!state()?.online}>{a}</button>
                      </form>
                    )}
                  </For>
                </fieldset>
              );
            }}
          </For>
        )}
      </For>
      <button
        onClick={() => {
          if (
            confirm(
              "Remove the underlying Matter node and all sibling devices?",
            )
          )
            attempt(() =>
              operation(
                "device.remove",
                { device_id: d.device_id },
                "membership",
              ),
            );
        }}
      >
        Remove node
      </button>
    </article>
  );
}

export function DevicePanel() {
  const [room, setRoom] = createSignal("");
  const rooms = () =>
    [...new Set(devices.map((d) => d.room).filter(Boolean))].sort();
  const activeRoom = () => (rooms().includes(room()) ? room() : "");
  return (
    <section id="devices">
      <h2>
        Devices <small>{devices.length}</small>
      </h2>
      <form
        onSubmit={(e) => {
          e.preventDefault();
          const code = e.currentTarget.code.value;
          attempt(() => operation("commission.start", { code }, "membership"));
        }}
      >
        <label>
          Onboarding code
          <input name="code" required />
        </label>
        <button>Commission</button>
        <button
          type="button"
          onClick={() => attempt(() => mcp.call("commission.cancel", {}))}
        >
          Cancel
        </button>
      </form>
      <Show when={rooms().length}>
        <label class="mt-2 max-w-60">
          Filter by room
          <select
            value={activeRoom()}
            onChange={(e) => setRoom(e.currentTarget.value)}
          >
            <option value="">All rooms</option>
            <For each={rooms()}>
              {(r) => (
                <option selected={r === activeRoom()} value={r}>
                  {r}
                </option>
              )}
            </For>
          </select>
        </label>
      </Show>
      <div class="mt-3 grid items-start gap-3 grid-cols-[repeat(auto-fit,minmax(min(100%,22rem),1fr))]">
        <For
          each={devices.filter((d) => !activeRoom() || d.room === activeRoom())}
          fallback={<p>No devices reported yet.</p>}
        >
          {(d) => <Device device={d} />}
        </For>
      </div>
    </section>
  );
}

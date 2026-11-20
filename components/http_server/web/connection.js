import { createSignal, createEffect, createStore, reconcile } from "solid-js";
import { createClient } from "./mcp.js";

let mcp;

const [token, setToken] = createSignal(""),
  [link, setLink] = createSignal("Disconnected"),
  [notice, setNotice] = createSignal("");
const [devices, setDevices] = createStore([]),
  [deviceState, setDeviceState] = createStore({}),
  [deviceActivity, setDeviceActivity] = createSignal(null),
  [rules, setRules] = createSignal(null),
  [localActions, setLocalActions] = createSignal([]);
async function attempt(fn) {
  try {
    await fn();
  } catch (e) {
    setNotice(e.message);
  }
}
const wait = (ms) => new Promise((r) => setTimeout(r, ms));
async function operation(path, body, kind) {
  await mcp.call(path, body);
  setNotice("Accepted; waiting for " + kind);
  for (let i = 0; i < 120; i++) {
    await wait(500);
    const s = await mcp.read("fabric://status");
    if (s[kind] !== "pending") {
      setNotice(kind + ": " + s[kind]);
      return;
    }
  }
  setNotice("Still pending. Check status before retrying.");
}
function connect() {
  createEffect(token, (secret) => {
    if (!secret) {
      setDevices(reconcile([]));
      setDeviceState(reconcile({}));
      setRules(null);
      setDeviceActivity(null);
      setLocalActions([]);
      return;
    }
    const abort = new AbortController();
    const client = createClient(secret);
    mcp = client;

    const signal = abort.signal;
    (async () => {
      while (!signal.aborted) {
        try {
          setLink("Connecting");
          for await (const { initial, uris } of client.watch(
            [
              "fabric://devices/metadata",
              "fabric://devices/state",
              "fabric://activity",
              "fabric://automation/actions",
              "fabric://automation/rules",
            ],
            signal,
          )) {
            if (initial) setLink("Live");
            for (const uri of uris) {
              const snapshot = await client.read(uri);
              if (signal.aborted) return;
              if (uri === "fabric://devices/metadata")
                setDevices(
                  reconcile(
                    Object.entries(snapshot.devices).map(([id, identity]) => ({
                      ...identity,
                      device_id: Number(id),
                    })),
                    "device_id",
                  ),
                );
              if (uri === "fabric://devices/state")
                setDeviceState(reconcile(snapshot.devices));
              if (uri === "fabric://activity") setDeviceActivity(snapshot);
              if (uri === "fabric://automation/actions")
                setLocalActions(snapshot);
              if (uri === "fabric://automation/rules") setRules(snapshot);
            }
          }
        } catch (e) {
          if (signal.aborted) return;
          if (e.message.startsWith("401 ")) {
            setToken("");
            setNotice("Token rejected. Use the current serial token.");
            return;
          }
          setNotice(e.message);
        }
        setLink("Reconnecting");
        await wait(1000);
      }
    })();
    return () => abort.abort();
  });
}

export {
  mcp,
  token,
  setToken,
  link,
  notice,
  setNotice,
  devices,
  deviceState,
  deviceActivity,
  rules,
  setRules,
  localActions,
  attempt,
  operation,
  connect,
};

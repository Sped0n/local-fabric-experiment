// Disposable Chipmunk dimmable light. Verifies exact filtering within the SAME device.
import assert from "node:assert/strict";
import { createClient } from "../components/http_server/web/mcp.js";
const { FABRIC_URL: url, FABRIC_TOKEN: token } = process.env;
if (!url || !token) throw Error("Set FABRIC_URL and FABRIC_TOKEN");
const c = createClient(token, url);
const devices = (await c.read("fabric://devices/state")).devices;
const match = Object.entries(devices).find(
  ([, d]) =>
    d.online &&
    d.endpoints["1"]?.clusters.level_control &&
    d.endpoints["1"].clusters.on_off &&
    !d.endpoints["1"].clusters.color_control,
);
assert(match, "commission dimmable light");
const device_id = Number(match[0]);
const base = `fabric://devices/${device_id}/endpoints/1/clusters/`,
  on = base + "on_off/properties/on",
  level = base + "level_control/properties/level_percent";
const action = (cluster, action, args) =>
    c.call("device.action", {
      device_id,
      endpoint_id: 1,
      cluster,
      action,
      arguments: args,
    }),
  pause = (ms) => new Promise((r) => setTimeout(r, ms));
async function until(uri, v) {
  for (let i = 0; i < 40; i++) {
    if (
      (await c.read(uri)).value === v &&
      (await c.read("fabric://status")).control === "ESP_OK"
    )
      return;
    await pause(150);
  }
  throw Error(uri + " did not reach " + v);
}
const oldOn = (await c.read(on)).value,
  oldLevel = (await c.read(level)).value;
const ac = new AbortController(),
  bc = new AbortController(),
  timeout = AbortSignal.timeout(18000);
const a = c.watch([on], AbortSignal.any([ac.signal, timeout])),
  b = c.watch([level], AbortSignal.any([bc.signal, timeout]));
try {
  await action("on_off", "set", { on: true });
  await until(on, true);
  assert.deepEqual((await a.next()).value, { initial: true, uris: [on] });
  assert.deepEqual((await b.next()).value, { initial: true, uris: [level] });
  let sawOn = false;
  const pending = a.next().then((r) => {
    sawOn = true;
    return r;
  });
  const nextLevel = oldLevel === 55 ? 70 : 55;
  await action("level_control", "set_level", { level_percent: nextLevel });
  assert.deepEqual((await b.next()).value, { initial: false, uris: [level] });
  await until(level, nextLevel);
  await pause(350);
  assert.equal(sawOn, false, "level change woke OnOff subscriber");
  bc.abort();
  await b.return();
  await action("on_off", "set", { on: false });
  assert.deepEqual((await pending).value, { initial: false, uris: [on] });
  await until(on, false);
  console.log(
    "PASS: exact property read, filtered change on same device, independent cancellation",
  );
} finally {
  ac.abort();
  bc.abort();
  await Promise.allSettled([a.return(), b.return()]);
  if (oldLevel > 0) {
    await action("level_control", "set_level", { level_percent: oldLevel });
    await until(level, oldLevel);
  }
  await action("on_off", "set", { on: oldOn });
  await until(on, oldOn);
}

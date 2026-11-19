// Exercises the four Chipmunk fixtures documented in README on a commissioned controller.
// Run only against disposable fixtures: this intentionally changes device states.
import assert from "node:assert/strict";
import { createClient } from "../components/http_server/web/mcp.js";
const { FABRIC_URL: url, FABRIC_TOKEN: token } = process.env;
if (!url || !token)
  throw Error("Set FABRIC_URL and FABRIC_TOKEN (current serial bearer token)");
const mcp = createClient(token, url);
async function api(path, body, ok = true) {
  if (path === "/action") {
    if (!ok) {
      await assert.rejects(
        mcp.call("device.action", body),
        /ESP_ERR_INVALID_ARG/,
      );
      return;
    }
    return mcp.call("device.action", body);
  }
  if (path.startsWith("/devices/"))
    return JSON.parse(
      await mcp.call("device.get", { device_id: Number(path.slice(9)) }),
    );
  return mcp.read("fabric:/" + path);
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
async function until(read, match, label) {
  let value;
  for (let i = 0; i < 40; i++) {
    value = await read();
    if (match(value)) return value;
    await sleep(250);
  }
  throw Error(`${label}: last observed ${JSON.stringify(value)}`);
}
const list = await api("/devices/metadata"),
  clusters = new Map();
for (const device_id of Object.keys(list.devices).map(Number)) {
  const d = await api("/devices/" + device_id);
  assert(d.online, `offline: ${d.name}`);
  for (const [ep, endpoint] of Object.entries(d.endpoints))
    for (const [cluster, c] of Object.entries(endpoint.clusters)) {
      // Exercise pause/resume on the same RVC that this test puts into Cleaning.
      if (
        cluster === "rvc_operational_state" &&
        !endpoint.clusters.rvc_run_mode
      )
        continue;
      const candidate = { device_id, endpoint_id: +ep, cluster, c };
      if (
        !clusters.has(cluster) ||
        Object.keys(c.actions).length >
          Object.keys(clusters.get(cluster).c.actions).length
      )
        clusters.set(cluster, candidate);
    }
}
const measurements = {
  power_source: ["battery_percent", "percent"],
  temperature_measurement: ["temperature_celsius", "celsius"],
  relative_humidity_measurement: ["humidity_percent", "percent"],
  illuminance_measurement: ["illuminance_lux", "lux"],
  pressure_measurement: ["pressure_hpa", "hectopascals"],
};
for (const [key, [property, unit]] of Object.entries(measurements)) {
  const p = clusters.get(key).c.properties[property];
  assert.equal(p.unit, unit);
  assert.equal(p.type, "number");
  assert(Number.isFinite(p.value) && p.value >= p.min && p.value <= p.max);
  assert.equal(
    p.step,
    {
      temperature_measurement: 0.1,
      relative_humidity_measurement: 0.1,
      power_source: 0.5,
      pressure_measurement: 1,
      illuminance_measurement: 1,
    }[key],
  );
  assert(Math.abs(p.value / p.step - Math.round(p.value / p.step)) < 1e-7);
  console.log(key, p.value, p.unit);
}
for (const [key, p] of [
  ["occupancy_sensing", "occupied"],
  ["boolean_state", "state"],
])
  assert.equal(typeof clusters.get(key).c.properties[p].value, "boolean");
async function act(key, name, args, verify, invalid = false) {
  const { device_id, endpoint_id, cluster, c } = clusters.get(key);
  assert(c.actions[name], `${key}.${name} not advertised`);
  await api(
    "/action",
    { device_id, endpoint_id, cluster, action: name, arguments: args },
    !invalid,
  );
  if (invalid) return;
  await until(
    () => api("/status"),
    (s) => s.control === "ESP_OK",
    key + "." + name + " completion",
  );
  const snap = await until(
    () => api("/devices/" + device_id),
    (d) => verify(d.endpoints[endpoint_id].clusters[key].properties),
    key + "." + name,
  );
  console.log(
    key,
    name,
    JSON.stringify(snap.endpoints[endpoint_id].clusters[key].properties),
  );
}
await act("on_off", "set", { on: false }, (p) => p.on.value === false);
await act(
  "level_control",
  "set_level",
  { level_percent: 43 },
  (p) => Math.abs(p.level_percent.value - 43) <= 1,
);
await act(
  "fan_control",
  "set_percent",
  { percent: 55 },
  (p) => p.percent.value === 55,
);
const lamp = clusters.get("color_control");
await api("/action", {
  device_id: lamp.device_id,
  endpoint_id: lamp.endpoint_id,
  cluster: "on_off",
  action: "set",
  arguments: { on: true },
});
await until(
  () => api("/devices/" + lamp.device_id),
  (d) =>
    d.endpoints[lamp.endpoint_id].clusters.on_off.properties.on.value === true,
  "color lamp on",
);
await until(
  () => api("/status"),
  (s) => s.control === "ESP_OK",
  "lamp On terminal result",
);
await act(
  "color_control",
  "set_color_temperature",
  { color_temperature_kelvin: 3500 },
  (p) => Math.abs(p.color_temperature_kelvin.value - 3500) < 20,
);
await act(
  "color_control",
  "set_color",
  { hue_degrees: 160, saturation_percent: 70 },
  (p) =>
    Math.abs(p.hue_degrees.value - 160) < 4 &&
    Math.abs(p.saturation_percent.value - 70) < 3,
);
await act(
  "thermostat",
  "set_mode",
  { mode: "heat" },
  (p) => p.system_mode.value === "heat",
);
await act(
  "thermostat",
  "set_heating_setpoint",
  { temperature_c: 21.5 },
  (p) => p.heating_setpoint_c.value === 21.5,
);
await act(
  "thermostat",
  "set_mode",
  { mode: "auto" },
  (p) => p.system_mode.value === "auto",
);
const thermostat = clusters.get("thermostat");
const auto = (await api("/devices/" + thermostat.device_id)).endpoints[
  thermostat.endpoint_id
].clusters.thermostat;
assert(
  auto.actions.set_heating_setpoint.arguments.temperature_c.max <
    auto.properties.cooling_setpoint_c.value,
);
await act(
  "thermostat",
  "set_heating_setpoint",
  { temperature_c: 21.55 },
  () => false,
  true,
);
await act(
  "thermostat",
  "set_heating_setpoint",
  { temperature_c: auto.properties.cooling_setpoint_c.value },
  () => false,
  true,
);
await act("rvc_run_mode", "set_mode", { mode: 0 }, (p) => p.mode.value === 0);
await act("rvc_clean_mode", "set_mode", { mode: 1 }, (p) => p.mode.value === 1);
await act("rvc_run_mode", "set_mode", { mode: 1 }, (p) => p.mode.value === 1);
await act(
  "rvc_operational_state",
  "pause",
  {},
  (p) => p.state.value === "paused",
);
await act(
  "rvc_operational_state",
  "resume",
  {},
  (p) => p.state.value === "running",
);
await act("rvc_run_mode", "set_mode", { mode: 0 }, (p) => p.mode.value === 0);
await act("rvc_clean_mode", "set_mode", { mode: 255 }, () => false, true);
await act(
  "color_control",
  "set_color_temperature",
  { color_temperature_kelvin: 1 },
  () => false,
  true,
);
await act(
  "level_control",
  "set_level",
  { level_percent: 200 },
  () => false,
  true,
);
assert.equal(clusters.size, 15);
console.log("PASS: 15 semantic clusters");

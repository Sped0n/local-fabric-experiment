// Uses ALL 32 slots on a disposable board. Run fill, reboot the board, then run restore.
import assert from "node:assert/strict";
import { writeFile } from "node:fs/promises";
import { createClient } from "../components/http_server/web/mcp.js";
const { FABRIC_URL: url, FABRIC_TOKEN: token, FABRIC_PIPE: pipe } = process.env;
const phase = process.argv[2];
if (!url || !token || !pipe || !["fill", "restore", "clean"].includes(phase))
  throw Error("Set FABRIC_URL/TOKEN/PIPE; choose fill, restore or clean");
const c = createClient(token, url),
  rules = () => c.read("fabric://automation/rules"),
  pause = (ms) => new Promise((r) => setTimeout(r, ms));
const prefix = "slots32-e2e-";
async function clean() {
  for (const [i, r] of (await rules()).entries())
    if (r?.title === prefix + (i + 1))
      await c.call("rule.remove", { slot: i + 1 });
}
async function main() {
  if (phase === "clean") return clean();
  const initial = await rules();
  assert.equal(initial.length, 32);
  if (phase === "fill")
    assert(
      initial.every((r) => r === null),
      "all slots must be empty",
    );
  else
    assert(
      initial.every((r, i) => r?.title === prefix + (i + 1)),
      "expected the test-owned slots after reboot",
    );
  const match = Object.entries(
    (await c.read("fabric://devices/state")).devices,
  ).find(([, d]) => d.online && d.endpoints["2"]?.clusters.boolean_state);
  assert(match, "wait for contact fixture to come online");
  const when = {
      device: Number(match[0]),
      endpoint: 2,
      cluster: "boolean_state",
      property: "state",
    },
    uri = `fabric://devices/${when.device}/endpoints/2/clusters/boolean_state/properties/state`;
  function definition(slot, replaced = false) {
    let source = `return '${replaced ? "replacement" : slot}:' .. (event.current and 'on' or 'off')\n--`;
    const def = {
      title: prefix + slot,
      enabled: true,
      mode: "raw",
      when,
      source,
    };
    def.source += "x".repeat(3072 - Buffer.byteLength(JSON.stringify(def)));
    assert.equal(Buffer.byteLength(JSON.stringify(def)), 3072);
    return def;
  }
  if (phase === "fill") {
    for (let slot = 1; slot <= 32; slot++)
      await c.call("rule.save", { slot, definition: definition(slot) });
    await c.call("rule.save", { slot: 17, definition: definition(17, true) });
  } else {
    for (let slot = 1; slot <= 32; slot++)
      assert.equal(
        initial[slot - 1].source,
        definition(slot, slot === 17).source,
      );
  }
  const before = (await c.read(uri)).value;
  assert.equal(typeof before, "boolean");
  await pause(400);
  const start = Date.now();
  await writeFile(
    pipe,
    JSON.stringify({
      Name: "SetBooleanState",
      EndpointId: 2,
      NewState: !before,
    }) + "\n",
  );
  for (let i = 0; ; i++) {
    const list = await rules();
    if (
      list.every(
        (r, j) =>
          r.diagnostic ===
          `${j === 16 ? "replacement" : j + 1}:${before ? "off" : "on"}`,
      )
    )
      break;
    if (i === 25) throw Error("not all matching Lua bodies executed");
    await pause(200);
  }
  console.log(
    `PASS ${phase}: all 32 full-size rules executed on one contact report, including replacement (${Date.now() - start} ms including MCP readback)`,
  );
  if (phase === "restore") await clean();
}
await main();

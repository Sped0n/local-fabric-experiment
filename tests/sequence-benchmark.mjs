// S31, isolated empty rules partition, SNTP synchronized. No Matter fixture needed for scheduler timing.
// Use CONFIG_FABRIC_AUTOMATION_TIMING for worker time and wake lateness (not network latency).
import assert from "node:assert/strict";
import { createClient } from "../components/http_server/web/mcp.js";
const { FABRIC_URL: url, FABRIC_TOKEN: token } = process.env;
assert(url && token, "set FABRIC_URL and FABRIC_TOKEN");
const client = createClient(token, url),
  rules = () => client.read("fabric://automation/rules"),
  sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const empty = await rules();
assert.equal(empty.length, 32);
assert(
  empty.every((v) => v === null),
  "requires 32 empty dedicated test slots",
);
const prefix = "bench-sequence-",
  owned = [];
const start = Date.now();
try {
  for (let i = 0; i < 32; i++) {
    const slot = i + 1,
      when = [{ name: "clock", clock: "minute" }];
    const source = `local minute=event.current.clock.minute; automation.delay(3); automation.delay(1+(minute+${i})%32)`;
    await client.call("rule.save", {
      slot,
      definition: {
        title: prefix + slot,
        enabled: true,
        mode: "raw",
        when,
        source,
      },
    });
    owned.push(slot);
  }
  console.log("Installed 32 different minute-triggered sequences");
  let samples = 0,
    active = false;
  const end = Date.now() + 260000;
  while (samples < 3 && Date.now() < end) {
    const rows = await rules();
    const running = rows.filter((r) => r.diagnostic === "running").length;
    if (running === 32) active = true;
    if (active && rows.every((r) => r.diagnostic === "completed")) {
      samples++;
      active = false;
      console.log(
        `batch ${samples}: all 32 started and both delay steps completed (+${Date.now() - start} ms)`,
      );
    }
    await sleep(550);
  }
  assert.equal(samples, 3, "three synchronized batches did not complete");
} finally {
  const rows = await rules();
  for (const slot of owned)
    if (rows[slot - 1]?.title === prefix + slot)
      await client.call("rule.remove", { slot });
}

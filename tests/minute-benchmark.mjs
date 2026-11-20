// S31 benchmark. Requires free rule slots on an otherwise idle board and SNTP sync.
// Enable CONFIG_FABRIC_AUTOMATION_TIMING for on-board `rule_timing` UART samples.
import assert from "node:assert/strict";
import { createClient } from "../components/http_server/web/mcp.js";
const { FABRIC_URL: url, FABRIC_TOKEN: token } = process.env;
if (!url || !token) throw Error("Set FABRIC_URL and FABRIC_TOKEN");
const m = createClient(token, url),
  rows = () => m.read("fabric://automation/rules"),
  wait = (ms) => new Promise((r) => setTimeout(r, ms)),
  prefix = "minute-bench-";
async function clean() {
  for (const [i, r] of (await rows()).entries())
    if (r?.title === prefix + (i + 1))
      await m.call("rule.remove", { slot: i + 1 });
}
if (process.argv[2] === "clean") {
  await clean();
  process.exit(0);
}
const slots = (await rows()).flatMap((r, i) => (r ? [] : [i + 1]));
assert(slots.length >= 2, "need two or more free slots");
const match = Object.entries(
  (await m.read("fabric://devices/state")).devices,
).find(([, d]) => d.online && d.endpoints["2"]?.clusters.boolean_state);
assert(match, "commission the Chipmunk contact fixture");
const when = [
  {
    name: "occupied",
    device: Number(match[0]),
    endpoint: 2,
    cluster: "boolean_state",
    property: "state",
  },
  { name: "clock", clock: "minute" },
];
const source = `local function ready(s)
  return s.occupied and s.clock.weekday == 5 and s.clock.hour >= 18 and s.clock.hour < 22
end
local p,c=event.previous,event.current
if ready(c) and not ready(p) then return 'edge:' .. c.clock.minute end
return c.clock.weekday .. ':' .. c.clock.hour .. ':' .. c.clock.minute`;
let installed = 0;
const phase = async (count, samples) => {
  while (installed < count) {
    const slot = slots[installed++];
    assert.equal((await rows())[slot - 1], null, "slot taken");
    await m.call("rule.save", {
      slot,
      definition: {
        enabled: true,
        title: prefix + slot,
        when,
        source,
      },
    });
  }
  let last = (await rows())[slots[count - 1] - 1].diagnostic;
  let seen = 0;
  const until = Date.now() + (samples + 1) * 65000;
  while (seen < samples && Date.now() < until) {
    await wait(1300);
    const all = await rows(),
      rs = slots.slice(0, count).map((slot) => all[slot - 1]);
    const diag = rs[count - 1].diagnostic;
    if (!diag || diag === last) continue;
    assert(
      rs.every((r) => r.diagnostic === diag),
      "handlers disagreed about current minute",
    );
    seen++;
    last = diag;
    console.log(`${count} rules: tick ${seen} at ${diag}`);
  }
  assert.equal(seen, samples, "missing synchronized minute wakes");
};
try {
  await phase(1, 1);
  await phase(slots.length, 3);
} finally {
  await clean();
}

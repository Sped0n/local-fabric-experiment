// Real board and disposable Chipmunk contact. Reserve a spare rule slot during this test.
import assert from "node:assert/strict";
import { writeFile } from "node:fs/promises";
import { createClient } from "../components/http_server/web/mcp.js";
const { FABRIC_URL: url, FABRIC_TOKEN: token, FABRIC_PIPE: pipe } = process.env;
assert(url && token && pipe, "set FABRIC_URL, FABRIC_TOKEN, FABRIC_PIPE");
const c = createClient(token, url),
  index = () => c.read("fabric://automation/runs"),
  rules = () => c.read("fabric://automation/rules"),
  detail = (id) => c.read(`fabric://automation/runs/${id}`),
  sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const slot = (await rules()).findLastIndex((r) => !r) + 1;
assert(slot, "need a spare slot");
const match = Object.entries(
  (await c.read("fabric://devices/state")).devices,
).find(([, d]) => d.online && d.endpoints["2"]?.clusters.boolean_state);
assert(match, "contact fixture is offline");
const path = {
    device: Number(match[0]),
    endpoint: 2,
    cluster: "boolean_state",
    property: "state",
  },
  property = `fabric://devices/${path.device}/endpoints/2/clusters/boolean_state/properties/state`,
  original = (await c.read(property)).value;
const key = `run-test-${Date.now()}`,
  title = (suffix) => key + "/" + suffix;
async function until(fn, label) {
  for (let i = 0; i < 60; i++) {
    const v = await fn();
    if (v) return v;
    await sleep(100);
  }
  throw Error("timeout: " + label);
}
async function report(value) {
  await writeFile(
    pipe,
    JSON.stringify({
      Name: "SetBooleanState",
      EndpointId: 2,
      NewState: value,
    }) + "\n",
  );
  await until(
    async () => (await c.read(property)).value === value,
    "contact report",
  );
}
async function save(label, def) {
  await c.call("rule.save", {
    slot,
    definition: { title: title(label), enabled: true, ...def },
  });
  await sleep(200);
  return (await rules())[slot - 1].rule_id;
}
const remove = () => c.call("rule.remove", { slot });
const finished = (label, outcome) =>
  until(
    async () =>
      (await index()).finished.find(
        (r) => r.title === title(label) && r.outcome === outcome,
      ),
    label,
  );
try {
  await report(false);
  const first = await save("guard", {
    when: path,
    source:
      "if not event.current then return end; automation.invoke_local('demo.enqueue_note',{text='private-action-payload'}); return automation.skip('expected false',event.current)",
  });
  await report(true);
  const skipped = await detail((await finished("guard", "skipped")).id);
  assert.match(skipped.reason, /false: observed true/);
  assert.match(skipped.trigger, /false → true/);
  await report(false);
  await sleep(200);
  assert.equal(
    (await index()).finished.filter((r) => r.rule_id === first).length,
    1,
  );
  await save("waiting", {
    when: path,
    source: `automation.delay(60); automation.invoke_local('demo.enqueue_note',{text='private-action-payload'})`,
  });
  await report(true);
  // Trigger reaches its 60-second delay, but pending and ignored retriggers do not publish a trace.
  await sleep(300);
  await report(false);
  assert(!(await index()).finished.some((r) => r.title === title("waiting")));
  await remove();
  const cancelled = await detail((await finished("waiting", "cancelled")).id);
  assert.match(cancelled.reason, /removed/);
  assert(cancelled.elapsed_ms >= 250);
  assert(!JSON.stringify(cancelled).includes("private-action-payload"));
  const second = await save("failure", {
    when: path,
    source: `automation.delay(1); matter.invoke_action(${path.device},2,'on_off','not_a_fixture_action',{}); automation.delay(42)`,
  });
  assert.notEqual(first, second);
  await report(true);
  const failure = await detail((await finished("failure", "failed")).id);
  assert.match(failure.reason, /Step 2: .*not_a_fixture_action: ESP_ERR_/);
  assert(failure.elapsed_ms >= 1000);
  await save("lua", {
    when: path,
    source: `automation.delay(1); missing_function()`,
  });
  await report(false);
  const lua = await detail((await finished("lua", "failed")).id);
  assert.match(lua.reason, /missing_function|nil value/);
  assert.equal((await detail(cancelled.id)).title, title("waiting"));
  assert.equal((await detail(skipped.id)).rule_id, first);
  const all = await index();
  assert(
    all.finished
      .filter((r) => r.rule_id === second)
      .every((r) => r.title === title("lua") || r.title === title("failure")),
  );
  console.log(
    "PASS: skipped guard, quiet mismatch and delay, cancellation, slot reuse, failed action and Lua error",
  );
} finally {
  if ((await rules())[slot - 1]?.title.startsWith(key + "/")) await remove();
  await report(original);
}

// Run only on the disposable all-devices-app fixture documented in README.
import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { constants } from "node:fs";
import { open, realpath } from "node:fs/promises";
import { join } from "node:path";
import { setTimeout as sleep } from "node:timers/promises";
import { createClient } from "../components/http_server/web/mcp.js";

const { FABRIC_URL, FABRIC_TOKEN, FABRIC_FIXTURE, FABRIC_PIPE } = process.env;
assert(
  FABRIC_URL &&
    FABRIC_TOKEN &&
    FABRIC_PIPE &&
    /^[\w][\w.-]*$/.test(FABRIC_FIXTURE ?? ""),
  "set FABRIC_URL, FABRIC_TOKEN, FABRIC_PIPE and FABRIC_FIXTURE (disposable container name)",
);
const docker = (command) =>
  execFileSync("docker", [command, FABRIC_FIXTURE], { encoding: "utf8" });
const [fixture] = JSON.parse(docker("inspect"));
assert(
  fixture.State.Running &&
    !fixture.State.Paused &&
    fixture.Path.endsWith("all-devices-app"),
);
const mount = fixture.Mounts.find((m) => m.Destination === "/tmp");
assert(
  mount &&
    (await realpath(FABRIC_PIPE)) ===
      (await realpath(join(mount.Source, "actions"))),
  "wrong fixture FIFO",
);
const c = createClient(FABRIC_TOKEN, FABRIC_URL);
const devices = async () => (await c.read("fabric://devices/state")).devices;
const before = await devices();
const contact = Object.keys(before).find(
  (id) => before[id].endpoints[2]?.clusters.boolean_state,
);
const state = (snapshot) =>
  snapshot[contact].endpoints[2].clusters.boolean_state.properties.state.value;
assert(contact && before[contact].online);
const original = state(before);
assert.equal(typeof original, "boolean");
const contactRows = async () =>
  (await c.read("fabric://activity")).filter(
    (r) =>
      r.device === Number(contact) &&
      r.endpoint === 2 &&
      r.cluster === "boolean_state" &&
      r.property === "state",
  );
const shape = (d) =>
  Object.fromEntries(
    Object.entries(d.endpoints).map(([ep, e]) => [
      ep,
      Object.keys(e.clusters).sort(),
    ]),
  );
async function until(check, label) {
  const deadline = Date.now() + 120000;
  while (Date.now() < deadline) {
    const result = await check();
    if (result) return result;
    await sleep(300);
  }
  throw Error("timed out: " + label);
}
const pipe = await open(FABRIC_PIPE, constants.O_WRONLY | constants.O_NONBLOCK);
const setContact = (on) =>
  pipe.write(
    JSON.stringify({ Name: "SetBooleanState", EndpointId: 2, NewState: on }) +
      "\n",
  );
let queued = false;
try {
  assert((await pipe.stat()).isFIFO());
  let members, cutoff;
  docker("pause");
  try {
    members = await until(async () => {
      const snapshot = await devices();
      if (snapshot[contact]?.online !== false) return;
      return Object.keys(before).filter(
        (id) => before[id].online && snapshot[id]?.online === false,
      );
    }, "fixture offline");
    assert(
      members.length >= 2,
      "expected several logical devices on one subscription",
    );
    cutoff = (await contactRows())[0]?.uptime_ms ?? -1;
    await setContact(!original); // Applied on resume, before the new subscription's priming snapshot.
    queued = true;
  } finally {
    docker("unpause");
  }
  await until(async () => {
    const snapshot = await devices();
    if (!members.some((id) => snapshot[id]?.online)) return false;
    for (const id of members) {
      assert(snapshot[id]?.online, "partial reconnect");
      assert.deepEqual(
        shape(snapshot[id]),
        shape(before[id]),
        "partial topology",
      );
    }
    assert.equal(state(snapshot), !original, "missing new baseline");
    return true;
  }, "complete snapshot");
  assert(
    !(await contactRows()).some((r) => r.uptime_ms > cutoff),
    "reconnect produced false contact activity",
  );
  await setContact(original);
  await until(
    async () =>
      (await contactRows()).some(
        (r) => r.uptime_ms > cutoff && r.current === original,
      ),
    "incremental contact activity",
  );
  queued = false;
  console.log(
    `PASS: ${members.length} logical devices returned together; changed baseline stayed quiet; subsequent contact change recorded`,
  );
} finally {
  try {
    if (queued) await setContact(original);
  } finally {
    await pipe.close();
  }
}

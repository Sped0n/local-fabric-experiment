import assert from "node:assert/strict";
import {
  request,
  createClient,
  version,
} from "../components/http_server/web/mcp.js";
const { FABRIC_URL: url, FABRIC_TOKEN: token } = process.env;
if (!url || !token) throw Error("Set FABRIC_URL and FABRIC_TOKEN");
async function post(method, params, modify) {
  const req = request(method, params, "wire-test", token);
  modify?.(req);
  return fetch(url + "/mcp", { ...req, signal: AbortSignal.timeout(5000) });
}
async function checkError(res, status, code) {
  assert.equal(res.status, status);
  assert.equal((await res.json()).error.code, code);
}
const uri = "fabric://status";
assert.equal(
  (await post("server/discover", {}, (r) => delete r.headers.Authorization))
    .status,
  401,
);
assert.equal(
  (
    await post(
      "server/discover",
      {},
      (r) => (r.headers.Origin = "http://other.invalid"),
    )
  ).status,
  403,
);
const discovery = await post("server/discover", {});
assert.equal(discovery.headers.get("Mcp-Session-Id"), null);
const { result } = await discovery.json();
assert.deepEqual(result.supportedVersions, [version]);
assert.deepEqual(result.capabilities, {
  tools: {},
  resources: { subscribe: true },
});
assert.equal(result.cacheScope, "private");
assert.equal(result.resultType, "complete");
await checkError(
  await post("ping", {}, (r) => delete r.headers["Mcp-Method"]),
  400,
  -32020,
);
await checkError(
  await post(
    "resources/read",
    { uri },
    (r) => (r.headers["Mcp-Name"] = "fabric://devices/metadata"),
  ),
  400,
  -32020,
);
await checkError(
  await post(
    "ping",
    {},
    (r) => (r.headers["MCP-Protocol-Version"] = "2025-11-25"),
  ),
  400,
  -32020,
);
const legacy = await post("ping", {}, (r) => {
  r.body = r.body.replace(version, "2025-11-25");
  r.headers["MCP-Protocol-Version"] = "2025-11-25";
});
assert.equal(legacy.status, 400);
const e = (await legacy.json()).error;
assert.equal(e.code, -32022);
assert.deepEqual(e.data.supported, [version]);
await checkError(await post("unknown", {}), 404, -32601);
await checkError(
  await post("tools/call", {
    name: "device.get",
    arguments: { device_id: -1 },
  }),
  400,
  -32602,
);
assert.equal(
  (
    await post(
      "resources/read",
      { uri },
      (r) =>
        (r.headers["Mcp-Name"] =
          `=?base64?${Buffer.from(uri).toString("base64")}?=`),
    )
  ).status,
  200,
);
const client = createClient(token, url),
  a = new AbortController(),
  b = new AbortController();
const resources = (
  await (await post("resources/list", {})).json()
).result.resources.map((r) => r.uri);
assert(
  resources.includes("fabric://automation/rules") &&
    resources.includes("fabric://automation/actions"),
);
const note = (await client.read("fabric://automation/actions")).find(
  (a) => a.name === "demo.enqueue_note",
);
assert.equal(note.arguments.text.type, "string");
assert(note.description);
const tools = (await (await post("tools/list", {})).json()).result.tools;
const schema = tools.find((t) => t.name === "rule.save").inputSchema.properties;
assert.equal(schema.slot.maximum, 32);
assert(schema.definition.required.includes("when"));
const signal = AbortSignal.timeout(20000);
const first = client.watch([uri], AbortSignal.any([a.signal, signal])),
  second = client.watch([uri], AbortSignal.any([b.signal, signal]));
try {
  for (const s of [first, second])
    assert.deepEqual((await s.next()).value, { initial: true, uris: [uri] });
  const { devices } = await client.read("fabric://devices/metadata");
  const device_id = Number(Object.keys(devices)[0]);
  assert(device_id, "commission a fixture");
  assert.equal(
    JSON.parse(await client.call("device.get", { device_id })).device_id,
    device_id,
  );
  assert(resources.includes("fabric://devices/state"));
  for (const s of [first, second])
    assert.deepEqual((await s.next()).value, { initial: false, uris: [uri] });
  a.abort();
  await first.return();
  assert.deepEqual((await second.next()).value, {
    initial: false,
    uris: [uri],
  });
} finally {
  a.abort();
  b.abort();
  await Promise.allSettled([first.return(), second.return()]);
}
console.log(
  "PASS: stateless discovery, headers, auth, inline tool and independent subscription lifetimes",
);

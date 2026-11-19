// Destructive opt-in. Recover the new token on UART with `fabric token`.
import assert from "node:assert/strict";
import { createClient, request } from "../components/http_server/web/mcp.js";
const {
  FABRIC_URL: url,
  FABRIC_TOKEN: token,
  FABRIC_ROTATE_TOKEN: optIn,
} = process.env;
if (!url || !token || optIn !== "1")
  throw Error(
    "Set FABRIC_URL, FABRIC_TOKEN and FABRIC_ROTATE_TOKEN=1 for a disposable test board",
  );
const headers = { Authorization: `Bearer ${token}` };
const rotate = (h, body) =>
  fetch(url + "/settings/token/rotate", {
    method: "POST",
    headers: h,
    body,
    signal: AbortSignal.timeout(4000),
  });
assert.equal((await rotate({})).status, 401);
assert.equal(
  (await rotate({ ...headers, Origin: "http://other.invalid" })).status,
  403,
);
assert.equal((await rotate(headers, "unexpected")).status, 400);
const timeout = AbortSignal.timeout(6000),
  watch = createClient(token, url).watch(["fabric://status"], timeout);
try {
  assert((await watch.next()).value.initial);
  const reply = await rotate(headers);
  assert.equal(reply.status, 200);
  assert.equal(reply.headers.get("Cache-Control"), "no-store");
  const replacement = await reply.text();
  assert(
    /^[0-9a-f]{32}$/.test(replacement) && replacement !== token,
    "expected a new token",
  );
  try {
    while (!(await watch.next()).done) {}
  } catch (e) {
    if (timeout.aborted) throw e;
  }
  assert.equal(
    (await fetch(url + "/mcp", request("ping", {}, 1, token))).status,
    401,
  );
  assert.equal(
    (await createClient(replacement, url).rpc("ping", {})).resultType,
    "complete",
  );
} finally {
  await watch.return();
}
console.log(
  "PASS: rotation revoked old token and active subscription; new token works",
);

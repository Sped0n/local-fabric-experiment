export const version = "2026-07-28";
export function request(method, params = {}, id, token) {
  const headers = {
    Authorization: `Bearer ${token}`,
    Accept: "application/json, text/event-stream",
    "Content-Type": "application/json",
    "MCP-Protocol-Version": version,
    "Mcp-Method": method,
  };
  if (method === "tools/call") headers["Mcp-Name"] = params.name;
  if (method === "resources/read") headers["Mcp-Name"] = params.uri;
  const body = {
    jsonrpc: "2.0",
    id,
    method,
    params: {
      ...params,
      _meta: {
        "io.modelcontextprotocol/protocolVersion": version,
        "io.modelcontextprotocol/clientInfo": {
          name: "local-fabric",
          version: "1",
        },
        "io.modelcontextprotocol/clientCapabilities": {},
      },
    },
  };
  return { method: "POST", headers, body: JSON.stringify(body) };
}
export function createClient(token, base = "") {
  let sequence = 0;
  async function send(method, params, id, signal) {
    const res = await fetch(base + "/mcp", {
      ...request(method, params, id, token),
      cache: "no-store",
      signal,
    });
    if (!res.ok) throw Error(res.status + " " + (await res.text()));
    return res;
  }
  async function rpc(method, params) {
    const id = ++sequence;
    const msg = await (await send(method, params, id)).json();
    if (msg.id !== id) throw Error("MCP response ID mismatch");
    if (msg.error) throw Error(msg.error.message);
    if (msg.result?.resultType !== "complete")
      throw Error("Expected complete result");
    return msg.result;
  }
  async function read(uri) {
    const r = await rpc("resources/read", { uri });
    return JSON.parse(r.contents[0].text);
  }
  async function call(name, args) {
    const r = await rpc("tools/call", { name, arguments: args });
    if (r.isError) throw Error(r.content[0].text);
    return r.content[0].text;
  }
  async function* watch(uris, signal) {
    const id = ++sequence;
    const r = await send(
      "subscriptions/listen",
      { notifications: { resourceSubscriptions: uris } },
      id,
      signal,
    );
    const reader = r.body.pipeThrough(new TextDecoderStream()).getReader();
    let rest = "",
      acknowledged = false;
    try {
      for (;;) {
        const { value, done } = await reader.read();
        if (done) return;
        rest += value;
        let end;
        while ((end = rest.indexOf("\n\n")) !== -1) {
          const frame = rest.slice(0, end);
          rest = rest.slice(end + 2);
          const line = frame.split("\n").find((s) => s.startsWith("data: "));
          if (!line) continue;
          const msg = JSON.parse(line.slice(6));
          if (msg.id === id && msg.result?.resultType === "complete") return;
          if (
            msg.params?._meta?.["io.modelcontextprotocol/subscriptionId"] !== id
          )
            throw Error("Subscription ID mismatch");
          if (!acknowledged) {
            if (msg.method !== "notifications/subscriptions/acknowledged")
              throw Error("Expected subscription acknowledgment");
            acknowledged = true;
            yield {
              initial: true,
              uris: msg.params.notifications.resourceSubscriptions ?? [],
            };
          } else if (msg.method === "notifications/resources/updated")
            yield { initial: false, uris: [msg.params.uri] };
        }
      }
    } finally {
      await reader.cancel().catch(() => {});
      reader.releaseLock();
    }
  }
  return { read, call, rpc, watch };
}

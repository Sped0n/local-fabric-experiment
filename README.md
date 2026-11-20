# Local Fabric

Local Matter controller prototype for ESP32-S31. `examples/demo` runs the controller, HTTP server, SolidJS UI, and UART console on the board. `components/local_fabric` owns commissioning, stable logical device IDs, committed Matter reports, and optional Lua automation. See the [supported cluster contracts](components/local_fabric/data_model/SCHEMA.md).

## Run on the board

Use ESP-IDF 6.2 and a multicast-capable lab LAN. The component manifest pins `sped0n/esp_matter` to `1.7.0-rc0.2`; the controller uses CHIP minimal mDNS, on-network commissioning, and test credentials. Put `CONFIG_DEMO_SSID` and `CONFIG_DEMO_PASSWORD` in the untracked `examples/demo/sdkconfig.local`.

```sh
cd examples/demo
idf.py -DIDF_TARGET=esp32s31 -DSDKCONFIG_DEFAULTS='sdkconfig.defaults;sdkconfig.local' build
idf.py -p /dev/ttyUSB0 flash monitor
```

Open the IP address printed on UART. At `fabric>`, run `fabric token` and paste the token into the web UI. The board generates a random access token on first boot and stores it in NVS. **Settings → Rotate access token** replaces it and disconnects every MCP client, including open subscription streams. Copy the replacement before dismissing it; UART can recover the current credential. Avoid sharing console logs containing tokens. Plain HTTP exposes bearer tokens on the wire: use only on a trusted isolated LAN or terminate TLS before exposing it elsewhere. This is a single-operator credential, not OAuth or per-user authorization.

The UI is embedded in the firmware. For frontend changes, run `pnpm install --frozen-lockfile && pnpm run build` in `components/http_server/web` and rebuild the firmware. Firmware builds consume `ui.js.gz` and `ui.css.gz`; Vite compiles SolidJS and Tailwind on the host. No CDN or on-board compiler is required.

## HTTP and MCP

The local [2026-07-28](https://modelcontextprotocol.io/specification/2026-07-28) implementation is in `components/http_server/mcp/`: HTTP validation and dispatch, a fixed tool/resource catalog, and bounded subscription delivery. Both the web UI and the UART console are clients. There is no initialization handshake, protocol session, or 2025 compatibility adapter. Credential rotation is a separate authenticated Settings operation, never an MCP tool.

- Send one JSON-RPC request per `POST /mcp`. Include the bearer token, `Content-Type: application/json`, and `Accept: application/json, text/event-stream`. Supply the protocol version, client info and capabilities in `params._meta`; mirror the version and method in `MCP-Protocol-Version` and `Mcp-Method`. Mirror the tool name or resource URI in `Mcp-Name` for calls and reads. The [web client](components/http_server/web/mcp.js) has the request builder. Bodies are limited to 6144 bytes and 24 levels of nesting.
- Use `server/discover` to inspect capabilities, or call directly. Device discovery is split: `fabric://devices/metadata` maps stable IDs to type, name and room; `fabric://devices/state` maps the same IDs to online status, endpoints, properties and advertised actions. Both return `{"devices":{"7":{...}}}`. Subscribe to either or both on one stream, then read after acknowledgment. `device.get` still returns one combined detail object by numeric ID. Other resources include `fabric://status` and, with automation, rules and local actions. `tools/list` describes commissioning, device actions and optional rule editing.
- `resources/templates/list` advertises `fabric://devices/{device}/endpoints/{endpoint}/clusters/{cluster}/properties/{property}`. Read this URI for the property descriptor, or subscribe to receive updates only when that descriptor changes. Each stream can accept up to eight distinct exact properties plus the aggregate URIs. Inspect the acknowledgment for the accepted subset. These interests live only in RAM and never consume automation slots.
- For realtime updates, POST `subscriptions/listen` with `notifications.resourceSubscriptions`. The SSE response first acknowledges the accepted URIs. Read those resources for the initial snapshot, then reread each URI on `notifications/resources/updated`. Each message carries the listen request ID. Close the response to unsubscribe; reconnect with a fresh POST and snapshot. Updates may coalesce. At most six watches run concurrently; ordinary tool calls and reads use no watch slots.

The UART console adds request metadata for you. Paste compact JSON without spaces at its prompt:

```text
fabric resources/list
fabric resources/read {"uri":"fabric://status"}
fabric tools/call {"name":"commission.start","arguments":{"code":"CODE-FROM-FIXTURE"}}
fabric resources/read {"uri":"fabric://devices/metadata"}
fabric tools/call {"name":"device.set_identity","arguments":{"device_id":7,"name":"Desk lamp","room":"Office"}}
fabric tools/call {"name":"device.action","arguments":{"device_id":1,"endpoint_id":1,"cluster":"on_off","action":"set","arguments":{"on":true}}}
```

Use IDs and capabilities actually reported by the board. `accepted` means admitted, not completed: inspect `fabric://status` for the terminal operation result and `fabric://devices/state` for what was observed. Renaming or moving a device invalidates only metadata after persistence; an unchanged save sends nothing. Live reports do not wake metadata subscribers. Adding or removing a device invalidates both resources. A room is a label (empty clears it), never part of the addressing scheme. Removing a logical device removes its whole native node, including siblings. Core membership, CHIP credentials, and Manager identity are separate durable stores, not a power-loss-atomic transaction.

## Rules

With `CONFIG_FABRIC_AUTOMATION=y`, 32 slots share a dedicated 256 KiB rules partition. Every writer saves the same definition: `enabled`, optional `title`, watched inputs (`when`), and Lua `source`. The visual editor generates the watches and source together and includes its editable form in an optional `editor` object. The board stores that object without interpreting it. Switching editor mode clears the draft; saving a raw replacement omits the editor data. A save checks watched properties and compiles (does not execute) Lua before replacing the stored rule. The editor validates visual operands and action arguments against the catalog; at execution the board validates invoked actions and hook arguments. All fields together must fit 3072 UTF-8 bytes.

Example raw definition (replace IDs with actual commissioned devices):

```json
{"enabled":true,"when":{"device":10,"endpoint":4,"cluster":"temperature_measurement","property":"temperature_celsius"},"source":"if event.previous <= 28 and event.current > 28 then matter.invoke_action(6, 8, 'fan_control', 'set_percent', {percent=55}) end"}
```

One worker runs Lua on known changes to watched values, without replay or persistent Lua globals. New, missing and reconnected devices establish fresh baselines. Rules restore without requiring devices to be online. This prototype starts with a fresh automation namespace. Earlier rules are not imported; device credentials are unchanged.

**Wake on a property or time.** Choose a property or **Local clock** from the **When** picker; use **Add input** to watch another. Visual rules combine the tests automatically. Raw Lua shows editable input names for clocks and multi-input rules. For example, this raw `when` watches a contact and the local minute:

```json
[{"name":"open","device":7,"endpoint":2,"cluster":"boolean_state","property":"state"},
 {"name":"clock","clock":"minute"}]
```

```lua
local function ready(s)
  return s.open and s.clock.weekday == 5 and s.clock.hour >= 18 and s.clock.hour < 22
end
if not ready(event.previous) and ready(event.current) then
  matter.invoke_action(3, 1, "on_off", "set", { on = true })
end
```

Named properties are captured together under the Manager lock. A changed value wakes Lua once with the whole before/after pair; the clock advances once per minute, **even when the predicate stays true**. Rules without clock inputs do not pay for clock dispatch. No clock input runs before sync; the first synchronized reading or a jump over missed minutes resets the baseline, without catch-up actions. The visual editor generates the equivalent false-to-true test in Lua. For visual rules, **When → Local clock → At** fires on arrival at the specified local minute; another watched property becoming true later in that minute does not fire it again. **During** is a state: entering the window or changing another watched property can make the combined tests become true. Choose any nonempty set of weekdays (default: every day). For example, in the editor draft `{"name":"clock","clock":"minute","at":1080,"weekdays":[2,3]}` means Tuesday and Wednesday at 18:00. For a same-day window use `start` (inclusive) and `end` (exclusive, 1440 = midnight) instead of `at`. To gate a property trigger without waking on time, choose **Add condition → Local clock**.

For a time **condition only**, do not watch the clock. Read it in the handler before invoking the action:

```lua
local t = automation.local_time()
if not t or t.weekday ~= 5 or t.hour < 18 or t.hour >= 22 then return end
```

This permits Friday 18:00–22:00 (end exclusive). Weekdays are Monday = 1 through Sunday = 7; hour is 0–23 and minute is 0–59. Each call reads the current local time. It is `nil` until the first successful SNTP sync since startup; after sync, the system clock keeps running through network outages. The demo uses `CONFIG_DEMO_TIMEZONE` (POSIX TZ, default `UTC0`) and `CONFIG_DEMO_NTP_SERVER` (default `pool.ntp.org`; set a reachable LAN server on isolated networks). Calling `local_time()` does not register a wake input.

`event.previous` and `event.current` are the trigger values. `matter.get_property()` reads the latest committed scalar on each call; missing/offline values return `nil`. A handler builds up to eight ordered steps. Nothing is submitted unless the whole body returns successfully. `automation.skip(reason[, observed])` terminates Lua, discards queued steps and records a skipped trace. A plain return without actions creates no trace. In **Then, in order**, choose a device action, registered local action or **Delay** from the same picker. **Add step** appends the next step:

```lua
matter.invoke_action(3, 1, "on_off", "set", {on=true})
automation.delay(30) -- seconds; schedules the next step, does not suspend Lua
matter.invoke_action(3, 1, "on_off", "set", {on=false})
```

Steps may be a Matter action, a registered local action, or a delay of 1–86400 whole seconds. Visual editor drafts accept one action object or an ordered array under `action`; a delay is `{"delay_s":30}`. Arguments are evaluated before execution. A Matter command must receive success before the next step starts; reports, not command replies, confirm device state. Errors and timeouts discard the remaining steps. Delays use monotonic time, even without network time. No task, timer or VM is retained per sleeping rule. The rule ignores retriggers while active; editing, disabling, deleting or rebooting drops unsent steps and never rolls back commands already sent. There are no Lua standard libraries or direct network, file or attribute-write APIs. Limits: 3072 bytes per saved JSON definition, 64 KiB per VM, 12,000 instructions per load or invocation.

### Device activity

**Device activity** and the read/subscribable MCP resource `fabric://activity` show changes to any modeled property, newest first. Rows contain device, endpoint, cluster, property, and the new scalar (`current`); numbers keep their published precision and unit. Long strings are shortened at a UTF-8 boundary and marked `truncated`. The RAM ring defaults to 32 entries (`CONFIG_FABRIC_DEVICE_ACTIVITY_CAPACITY`). Time is the board’s observation time. A subscription snapshot and the first known value after an unknown establish a baseline. A duplicate public value, a change to command capabilities alone, or a local metadata edit adds no row. Activity works without automation and does not infer who caused a change.

### Automation traces

Open **Automation rules → Automation traces** for finished runs. Each row records the run ID (assigned at start, reset on reboot), rule ID, slot, title, trigger, outcome and elapsed time. If several inputs changed together the trigger gives their count and the first change. A skipped visual condition records the unmet requirement and observed value. Failures identify the Lua error or failed action and its position. Editing/removing a running rule records cancellation; an already submitted Matter command cannot be recalled.

The PSRAM ring retains the last `CONFIG_FABRIC_AUTOMATION_ACTIVITY_CAPACITY` terminal runs (default 32). MCP `fabric://automation/runs` returns `{capacity, finished}`, newest completion first; `fabric://automation/runs/{id}` reads a retained summary. Finishing a run invalidates the resource; delays in progress, trigger mismatches, and raw Lua returning without actions do not. Device Activity records observed property changes separately.

### Register a local action

Pass a static array of `fabric_automation_hook_t` to `fabric_automation_init()`. Each registration has a unique name, description, required scalar arguments (string, boolean, integer or number), a `try_enqueue` callback and its context. See `examples/demo/main/demo_automation.c` for a working integration. The array and its fields must outlive the automation service. This registers a callable action, **not** a subscription.

The engine calls the hook on its worker. Copy the borrowed JSON and enqueue **without waiting**; do not perform speech, network I/O, or wait for queue space. Return `ESP_OK` on acceptance; a full queue or unavailable agent returns an error and stops the remaining steps. Acceptance does not mean speech or other external work has finished.

MCP clients discover local actions in `fabric://automation/actions`. The visual editor uses the same descriptors to render named inputs. For example, a visual step is `{"native":"demo.enqueue_note","arguments":{"text":"Door opened"}}`; raw Lua uses `automation.invoke_local("demo.enqueue_note", {text="Door opened"})`. The catalog is fixed until restart, independent of the rule list and its notifications. Authenticated clients save/remove definitions with `rule.save` / `rule.remove` and read/subscribe to `fabric://automation/rules`. Only firmware can register callbacks.

## Verification

From the repo root, live tests use Node and `FABRIC_URL` (for example `http://192.168.31.126`) and `FABRIC_TOKEN` (token only):

- `node tests/mcp-stateless.mjs` — discovery, header and version checks, auth, a read-only `device.get` on a commissioned fixture, and independent stream cancellation.
- `FABRIC_ROTATE_TOKEN=1 node tests/token-rotation.mjs` — **rotates the board token and signs out all clients**. Retrieve the replacement with `fabric token` before running further tests.
- `node tests/browser-sync.mjs` — needs `WEBDRIVER` pointing at geckodriver, a commissioned contact sensor and dimmable light, empty rule slot 2, and `FABRIC_PIPE` pointing to the fixture's command FIFO. Saves a visual rule via Firefox and verifies two tabs observe the same light change without reload.
- `node tests/mcp-properties.mjs` — changes a fixture light and verifies same-device filtering plus independent stream cancellation.
- `node tests/visual-rules.mjs` — executes frontend-generated rules with a host Lua interpreter (`LUA=/path/to/lua` or `lua` on PATH) and checks the complete saved size.
- `node tests/activity.mjs` — uses a spare slot and a disposable contact fixture to verify run details, cancellation, slot reuse, and failures; restores the original contact value.
- `node tests/sequence-benchmark.mjs` — requires an isolated empty 32-slot store, synchronized board clock. Measures three batches of distinct clock-triggered rules without depending on Matter traffic with two delays each. UART `batch_us` covers input sampling, Lua and arming delays; `max_late_us` measures delay deadline to worker resumption, not physical action latency.
- `node tests/minute-benchmark.mjs` — installs short property-plus-clock predicates in available slots, first one then the remaining capacity, measures real synchronized ticks, and removes its own rules. Use `CONFIG_FABRIC_AUTOMATION_TIMING=y` to log worker elapsed microseconds on UART rather than timing HTTP readbacks. Reserve the test slots and keep the fixture stable during measurement.
- `node tests/rules-capacity.mjs fill` — requires all slots empty and the contact FIFO. Fills every slot with a 3072-byte definition, replaces one and exercises all bodies. Reboot the board, wait for fixture reports, then run `node tests/rules-capacity.mjs restore` to check persistence and remove test rules. `clean` removes only the test-owned slots.
- `node tests/chipmunk-clusters.mjs` — **changes device states**. Use disposable fixtures on the same fabric. The four-fixture layout below covers the 15 registered cluster IDs; a successful matrix does not assert every optional attribute or vendor behavior.

### Chipmunk fixtures

Use `ghcr.io/sped0n/chipmunk:1.6.1.0-dirty` with host networking and one disk-backed `/tmp` volume per fixture. For the multi-endpoint fixture:

```sh
mkdir -p .test/all
docker run --rm --network host --user "$(id -u):$(id -g)" -v "$PWD/.test/all:/tmp" \
  ghcr.io/sped0n/chipmunk:1.6.1.0-dirty all-devices-app --discriminator 3671 --app-pipe /tmp/actions \
  --device dimmable-light:1 --device contact-sensor:2 --device pressure-sensor:3 \
  --device temperature-sensor:4 --device humidity-sensor:5 --device light-sensor:6 \
  --device occupancy-sensor:7 --device fan:8 --device robotic-vacuum-cleaner:9 --device power-source:10
```

Add `chip-lighting-app --secured-device-port 5542 --discriminator 3621`, `chip-rvc-app --secured-device-port 5543 --discriminator 3602 --app-pipe /tmp/actions`, and `thermostat-app --secured-device-port 5544 --discriminator 3553` in separate containers. Commission with each fixture's **QR payload** (manual codes can share a short discriminator). The RVC must be ready to clean; if a previous run left it seeking its charger, simulate docking/charging on its own FIFO before repeating. Use `chip-tool discover commissionables` as an independent discovery check, not to commission the fixtures instead of the board.

### Emulator

From `examples/demo`, use a separate build directory to avoid flashing the contract test by accident:

```sh
idf.py -B build-contract -DIDF_TARGET=esp32s31 -DFABRIC_MODEL_TEST=ON build merge-bin --output "$PWD/build-contract/test.bin"
esp-emu --chip esp32s31 --firmware build-contract/test.bin --timeout 20 --exit-on 'PASS: pressure'
```

The contract test checks real TLV reports, pressure units, threshold crossings and AND gates, atomic report discard, change signals and property reads, Lua rule restore and invalid-edit rejection, and cJSON allocations in PSRAM. The app installs the allocator hooks before starting any cJSON users.

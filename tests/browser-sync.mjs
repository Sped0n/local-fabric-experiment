// Firefox WebDriver smoke test for the Chipmunk contact + light fixture. Uses rule slot 2.
// Requires an empty slot 2, a writable fixture command FIFO, and a running geckodriver.
import assert from "node:assert/strict";
import { createClient } from "../components/http_server/web/mcp.js";
import { writeFile } from "node:fs/promises";
const {
  FABRIC_URL: url,
  FABRIC_TOKEN: token,
  FABRIC_PIPE: pipe,
  WEBDRIVER = "http://127.0.0.1:4444",
} = process.env;
if (!url || !token || !pipe)
  throw Error("Set FABRIC_URL, FABRIC_TOKEN and FABRIC_PIPE");
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
async function until(f, label) {
  for (let i = 0; i < 60; i++) {
    if (await f()) return;
    await sleep(250);
  }
  throw Error("timeout: " + label);
}
async function driver(
  path,
  body,
  method = body === undefined ? "GET" : "POST",
) {
  const res = await fetch(WEBDRIVER + path, {
    method,
    headers: { "Content-Type": "application/json" },
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  let { value } = await res.json();
  if (!res.ok) throw Error(value.message);
  return value;
}
const mcp = createClient(token, url);
async function api(path, body, method) {
  if (method === "DELETE") return mcp.call("rule.remove", { slot: 2 });
  if (body) return mcp.call("device.action", body);
  if (path.startsWith("/devices/"))
    return JSON.parse(
      await mcp.call("device.get", { device_id: Number(path.slice(9)) }),
    );
  return mcp.read("fabric:/" + path);
}
const rules = await api("/automation/rules");
assert.equal(rules[1], null, "slot 2 must be unused");
const { devices } = await mcp.read("fabric://devices/metadata");
let sensor, light, ep;
for (const id of Object.keys(devices)) {
  const detail = await api("/devices/" + id),
    d = detail;
  for (const [name, e] of Object.entries(detail.endpoints)) {
    if (e.clusters.boolean_state && name === "2") sensor = d.device_id;
    if (
      e.clusters.on_off &&
      e.clusters.level_control &&
      !e.clusters.color_control &&
      name === "1"
    ) {
      light = d.device_id;
      ep = +name;
    }
  }
}
assert(sensor && light, "missing contact + dimmable light");
const session = await driver("/session", {
  capabilities: {
    alwaysMatch: {
      browserName: "firefox",
      "moz:firefoxOptions": {
        args: ["-headless"],
        prefs: { "network.proxy.type": 0 },
      },
    },
  },
});
const root = "/session/" + session.sessionId,
  cmd = (path, body) => driver(root + path, body),
  js = (script, ...args) => cmd("/execute/sync", { script, args });
// Multiple user interactions in one WebDriver call still need separate event-loop turns.
function editRule(script, ...args) {
  return js(
    `return (async()=>{const f=document.querySelector('#rules form');const pause=()=>new Promise(resolve=>setTimeout(resolve,0));
 const field=(name,root=f)=>[...root.querySelectorAll('label')].find(l=>l.firstChild.textContent.trim()===name)?.querySelector('select,input,textarea');
 const button=(name,root=f)=>[...root.querySelectorAll('button')].find(b=>b.textContent.trim()===name);
 const change=async(el,value,event='change')=>{if(!el)throw Error('missing control for '+value);el.value=value;if(el.value!==String(value))throw Error('missing value '+value);el.dispatchEvent(new Event(event,{bubbles:true}));await pause();};
 const click=async el=>{el.click();await pause();};const mode=async(v,accept)=>{const orig=window.confirm;let prompted=false;window.confirm=()=>{prompted=true;return accept;};try{await change(field('Mode'),v);}finally{window.confirm=orig;}if(!prompted)throw Error('missing reset confirmation');};
 ${script}
 })()`,
    ...args,
  );
}
let installed = false;
async function login() {
  await cmd("/url", { url });
  await js(
    "const f=document.forms[0];f.secret.value=arguments[0];f.requestSubmit()",
    token,
  );
  try {
    await until(
      () =>
        js(
          'return !!document.querySelector("article") && !!document.querySelector("section:last-of-type fieldset")',
        ),
      "device and rule snapshot",
    );
  } catch (e) {
    console.error(
      await js(
        'return [document.querySelector("header")?.innerText,document.querySelector("output")?.innerText,document.querySelectorAll("article").length,Array.from(document.querySelectorAll("h2"),e=>e.innerText)]',
      ),
    );
    throw e;
  }
}
async function reading(expected) {
  return js(
    'const d=[...document.querySelectorAll("article")].find(a=>a.querySelector("h3").innerText.includes("#"+arguments[0]+" ·"));return d?.querySelector(".value strong")?.innerText===arguments[1]',
    light,
    String(expected),
  );
}
try {
  await login();
  await cmd("/window/rect", { width: 390, height: 820 });
  assert(
    await js("return document.documentElement.scrollWidth<=innerWidth"),
    "mobile viewport overflow",
  );
  await cmd("/window/rect", { width: 1280, height: 850 });
  assert(
    await js(
      "return getComputedStyle(document.querySelector('#devices>div')).display==='grid' && document.documentElement.scrollWidth<=innerWidth",
    ),
    "desktop device grid",
  );
  const first = await cmd("/window");
  const second = await cmd("/window/new", { type: "tab" });
  await cmd("/window", { handle: second.handle });
  await login();
  // Select the disposable light by ID. A metadata notification updates another tab without discarding its draft.
  const cardJs = (body, ...args) =>
    js(
      "const card=[...document.querySelectorAll('article')].find(a=>a.querySelector('h3 small').textContent.startsWith('#'+arguments[0]+' ·'));" +
        body,
      light,
      ...args,
    );
  const original = (await mcp.read("fabric://devices/metadata")).devices[light];
  assert.deepEqual(Object.keys(original).sort(), ["name", "room", "type"]);
  const state = (await mcp.read("fabric://devices/state")).devices[light];
  assert.deepEqual(Object.keys(state).sort(), ["endpoints", "online"]);
  const suffix = Date.now().toString(36),
    name = "Fixture " + suffix,
    room = "Room " + suffix;
  try {
    await cardJs(
      "[...card.querySelectorAll('button')].find(b=>b.textContent.trim()==='Edit name & room').click()",
    );
    await cardJs(
      "card.querySelector('input[name=name]').value='unsaved draft'",
    );
    await cmd("/window", { handle: first });
    await cardJs(
      "[...card.querySelectorAll('button')].find(b=>b.textContent.trim()==='Edit name & room').click()",
    );
    await cardJs(
      "const f=card.querySelector('input[name=name]').form;f.elements.name.value=arguments[1];f.elements.room.value=arguments[2];f.requestSubmit()",
      name,
      room,
    );
    await cmd("/window", { handle: second.handle });
    await until(
      () =>
        cardJs(
          "return card.querySelector('h3').textContent.includes(arguments[1]) && card.querySelector('h3 small').textContent.includes(arguments[2])",
          name,
          room,
        ),
      "metadata rename and move in second tab",
    );
    assert.equal(
      await cardJs("return card.querySelector('input[name=name]').value"),
      "unsaved draft",
    );
    await js(
      "const s=document.querySelector('#devices>label select');s.value=arguments[0];s.dispatchEvent(new Event('change',{bubbles:true}))",
      room,
    );
    await until(
      () => js("return document.querySelectorAll('article').length===1"),
      "room filter",
    );
  } finally {
    const current = (await mcp.read("fabric://devices/metadata")).devices[
      light
    ];
    if (current?.name === name && current.room === room)
      await mcp.call("device.set_identity", {
        device_id: light,
        name: original.name,
        room: original.room,
      });
  }
  await until(
    () =>
      cardJs(
        "return card.querySelector('h3').textContent.includes(arguments[1])",
        original.name,
      ),
    "restored fixture identity",
  );
  await writeFile(
    pipe,
    '{"Name":"SetBooleanState","EndpointId":2,"NewState":false}\n',
  );
  await api("/action", {
    device_id: light,
    endpoint_id: ep,
    cluster: "on_off",
    action: "set",
    arguments: { on: false },
  });
  await until(() => reading(false), "second tab light off");
  await cmd("/window", { handle: first });
  await until(() => reading(false), "first tab light off");
  await editRule(
    `await change(document.querySelector('#rules>label select'),'2');const trigger=field('When input');const when=trigger.closest('fieldset');
    if(f.querySelector('.conditions legend'))throw Error('empty If section visible');
    const lux=[...trigger.options].find(o=>o.value.endsWith('/illuminance_lux'));const enumMode=[...trigger.options].find(o=>o.value.endsWith('/rvc_run_mode/mode'));
    if(lux&&enumMode){await change(trigger,lux.value);const num=when.querySelector('input[type=number]');if(num.step!=='1'||num.min!=='0')throw Error('wrong lux schema');await change(trigger,enumMode.value);if(when.querySelector('input')||![...when.querySelectorAll('option')].some(o=>o.textContent==='Cleaning'&&o.value==='1'))throw Error('missing numeric enum choices');}
    await change(trigger,arguments[0]+'/2/boolean_state/state');if(when.querySelector('input'))throw Error('boolean must not have an operand');
    await click(button('Add condition'));await change(field('If input'),arguments[1]+'/1/on_off/on');await change(field('Operator',field('If input').closest('fieldset')),'false');
    await click(button('Add condition'));await change(field('And condition'),arguments[1]+'/1/level_control/level_percent');const row=field('And condition').closest('fieldset');await change(field('Operator',row),'>=');await change(row.querySelector('input'),'0','input');
    await change(field('Action'),arguments[1]+'/1/on_off/set');await change(f.elements.on,'true','input');f.requestSubmit();`,
    sensor,
    light,
  );
  await until(
    async () => Boolean((await api("/automation/rules"))[1]),
    "visual rule saved",
  );
  installed = true;
  const saved = (await api("/automation/rules"))[1];
  assert.equal(saved.editor.when.op, "true");
  assert.deepEqual(
    saved.editor.conditions.map((c) => c.op),
    ["false", ">="],
  );
  assert.equal(saved.editor.action.arguments.on, true);
  assert.equal(saved.when.property, "state");
  assert.match(saved.source, /matter\.invoke_action/);
  // An invalid source cannot replace a valid rule; the board compiles, but never executes on save.
  await assert.rejects(
    mcp.call("rule.save", {
      slot: 2,
      definition: { enabled: true, when: saved.when, source: "not Lua !" },
    }),
  );
  await editRule(
    `await mode('raw',false);if(field('Mode').value!=='visual')throw Error('lost draft');await mode('raw',true);if(f.querySelector('textarea')?.value!==''||f.querySelectorAll('fieldset select').length!==2)throw Error('raw mode retained visual selectors');`,
  );
  await sleep(500);
  await writeFile(
    pipe,
    '{"Name":"SetBooleanState","EndpointId":2,"NewState":true}\n',
  );
  await until(() => reading(true), "first tab observed light on");
  await cmd("/window", { handle: second.handle });
  await until(() => reading(true), "second tab observed light on");
  // The unsaved switch to raw did not stop the visual rule. Save a self-contained
  // falling-edge rule and verify its Matter action through the other browser.
  await cmd("/window", { handle: first });
  const source = `if event.current or not matter.get_property(${light}, 1, "on_off", "on") then return end
    matter.invoke_action(${light}, 1, "on_off", "set", {on=false})`;
  await editRule(
    `await change(field('When input'),arguments[1]+'/2/boolean_state/state');await change(f.querySelector('textarea'),arguments[0],'input');f.requestSubmit();`,
    source,
    sensor,
  );
  await until(
    async () => (await api("/automation/rules"))[1]?.source === source,
    "raw rule saved",
  );
  const raw = (await api("/automation/rules"))[1];
  assert(!raw.editor);
  assert.equal(raw.when.device, sensor);
  assert(
    !(
      "trigger" in raw ||
      "action" in raw ||
      "conditions" in raw ||
      "generated" in raw
    ),
  );
  await sleep(400);
  await writeFile(
    pipe,
    '{"Name":"SetBooleanState","EndpointId":2,"NewState":false}\n',
  );
  await cmd("/window", { handle: second.handle });
  await until(() => reading(false), "raw Lua turned off light");
  await cmd("/window", { handle: first });
  await editRule(
    `await mode('visual',true);await change(field('When input'),arguments[0]+'/2/boolean_state/state');await click(button('Add input'));await change(field('And input'),'clock:minute');if(field('Schedule').value!=='at')throw Error('expected At');await change(field('Schedule'),'during');
  await change(field('From'),'00:00','input');await change(field('Until'),'00:00','input');const target=arguments[1]+'/1/on_off/set';await change(field('Action'),target);await change(f.querySelector('[data-then-step]').elements.on,'true','input');
  const append=async key=>{await click(button('Add step'));const row=[...f.querySelectorAll('[data-then-step]')].at(-1);await change(field('Action',row),key);return row;};await append('delay');await change((await append(target)).elements.on,'false','input');const note=await append('local:demo.enqueue_note');if(!note.textContent.includes('Queue a text note'))throw Error('missing action description');await change(note.elements.text,'browser-hook-e2e','input');f.requestSubmit();`,
    sensor,
    light,
  );
  await until(
    async () => Array.isArray((await api("/automation/rules"))[1]?.when),
    "compound visual rule saved",
  );
  const compound = (await api("/automation/rules"))[1];
  assert.deepEqual(compound.when[1], { name: "clock", clock: "minute" });
  assert.deepEqual(compound.editor.when[1], {
    name: "clock",
    clock: "minute",
    start: 0,
    end: 1440,
  });
  assert.deepEqual(
    compound.editor.action.map(
      (a) => a.delay_s ?? a.arguments.on ?? a.arguments.text,
    ),
    [true, 1, false, "browser-hook-e2e"],
  );
  assert.equal(compound.editor.action[3].native, "demo.enqueue_note");
  assert.match(compound.source, /ready\(event.previous\)/);
  await js(
    "[...document.querySelectorAll('summary')].find(s=>s.textContent==='Automation traces').click()",
  );
  await until(
    () =>
      js(
        "return !![...document.querySelectorAll('#rules details select')].find(s=>s.options[0]?.text==='All rules')",
      ),
    "live traces opened",
  );
  await sleep(400);
  await writeFile(
    pipe,
    '{"Name":"SetBooleanState","EndpointId":2,"NewState":true}\n',
  );
  await until(
    () => reading(true),
    "visual property plus minute rule turned on light",
  );
  await until(() => reading(false), "delayed OFF reported by Matter peer");
  await until(
    () =>
      js(
        "return [...document.querySelectorAll('#activity li')].filter(e=>e.textContent.includes('#'+arguments[0]+'/'+arguments[1]+' on_off.on:')).slice(0,2).map(e=>e.textContent.split('on_off.on: ')[1].split(' ·')[0]).join(',') === 'off,on'",
        light,
        ep,
      ),
    "reported light transitions visible separately from rule traces",
  );
  await until(
    async () => (await api("/automation/rules"))[1]?.diagnostic === "completed",
    "last command acknowledged",
  );
  const feed = await mcp.read("fabric://automation/runs");
  const latest = feed.finished.find(
    (r) => r.rule_id === compound.rule_id && r.outcome === "completed",
  );
  assert(latest);
  assert(latest.elapsed_ms >= 1000);
  assert.match(latest.trigger, /boolean_state.state: false → true/);
  assert(!JSON.stringify(latest).includes("browser-hook-e2e"));
  await until(
    () =>
      js(
        "return [...document.querySelectorAll('#rules details li')].some(r=>r.textContent.trim().startsWith('Run #'+arguments[0]+' ·') && r.textContent.includes('false → true'))",
        latest.id,
      ),
    "terminal trace delivered to open viewer",
  );
  await js(
    "const s=document.querySelector('#rules details select');s.value='rule';s.dispatchEvent(new Event('change',{bubbles:true}));",
  );
  await editRule(
    `await change(field('Schedule'),'at');await change(field('Time'),'18:00','input');for(const day of ['Mon','Thu','Fri','Sat','Sun'])await click(button(day));
  const days=[...f.querySelectorAll('.when .days button')];if(days.map(b=>b.getAttribute('aria-pressed')).join(',')!=='false,true,true,false,false,false,false')throw Error('weekday toggle states do not match Tue/Wed');
  if(getComputedStyle(button('Tue')).backgroundColor===getComputedStyle(button('Mon')).backgroundColor)throw Error('selected weekday not visibly distinguished');f.requestSubmit();`,
  );
  await until(
    async () =>
      (await api("/automation/rules"))[1]?.editor?.when?.[1]?.at === 1080,
    "At rule saved",
  );
  assert.deepEqual((await api("/automation/rules"))[1].editor.when[1], {
    name: "clock",
    clock: "minute",
    at: 1080,
    weekdays: [2, 3],
  });
  await editRule(
    `await mode('raw',true);await change(field('When input'),'clock:minute');await change(field('Lua input name'),'now','input');await change(f.querySelector('textarea'),'return event.current.now.minute','input');f.requestSubmit();`,
  );
  await until(
    async () =>
      (await api("/automation/rules"))[1]?.source ===
      "return event.current.now.minute",
    "raw clock rule saved",
  );
  assert.deepEqual((await api("/automation/rules"))[1].when, [
    { name: "now", clock: "minute" },
  ]);
  await editRule(
    `await change(document.querySelector('#rules>label select'),'1');`,
  );
  await until(
    () =>
      js(
        "return ![...document.querySelectorAll('#rules details li')].some(b=>b.textContent.trim().startsWith('Run #'+arguments[0]+' ·'))",
        latest.id,
      ),
    "selected rule excludes another rule’s history",
  );
  await js(
    "const s=document.querySelector('#rules details select');s.value='all';s.dispatchEvent(new Event('change',{bubbles:true}));",
  );
  await until(
    () =>
      js(
        "return [...document.querySelectorAll('#rules details li')].some(b=>b.textContent.trim().startsWith('Run #'+arguments[0]+' ·'))",
        latest.id,
      ),
    "global history includes earlier rule run",
  );
  console.log(
    "PASS: cross-tab metadata, live automation, device activity and filtered terminal traces",
  );
} finally {
  await driver(root, undefined, "DELETE");
  if (installed) await api("/rules/2", undefined, "DELETE");
}

// Run with Node and a Lua interpreter (LUA=/path/to/lua). Exercise executable output, not its spelling.
import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { savedRule } from "../components/http_server/web/rules/compile.js";
const door = {
    device: 12,
    endpoint: 2,
    cluster: "boolean_state",
    property: "state",
  },
  temperature = {
    device: 10,
    endpoint: 4,
    cluster: "temperature_measurement",
    property: "temperature_celsius",
  },
  lamp = { device: 13, endpoint: 1, cluster: "on_off", name: "set" },
  hook = { native: "demo.enqueue_note" };
const props = [
  { ...door, meta: { type: "boolean" } },
  { ...temperature, meta: { type: "number", min: -40, max: 100, step: 0.1 } },
];
const actions = [
  { ...lamp, meta: { arguments: { on: { type: "boolean" } } } },
  { ...hook, meta: { arguments: { text: { type: "string" } } } },
];
const message = 'Door "opened"\npath \\ on Friday',
  draft = {
    mode: "visual",
    title: "Hallway arrival",
    enabled: true,
    when: [
      { ...door, type: "boolean", op: "true", name: "door" },
      {
        clock: "minute",
        name: "clock",
        weekdays: [2, 5],
        start: 1080,
        end: 1320,
      },
    ],
    conditions: [{ ...temperature, type: "number", op: "<", value: 24.5 }],
    action: [
      { ...lamp, arguments: { on: true } },
      { delay_s: 1 },
      { ...lamp, arguments: { on: false } },
      { ...hook, arguments: { text: message } },
    ],
  };
const make = (d) => savedRule(d, props, actions),
  rule = make(draft);
assert.deepEqual(rule.when, [
  { name: "door", ...door },
  { name: "clock", clock: "minute" },
]);
assert.deepEqual(
  make({
    ...rule.editor,
    mode: "visual",
    title: rule.title,
    enabled: rule.enabled,
  }),
  rule,
);
assert.deepEqual(
  make({ ...draft, mode: "raw", when: door, source: "return" }),
  { enabled: true, title: draft.title, when: door, source: "return" },
);
assert.throws(
  () =>
    make({
      ...draft,
      conditions: [{ ...temperature, type: "number", op: "<", value: 24.55 }],
    }),
  /comparison/,
);
assert.throws(
  () => make({ ...draft, action: { ...hook, arguments: { text: 4 } } }),
  /arguments/,
);
assert.throws(
  () =>
    make({
      ...draft,
      action: { ...lamp, arguments: { on: true, unknown: 5 } },
    }),
  /arguments/,
);
assert.throws(
  () =>
    make({ ...draft, when: [{ name: "clock", clock: "minute", at: 1440 }] }),
  /clock time/,
);
assert.throws(
  () => make({ ...draft, mode: "raw", source: "--" + "光".repeat(1100) }),
  /3072/,
);
const at = make({
  ...draft,
  when: [
    draft.when[0],
    { clock: "minute", name: "clock", at: 1080, weekdays: [5] },
  ],
});
execFileSync(process.env.LUA ?? "lua", ["-"], {
  input: `
local notes, switched, delays = {}, {}, {}
local temp=21
matter={get_property=function(d,e,c,p) assert(d==10 and e==4 and c=='temperature_measurement' and p=='temperature_celsius'); return temp end,
 invoke_action=function(d,e,c,n,args) assert(d==13 and e==1 and c=='on_off' and n=='set'); table.insert(switched,args.on) end}
automation={delay=function(s) table.insert(delays,s) end, invoke_local=function(n,args) assert(n=='demo.enqueue_note'); table.insert(notes,args.text) end, skip=function(reason,observed) return {reason,observed} end}
local function window() ${rule.source}\nend
local function at() ${at.source}\nend
local function s(door,weekday,hour,minute) return {door=door,clock={weekday=weekday,hour=hour,minute=minute}} end
local function run(fn,p,c) event={previous=p,current=c};return fn() end
run(window,s(false,5,18,3),s(true,5,18,3));assert(notes[1]==[[${message}]] and switched[1] and switched[2]==false and delays[1]==1)
run(window,s(true,5,18,3),s(true,5,18,4));run(window,s(false,3,18,3),s(true,3,18,3));assert(#notes==1)
temp=25;local skip=run(window,s(false,5,18,4),s(true,5,18,4));assert(skip[2]==25 and #notes==1 and #delays==1)
temp=21;run(at,s(true,5,17,59),s(true,5,18,0));run(at,s(false,5,18,0),s(true,5,18,0));assert(#notes==2)
`,
});
console.log(
  `PASS: generated Lua edges, clock, skipped guard, quoting, actions, editor round-trip; realistic saved rule ${Buffer.byteLength(JSON.stringify(rule))}/3072 bytes`,
);

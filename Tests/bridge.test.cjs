const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync(require('node:path').join(__dirname,'../Plugins/UnrealWebUI/Resources/webui.js'), 'utf8');
function setup() {
  const messages = [];
  const context = vm.createContext({setTimeout, clearTimeout, console,
    ue: {webui: {broadcast: (...args) => { messages.push(args); return Promise.resolve(); }}}});
  vm.runInContext(source, context);
  return {context, messages};
}
test('nested JSON and unicode survive the native boundary', async () => {
  const {context: c, messages} = setup();
  await c.ue5('event', {text: 'hello "世界"\n', nested: [true, null, 1.25]});
  assert.deepEqual(JSON.parse(messages[0][1]), {text: 'hello "世界"\n', nested: [true, null, 1.25]});
});
test('request callback is one-shot and cleans up', async () => {
  const {context: c, messages} = setup();
  const result = c.webui.request('get', {n: 1}, 2);
  const token = messages[0][2];
  c.ue.interface[token]({ok: true});
  assert.deepEqual(await result, {ok: true});
  assert.equal(c.ue.interface[token], undefined);
});
test('callback overload without data and explicit timeout', async () => {
  const {context: c, messages} = setup();
  let value;
  await c.ue5('get', x => value = x, 3);
  assert.equal(messages[0][1], 'null');
  c.ue.interface[messages[0][2]](42);
  assert.equal(value, 42);
});
test('timeout rejects request and deletes callback', async () => {
  const {context: c, messages} = setup();
  await assert.rejects(c.webui.request('late', null, 0.01), /timed out/);
  assert.equal(c.ue.interface[messages[0][2]], undefined);
});
test('reinjection preserves registered interface functions', async () => {
  const {context: c} = setup();
  c.ue.interface.update = () => 42;
  const api = c.webui;
  vm.runInContext(source, c);
  assert.equal(c.webui, api);
  assert.equal(c.ue.interface.update(), 42);
});
test('raw broadcast validates JSON and preserves callback name', async () => {
  const {context: c, messages} = setup();
  await assert.rejects(c.ue.interface.broadcast('x', '{bad'), /JSON|property/i);
  await c.ue.interface.broadcast('x', '{"a":1}', 'reply');
  assert.deepEqual(Array.from(messages[0]), ['x', '{"a":1}', 'reply']);
});
test('bad inputs do not create callbacks', () => {
  const {context: c, messages} = setup();
  const cycle = {}; cycle.self = cycle;
  assert.throws(() => c.ue5('x', cycle), /circular/i);
  assert.throws(() => c.ue5('', null), /Event name/);
  assert.equal(messages.length, 0);
});
test('DOM hit test ignores page roots but keeps transparent controls', () => {
  const {context: c} = setup();
  c.innerWidth = 1000; c.innerHeight = 700;
  const html = {}, body = {}, root = {matches: () => true}, control = {matches: () => false};
  let target = null;
  c.document = {documentElement: html, body, elementFromPoint: () => target};
  for (const background of [null, html, body, root]) {
    target = background;
    assert.equal(c.webui.hitTest(10, 10), false);
  }
  target = control;
  assert.equal(c.webui.hitTest(10, 10), true);
  assert.equal(c.webui.hitTest(-1, 10), false);
  assert.equal(c.webui.hitTest(1000, 10), false);
  assert.equal(c.webui.hitTest(NaN, 10), false);
});
test('native DOM requests convert normalized coordinates without relying on pointermove', () => {
  const {context: c} = setup();
  c.innerWidth = 800; c.innerHeight = 600;
  const points = [], replies = [];
  c.document = {elementFromPoint: (x, y) => { points.push([x, y]); return {matches: () => false}; }};
  c.ue.webui.hittestresult = (...args) => { replies.push(args); return Promise.resolve(); };
  c.webui._queryHitTest(7, 0.25, 0.75);
  assert.deepEqual(points, [[200, 450]]);
  assert.deepEqual(replies, [[7, true]]);
});

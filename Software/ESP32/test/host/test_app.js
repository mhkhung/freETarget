"use strict";

// Exercise the actual page's message handler without a browser or a target.
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

const html = fs.readFileSync(path.join(__dirname, "../../main/HTML/app.html"), "utf8");
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
const startup = /render\(\);\s*connect\(\);\s*$/;
assert(startup.test(script), "Expected the page's initial render/connect calls");

const elements = new Map();
const sent = [];
const context = vm.createContext({
  assert,
  console,
  sent,
  localStorage: { getItem: () => null },
  setInterval: () => {},
  document: {
    getElementById(id) {
      if (!elements.has(id)) elements.set(id, {
        classList: { contains: () => true }, face: {}, kind: {}, decimal: {},
      });
      return elements.get(id);
    },
    addEventListener: () => {},
  },
});
vm.runInContext(script.replace(startup, ""), context);
vm.runInContext(`
  // Rendering is independent of the replay protocol under test.
  render = () => {};
  ws = { readyState: 1, send: (text) => sent.push(JSON.parse(text)) };
  const receive = (message) => handle(JSON.stringify(message));
  const helloMessage = { hello: "freETarget", name: "Lane 1", next_seq: 20,
    clock: "client", target: 110, session: 2 };
  const start = { history: 1, session: 1, seq: 1, change: "new", session_type: 0, target: 100, utc: 1795000000 };
  const first = { history: 1, session: 1, seq: 2, shot: 1, session_type: 1, x: 0, y: 0 };
  const second = { history: 1, session: 1, seq: 3, shot: 2, session_type: 1, x: 8, y: 0 };

  past = newSession(1);
  pastLoading = 1;
  receive(start);
  receive(first);
  receive(helloMessage); // Connection drops partway through the replay
  assert.equal(sent.filter((m) => m.HISTORY === 1).length, 1);
  receive(start);
  receive(first);
  receive(helloMessage); // Another disconnect at the same point
  receive(start);
  receive(first);
  receive(second);
  receive({ history_done: 1 });
  assert.equal(pastLoading, 0);
  assert.equal(past.modes.length, 1);
  assert.equal(past.shots.length, 2);
  assert.equal(past.shots[0].seq, 2);
  assert.equal(past.shots[1].seq, 3);
  assert.equal(total(analyse(past).record).int, 20);
  assert.equal(total(analyse(past).record).dec, 20.9); // 10.9 + 10.0

  // A live stream may interleave with history; sequence numbers are tracked
  // independently so live events must not suppress older history entries.
  past = newSession(1);
  pastLoading = 1;
  live = newSession(2);
  receive({ session: 2, seq: 10, change: "new", session_type: 4, target: 110 });
  receive({ session: 2, seq: 11, shot: 1, session_type: 5, x: 0, y: 0 });
  receive(start);
  receive(first);
  receive(second);
  receive({ history_done: 1 });
  assert.equal(live.shots.length, 1);
  assert.equal(lastSeq, 11);
  assert.equal(past.shots.length, 2);
  assert.equal(past.target, 100);
  receive({ history: 999, session: 999, seq: 100, shot: 1, x: 0, y: 0 });
  assert.equal(past.shots.length, 2);

  // The session ends: it stays on show, marked ended, until a shot starts the next one.
  receive({ session: 2, seq: 12, change: "end", session_type: 4, target: 110, utc: 1795003600, reason: "idle" });
  assert.equal(live.session, 2);
  assert.equal(live.ended.reason, "idle");
  assert.equal(live.modes.length, 1);                            // An end is not a session type change
  receive({ session: 3, seq: 13, change: "new", session_type: 0, target: 100, utc: 1795007200 });
  assert.equal(live.session, 3);
  assert.equal(live.ended, undefined);
  assert.equal(live.shots.length, 0);

  // A different session is allowed to start from a lower sequence number.
  const other = newSession(3);
  addEvent(other, { seq: 1, change: "new", session_type: 2, target: 110 });
  addEvent(other, { seq: 2, shot: 1, session_type: 3, x: 0, y: 0 });
  assert.equal(other.modes.length, 1);
  assert.equal(analyse(other).sighters.length, 1);
`, context);

console.log("Athlete page: interrupted history replays, totals and interleaved live shots passed");

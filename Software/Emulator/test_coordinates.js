"use strict";
const assert = require("node:assert/strict");
const fs = require("node:fs");
const vm = require("node:vm");
const source = fs.readFileSync(__dirname + "/dashboard.html", "utf8");
const functions = ["clickPosition", "zoomAt", "presetView", "firmwareFace", "firmwarePosition", "firmwarePage", "sessionShots"].map(name => {
  const match = source.match(new RegExp(`function ${name}\\([^)]*\\) \\{[\\s\\S]*?^\\}`, "m"));
  assert.ok(match, name);
  return match[0];
}).join("\n");
const context = vm.createContext({});
vm.runInContext(functions + "\nconst FACES = {pistol: {card: 170}, rifle: {card: 80}};", context);
const same = (actual, expected) => assert.equal(JSON.stringify(actual), JSON.stringify(expected));
const rect = {left: 10, top: 20, width: 400, height: 400};
for (const [event, expected] of [
  [{clientX: 210, clientY: 220}, {x: 0, y: 0}],
  [{clientX: 210, clientY: 120}, {x: 0, y: -75}],
  [{clientX: 310, clientY: 220}, {x: 75, y: 0}],
  [{clientX: 110, clientY: 320}, {x: -75, y: 75}],
]) {
  same(context.clickPosition(event, rect, {cx: 0, cy: 0, extent: 150}), expected);
}
// Zoomed in 10x around a point right of and above centre
same(context.clickPosition({clientX: 310, clientY: 120}, rect, {cx: 20, cy: -10, extent: 15}), {x: 27.5, y: -17.5});
// Zooming keeps the point under the cursor still
const zoomed = context.zoomAt({cx: 0, cy: 0, extent: 150}, {x: 30, y: -60}, 0.5);
same(zoomed, {cx: 15, cy: -30, extent: 75});
const still = context.clickPosition({clientX: 210 + 30 / 150 * 200, clientY: 220 - 60 / 150 * 200}, rect, zoomed);
assert.ok(Math.abs(still.x - 30) < 1e-9 && Math.abs(still.y + 60) < 1e-9, JSON.stringify(still));
assert.equal(context.zoomAt({cx: 0, cy: 0, extent: 2}, {x: 0, y: 0}, 0.1).extent, 1);   // Limits
assert.equal(context.zoomAt({cx: 0, cy: 0, extent: 200}, {x: 0, y: 0}, 4).extent, 300);
same(context.presetView("card", "rifle"), {cx: 0, cy: 0, extent: 45});
same(context.presetView("lanes", "rifle"), {cx: 0, cy: 0, extent: 300});
// The drawn face follows the firmware's latest session
assert.equal(context.firmwareFace([{hello: "freETarget", target: 101}, {change: "new", target: 111}, {shot: 1, target: 111}]), "rifle");
assert.equal(context.firmwareFace([{change: "new", target: 110}, {change: "new", target: 100}]), "pistol");
assert.equal(context.firmwareFace([{shot: 1}]), null);
assert.equal(JSON.stringify(context.firmwarePosition({x: 4, y: 6})), JSON.stringify({x: 4, y: -6}));
assert.equal(context.firmwarePage({protocol: "http:", hostname: "127.0.0.1"}, {http_port: 18000}), "http://127.0.0.1:18000/");
assert.equal(context.firmwarePage({protocol: "http:", hostname: "192.168.1.20"}, {http_port: 18000}), "http://192.168.1.20:18000/");
// Only the current session's expected crosses, reported marks and count remain.
const fired = (id, session, seq) => ({id, status: "fired", ...(seq ? {result: {session, seq}} : {})});
const report = (session, seq, target = 101) => ({shot: seq, session, seq, target});
let data = {events: [{change: "new", session: 1, seq: 1, target: 101}, report(1, 2)], expected: [fired(1, 1, 2)]};
let visible = context.sessionShots(data, "pistol", {});
same(visible.reports.map(s => s.seq), [2]);
same(visible.expected.map(s => s.id), [1]);
data.events.push({change: "new", session: 2, seq: 3, target: 101});
visible = context.sessionShots(data, "pistol", visible);
same(visible.reports, []);
same(visible.expected, []);
// A newly fired shot appears while waiting for firmware, then keeps its cross.
data.expected.push(fired(2));
visible = context.sessionShots(data, "pistol", visible);
same(visible.expected.map(s => s.id), [2]);
data.events.push(report(2, 4)); data.expected[1].result = {session: 2, seq: 4};
visible = context.sessionShots(data, "pistol", visible);
same(visible.reports.map(s => s.seq), [4]);
same(visible.expected.map(s => s.id), [2]);
// Face changes can come from another client, even without a new session.
data.events.push({change: "mode", session: 2, seq: 5, target: 111});
visible = context.sessionShots(data, "rifle", visible);
same(visible.reports, []); same(visible.expected, []);
data.events.push(report(2, 6, 111)); data.expected.push(fired(3, 2, 6));
visible = context.sessionShots(data, "rifle", visible);
same(visible.reports.map(s => s.seq), [6]);
same(visible.expected.map(s => s.id), [3]);
// Switching back must not resurrect the earlier pistol marks.
data.events.push({change: "mode", session: 2, seq: 7, target: 101});
visible = context.sessionShots(data, "pistol", visible);
same(visible.reports, []); same(visible.expected, []);
// A new session and its first shot may arrive in the same dashboard poll.
data.events.push({change: "new", session: 3, seq: 8, target: 101}, report(3, 9));
data.expected.push(fired(4, 3, 9));
visible = context.sessionShots(data, "pistol", visible);
same(visible.reports.map(s => s.seq), [9]);
same(visible.expected.map(s => s.id), [4]);
data.events.push({...report(1, 2), history: 1});
visible = context.sessionShots(data, "pistol", visible);
same(visible.reports.map(s => s.seq), [9]);
// Polling old data after the local clear cannot bring old marks back.
visible = {...visible, firstSeq: 10, expectedFloor: 4, reports: [], expected: []};
visible = context.sessionShots(data, "pistol", visible);
same(visible.reports, []); same(visible.expected, []);
console.log("PASS: dashboard click coordinates (zoomed and panned), zoom limits, face presets and firmware overlays use negative Y above centre; the firmware page follows the dashboard host");
console.log("PASS: dashboard clears marks and shot totals on new sessions and target changes, including changes from other clients");

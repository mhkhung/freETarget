"use strict";
const assert = require("node:assert/strict");
const fs = require("node:fs");
const vm = require("node:vm");
const source = fs.readFileSync(__dirname + "/dashboard.html", "utf8");
const functions = ["clickPosition", "zoomAt", "presetView", "firmwareFace", "firmwarePosition"].map(name => {
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
console.log("PASS: dashboard click coordinates (zoomed and panned), zoom limits, face presets and firmware overlays use negative Y above centre");

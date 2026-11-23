"use strict";
const assert = require("node:assert/strict");
const fs = require("node:fs");
const vm = require("node:vm");
const source = fs.readFileSync(__dirname + "/dashboard.html", "utf8");
const functions = ["clickPosition", "firmwarePosition"].map(name => {
  const match = source.match(new RegExp(`function ${name}\\([^)]*\\) \\{[\\s\\S]*?^\\}`, "m"));
  assert.ok(match, name);
  return match[0];
}).join("\n");
const context = vm.createContext({});
vm.runInContext(functions, context);
const rect = {left: 10, top: 20, width: 400, height: 400};
for (const [event, expected] of [
  [{clientX: 210, clientY: 220}, {x: 0, y: 0}],
  [{clientX: 210, clientY: 120}, {x: 0, y: -75}],
  [{clientX: 310, clientY: 220}, {x: 75, y: 0}],
  [{clientX: 110, clientY: 320}, {x: -75, y: 75}],
]) {
  assert.equal(JSON.stringify(context.clickPosition(event, rect, 150)), JSON.stringify(expected));
}
assert.equal(JSON.stringify(context.firmwarePosition({x: 4, y: 6})), JSON.stringify({x: 4, y: -6}));
console.log("PASS: dashboard click coordinates and firmware overlays use negative Y above centre");

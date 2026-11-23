"use strict";

// Run the page's real rendering and message handlers with a minimal DOM.
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const html = fs.readFileSync(process.argv[2] || path.join(__dirname, "../../main/HTML/app.html"), "utf8");
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
const startup = /render\(\);\s*connect\(\);\s*$/;
assert(startup.test(script));

class Element {
  constructor(tag) {
    this.tag = tag;
    this.children = [];
    this.attrs = {};
    this.classes = new Set();
    this.value = "";
    this._text = "";
    this.classList = {
      add: name => this.classes.add(name),
      remove: name => this.classes.delete(name),
      contains: name => this.classes.has(name),
      toggle: (name, force) => {
        const present = force === undefined ? !this.classes.has(name) : force;
        if (present) this.classes.add(name); else this.classes.delete(name);
        return present;
      },
    };
  }
  set className(value) { this.classes = new Set(value.split(/\s+/).filter(Boolean)); }
  setAttribute(name, value) { this.attrs[name] = String(value); if (name === "class") this.className = value; }
  append(...children) { this.children.push(...children); }
  replaceChildren(...children) { this._text = ""; this.children = [...children]; }
  set textContent(value) { this._text = String(value); this.children = []; }
  get textContent() { return this._text + this.children.map(child => child instanceof Element ? child.textContent : String(child)).join(""); }
  click() { if (!this.disabled && this.onclick) this.onclick(); }
  remove() {}
}

function page() {
  const elements = new Map(), saved = new Map(), downloads = [];
  const ticks = [];
  let nowMs = 1795000000 * 1000;
  function get(id) {
    if (!elements.has(id)) {
      const tag = html.match(new RegExp(`<([\\w-]+)\\b[^>]*\\bid="${id}"[^>]*>`));
      assert.ok(tag, `Page element #${id} must exist`);
      const node = new Element(tag[1]);
      const classes = tag[0].match(/class="([^"]*)"/);
      if (classes) node.className = classes[1];
      if (id === "newForm") for (const name of ["face", "kind", "decimal", "athlete", "shots", "sightMin", "matchMin"]) node[name] = new Element("input");
      elements.set(id, node);
    }
    return elements.get(id);
  }
  const context = vm.createContext({
    console, URLSearchParams,
    Date: class extends Date { static now() { return nowMs; } },
    localStorage: { getItem: key => saved.get(key) || null, setItem: (key, value) => saved.set(key, value) },
    setInterval(callback) { ticks.push(callback); }, window: {},
    Blob: class { constructor(parts) { this.text = parts.join(""); } },
    URL: { createObjectURL(blob) { downloads.push(blob.text); return "blob:test"; } },
    document: { getElementById: get, createElement: tag => new Element(tag),
      createElementNS: (ns, tag) => new Element(tag), addEventListener() {}, body: new Element("body") },
  });
  const run = code => vm.runInContext(code, context);
  run(script.replace(startup, ""));
  run(`
    ws = { readyState: 1, send() {} };
    const receive = m => handle(JSON.stringify(m));
    function begin(target = 101, type = 0, utc = 0) {
      receive({hello: 'freETarget', name: 'Test lane', next_seq: 1, target, clock: 'client'});
      receive({session: 1, seq: 1, change: 'new', target, session_type: type, utc});
      receive({caught_up: 1});
    }
    function shot(n, x = 0, y = 0, miss = false, type = 1) {
      // Test fixtures use displayed target coordinates; the protocol is positive-up.
      receive({session: 1, seq: n + 1, shot: n, x, y: -y, miss, session_type: type});
    }
    render();
  `);
  const rows = () => get("list").children[0].children[1].children;
  const cross = () => get("target").children.find(node => node.classes?.has("average-point"));
  const boardRows = () => get("seriesBoard").children[1].children;
  const advance = seconds => { nowMs += seconds * 1000; for (const tick of ticks) tick(); };
  return { run, get, rows, cross, saved, downloads, boardRows, advance };
}

{
  const p = page();
  assert.equal(p.rows().length, 10);
  assert.ok(p.rows().every(row => row.classes.has("empty")));
  assert.equal(p.boardRows().length, 1);
  assert.equal(p.boardRows()[0].children.length, 13);
  assert.equal(p.boardRows()[0].children[1].textContent, "—");
  assert.equal(p.boardRows()[0].children[11].textContent, "0");
  assert.equal(p.boardRows()[0].children[12].textContent, "0.0");
  p.run("begin(); for (let i = 0; i < 10; i++) shot(i + 1, (i % 2) * 8, 0)");
  assert.equal(p.get("seriesTitle").textContent, "Series 1");
  assert.equal(p.get("seriesCount").textContent, "10 / 10 shots");
  assert.equal(p.get("decimalTotal").textContent, "104.5");
  assert.equal(p.get("integerTotal").textContent, "100");
  assert.equal(p.boardRows()[0].children[11].textContent, "100");
  assert.equal(p.boardRows()[0].children[12].textContent, "104.5");
  assert.match(p.boardRows()[0].children[1].textContent, /10\.9/);
  assert.match(p.rows()[0].children[1].textContent, /10\.9/); // Pistol still shows decimals.
  assert.match(p.get("centre").textContent, /X \+4\.0 mm · Y 0\.0 mm/);
  assert.equal(p.cross().attrs["data-x"], "4");
  assert.equal(p.cross().attrs["data-y"], "0");
  assert.equal(p.cross().children.filter(node => node.tag === "path").length, 2);

  p.run("shot(11, -8, 0)");
  assert.equal(p.get("seriesTitle").textContent, "Series 2");
  assert.equal(p.get("seriesCount").textContent, "1 / 10 shots");
  assert.equal(p.rows().length, 10);
  assert.equal(p.rows()[0].children[0].textContent, "11");
  assert.equal(p.rows().filter(row => row.classes.has("empty")).length, 9);
  assert.equal(p.get("decimalTotal").textContent, "10.0");
  assert.equal(p.get("integerTotal").textContent, "10");
  assert.equal(p.boardRows().length, 2);
  assert.equal(p.boardRows()[1].children[1].textContent, "10.0");
  assert.equal(p.boardRows()[1].children[2].textContent, "—");
  assert.equal(p.boardRows()[1].children[11].textContent, "10");
  assert.equal(p.boardRows()[1].children[12].textContent, "10.0");
  assert.match(p.get("tally").textContent, /Decimal 114\.5 · Integer 110/);
  assert.equal(p.cross().attrs["data-x"], "-8"); // One hit has an average point too.

  p.boardRows()[0].children[0].children[0].click();
  p.run("shot(12, -4, 2)");
  assert.equal(p.get("seriesTitle").textContent, "Series 1"); // Incoming shots do not interrupt review.
  assert.equal(p.get("decimalTotal").textContent, "104.5");
  assert.match(p.get("latest").textContent, /^#10/);
  assert.ok(p.boardRows()[0].classes.has("selected"));
  p.boardRows()[0].children[3].children[0].click();
  assert.ok(p.rows()[2].classes.has("picked")); // The strip can select an individual shot.
  p.get("follow").click();
  assert.equal(p.get("seriesTitle").textContent, "Series 2");
  assert.equal(p.get("seriesCount").textContent, "2 / 10 shots");
  assert.equal(p.cross().attrs["data-x"], "-6");
  assert.equal(p.cross().attrs["data-y"], "1");
  assert.match(p.get("centre").textContent, /X -6\.0 mm · Y \+1\.0 mm/);
  p.run("for (let n = 13; n <= 21; n++) shot(n, 0, 0)");
  assert.equal(p.get("seriesTitle").textContent, "Series 3");
  assert.equal(p.rows()[0].children[0].textContent, "21");
  assert.equal(p.boardRows().length, 3);
  p.get("seriesView").value = "0";
  p.get("seriesView").onchange();
  assert.equal(p.get("seriesTitle").textContent, "Series 1");
  assert.equal(p.rows().length, 10);

  p.get("zoom").value = "1";
  p.get("zoom").onchange();
  const width = () => Number(p.get("target").attrs.viewBox.split(" ")[2]);
  const full = width();
  p.get("zoomIn").click();
  const closer = width();
  assert.ok(closer < full);
  p.get("zoomIn").click();
  assert.ok(width() < closer);
  p.get("zoomOut").click();
  assert.equal(width(), closer);
  assert.equal(p.cross().attrs["data-x"], "4");
  assert.equal(p.get("seriesTitle").textContent, "Series 1");
  assert.equal(JSON.parse(p.saved.get("fet")).zoom, 2);
}

{
  const p = page();
  p.run(`
    begin(111, SIGHT);
    shot(1, 0, 0, false, 3); shot(2, 0, 0, false, 3);
    receive({session: 1, seq: 4, change: 'mode', target: 111, session_type: MATCH});
  `);
  assert.equal(p.get("seriesTitle").textContent, "Series 1");
  assert.equal(p.get("seriesCount").textContent, "0 / 10 shots");
  assert.equal(p.rows().length, 10);
  assert.equal(p.get("decimalTotal").textContent, "0.0");
  p.run("for (let n = 4; n <= 13; n++) shot(n, 0, 0, false, 5)");
  assert.equal(p.get("seriesCount").textContent, "10 / 10 shots");
  assert.equal(p.get("decimalTotal").textContent, "109.0");
  assert.equal(p.get("integerTotal").textContent, "100");
  assert.match(p.get("tally").textContent, /10\/60 shots/); // Sighters are excluded.
  p.get("prev").click();
  assert.equal(p.get("seriesTitle").textContent, "Sighters");
  assert.equal(p.rows().length, 2);
}

{
  const p = page();
  p.run("begin(); shot(1, 2, 4); shot(2, 6, 0); for (let n = 3; n <= 10; n++) shot(n, 0, 0, true)");
  assert.equal(p.rows().length, 10);
  assert.equal(p.rows()[9].children[1].textContent, "M");
  assert.equal(p.rows()[9].children[2].textContent, "0");
  assert.equal(p.boardRows()[0].children[10].textContent, "M");
  assert.equal(p.cross().attrs["data-x"], "4");
  assert.equal(p.cross().attrs["data-y"], "2"); // Miss coordinates must not pull the mean toward zero.
  const coordinates = p.cross().children.find(node => node.tag === "path").attrs.d.match(/-?[\d.]+/g).map(Number);
  assert.equal((coordinates[0] + coordinates[2]) / 2, 4);
  assert.equal(coordinates[1], 2); // Drawing uses the same negative-up Y convention as the text.
  assert.equal(coordinates[3], 4);
  assert.equal((coordinates[4] + coordinates[5]) / 2, 2);
  p.run("for (let n = 11; n <= 20; n++) shot(n, 0, 0, true)");
  assert.equal(p.get("decimalTotal").textContent, "0.0");
  assert.equal(p.get("integerTotal").textContent, "0");
  assert.equal(p.cross(), undefined);
  assert.equal(p.get("centre").textContent, "Average point: no hits yet");
}

{
  const p = page();
  p.run(`
    begin();
    past = newSession(99); past.target = 100;
    for (let n = 1; n <= 11; n++) addEvent(past, {seq: n, shot: n, session_type: 1, x: 4, y: 2});
    view = 1; render();
    receive({session: 2, seq: 50, change: 'new', target: 101, session_type: 0});
    receive({session: 2, seq: 51, shot: 1, session_type: 1, x: -10, y: 0});
  `);
  assert.equal(p.get("seriesTitle").textContent, "Series 2");
  assert.equal(p.rows()[0].children[0].textContent, "11");
  assert.equal(p.cross().attrs["data-x"], "4");
  assert.equal(p.cross().attrs["data-y"], "-2");
  assert.ok(p.get("follow").classList.contains("hidden"));
}

{
  const p = page();
  p.run(`
    begin();
    receive({session: 1, seq: 2, shot: 1, session_type: 1, x: 4, y: 6});
  `);
  assert.equal(p.rows()[0].children[3].textContent, "+4.0");
  assert.equal(p.rows()[0].children[4].textContent, "-6.0");
  assert.equal(p.rows()[0].children[5].textContent, "↗");
  assert.equal(p.cross().attrs["data-y"], "-6");
  assert.match(p.get("centre").textContent, /X \+4\.0 mm · Y -6\.0 mm/);
  const hit = p.get("target").children.find(node => node.tag === "circle" && node.attrs.fill === "#ff3c3c");
  assert.equal(hit.attrs.cy, "-6");
  p.get("csv").click();
  const csv = p.downloads[0].trim().split("\n").map(row => row.split(","));
  assert.equal(csv[0][5], "y_mm");
  assert.equal(csv[1][5], "6.00"); // Exports keep the firmware's positive-up Y, like the journal and PC client.
  assert.equal(p.run('arrow({x: 0, y: -10})'), "↑");
  assert.equal(p.run('arrow({x: 0, y: 10})'), "↓");
  assert.equal(p.run('arrow({x: -10, y: 0})'), "←");
  assert.equal(p.run('arrow({x: 10, y: 0})'), "→");
  assert.equal(p.run('formatMm(-0.001)'), "0.0");
}

{
  const p = page();
  assert.match(p.get("wallClock").textContent, /^\d{2}:\d{2}:\d{2}$/);
  assert.equal(p.get("clock").textContent, "No active session");
  p.run("begin(101, 0, 1795000000)");
  assert.equal(p.get("clock").textContent, "Practice 0:00 elapsed");
  const wallTime = p.get("wallClock").textContent;
  p.advance(65);
  assert.notEqual(p.get("wallClock").textContent, wallTime);
  assert.equal(p.get("clock").textContent, "Practice 1:05 elapsed");
  p.run("past = newSession(99); past.target = 100; render()");
  assert.equal(p.get("clock").textContent, "Practice 1:05 elapsed"); // Review does not hide the live clock.
  p.run("receive({session: 1, seq: 2, change: 'end', utc: 1795000065})");
  p.advance(10);
  assert.equal(p.get("clock").textContent, "Session ended");
}

{
  const p = page();
  p.run("begin(110, SIGHT, 1795000000)");
  assert.equal(p.get("clock").textContent, "Sighting 15:00 left");
  p.advance(30);
  assert.equal(p.get("clock").textContent, "Sighting 14:30 left");
  p.run("receive({session: 1, seq: 2, change: 'mode', target: 110, session_type: MATCH, utc: 1795000030})");
  assert.equal(p.get("clock").textContent, "Match 1:15:00 left");
  p.advance(60);
  assert.equal(p.get("clock").textContent, "Match 1:14:00 left");
  p.run("prefs.matchMin = 1; renderClock()");
  assert.equal(p.get("clock").textContent, "Match time over");
  assert.ok(p.get("clock").classes.has("over"));
}

{
  const p = page();
  p.run("begin(101, 0, 1794999400)");
  assert.equal(p.get("clock").textContent, "Practice 10:00 elapsed"); // Replaying does not restart the clock.
}

console.log("Athlete page: series overview, dual totals, clocks, average point, zoom, browsing and coordinates/CSV passed");

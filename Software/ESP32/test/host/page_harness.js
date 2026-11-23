"use strict";

// A minimal DOM and timer harness that runs the athlete page's real script.
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

function load(file) {
  const html = fs.readFileSync(file || path.join(__dirname, "../../main/HTML/app.html"), "utf8");
  const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
  const startup = /render\(\);\s*connect\(\);\s*$/;
  assert(startup.test(script));
  return { html, script, startup };
}

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
  querySelector(selector) { return ((this.selected ||= {})[selector] ||= new Element("div")); }
  querySelectorAll(selector) { return [this.querySelector(selector)]; }
  showModal() { this.open = true; }
  remove() {}
}

function page(file, extra = {}) {
  const { html, script, startup } = load(file);
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
      if (id === "newForm") for (const name of ["face", "kind", "decimal", "athlete", "shots", "sightMin", "matchMin", "finalSightMin", "commands"]) node[name] = new Element("input");
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
    ...extra,
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


module.exports = { page };

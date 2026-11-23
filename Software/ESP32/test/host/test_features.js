"use strict";

// Finals, spoken range commands, calibration, shot timing and the printed report,
// run through the page's real script with a minimal DOM.
const assert = require("node:assert/strict");
const { page: open } = require("./page_harness");

const T = 1795000000;                                            // The harness clock starts here (seconds)

function withSpeech(extra = {}) {
  const said = [];
  const speechSynthesis = { speak: (u) => { if (u.text) said.push(u.text); }, cancel() {} };
  const p = open(process.argv[2], {
    speechSynthesis, SpeechSynthesisUtterance: class { constructor(text) { this.text = text; } },
    window: { speechSynthesis, print() { p.printed = (p.printed || 0) + 1; } }, ...extra,
  });
  return { p, said };
}

function startFinal(p, { sightAt = T, matchAt = null } = {}) {
  p.run(`
    prefs.commands = true;
    receive({hello: 'freETarget', name: 'Lane 2', next_seq: 1, target: 100, clock: 'ntp', event: 'Pistol Final'});
    receive({session: 4, seq: 1, change: 'new', target: 100, session_type: 0, utc: ${sightAt}});
    receive({session: 4, seq: 2, change: 'mode', target: 100, session_type: 2, utc: ${sightAt}});
    ${matchAt ? `receive({session: 4, seq: 3, change: 'mode', target: 100, session_type: 4, utc: ${matchAt}});` : ""}
    receive({caught_up: 3});
  `);
}
const shot = (p, seq, n, type, x = 0, y = 0, utc = 0) =>
  p.run(`receive({session: 4, seq: ${seq}, shot: ${n}, x: ${x}, y: ${-y}, session_type: ${type}, utc: ${utc}})`);

// ---- A final: sighters, then 2 x 5-shot series and single shots in pairs ----
{
  const { p } = withSpeech();
  startFinal(p);
  assert.match(p.get("title").textContent, /Final · sighters/);
  assert.equal(p.get("mode").textContent, "Start final");
  assert.ok(!p.get("mode").classList.contains("hidden"));
  shot(p, 3, 1, 3, 1, 1);
  shot(p, 4, 2, 3, 2, 2);
  p.run("receive({session: 4, seq: 5, change: 'mode', target: 100, session_type: 4, utc: " + (T + 400) + "})");
  assert.ok(p.get("mode").classList.contains("hidden"));       // No way back to sighters in a final
  for (let n = 1; n <= 24; n++) shot(p, 5 + n, n + 2, 5, 0, 0);
  assert.equal(p.get("title").textContent, "10m Air Pistol · Final");
  assert.match(p.get("tally").textContent, /^Final total · 24\/24 shots · Decimal 261\.6 · Integer 240/);
  const board = p.boardRows();
  assert.equal(board.length, 9);                                // Series 1, Series 2, then seven pairs
  assert.equal(board[0].children[0].textContent, "Series 1");
  assert.equal(board[2].children[0].textContent, "Shots 11–12");
  assert.equal(board[8].children[0].textContent, "Shots 23–24");
  assert.equal(board[0].children[5].textContent, "10.9*");      // Five shots (inner tens)...
  assert.equal(board[0].children[6].textContent, "");           // ...then no slots, not dashes
  assert.equal(board[2].children[3].textContent, "");
  assert.equal(board[0].children[12].textContent, "54.5");
  assert.ok(JSON.parse(p.saved.get("fet")).finals.includes(4)); // Remembered for History
}

// ---- The range officer: timed from the journal, spoken once, resumed after a reload ----
{
  const { p, said } = withSpeech();
  startFinal(p, { matchAt: T });
  p.advance(0);
  assert.deepEqual(said, []);                                   // "Get ready" is silent
  assert.equal(p.get("clock").textContent, "Get ready");
  p.advance(3);
  assert.deepEqual(said, ["For the first competition series, load."]);
  p.advance(7);
  assert.deepEqual(said.slice(1), ["Start."]);
  assert.equal(p.get("clock").textContent, "Series 1 · 4:10 left");
  p.advance(236);                                               // 14 s left
  assert.ok(p.get("clock").classes.has("warn"));
  p.advance(10);                                                // 4 s left
  assert.ok(p.get("clock").classes.has("over"));
  p.advance(4);
  assert.deepEqual(said.slice(2), ["Stop."]);
  p.advance(18);                                                // Stop (15 s), get ready (3 s), then load the second series
  assert.deepEqual(said.slice(3), ["For the next competition series, load."]);
  p.advance(7);
  assert.equal(p.get("clock").textContent, "Series 2 · 4:10 left");
  p.advance(30);                                                // Commands more than 5 s late are dropped, not spoken late
  assert.deepEqual(said.slice(4), ["Start."]);
  p.run("spoken.t -= 60");                                       // As if the tab had been asleep for a minute
  p.advance(1);
  assert.equal(said.length, 5);

  // A reload during series 2 carries on without repeating "Start."
  const again = withSpeech();
  again.p.advance(T + 300 - 1795000000);
  startFinal(again.p, { matchAt: T });
  again.p.advance(1);
  assert.deepEqual(again.said, []);
  assert.match(again.p.get("clock").textContent, /^Series 2 · /);

  // The singles: shot 11 starts after both series, each 3 + 7 + 250 + 15 s
  const third = withSpeech();
  startFinal(third.p, { matchAt: T - (2 * 275 + 10) });
  third.p.advance(0);
  assert.equal(third.p.get("clock").textContent, "Shot 11 · 0:50 left");
  assert.deepEqual(third.said, ["Start."]);                     // Opened just as it was due
  third.p.advance(50);
  assert.deepEqual(third.said.slice(1), ["Stop."]);
  third.p.advance(3 + 3);                                       // A short pause after an odd shot
  third.p.advance(7);
  assert.equal(third.p.get("clock").textContent, "Shot 12 · 0:50 left");
  assert.deepEqual(third.said.slice(2), ["For the next competition shot, load.", "Start."]);
  third.p.advance(50 + 15 + 3 + 7);                             // A longer pause for the elimination after a pair
  assert.equal(third.p.get("clock").textContent, "Shot 13 · 0:50 left");
}

// ---- Match commands: preparation, 30 seconds and stop, then match firing ----
{
  const { p, said } = withSpeech();
  p.run(`
    prefs.commands = true; prefs.sightMin = 15; prefs.matchMin = 75;
    receive({hello: 'freETarget', name: 'Lane 3', next_seq: 1, target: 110, clock: 'ntp', event: 'Rifle Match'});
    receive({session: 5, seq: 1, change: 'new', target: 110, session_type: 0, utc: ${T}});
    receive({session: 5, seq: 2, change: 'mode', target: 110, session_type: 2, utc: ${T}});
    receive({caught_up: 2});
  `);
  p.advance(0);
  assert.deepEqual(said, ["Your preparation and sighting time starts now."]);
  p.advance(870);
  assert.deepEqual(said.slice(1), ["30 seconds."]);
  p.advance(30);
  assert.deepEqual(said.slice(2), ["Stop."]);
  assert.equal(p.get("clock").textContent, "Sighting time over");
  p.advance(60);                                                // The athlete starts the match a minute later
  p.run(`receive({session: 5, seq: 3, change: 'mode', target: 110, session_type: 4, utc: ${T + 960}})`);
  p.advance(0);
  assert.deepEqual(said.slice(3), ["Match firing. Start."]);
  p.advance(75 * 60 - 600);
  assert.deepEqual(said.slice(4), ["10 minutes."]);
  p.run("prefs.commands = false");                              // Off: nothing more is spoken
  p.advance(600);
  assert.equal(said.length, 5);
  assert.equal(p.get("clock").textContent, "Match time over");
}

// ---- Shot timing: gaps, times, the summary line and the CSV ----
{
  const { p } = withSpeech();
  p.run(`
    receive({hello: 'freETarget', name: 'Lane 4', next_seq: 1, target: 100, clock: 'ntp', event: 'Pistol Practice'});
    receive({session: 4, seq: 1, change: 'new', target: 100, session_type: 0, utc: ${T}});
    receive({caught_up: 1});
  `);
  shot(p, 2, 1, 1, 0, 0, T + 60);
  shot(p, 3, 2, 1, 0, 0, T + 100);
  shot(p, 4, 3, 1, 0, 0, T + 230);
  shot(p, 5, 4, 1, 0, 0, 0);                                    // No time: its gap and the next are unknown
  shot(p, 6, 5, 1, 0, 0, T + 300);
  shot(p, 7, 6, 1, 0, 0, T + 315);
  const rows = p.rows();
  assert.equal(rows[0].children[6].textContent, "—");
  assert.equal(rows[1].children[6].textContent, "0:40");
  assert.equal(rows[2].children[6].textContent, "2:10");
  assert.equal(rows[3].children[7].textContent, "—");
  assert.equal(rows[4].children[6].textContent, "—");
  assert.equal(rows[5].children[6].textContent, "0:15");
  assert.match(rows[1].children[7].textContent, /^\d\d:\d\d:\d\d$/);
  assert.match(p.get("timing").textContent, /\(4:15\) · average gap 1:01 · shortest 0:15 \(#6\) · longest 2:10 \(#3\)$/);
  p.get("csv").click();
  const csv = p.downloads[0].trim().split("\n").map((row) => row.split(","));
  assert.equal(csv[0][12], "gap_s");
  assert.deepEqual(csv.slice(1).map((row) => row[12]), ["", "40", "130", "", "", "15"]);
}

// ---- Calibration through /json: read, preview, save, and a locked target ----
(async () => {
  const requests = [];
  let offsets = { x: 0.25, y: -0.5 }, locked = false;
  const fetch = async (url) => {
    const command = JSON.parse(decodeURIComponent(url.replace(/^\/json\?/, "")));
    requests.push(command);
    if ("X_OFFSET" in command) {
      if (locked) return { ok: true, text: async () => "\r\nInvalid input or locked: {...}\r\n" };
      offsets = { x: command.X_OFFSET, y: command.Y_OFFSET };
      return { ok: true, text: async () => "" };
    }
    return { ok: true, text: async () => `{\r\n"NAME": "FET",\r\n"X_OFFSET":         ${offsets.x.toFixed(2)}, "Y_OFFSET":   ${offsets.y.toFixed(2)},\r\n}` };
  };
  const settle = () => new Promise((resolve) => setImmediate(resolve));
  const { p } = withSpeech({ fetch });
  p.run(`
    receive({hello: 'freETarget', name: 'Lane 5', next_seq: 1, target: 100, clock: 'ntp', event: 'Pistol Practice'});  // Older firmware: no offsets
    receive({session: 4, seq: 1, change: 'new', target: 100, session_type: 0, utc: ${T}});
    receive({caught_up: 1});
  `);
  shot(p, 2, 1, 1, 10, -5);                                     // 10 mm right, 5 mm up (display Y is negative up)
  const hit = () => p.get("target").children.find((node) => node.tag === "circle" && node.attrs.fill === "#ff3c3c");
  p.get("calibrate").click();
  assert.ok(!p.get("calib").classList.contains("hidden"));
  await settle();
  assert.deepEqual(requests[0], { ECHO: 0 });
  assert.equal(p.get("calText").textContent, "Offset 0.25 mm right · 0.50 mm down");
  assert.ok(p.get("calSave").disabled);                          // Nothing changed yet
  p.run("prefs.calStep = 0.5");
  p.get("calUp").click();
  p.get("calUp").click();
  p.get("calRight").click();
  assert.equal(p.get("calText").textContent, "Offset 0.75 mm right · 0.50 mm up (saved: 0.25 mm right · 0.50 mm down)");
  assert.equal(hit().attrs.cx, "10.5");                         // The preview moves the shot right 0.5...
  assert.equal(hit().attrs.cy, "-6");                           // ...and up 1 mm
  assert.ok(!p.get("calSave").disabled);
  p.get("calSave").click();
  await settle(); await settle();
  assert.deepEqual(requests.slice(1), [{ X_OFFSET: 0.75, Y_OFFSET: 0.5 }, { ECHO: 0 }]);
  assert.match(p.get("calNote").textContent, /^Saved on the target/);
  assert.equal(hit().attrs.cx, "10");                           // Saved: the old shot keeps its place
  locked = true;
  p.get("calLeft").click();
  p.get("calSave").click();
  await settle(); await settle();
  assert.match(p.get("calNote").textContent, /locked/);
  assert.ok(p.get("calNote").classes.has("error"));
  p.get("calClose").click();
  assert.ok(p.get("calib").classList.contains("hidden"));

  // Current firmware: the hello carries the offsets, so only the setting is sent
  requests.length = 0; locked = false;
  p.run("receive({hello: 'freETarget', name: 'Lane 5', next_seq: 3, target: 100, clock: 'ntp', x_offset: 1.5, y_offset: -0.25})");
  p.get("calibrate").click();
  assert.equal(p.get("calText").textContent, "Offset 1.50 mm right · 0.25 mm down");
  p.get("calDown").click();
  p.get("calSave").click();
  await settle(); await settle();
  assert.deepEqual(requests, [{ X_OFFSET: 1.5, Y_OFFSET: -0.75 }]);
  assert.match(p.get("calNote").textContent, /^Saved on the target/);
  assert.equal(p.run("hello.y_offset"), -0.75);
  p.get("calClose").click();

  // ---- The report: eight sections per page, sighters first, running totals ----
  const r = withSpeech();
  r.p.run(`
    receive({hello: 'freETarget', name: 'Lane 6', next_seq: 1, target: 110, clock: 'ntp', event: 'Rifle Match', athelete: 'Sam'});
    receive({session: 4, seq: 1, change: 'new', target: 110, session_type: 0, utc: ${T}});
    receive({session: 4, seq: 2, change: 'mode', target: 110, session_type: 2, utc: ${T}});
    receive({caught_up: 2});
  `);
  for (let n = 1; n <= 3; n++) shot(r.p, 2 + n, n, 3, 0, 0, T + 10 * n);
  r.p.run(`receive({session: 4, seq: 6, change: 'mode', target: 110, session_type: 4, utc: ${T + 100}})`);
  for (let n = 1; n <= 23; n++) shot(r.p, 6 + n, 3 + n, 5, n === 23 ? 3 : 0, 0, T + 100 + 20 * n);
  r.p.get("print").click();
  assert.equal(r.p.printed, 1);
  const pages = r.p.get("report").children;
  assert.equal(pages.length, 1);
  const [head, who, summary] = pages[0].children;
  assert.equal(head.children[0].textContent, "MATCH REPORT");
  assert.match(who.textContent, /^Sam · Session 4 · /);
  assert.equal(summary.textContent, "23/60 match shots · Score 229 · Decimal 249.6 · 22X");
  const grid = pages[0].children[5], panels = grid.children;
  assert.equal(panels.length, 8);
  assert.match(panels[0].children[0].textContent, /^Sighters 1–3/);
  assert.match(panels[1].children[0].textContent, /^Match series 1  \|  1–10/);
  assert.match(panels[3].children[0].textContent, /^Match series 3  \|  21–23/);
  const rows = panels[3].children[2].children[0].children[1].children;
  assert.equal(rows.length, 10);                                // Unused rows stay, as on the card
  assert.equal(rows[2].children[0].textContent, "23");
  assert.equal(rows[2].children[1].textContent, "9.8");
  assert.equal(rows[3].children[1].textContent, "–");
  assert.equal(panels[3].children[2].children[2].textContent, "Match running  229 (249.6)");
  assert.equal(panels[4].children.length, 0);                   // Empty slots keep the grid
  const plot = panels[1].children[1].children[0];
  assert.ok(plot.children.some((node) => node.tag === "circle" && node.attrs.fill === "#6b7379"));

  // A final's report: series of five, then the single shots in tens
  const f = withSpeech();
  startFinal(f.p, { matchAt: T });
  for (let n = 1; n <= 24; n++) shot(f.p, 3 + n, n, 5);
  f.p.get("print").click();
  const finalPanels = f.p.get("report").children[0].children[5].children;
  assert.match(finalPanels[0].children[0].textContent, /^Final series 1  \|  1–5/);
  assert.match(finalPanels[2].children[0].textContent, /^Final shots 11–20/);
  assert.match(finalPanels[3].children[0].textContent, /^Final shots 21–24/);
  assert.equal(finalPanels[3].children[2].children[0].children[1].children.length, 4);
  assert.equal(f.p.get("report").children[0].children[0].children[0].textContent, "FINAL REPORT");

  console.log("Athlete page: finals, range commands, calibration, shot timing and the report passed");
})().catch((error) => { console.error(error); process.exit(1); });

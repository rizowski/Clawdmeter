#!/usr/bin/env node
/**
 * Authors the "baking" Clawd scene → research/clawd-official/Clawd-Baking.gif.
 *
 * FAN-MADE, NOT AN OFFICIAL ANTHROPIC ASSET. No official baking/cookie Clawd
 * exists (probed claude.ai/images/clawd/{core,persona}/ for Baking, Cookie(s),
 * Chef, Baker, Cooking, Oven, Bake, Kitchen, Whisk — all HTML catch-alls).
 * Drawn in the official style with the shared kit in clawd_art.js so it drops
 * into the same pipeline as the official GIFs.
 *
 * Story: idle → bowl + whisk, stir → oven, bake (window glows, dough browns)
 * → ding → proud arms-up show-off with one Crumbl-size cookie, knees bobbing (the scene loop —
 * drawn twice so the converter detects it) → into a pink box, pushed along a counter until it slides off stage → idle.
 *
 * Requires ImageMagick (`magick`) on PATH.
 * Usage: node author_clawd_baking.js [--out FILE] [--preview FILE]
 *   --preview FILE  also writes a 10 px/cell GIF on black for eyeballing.
 */

const { C, put, rect, line, clawdIdle, clawdArmsUp, bigCookie, cookieOut, box, sparkle, stamp, scene } =
  require('./clawd_art');

// ── Baking-only props ────────────────────────────────────────────────────────

// Mixing bowl on the floor right of Clawd (x 40..51). `grow` 0..1 pops it in.
function bowl(g, { grow = 1, batter = true } = {}) {
  if (grow < 1) { rect(g, 43, 33, 48, 36, C.bowl); rect(g, 47, 33, 48, 35, C.bowlSh); return; }
  if (batter) rect(g, 42, 30, 49, 30, C.dough);
  rect(g, 40, 31, 51, 31, C.bowl);
  rect(g, 41, 32, 50, 34, C.bowl);
  rect(g, 42, 35, 49, 35, C.bowl);
  rect(g, 49, 32, 50, 34, C.bowlSh);
  put(g, 49, 35, C.bowlSh);
  rect(g, 44, 36, 47, 36, C.bowlSh);
}

// Whisk from the right hand into the bowl; `tip` is its head x (43..48).
function whisk(g, tip, bend = 0) {
  line(g, 39, 26 + bend, tip, 29, C.steel);
  rect(g, tip - 1, 29, tip + 1, 30, C.steel);
  put(g, tip, 29, null);        // wire loop gap reads as a whisk head
}

// Flecks of batter flicked up by the whisk.
function splat(g, pts) { for (const [x, y] of pts) put(g, x, y, C.dough); }

// Oven on the left (x 1..13). `win`: 'dark' | 'raw' | 'glow' | 'glow2' |
// 'done' | 'open'. `grow` < 1 draws a squat pop-in frame.
function oven(g, { grow = 1, win = 'dark', browning = C.dough } = {}) {
  if (grow < 1) { rect(g, 3, 31, 11, 36, C.steel); rect(g, 4, 33, 10, 35, C.dark); return; }
  rect(g, 1, 22, 13, 35, C.steel);
  rect(g, 2, 36, 3, 36, C.dark); rect(g, 11, 36, 12, 36, C.dark);   // feet
  rect(g, 1, 22, 13, 22, C.dark);                                    // top trim
  put(g, 3, 24, C.dark); put(g, 6, 24, C.dark); put(g, 9, 24, C.dark); // knobs
  rect(g, 3, 26, 11, 26, C.ivory);                                   // handle
  const glass = { dark: C.dark, raw: C.dark, done: C.dark, glow: C.glow, glow2: C.spark, open: null }[win];
  rect(g, 3, 28, 11, 34, glass);
  if (win === 'open') return;
  if (win !== 'dark') {                                              // tray + dough
    rect(g, 4, 33, 10, 33, C.steel);
    rect(g, 5, 31, 9, 31, browning);                                 // one giant
    rect(g, 4, 32, 10, 32, browning);                                // cookie puck
  }
}

// The giant cookie held overhead, resting on the raised arms.
function cookieOverhead(g, { bend = 0 } = {}) { bigCookie(g, 17, 8 + bend); }

// Steam wisps above the cookie; phase 0..3 wiggles them.
function steam(g, phase, bend = 0) {
  const wig = [0, 1, 0, -1];
  for (const [i, x] of [21, 27, 32].entries()) {
    const o = wig[(phase + i) % 4];
    put(g, x + o, 6 + bend, C.ivory);
    put(g, x - o, 5 + bend, C.ivory);
    if ((phase + i) % 2 === 0) put(g, x + o, 4 + bend, C.ivory);
  }
}

// Counter on the right at hand height (x 41..54, top at row 27).
function counter(g, { grow = 1 } = {}) {
  if (grow < 1) { rect(g, 45, 32, 54, 32, C.ivory); rect(g, 45, 33, 54, 36, C.steel); return; }
  rect(g, 41, 27, 54, 27, C.ivory);
  rect(g, 42, 28, 54, 36, C.steel);
  rect(g, 42, 28, 54, 28, C.dark);
  rect(g, 44, 31, 54, 31, C.dark);                     // drawer line
}

// ── Storyboard ───────────────────────────────────────────────────────────────

const { frame, finish } = scene('Clawd-Baking.gif');

// Idle bookend.
frame(400, g => clawdIdle(g));
frame(120, g => clawdIdle(g, { eyes: 'squint' }));   // blink
frame(200, g => clawdIdle(g));

// Bowl pops in, then stirring. Each pass flicks different batter so no two
// stir cycles are identical (keeps the converter's loop detector on the
// show-off loop below).
frame(100, g => { clawdIdle(g); bowl(g, { grow: 0.5 }); });
frame(180, g => { clawdIdle(g); bowl(g); });
const tips = [44, 46, 48, 46];
const flecks = [
  [], [[45, 28]], [], [[48, 27]],
  [[43, 28]], [], [[47, 28], [50, 27]], [],
  [], [[44, 27]], [[49, 28]], [],
  [[46, 27]], [], [], [[43, 27], [48, 26]],
];
for (let i = 0; i < 16; i++)
  frame(110, g => {
    const bend = i % 4 === 1 ? 1 : 0;
    clawdIdle(g, { eyes: 'squint', bend });
    bowl(g); whisk(g, tips[i % 4], bend); splat(g, flecks[i]);
  });
frame(250, g => { clawdIdle(g); bowl(g); whisk(g, 46); });

// Oven pops in on the left; bowl packs away with a puff.
frame(100, g => { clawdIdle(g, { eyes: 'left' }); bowl(g); whisk(g, 46); oven(g, { grow: 0.5 }); });
frame(160, g => { clawdIdle(g, { eyes: 'left' }); bowl(g); oven(g); });
frame(100, g => {
  clawdIdle(g, { eyes: 'left' }); oven(g);
  for (const [x, y] of [[42, 33], [46, 31], [50, 34], [44, 35], [48, 36]]) put(g, x, y, C.ivory);
});
frame(200, g => { clawdIdle(g, { eyes: 'left' }); oven(g); });

// Tray of dough goes in: door opens, tray slides in from the left hand.
frame(160, g => { clawdIdle(g, { eyes: 'left' }); oven(g, { win: 'open' });
  rect(g, 12, 27, 14, 27, C.steel); put(g, 13, 26, C.dough); });
frame(160, g => { clawdIdle(g, { eyes: 'left' }); oven(g, { win: 'open' });
  rect(g, 6, 33, 12, 33, C.steel); for (const x of [7, 10]) put(g, x, 32, C.dough); });
frame(200, g => { clawdIdle(g, { eyes: 'left' }); oven(g, { win: 'raw' }); });

// Bake: the window pulses while the dough browns (dough → tan → cookie).
// Clawd watches, then bounces impatiently.
const bake = [
  ['glow', C.dough, 0], ['glow2', C.dough, 0], ['glow', C.tan, 1], ['glow2', C.tan, 0],
  ['glow', C.tan, 0], ['glow2', C.cookie, 1], ['glow', C.cookie, 0], ['glow2', C.cookie, 0],
];
for (const [win, browning, bend] of bake)
  frame(220, g => { clawdIdle(g, { eyes: 'left', bend }); oven(g, { win, browning }); });

// Ding! Glow off, sparkle over the oven.
frame(120, g => { clawdIdle(g); oven(g, { win: 'done', browning: C.cookie }); sparkle(g, 7, 19, 1); });
frame(280, g => { clawdIdle(g, { bend: 1 }); oven(g, { win: 'done', browning: C.cookie }); sparkle(g, 7, 18, 2); });
frame(160, g => { clawdIdle(g); oven(g, { win: 'done', browning: C.cookie }); sparkle(g, 7, 18, 1, C.ivory); });

// Tray comes out; oven packs away.
frame(160, g => { clawdIdle(g, { eyes: 'left' }); oven(g, { win: 'open' });
  rect(g, 12, 27, 14, 27, C.steel); cookieOut(g, 9, 23); });
frame(120, g => { clawdIdle(g); oven(g, { grow: 0.5 });
  rect(g, 12, 27, 14, 27, C.steel); cookieOut(g, 9, 23); });
frame(120, g => { clawdIdle(g);
  for (const [x, y] of [[4, 33], [8, 31], [11, 35], [6, 36]]) put(g, x, y, C.ivory);
  rect(g, 12, 27, 14, 27, C.steel); cookieOut(g, 9, 23); });

// Show off: arms up, giant cookie overhead, knees bobbing. Loop body
// (4 frames) drawn twice so the converter detects it.
frame(140, g => { clawdArmsUp(g); cookieOverhead(g); });
for (let rep = 0; rep < 2; rep++) {
  frame(160, g => { clawdArmsUp(g); cookieOverhead(g); steam(g, 0); sparkle(g, 12, 9, 2); });
  frame(160, g => { clawdArmsUp(g, { bend: 1 }); cookieOverhead(g, { bend: 1 }); steam(g, 1, 1); sparkle(g, 12, 9, 1); sparkle(g, 42, 6, 1); });
  frame(160, g => { clawdArmsUp(g); cookieOverhead(g); steam(g, 2); sparkle(g, 42, 6, 2); });
  frame(160, g => { clawdArmsUp(g, { bend: 1 }); cookieOverhead(g, { bend: 1 }); steam(g, 3, 1); sparkle(g, 42, 6, 1); sparkle(g, 12, 9, 1); });
}

// Outro: box them up. Counter + open pink box pop in, the cookie comes down
// into the box, lid closes, Clawd pushes it along the counter and it slides
// off the stage. Counter packs away → idle.
frame(120, g => { clawdArmsUp(g); cookieOverhead(g); counter(g, { grow: 0.5 }); });
frame(160, g => { clawdArmsUp(g); cookieOverhead(g); counter(g); box(g, 42, { open: true }); });
frame(140, g => { clawdIdle(g, { eyes: 'happy' }); counter(g); box(g, 42, { open: true }); cookieOut(g, 33, 15); });
frame(140, g => { clawdIdle(g, { eyes: 'happy' }); counter(g); box(g, 42, { open: true }); cookieOut(g, 40, 14); });
frame(140, g => { clawdIdle(g); counter(g); box(g, 42, { open: true }); cookieOut(g, 45, 18); });
frame(160, g => { clawdIdle(g); counter(g); box(g, 42, { open: true });
  stamp(g, 45, 22, ['TKTTKT'], { T: C.tan, K: C.chip }); });
frame(200, g => { clawdIdle(g, { eyes: 'happy' }); counter(g); box(g, 42); sparkle(g, 48, 19, 2); });
// Push: knees bend, the right hand reaches out, the box slides away.
frame(140, g => { clawdIdle(g, { bend: 1 }); counter(g); box(g, 42); });
for (const [i, x] of [44, 47, 51, 55].entries())
  frame(110, g => { clawdIdle(g, { bend: i < 2 ? 1 : 0 }); counter(g); box(g, x);
    if (i === 0) rect(g, 39, 26, 43, 26, C.body); });
frame(220, g => { clawdIdle(g, { eyes: 'happy' }); counter(g); });
frame(120, g => { clawdIdle(g); counter(g, { grow: 0.5 }); });
frame(120, g => { clawdIdle(g);
  for (const [x, y] of [[46, 33], [50, 31], [53, 35], [48, 36]]) put(g, x, y, C.ivory); });
frame(400, g => clawdIdle(g));

finish();

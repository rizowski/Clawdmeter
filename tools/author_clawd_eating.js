#!/usr/bin/env node
/**
 * Authors the "eating" Clawd scene → research/clawd-official/Clawd-Eating.gif.
 *
 * FAN-MADE, NOT AN OFFICIAL ANTHROPIC ASSET — the companion to the baking
 * scene (tools/author_clawd_baking.js), drawn with the same kit
 * (clawd_art.js) so it drops into the official pipeline.
 *
 * Story: idle → a pink cookie box slides in along the floor → he opens it and
 * lifts out a Crumbl-size cookie → eats it bite by bite, crumbs falling to
 * the ground → content knee-bob with a floating heart over the crumbs (the
 * scene loop — drawn twice so the converter detects it) → box and crumbs
 * poof away → idle.
 *
 * Requires ImageMagick (`magick`) on PATH.
 * Usage: node author_clawd_eating.js [--out FILE] [--preview FILE]
 *   --preview FILE  also writes a 10 px/cell GIF on black for eyeballing.
 */

const { C, put, stamp, clawdIdle, bigCookie, cookieOut, box, sparkle, scene } =
  require('./clawd_art');

const { frame, finish } = scene('Clawd-Eating.gif');

const BOX_X = 41;          // resting spot on the floor, right of Clawd
const FLOOR = 36;

// The cookie held in both hands at chest height, in front of his body.
function held(g, bites, bend = 0) { bigCookie(g, 17, 25 + bend, bites); }

// Crumbs: `ground` are landed [x, color] pairs on the floor row; `air` are
// [x, y, color] mid-fall.
function crumbs(g, ground, air = []) {
  for (const [x, c] of ground) put(g, x, FLOOR, c);
  for (const [x, y, c] of air) put(g, x, y, c);
}

function heart(g, x, y) {
  stamp(g, x, y, ['.P.P.', 'PPPPP', '.PPP.', '..P..'], { P: C.pink });
}

function puff(g, pts) { for (const [x, y] of pts) put(g, x, y, C.ivory); }

// ── Delivery ─────────────────────────────────────────────────────────────────

frame(400, g => clawdIdle(g));
frame(120, g => clawdIdle(g, { eyes: 'squint' }));   // blink
frame(300, g => clawdIdle(g));

// The box slides in from off-stage right; he glances over.
for (const x of [54, 50, 46, 43])
  frame(110, g => { clawdIdle(g, { eyes: 'right' }); box(g, x, { base: FLOOR }); });
frame(200, g => { clawdIdle(g, { eyes: 'right' }); box(g, BOX_X, { base: FLOOR }); });
frame(160, g => { clawdIdle(g, { eyes: 'happy', bend: 1 }); box(g, BOX_X, { base: FLOOR }); sparkle(g, 47, 29, 2); });
frame(200, g => { clawdIdle(g, { eyes: 'happy' }); box(g, BOX_X, { base: FLOOR }); sparkle(g, 47, 29, 1); });

// Lid up, cookie rises out and into his hands — it's huge.
frame(180, g => { clawdIdle(g, { eyes: 'right' }); box(g, BOX_X, { base: FLOOR, open: true }); });
frame(160, g => { clawdIdle(g, { eyes: 'right' }); box(g, BOX_X, { base: FLOOR, open: true });
  cookieOut(g, 44, 29); });
frame(140, g => { clawdIdle(g, { eyes: 'right' }); box(g, BOX_X, { base: FLOOR, open: true });
  cookieOut(g, 40, 24); });
frame(220, g => { clawdIdle(g); box(g, BOX_X, { base: FLOOR, open: true }); held(g, []);
  sparkle(g, 14, 22, 2); sparkle(g, 40, 21, 1); });
frame(300, g => { clawdIdle(g, { eyes: 'happy' }); box(g, BOX_X, { base: FLOOR, open: true }); held(g, []);
  sparkle(g, 14, 22, 1); sparkle(g, 40, 21, 2); });

// ── Eating ───────────────────────────────────────────────────────────────────

// Bites in cookie-local cells, working from the top edge (his mouth) down.
const BITES = [
  [10, -1, 3], [4, 0, 3], [16, 0, 3], [10, 3, 4], [3, 5, 4],
  [17, 5, 4], [10, 7, 5], [4, 10, 4], [16, 10, 4],
];
// Where each bite's crumbs land — floor cells clear of the legs and splayed feet.
const DROPS = [
  [[26, C.tan]], [[22, C.cookie], [16, C.tan]], [[32, C.tan]], [[27, C.chip], [37, C.cookie]],
  [[21, C.tan]], [[36, C.tan], [31, C.cookie]], [[25, C.cookie], [28, C.tan]],
  [[17, C.chip]], [[38, C.tan], [15, C.cookie]],
];

const ground = [];
BITES.forEach((_, i) => {
  const bites = BITES.slice(0, i + 1);
  const last = i === BITES.length - 1;
  const air = DROPS[i].map(([x, c]) => [x, 34, c]);
  // Chomp: the bite vanishes, crumbs start to fall.
  frame(150, g => { clawdIdle(g, { eyes: 'squint' }); box(g, BOX_X, { base: FLOOR, open: true });
    held(g, bites); crumbs(g, ground, air); });
  ground.push(...DROPS[i]);
  const landed = ground.slice();
  // Chew: knees bend, crumbs land.
  frame(160, g => { clawdIdle(g, { eyes: 'happy', bend: 1 }); box(g, BOX_X, { base: FLOOR, open: true });
    held(g, bites, 1); crumbs(g, landed); });
  frame(last ? 200 : 130, g => { clawdIdle(g, { eyes: 'squint' }); box(g, BOX_X, { base: FLOOR, open: true });
    held(g, bites); crumbs(g, landed); });
});

// Last crumb of cookie gone: gulp.
const full = ground.slice();
frame(200, g => { clawdIdle(g, { eyes: 'squint' }); box(g, BOX_X, { base: FLOOR, open: true }); crumbs(g, full); });
frame(220, g => { clawdIdle(g, { eyes: 'open' }); box(g, BOX_X, { base: FLOOR }); crumbs(g, full); });

// ── Content loop: knee bob, heart floating up. Drawn twice. ─────────────────

for (let rep = 0; rep < 2; rep++) {
  frame(200, g => { clawdIdle(g, { eyes: 'happy' }); box(g, BOX_X, { base: FLOOR }); crumbs(g, full); heart(g, 36, 16); });
  frame(200, g => { clawdIdle(g, { eyes: 'happy', bend: 1 }); box(g, BOX_X, { base: FLOOR }); crumbs(g, full); heart(g, 37, 14); });
  frame(200, g => { clawdIdle(g, { eyes: 'happy' }); box(g, BOX_X, { base: FLOOR }); crumbs(g, full); heart(g, 36, 12); });
  frame(200, g => { clawdIdle(g, { eyes: 'happy', bend: 1 }); box(g, BOX_X, { base: FLOOR }); crumbs(g, full);
    sparkle(g, 38, 12, 1, C.pink); });
}

// ── Outro: box and crumbs poof → idle. ──────────────────────────────────────

frame(140, g => { clawdIdle(g); crumbs(g, full);
  puff(g, [[42, 34], [46, 31], [50, 35], [44, 36], [52, 32], [48, 33]]); });
frame(140, g => { clawdIdle(g);
  puff(g, full.map(([x], i) => [x, FLOOR - (i % 2)])); });
frame(400, g => clawdIdle(g));

finish();

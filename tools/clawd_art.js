/**
 * Shared pixel-art kit for the fan-made Clawd scenes (tools/author_clawd_*.js).
 *
 * Everything draws onto the official 55×37 stage as a grid of RGB triples
 * (null = transparent). Clawd's poses are cell-exact to the core GIFs' idle
 * frame (x 15..38, y 21..36); eyes are left as transparent holes, which the
 * converter inks. `scene()` collects frames and renders them to a GIF at
 * 50 px/cell, matching the core GIFs, plus an optional 10 px/cell preview.
 */

const fs = require('fs');
const os = require('os');
const path = require('path');
const { execFileSync } = require('child_process');

const W = 55, H = 37;

// Official colors first (body / shading / ivory / fedora gray), then props.
// Every pair stays well outside the converter's COLOR_MERGE_DIST2 (600).
const C = {
  body:   [0xD9, 0x77, 0x57],
  shade:  [0xBE, 0x68, 0x4D],
  ivory:  [0xF9, 0xF8, 0xF4],
  steel:  [0x8B, 0x8B, 0x8B],
  dark:   [0x4A, 0x4A, 0x48],
  bowl:   [0x6A, 0x9B, 0xC3],
  bowlSh: [0x4F, 0x7A, 0x9E],
  dough:  [0xF2, 0xD2, 0x9B],
  tan:    [0xDC, 0xA8, 0x62],
  cookie: [0xB8, 0x7A, 0x3E],
  chip:   [0x5A, 0x3A, 0x22],
  glow:   [0xF2, 0x8C, 0x28],
  spark:  [0xF5, 0xC5, 0x42],
  pink:   [0xF7, 0xA8, 0xC4],   // the pink cookie box
  pinkSh: [0xD4, 0x78, 0x9C],
};

// ── Canvas primitives ────────────────────────────────────────────────────────

const blank = () => Array.from({ length: H }, () => new Array(W).fill(null));
// Art is clipped to stage columns 1..53. splash.cpp's compose_stage() snaps
// any animation touching column 0 or 54 to the screen edge (that's for
// Lurking's peek-in); touching it would shift the whole scene — idle Clawd
// included — off the shared position. Props sliding "off stage" vanish here.
const X_MIN = 1, X_MAX = W - 2;
function put(g, x, y, c) { if (x >= X_MIN && x <= X_MAX && y >= 0 && y < H) g[y][x] = c; }
function rect(g, x0, y0, x1, y1, c) {
  for (let y = y0; y <= y1; y++) for (let x = x0; x <= x1; x++) put(g, x, y, c);
}
function line(g, x0, y0, x1, y1, c) {
  const dx = Math.abs(x1 - x0), dy = -Math.abs(y1 - y0);
  const sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
  let e = dx + dy;
  for (;;) {
    put(g, x0, y0, c);
    if (x0 === x1 && y0 === y1) break;
    const e2 = 2 * e;
    if (e2 >= dy) { e += dy; x0 += sx; }
    if (e2 <= dx) { e += dx; y0 += sy; }
  }
}
// Stamp an ASCII sprite; '.' is transparent, other chars index `map`.
function stamp(g, x, y, rows, map) {
  rows.forEach((row, r) => [...row].forEach((ch, c) => {
    if (ch !== '.') put(g, x + c, y + r, map[ch]);
  }));
}

// ── Clawd ────────────────────────────────────────────────────────────────────

// Standing idle, cell-exact to frame 0 of the core GIFs. `bend` (0..2)
// crouches him: head/arms/body sink while the feet stay planted, the legs
// shorten and the outer legs splay — a knee bend instead of a hop.
// Eyes: 'open' (2×2 holes), 'left' / 'right' (glancing aside), 'squint'
// (focused, bottom row only), 'happy' (^ ^, JumpingHappy's).
function clawdIdle(g, { bend = 0, eyes = 'open' } = {}) {
  rect(g, 19, 21 + bend, 34, 24 + bend, C.body);       // head
  rect(g, 15, 25 + bend, 38, 28 + bend, C.body);       // arms
  rect(g, 19, 29 + bend, 34, 32 + bend, C.body);       // body
  legs(g, bend);
  eyeHoles(g, 23 + bend, eyes);
}

// Arms raised overhead (JumpingHappy's pose), feet planted.
function clawdArmsUp(g, { bend = 0 } = {}) {
  rect(g, 20, 17 + bend, 23, 20 + bend, C.body);
  rect(g, 30, 17 + bend, 33, 20 + bend, C.body);
  rect(g, 19, 21 + bend, 34, 32 + bend, C.body);
  legs(g, bend);
  eyeHoles(g, 24 + bend, 'happy');
}

// Four legs from under the body (row 33 + bend) down to the floor (row 36).
// When bent, the outer legs' feet step out one cell.
function legs(g, bend) {
  for (const x of [19, 23, 29, 33]) {
    rect(g, x, 33 + bend, x + 1, 36, C.body);
    if (bend && (x === 19 || x === 33)) {
      const o = x === 19 ? -1 : 1;
      rect(g, x, 36, x + 1, 36, null);
      rect(g, x + o, 36, x + 1 + o, 36, C.body);
    }
  }
}

function eyeHoles(g, y, eyes) {
  if (eyes === 'open' || eyes === 'left' || eyes === 'right') {
    const s = eyes === 'left' ? -1 : eyes === 'right' ? 1 : 0;
    rect(g, 21 + s, y, 22 + s, y + 1, null);
    rect(g, 31 + s, y, 32 + s, y + 1, null);
  } else if (eyes === 'squint') {
    rect(g, 21, y + 1, 22, y + 1, null);
    rect(g, 31, y + 1, 32, y + 1, null);
  } else if (eyes === 'happy') {
    rect(g, 22, y, 23, y, null); put(g, 21, y + 1, null); put(g, 24, y + 1, null);
    rect(g, 30, y, 31, y, null); put(g, 29, y + 1, null); put(g, 32, y + 1, null);
  }
}

// ── Shared props ─────────────────────────────────────────────────────────────

// One Crumbl-size cookie: a thick 20×9 disc — golden top face, darker baked
// edge showing its thickness, big chocolate chunks. (x, y) is its top-left.
function bigCookie(g, x, y, bites = []) {
  const rows = [
    '......TTTTTTTT......',
    '...TTTTKKTTTTTTTT...',
    '.TTTTTTKKTTTTKKTTTT.',
    'TTKKTTTTTTTTTKKTTTTT',
    'TTKKTTTTTKKTTTTTTKKT',
    'TTTTTTTTTKKTTTTTTKKT',
    'ETTTTKKTTTTTTTTTTTTE',
    '.EEETTTTTTTTTTTTEEE.',
    '...EEEEEEEEEEEEEE...',
  ];
  // `bites`: [cx, cy, r] circles in cookie-local cells, removed from the disc.
  const bitten = (c, r) => bites.some(([bx, by, br]) => (c - bx) ** 2 + (r - by) ** 2 <= br * br);
  stamp(g, x, y, rows.map((row, r) => [...row].map((ch, c) => bitten(c, r) ? '.' : ch).join('')),
    { T: C.tan, K: C.chip, E: C.cookie });
}

// Smaller view of the same cookie, riding out of the oven on the peel.
function cookieOut(g, x, y) {
  stamp(g, x, y, ['.TTKT.', 'TKTTTT', 'ETTTKE', '.EEEE.'], { T: C.tan, K: C.chip, E: C.cookie });
}

// Pink cookie box, left edge at x, bottom at row `base` (26 = on the
// baking counter, 36 = on the floor). `open` stands the lid up behind it.
function box(g, x, { open = false, base = 26 } = {}) {
  const o = base - 26;
  rect(g, x, 23 + o, x + 11, 26 + o, C.pink);
  rect(g, x, 26 + o, x + 11, 26 + o, C.pinkSh);
  if (open) {
    rect(g, x + 1, 23 + o, x + 10, 23 + o, C.pinkSh);  // inside rim
    rect(g, x, 17 + o, x + 11, 21 + o, C.pink);        // lid, standing up
    rect(g, x, 22 + o, x + 11, 22 + o, C.pinkSh);      // hinge
  } else {
    rect(g, x, 22 + o, x + 11, 22 + o, C.pinkSh);      // lid seam
    rect(g, x + 5, 23 + o, x + 6, 24 + o, C.ivory);    // sticker
  }
}

// Four-point sparkle, `size` 1 (dot) or 2 (plus).
function sparkle(g, x, y, size = 2, c = C.spark) {
  put(g, x, y, c);
  if (size > 1) { put(g, x - 1, y, c); put(g, x + 1, y, c); put(g, x, y - 1, c); put(g, x, y + 1, c); }
}

// ── Scene: frame collection + rendering ─────────────────────────────────────

// Parses [--out FILE] [--preview FILE] (default out: research/clawd-official/
// <gifName>), collects frames via frame(ms, draw), and renders on finish().
function scene(gifName) {
  const args = process.argv.slice(2);
  const opt = (k, def) => { const i = args.indexOf(k); return i >= 0 ? args[i + 1] : def; };
  const out = path.resolve(opt('--out',
    path.join(__dirname, '..', 'research', 'clawd-official', gifName)));
  const preview = opt('--preview', null);
  const frames = [];   // { g, ms }

  // Write each frame as a 55×37 PAM with alpha, then let ImageMagick point-
  // sample up to `scale` px/cell and assemble the GIF.
  function render(out, scale, background) {
    const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'clawd-art-'));
    try {
      const argv = ['-dispose', 'Background'];   // must precede the frames
      frames.forEach(({ g, ms }, i) => {
        const buf = Buffer.alloc(W * H * 4);
        for (let y = 0; y < H; y++)
          for (let x = 0; x < W; x++) {
            const c = g[y][x], o = (y * W + x) * 4;
            if (c) { buf[o] = c[0]; buf[o + 1] = c[1]; buf[o + 2] = c[2]; buf[o + 3] = 255; }
          }
        const hdr = `P7\nWIDTH ${W}\nHEIGHT ${H}\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n`;
        const f = path.join(tmp, `f_${String(i).padStart(3, '0')}.pam`);
        fs.writeFileSync(f, Buffer.concat([Buffer.from(hdr), buf]));
        argv.push('-delay', String(Math.round(ms / 10)), f);
      });
      const tail = ['-sample', `${scale * 100}%`];
      if (background) tail.push('-background', background, '-alpha', 'remove');
      execFileSync('magick', [...argv, ...tail, '-loop', '0', out]);
    } finally {
      fs.rmSync(tmp, { recursive: true, force: true });
    }
  }

  return {
    frame(ms, draw) { const g = blank(); draw(g); frames.push({ g, ms }); },
    finish() {
      render(out, 50, null);
      console.log(`Wrote ${out} (${frames.length} frames, ` +
        `${(frames.reduce((s, f) => s + f.ms, 0) / 1000).toFixed(1)} s)`);
      if (preview) { render(path.resolve(preview), 10, 'black'); console.log(`Wrote ${preview}`); }
    },
  };
}

module.exports = {
  W, H, C, blank, put, rect, line, stamp,
  clawdIdle, clawdArmsUp, legs, eyeHoles,
  bigCookie, cookieOut, box, sparkle, scene,
};

#include "splash.h"
#include "splash_animations.h"
#include "splash_geometry.h"
#include "theme.h"
#include "usage_rate.h"
#include "hal/board_caps.h"
#include "hal/display_hal.h"
#include <Arduino.h>
#include <string.h>
#include <esp_heap_caps.h>

// 60×60 stage. CELL sized so the canvas fits the smaller display dimension —
// the canvas is square and centered, so on portrait or letterboxed panels
// it leaves vertical margin rather than cropping. On PSRAM-less boards the
// buffer is rendered tiny (cell == 1) and LVGL scales it up to fill the panel;
// the geometry decision lives in splash_compute_geometry() (splash_geometry.h).
//
// Animations are stored as bounding-box crops of the official 55×37 art stage
// (see tools/convert_official_clawd.js); compose_stage() places the current
// frame centered on the 60×60 stage. The oversized stage leaves room to later
// translate animations across the screen (walks, lurking).
#define GRID         SPLASH_GRID
static int  cell      = 8;         // recomputed in splash_init()
static int  canvas_w  = GRID * 8;
static int  canvas_h  = GRID * 8;

// Splash background: true black (matches THEME_BG and palette index 0
// emitted by tools/convert_official_clawd.js). Used for the stage margins
// and as palette fallback.
#define COL_EMPTY    0x0000

LV_FONT_DECLARE(font_styrene_28);

static lv_obj_t *splash_container = NULL;
static lv_obj_t *canvas = NULL;
static lv_obj_t *label_status = NULL;     // shown only when no animations loaded
static uint16_t *canvas_buf = NULL;        // 480x480 RGB565 (PSRAM)

static uint16_t cur_anim = 0;
static uint16_t cur_frame = 0;
static uint32_t frame_started_ms = 0;
static uint32_t last_pick_ms = 0;
static bool active = false;

// While splash is showing, auto-cycle to the next animation in the current
// rate-driven group every this many ms.
#define SPLASH_ROTATE_INTERVAL_MS 20000

// Usage-rate animation groups: 4 groups × up to 5 animations each.
// Filled at init by matching literal names from splash_anims[].
// (jumping is the only unassigned animation — still reachable via splash_next.)
#define GROUP_COUNT 4
#define GROUP_MAX   5
static int8_t  group_lists[GROUP_COUNT][GROUP_MAX];
static uint8_t group_size[GROUP_COUNT] = {0};
static uint8_t group_rotation[GROUP_COUNT] = {0};

static const char* GROUP_NAMES[GROUP_COUNT][GROUP_MAX] = {
    // Group 0 — idle / sleepy (calm, investigative, plus the fan-made
    // cookie snack). Magnifier first: it's the boot pick, and lurking-first
    // would boot to a near-empty screen.
    { "magnifier", "walking", "pointing", "lurking", "eating" },
    // Group 1 — normal pace
    { "crab walking", "waving", "trumpet", "basketball" },
    // Group 2 — active (typing along with you)
    { "laptop", "dancing", "skateboard", "soccer" },
    // Group 3 — heavy burn (high-energy rides + the most exuberant jump,
    // plus the fan-made baking scene)
    { "racing car", "cloud", "sailing scene", "jumping happy", "baking" },
};

// Scratch stage: the current animation frame composed centered onto the full
// 60×60 grid (index 0 = background elsewhere). 3.6 KB of static RAM.
static uint8_t stage_cells[GRID * GRID];

// The official 55×37 art stage sits at a fixed anchor on the 60×60 grid, and
// every animation is placed at its authored stage offset (ox/oy) — never
// centered per-animation. All animations share one idle-Clawd position
// (x 15..38, y 21..36 in stage cells), so transitions between them are
// seamless; centering per-crop would make the still pose jump around.
#define STAGE_ANCHOR_X ((GRID - 55) / 2)
#define STAGE_ANCHOR_Y ((GRID - 37) / 2)

// ─── Playback: intro → loop → outro ─────────────────────────────────────────
// Every animation carries a loop region (converter-detected gait cycles and
// scene middles; whole file when nothing repeats). Playback holds the loop
// until released — walkers release on arrival at their target x, scenes after
// SCENE_LOOP_MS — then the outro (pack-away, gait exit) plays and the
// animation completes on its idle bookend. Rotation never hard-cuts: it
// releases the loop and switches after the outro, so transitions always
// happen from the shared idle pose.
static bool     pb_done = false;        // completed; holding idle frame 0
static bool     in_loop = false;
static bool     loop_release = false;
static uint32_t loop_entered_ms = 0;
static bool     pending_pick = false;   // rotate requested; honor at completion
#define SCENE_LOOP_MS 6000

// ─── Walk translation ────────────────────────────────────────────────────────
// The walk gaits animate in place; screen travel is ours, locked to the feet:
// per-frame movement equals the measured backward drift of the planted feet,
// so planted feet stay put on screen.
//   crab walking (8-frame scuttle loop [1..8]): surges of 1 cell entering
//     frames 4, 5, 8 and the cycle wrap — 4 cells / 640 ms (6.25 cells/s).
//   walking (5-frame waddle loop [2..6]): 1,1,1,1,2 cells → 6 cells / 450 ms
//     (~13.3 cells/s).
// walk_begin(target) plays intro → gait loop, clamps to land exactly on the
// target, then releases the loop so the gait exits and Clawd stands. When
// walking left the frame is mirrored (eyes lead); facing persists standing.
// DEMO: until the BLE-driven state machine exists, a choreography loops
// stand → right edge → off-screen left → re-enter home.
enum WalkKind { WALK_NONE, WALK_CRAB, WALK_FRONT };
static WalkKind walk_kind = WALK_NONE;
static bool    walk_active = false;
static int     walk_x = 0;         // stage x of the frame origin, may be < 0
static int     walk_dir = 0;       // -1 left, +1 right, 0 standing
static int     walk_target = 0;
static uint8_t walk_phase = 0;
static uint32_t walk_phase_started = 0;
static int     walk_home_x = 0;    // authored position to return to
static int     walk_face = +1;     // facing, kept while standing (-1 = left)

// Cells the body moves when the gait advances INTO `frame` (see banner).
static int walk_gait_cells_k(WalkKind kind, uint16_t frame, bool from_loop) {
    if (kind == WALK_CRAB) {
        if (frame == 1) return from_loop ? 1 : 0;     // cycle wrap, mid-surge
        return (frame == 4 || frame == 5 || frame == 8) ? 1 : 0;
    }
    if (kind == WALK_FRONT) {
        if (frame < 2 || frame > 6) return 0;         // idle / wind-up / outro
        if (frame == 2 && !from_loop) return 0;       // first plant
        return (frame == 6) ? 2 : 1;
    }
    return 0;
}
static int walk_gait_cells(uint16_t frame, bool from_loop) {
    return walk_gait_cells_k(walk_kind, frame, from_loop);
}

static void anim_reset(const splash_anim_def_t *a) {
    pb_done = false;
    in_loop = false;
    loop_release = false;
    pending_pick = false;
    walk_active = false;
    walk_kind = WALK_NONE;
    if (strcmp(a->name, "crab walking") == 0) walk_kind = WALK_CRAB;
    else if (strcmp(a->name, "walking") == 0) walk_kind = WALK_FRONT;
    else return;
    walk_active = true;
    walk_home_x = STAGE_ANCHOR_X + a->ox;
    walk_x = walk_home_x;
    walk_dir = 0;
    walk_face = +1;
    walk_phase = 0;
    walk_phase_started = millis();
    pb_done = true;    // walkers start standing; the choreography sets off
}

static const uint8_t* compose_stage(const splash_anim_def_t *a, uint16_t frame);
static void render_frame(const uint8_t *cells, const uint16_t *palette);

// ─── Asleep and dreaming (usage at its limit) ────────────────────────────────
// Out of usage, Clawd lies down and sleeps until the window resets. No
// official lying-down Clawd exists, so the pose is derived at boot from an
// official frame: the turned Clawd of the walk wind-up (walking frame 1),
// laid on his back with his head to the right, legs out to the left and his
// face to the sky, each eye closed to a line. Those eye lines and the three
// thought-bubble dots are the only authored cells.
//
// On the splash he sleeps at the bottom of the stage and dreams: a story of
// official clips plays above him, raised DREAM_SHIFT cells to make room.
// Three stories of about a minute each play in turn, with a short dreamless
// pause between them. The corner mascot shows the sleeping pose only.
#define SLEEP_MAX_CELLS (32 * 24)
static uint8_t  sleep_cells[SLEEP_MAX_CELLS];   // walking-palette codes
static const uint16_t sleep_holds[1] = { 1000 };
static splash_anim_def_t sleep_def;
static bool sleep_ok = false;      // pose built
static bool s_asleep = false;      // currently asleep
static int  still_cols = 0;        // size of the standing Clawd, cells: the
static int  still_rows = 0;        // corner slot the lying pose must fit

typedef struct { const char *anim; uint16_t ms; } dream_beat_t;
#define DREAM_STORIES    3
#define DREAM_MAX_BEATS  6
// A beat holds its clip's loop for `ms`, then lets the outro play. Every clip
// starts and ends on the shared idle pose, so beats cut together cleanly.
static const dream_beat_t DREAMS[DREAM_STORIES][DREAM_MAX_BEATS] = {
    // Game Day: walks out, plays, and celebrates the win.
    { {"walking", 5000}, {"soccer", 10000}, {"basketball", 10000},
      {"skateboard", 9000}, {"trumpet", 11000}, {"jumping happy", 8000} },
    // The Voyage: searches, sets sail, flies, races home, waves goodbye.
    { {"magnifier", 9000}, {"sailing scene", 13000}, {"cloud", 12000},
      {"racing car", 12000}, {"waving", 6000}, {NULL, 0} },
    // The Bakery: finishes work, bakes, eats, dances it off.
    { {"laptop", 12000}, {"baking", 15000}, {"eating", 13000},
      {"dancing", 13000}, {NULL, 0}, {NULL, 0} },
};
#define DREAM_PAUSE_MS   4000      // dreamless sleep between stories
#define DREAM_DOT_MS      350      // bubble dots appear one at a time
#define DREAM_SHIFT      (-15)     // scenes rise this many rows (tallest is 33)
#define DREAM_SLEEP_BOTTOM (GRID - 1)          // the row he lies on
#define DREAM_DOT_COLOR  0x8C51    // official steam gray #8B8B8B

static uint8_t  dr_story = 0;
static int8_t   dr_beat = -1;              // -1 = dreamless pause
static const splash_anim_def_t *dr_anim = NULL;
static uint16_t dr_frame = 0;
static uint32_t dr_frame_started = 0;
static uint32_t dr_beat_started = 0;
static uint32_t dr_story_started = 0;      // also the pause start
static uint8_t  dr_dots = 0;
static bool     dr_dirty = false;
static uint16_t dream_pal[SPLASH_PALETTE_SIZE];
static uint8_t  dream_map[SPLASH_PALETTE_SIZE];   // sleeper code -> dream_pal slot
static uint8_t  dream_dot_idx = 1;

static const splash_anim_def_t* find_anim(const char *n) {
    if (!n) return NULL;
    for (int i = 0; i < SPLASH_ANIM_COUNT; i++)
        if (strcmp(splash_anims[i].name, n) == 0) return &splash_anims[i];
    return NULL;
}

// Ink = the converter's eye color (near black), as opposed to body or shade.
static bool is_ink(uint16_t c) {
    return ((c >> 11) & 31) + ((c >> 6) & 31) + (c & 31) < 12;
}

static void build_sleep_pose(void) {
    const splash_anim_def_t *a = find_anim("walking");
    if (!a || a->frame_count == 0 || (int)a->w * a->h > SLEEP_MAX_CELLS) return;
    const int w = a->w, h = a->h;
    uint8_t up[SLEEP_MAX_CELLS];
    // Frame 1 is the walk wind-up: standing, turned to face right.
    memcpy(up, &a->frames[(size_t)(a->frame_count > 1 ? 1 : 0) * w * h], (size_t)w * h);
    #define UP_INK(X, Y) (up[(Y) * w + (X)] && up[(Y) * w + (X)] < SPLASH_PALETTE_SIZE && \
                          is_ink(a->palette[up[(Y) * w + (X)]]))

    // Close the eyes. Each eye is a small block of ink cells: fill it with
    // the body cell beside it and leave a line on its bottom row, one cell
    // longer on each side (where there is body to draw on).
    int x = 0;
    while (x < w) {
        int y0 = h, y1 = -1;
        for (int y = 0; y < h; y++)
            if (UP_INK(x, y)) { if (y < y0) y0 = y; if (y > y1) y1 = y; }
        if (y1 < 0) { x++; continue; }
        int x1 = x;                                   // extend the block right
        while (x1 + 1 < w) {
            bool more = false;
            for (int y = y0; y <= y1; y++) if (UP_INK(x1 + 1, y)) more = true;
            if (!more) break;
            x1++;
        }
        const uint8_t ink = up[y1 * w + x];
        uint8_t skin = (x > 0) ? up[y1 * w + x - 1] : 0;
        if (!skin && x1 + 1 < w) skin = up[y1 * w + x1 + 1];
        for (int yy = y0; yy <= y1; yy++)
            for (int xx = x; xx <= x1; xx++) up[yy * w + xx] = skin;
        for (int xx = x - 1; xx <= x1 + 1; xx++)
            if (xx >= 0 && xx < w && up[y1 * w + xx]) up[y1 * w + xx] = ink;
        x = x1 + 2;
    }
    #undef UP_INK

    // Occupied box of the standing pose.
    int r0 = h, r1 = -1, c0 = w, c1 = -1;
    for (int y = 0; y < h; y++)
        for (int xx = 0; xx < w; xx++)
            if (up[y * w + xx]) {
                if (y < r0) r0 = y;
                if (y > r1) r1 = y;
                if (xx < c0) c0 = xx;
                if (xx > c1) c1 = xx;
            }
    if (r1 < 0) return;
    const int bw = c1 - c0 + 1, bh = r1 - r0 + 1;
    still_cols = w;
    still_rows = bh;

    // Lay him down: head (top) to the right, legs to the left, and the side
    // he faces (right) to the sky. The lying sprite is bh wide and bw tall.
    for (int y = 0; y < bh; y++)
        for (int xx = 0; xx < bw; xx++)
            sleep_cells[(bw - 1 - xx) * bh + (bh - 1 - y)] = up[(r0 + y) * w + c0 + xx];

    sleep_def = *a;
    sleep_def.name = "asleep";
    sleep_def.w = (uint8_t)bh;
    sleep_def.h = (uint8_t)bw;
    sleep_def.frame_count = 1;
    sleep_def.loop_start = 0;
    sleep_def.loop_end = 0;
    sleep_def.frames = sleep_cells;
    sleep_def.holds = sleep_holds;
    sleep_ok = true;
}

// Px per cell for the lying pose in a corner slot sized for the standing
// Clawd at `px` per cell: lying he is taller, so he is drawn smaller.
static int sleep_slot_cell(int px) {
    int c = (still_rows * px) / sleep_def.h;
    return c < 1 ? 1 : c;
}

static uint8_t dream_pal_slot(uint16_t color, uint8_t used) {
    for (uint8_t i = 1; i < used; i++) if (dream_pal[i] == color) return i;
    if (used < SPLASH_PALETTE_SIZE) { dream_pal[used] = color; return used; }
    // Palette full: nearest existing color.
    uint8_t best = 1; int best_d = 1 << 30;
    for (uint8_t i = 1; i < SPLASH_PALETTE_SIZE; i++) {
        const int dr = ((dream_pal[i] >> 11) & 31) - ((color >> 11) & 31);
        const int dg = (((dream_pal[i] >> 5) & 63) - ((color >> 5) & 63)) / 2;
        const int db = (dream_pal[i] & 31) - (color & 31);
        const int d = dr * dr + dg * dg + db * db;
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

// The scene's palette plus the sleeper's colors and the bubble gray.
static void dream_set_palette(const splash_anim_def_t *scene) {
    const splash_anim_def_t *src = scene ? scene : &sleep_def;
    memcpy(dream_pal, src->palette, sizeof(dream_pal));
    uint8_t used = src->palette_count;
    if (used < 1) used = 1;
    if (used > SPLASH_PALETTE_SIZE) used = SPLASH_PALETTE_SIZE;
    for (int c = 0; c < SPLASH_PALETTE_SIZE; c++) {
        const uint16_t col = sleep_def.palette[c];
        // Closed eyes are background black, which every palette has at 0.
        if (c == 0 || c >= sleep_def.palette_count || is_ink(col)) { dream_map[c] = 0; continue; }
        dream_map[c] = dream_pal_slot(col, used);
        if (dream_map[c] >= used) used = dream_map[c] + 1;
    }
    dream_dot_idx = dream_pal_slot(DREAM_DOT_COLOR, used);
}

static const uint8_t* compose_dream(void) {
    memset(stage_cells, 0, sizeof(stage_cells));
    const int sw = sleep_def.w;
    const int sx = (GRID - sw) / 2;                       // sleeper, centered
    const int sh = sleep_def.h;
    const int sy = DREAM_SLEEP_BOTTOM - sh + 1;           // his top row

    if (dr_anim) {                                        // the dream scene
        const splash_anim_def_t *a = dr_anim;
        int ax = STAGE_ANCHOR_X + a->ox;                  // same edge snap as
        if (a->ox == 0)         ax = 0;                   // compose_stage
        if (a->ox + a->w == 55) ax = GRID - a->w;
        const int ay = STAGE_ANCHOR_Y + a->oy + DREAM_SHIFT;
        const uint8_t *src = &a->frames[(size_t)dr_frame * a->w * a->h];
        for (int r = 0; r < a->h; r++) {
            const int dy = ay + r;
            if (dy < 0 || dy >= sy) continue;             // never onto the sleeper
            int c0 = 0, c1 = a->w;
            if (ax + c0 < 0)    c0 = -ax;
            if (ax + c1 > GRID) c1 = GRID - ax;
            if (c0 < c1) memcpy(&stage_cells[dy * GRID + ax + c0], &src[r * a->w + c0], c1 - c0);
        }
        // Thought bubbles: 1, 2, 3-cell dots rising from his head to the
        // dream. Drawn only on empty cells, so a wide scene stays intact.
        static const int8_t DOTS[3][3] = {    // {dx from his head (right edge), dy from his top, size}
            { 1, 8, 1 }, { 3, 4, 2 }, { 6, -1, 3 },
        };
        for (int d = 0; d < dr_dots && d < 3; d++) {
            const int n = DOTS[d][2];
            const int x0 = sx + sw + DOTS[d][0], y0 = sy + DOTS[d][1];
            for (int yy = y0; yy < y0 + n; yy++)
                for (int xx = x0; xx < x0 + n; xx++)
                    if (yy >= 0 && yy < GRID && xx >= 0 && xx < GRID &&
                        !stage_cells[yy * GRID + xx])
                        stage_cells[yy * GRID + xx] = dream_dot_idx;
        }
    }

    for (int r = 0; r < sh; r++) {                        // the sleeper
        const int dy = sy + r;
        if (dy < 0 || dy >= GRID) continue;
        for (int c = 0; c < sw; c++) {
            const uint8_t code = sleep_cells[r * sw + c];
            if (code && code < SPLASH_PALETTE_SIZE)
                stage_cells[dy * GRID + sx + c] = dream_map[code];
        }
    }
    return stage_cells;
}

static void dream_force_repaint(void);

// Start beat `b` of the current story, skipping clips this build lacks.
// Past the last beat the story ends: dreamless pause, then the next story.
static void dream_start_beat(int b) {
    const uint32_t now = millis();
    dr_anim = NULL;
    while (b < DREAM_MAX_BEATS && DREAMS[dr_story][b].anim &&
           !(dr_anim = find_anim(DREAMS[dr_story][b].anim))) b++;
    if (!dr_anim) {
        dr_beat = -1;
        dr_story = (dr_story + 1) % DREAM_STORIES;
        dr_story_started = now;
        dr_dots = 0;
    } else {
        if (dr_beat < 0) { dr_story_started = now; dr_dots = 0; }
        dr_beat = (int8_t)b;
        dr_frame = 0;
        dr_frame_started = now;
        dr_beat_started = now;
    }
    dream_set_palette(dr_anim);
    dream_force_repaint();          // the palette changed under the same pointer
    dr_dirty = true;
}

static void dream_render(void) {
    dr_dirty = false;
    render_frame(compose_dream(), dream_pal);
}

static void dream_tick(uint32_t now) {
    if (dr_beat < 0) {                                  // dreamless pause
        if (now - dr_story_started >= DREAM_PAUSE_MS) dream_start_beat(0);
    } else {
        const splash_anim_def_t *a = dr_anim;
        uint8_t dots = (uint8_t)((now - dr_story_started) / DREAM_DOT_MS);
        if (dots > 3) dots = 3;
        if (dots != dr_dots) { dr_dots = dots; dr_dirty = true; }
        if (now - dr_frame_started >= a->holds[dr_frame]) {
            const bool hold = now - dr_beat_started < DREAMS[dr_story][dr_beat].ms;
            uint16_t next = dr_frame + 1;
            if (dr_frame == a->loop_end && hold) next = a->loop_start;
            if (next >= a->frame_count) {
                dream_start_beat(dr_beat + 1);          // clip done: next beat
            } else {
                dr_frame = next;
                dr_frame_started = now;
                dr_dirty = true;
            }
        }
    }
    if (dr_dirty) dream_render();
}

// Draw `a`'s first frame, or the dream while he is asleep. Every path that
// (re)starts an animation goes through here.
static void render_first(const splash_anim_def_t *a) {
    if (s_asleep && sleep_ok) { dream_render(); return; }
    render_frame(compose_stage(a, 0), a->palette);
}

// Start walking toward `target` (stage x of the frame origin).
static void walk_begin(int target) {
    if (target == walk_x) return;          // already there; stay standing
    walk_target = target;
    walk_dir = (target > walk_x) ? +1 : -1;
    walk_face = walk_dir;
    cur_frame = 0;
    frame_started_ms = millis();
    pb_done = false;
    loop_release = false;
    in_loop = false;
}

// Demo choreography: advance phases whenever the current walk has completed.
static void walk_choreo(const splash_anim_def_t *a) {
    if (!pb_done) return;
    const uint32_t now = millis();
    switch (walk_phase) {
        case 0:  // standing at home
            if (now - walk_phase_started > 1200) { walk_phase = 1; walk_begin(GRID - a->w); }
            break;
        case 1:  // arrived at the right edge
            walk_phase = 2; walk_phase_started = now;
            break;
        case 2:  // standing at the edge
            if (now - walk_phase_started > 1200) { walk_phase = 3; walk_begin(-a->w); }
            break;
        case 3:  // fully off-screen left
            walk_phase = 4; walk_phase_started = now;
            break;
        case 4:  // hold off-screen (empty stage)
            if (now - walk_phase_started > 800) { walk_phase = 5; walk_begin(walk_home_x); }
            break;
        case 5:  // back home
            walk_phase = 0; walk_phase_started = now;
            break;
    }
}

static const uint8_t* compose_stage(const splash_anim_def_t *a, uint16_t frame) {
    memset(stage_cells, 0, sizeof(stage_cells));
    // Horizontal edge snap: art touching its canvas's left/right edge was
    // designed to hang off that edge (lurking peeks in from the left), so it
    // goes to the true screen edge instead of the anchored stage edge. Not
    // applied vertically — every animation touches the stage bottom, and
    // vertical placement should stay anchored (rounded panel corners).
    int ax = STAGE_ANCHOR_X + a->ox;
    if (a->ox == 0)           ax = 0;
    if (a->ox + a->w == 55)   ax = GRID - a->w;
    if (walk_active)          ax = walk_x;
    const bool mirror = walk_active && walk_face < 0;
    const int ay = STAGE_ANCHOR_Y + a->oy;
    const uint8_t *src = &a->frames[(size_t)frame * a->w * a->h];
    for (int r = 0; r < a->h; r++) {
        const int dy = ay + r;
        if (dy < 0 || dy >= GRID) continue;
        int c0 = 0, c1 = a->w;                 // clip for partial off-screen x
        if (ax + c0 < 0)     c0 = -ax;
        if (ax + c1 > GRID)  c1 = GRID - ax;
        if (c0 >= c1) continue;
        if (mirror) {
            for (int c = c0; c < c1; c++)
                stage_cells[dy * GRID + ax + c] = src[r * a->w + (a->w - 1 - c)];
        } else {
            memcpy(&stage_cells[dy * GRID + ax + c0], &src[r * a->w + c0], c1 - c0);
        }
    }
    return stage_cells;
}

static void resolve_group_lists(void) {
    for (int g = 0; g < GROUP_COUNT; g++) {
        group_size[g] = 0;
        for (int s = 0; s < GROUP_MAX; s++) {
            group_lists[g][s] = -1;
            const char* want = GROUP_NAMES[g][s];
            if (!want) continue;
            for (int i = 0; i < SPLASH_ANIM_COUNT; i++) {
                if (strcmp(splash_anims[i].name, want) == 0) {
                    group_lists[g][group_size[g]++] = (int8_t)i;
                    break;
                }
            }
        }
    }
}

static uint16_t *row_buf = NULL;   // scratch row, sized to canvas_w (PSRAM path)

// ─── Two render paths ────────────────────────────────────────────────────────
// PSRAM boards (S3) draw the pixel art into an LVGL canvas at native size and
// let LVGL flush it — they have the RAM and cores to spare, no transform needed.
//
// PSRAM-less boards (C6) can't hold a 480×480 canvas. The prior approach (tiny
// 20×20 canvas + LVGL image-scale) made LVGL software-transform the whole
// upscaled frame on every redraw — measured ~0.76 µs/output-px, i.e. 100–220 ms
// per frame on the single-core C6, and partial invalidation of a transformed
// image both fails to clip the transform and smears. Instead we upscale the
// stage cells ourselves with trivial nearest-neighbour replication and push only
// the *changed* cells straight to the panel via the display HAL, bypassing LVGL.
// That removes the transform cost (leaving just the QSPI flush) and the
// dirty-rect is exact, so no smearing.
#ifndef BOARD_HAS_PSRAM
#  define SPLASH_DIRECT_DRAW 1
#else
#  define SPLASH_DIRECT_DRAW 0
#endif

#if SPLASH_DIRECT_DRAW
static uint16_t*       strip_buf = NULL;   // one grid-row band: (GRID*scr_cell)×scr_cell
static int             scr_cell  = 24;     // on-screen px per grid cell
static int             scr_offx  = 0;      // centering offsets (square art on panel)
static int             scr_offy  = 0;
static uint8_t         prev_cells[GRID * GRID];
static const uint16_t* prev_palette = NULL;
static bool            prev_valid   = false;
static bool            force_full   = false;  // repaint everything on the next render
static bool            covered      = false;  // an LVGL overlay is on top: don't draw

// Upscale grid cells [gx0..gx1]×[gy0..gy1] and push them to the panel, one
// grid-row band at a time so the scratch buffer stays (GRID*scr_cell × scr_cell).
static void blit_cells(const uint8_t* cells, const uint16_t* palette,
                       int gx0, int gy0, int gx1, int gy1) {
    if (!strip_buf) return;
    const int spc = scr_cell;
    const int bw  = (gx1 - gx0 + 1) * spc;          // band width, px
    const int px  = scr_offx + gx0 * spc;
    for (int gy = gy0; gy <= gy1; gy++) {
        for (int gx = gx0; gx <= gx1; gx++) {       // expand one source row across
            uint8_t code = cells[gy * GRID + gx];
            uint16_t color = (palette && code < SPLASH_PALETTE_SIZE) ? palette[code] : COL_EMPTY;
            uint16_t* p = &strip_buf[(gx - gx0) * spc];
            for (int i = 0; i < spc; i++) p[i] = color;
        }
        for (int dy = 1; dy < spc; dy++)             // replicate that row down
            memcpy(&strip_buf[dy * bw], strip_buf, bw * 2);
        display_hal_draw_bitmap(px, scr_offy + gy * spc, bw, spc, strip_buf);
    }
}

static void render_frame(const uint8_t *cells, const uint16_t *palette) {
    if (!strip_buf) return;
    if (!active) return;          // never draw to the panel while not shown
    if (covered) return;          // would paint over the overlay
    bool full = force_full || !prev_valid || palette != prev_palette;
    force_full = false;

    int gx0 = 0, gy0 = 0, gx1 = GRID - 1, gy1 = GRID - 1;
    if (!full) {                                     // bounding box of changed cells
        gx0 = GRID; gy0 = GRID; gx1 = -1; gy1 = -1;
        for (int gy = 0; gy < GRID; gy++)
            for (int gx = 0; gx < GRID; gx++)
                if (cells[gy * GRID + gx] != prev_cells[gy * GRID + gx]) {
                    if (gx < gx0) gx0 = gx;
                    if (gx > gx1) gx1 = gx;
                    if (gy < gy0) gy0 = gy;
                    if (gy > gy1) gy1 = gy;
                }
        if (gx1 < 0) return;                         // identical frame, nothing to do
    }

    blit_cells(cells, palette, gx0, gy0, gx1, gy1);

    memcpy(prev_cells, cells, GRID * GRID);
    prev_palette = palette;
    prev_valid   = true;
}

#else  // ── PSRAM: LVGL canvas render (unchanged) ──

static void render_frame(const uint8_t *cells, const uint16_t *palette) {
    if (!row_buf || !canvas_buf) return;
    for (int gy = 0; gy < GRID; gy++) {
        for (int gx = 0; gx < GRID; gx++) {
            uint8_t code = cells[gy * GRID + gx];
            uint16_t color = (palette && code < SPLASH_PALETTE_SIZE) ? palette[code] : COL_EMPTY;
            uint16_t *p = &row_buf[gx * cell];
            for (int i = 0; i < cell; i++) p[i] = color;
        }
        for (int dy = 0; dy < cell; dy++) {
            memcpy(&canvas_buf[(gy * cell + dy) * canvas_w], row_buf, canvas_w * 2);
        }
    }
    if (canvas) lv_obj_invalidate(canvas);
}
#endif

static void dream_force_repaint(void) {
#if SPLASH_DIRECT_DRAW
    force_full = true;
#endif
}

// ---- Mini creature: a small animated creature for embedding in other screens
//      (e.g. the idle "sleeping" indicator). Self-contained — its own canvas and
//      buffer, independent of the full-screen splash above. ----
static lv_obj_t  *mini_canvas = NULL;
static uint16_t  *mini_buf = NULL;
static int        mini_cell = 0;
static int        mini_w = 0;      // canvas px, mini_anim->w * mini_cell
static int        mini_h = 0;
static const splash_anim_def_t *mini_anim = NULL;
static uint16_t   mini_frame = 0;
static uint32_t   mini_started = 0;

static void mini_render(void) {
    if (!mini_buf || !mini_anim) return;
    const int aw = mini_anim->w, ah = mini_anim->h;
    const uint8_t *cells = &mini_anim->frames[(size_t)mini_frame * aw * ah];
    const uint16_t *pal = mini_anim->palette;
    for (int gy = 0; gy < ah; gy++) {
        for (int gx = 0; gx < aw; gx++) {
            uint8_t code = cells[gy * aw + gx];
            uint16_t color = (pal && code < SPLASH_PALETTE_SIZE) ? pal[code] : COL_EMPTY;
            for (int dy = 0; dy < mini_cell; dy++) {
                uint16_t *dst = &mini_buf[(gy * mini_cell + dy) * mini_w + gx * mini_cell];
                for (int dx = 0; dx < mini_cell; dx++) dst[dx] = color;
            }
        }
    }
    if (mini_canvas) lv_obj_invalidate(mini_canvas);
}

lv_obj_t* splash_mini_create(lv_obj_t *parent, const char *anim_name, int px) {
    mini_anim = NULL;
    for (int i = 0; i < SPLASH_ANIM_COUNT; i++) {
        if (strcmp(splash_anims[i].name, anim_name) == 0) { mini_anim = &splash_anims[i]; break; }
    }
    if (!mini_anim) return NULL;
    const int amax = (mini_anim->w > mini_anim->h) ? mini_anim->w : mini_anim->h;
    mini_cell = px / amax;
    if (mini_cell < 1) mini_cell = 1;
    mini_w = mini_anim->w * mini_cell;
    mini_h = mini_anim->h * mini_cell;
#ifdef BOARD_HAS_PSRAM
    const uint32_t caps = MALLOC_CAP_SPIRAM;
#else
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
#endif
    mini_buf = (uint16_t*)heap_caps_malloc(mini_w * mini_h * 2, caps);
    if (!mini_buf) return NULL;
    mini_canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(mini_canvas, mini_buf, mini_w, mini_h, LV_COLOR_FORMAT_RGB565);
    mini_frame = 0;
    mini_started = millis();
    mini_render();
    return mini_canvas;
}

void splash_mini_tick(void) {
    if (!mini_buf || !mini_anim || mini_anim->frame_count == 0) return;
    if (millis() - mini_started < mini_anim->holds[mini_frame]) return;
    mini_started = millis();
    mini_frame = (mini_frame + 1) % mini_anim->frame_count;
    mini_render();
}

// ─── Corner mascot (usage screen) ────────────────────────────────────────────
// The corner logo slot, alive: the still Clawd idles, occasionally does a
// small act (waving, dancing, pointing) in place, and every few acts walks
// off the left edge, does the full-size lurking animation over the screen,
// and walks back into the slot. PSRAM boards only (ui.cpp falls back to the
// static clawd_still.h icon on the C6); driven by splash_mascot_tick() from
// the main loop, independent of the splash screen itself.
static lv_obj_t *mas_img = NULL;
static lv_obj_t *mas_lurk_img = NULL;
static uint8_t  *mas_buf = NULL;       // planar RGB565A8, sized for largest act
static uint8_t  *mas_lurk_buf = NULL;
static lv_image_dsc_t mas_dsc, mas_lurk_dsc;
static int  mas_cell = 3;
static int  mas_slot_x = 0;            // px of the slot (walk-in target)
static int  mas_feet_y = 0;            // px feet line (all art is bottom-anchored)
static int  mas_lurk_cell = 8;
static int  mas_screen_w = 480;
static bool mas_visible = false;

enum MasMode { MAS_STILL, MAS_ACT, MAS_WALK_OFF, MAS_LURK, MAS_WALK_IN, MAS_ASLEEP };
static MasMode mas_mode = MAS_STILL;
static const splash_anim_def_t *mas_anim = NULL;
static uint16_t mas_frame = 0;
static uint32_t mas_frame_started = 0;
static uint32_t mas_mode_started = 0;
static int  mas_x = 0;                 // widget x, px (may be off-screen)
static int  mas_face = +1;
static uint8_t mas_act_idx = 0;
static bool mas_from_loop = false;

// The corner mascot mirrors the splash's excitement: per usage-rate group,
// how long he idles between acts and which acts he does. "lurking" means the
// walk-off / full-size-lurk / walk-back trip. Acts must fit the 28×21-cell
// buffer (jumps are too tall for the corner).
static const char* MAS_ACTS_BY_RATE[4][4] = {
    { "pointing", "lurking", NULL,       NULL      },   // idle: sparse, sneaky
    { "waving",   "lurking", "pointing", NULL      },   // normal
    { "waving",   "dancing", "lurking",  NULL      },   // active
    { "dancing",  "waving",  "dancing",  "lurking" },   // heavy: can't sit still
};
static const uint16_t MAS_STILL_MS_BY_RATE[4] = { 10000, 7000, 5000, 3500 };

static const splash_anim_def_t* anim_by_name(const char *n) {
    for (int i = 0; i < SPLASH_ANIM_COUNT; i++)
        if (strcmp(splash_anims[i].name, n) == 0) return &splash_anims[i];
    return NULL;
}

// Render one frame into a planar RGB565A8 image (alpha 0 outside the art) and
// anchor the widget on the shared feet line.
static void mas_render(const splash_anim_def_t *a, uint16_t frame, bool mirror,
                       lv_image_dsc_t *dsc, uint8_t *buf, lv_obj_t *img,
                       int cell, int x, int feet_y) {
    const int w = a->w * cell, h = a->h * cell;
    uint16_t *color = (uint16_t*)buf;
    uint8_t  *alpha = buf + (size_t)w * h * 2;
    const uint8_t *src = &a->frames[(size_t)frame * a->w * a->h];
    for (int gy = 0; gy < a->h; gy++) {
        for (int gx = 0; gx < a->w; gx++) {
            uint8_t code = src[gy * a->w + (mirror ? a->w - 1 - gx : gx)];
            uint16_t c = (code && code < SPLASH_PALETTE_SIZE) ? a->palette[code] : 0;
            uint8_t  al = code ? 255 : 0;
            for (int dy = 0; dy < cell; dy++) {
                uint16_t *cp = &color[(gy * cell + dy) * w + gx * cell];
                uint8_t  *ap = &alpha[(gy * cell + dy) * w + gx * cell];
                for (int dx = 0; dx < cell; dx++) { cp[dx] = c; ap[dx] = al; }
            }
        }
    }
    dsc->header.w = w;
    dsc->header.h = h;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565A8;
    dsc->header.stride = w * 2;
    dsc->data = buf;
    dsc->data_size = (size_t)w * h * 3;
    lv_image_set_src(img, dsc);
    lv_obj_set_pos(img, x, feet_y - h);
    lv_obj_invalidate(img);
}

static void mas_show_still(void) {
    mas_anim = anim_by_name("walking");     // frame 0 == the official still pose
    mas_frame = 0;
    mas_mode = MAS_STILL;
    mas_mode_started = millis();
    mas_x = mas_slot_x;
    mas_face = +1;
    if (mas_anim)
        mas_render(mas_anim, 0, false, &mas_dsc, mas_buf, mas_img,
                   mas_cell, mas_x, mas_feet_y);
}

// Usage at its limit: he sleeps in the slot until splash_set_asleep(false).
static void mas_show_asleep(void) {
    if (!mas_img || !sleep_ok) return;
    if (mas_lurk_img) lv_obj_add_flag(mas_lurk_img, LV_OBJ_FLAG_HIDDEN);   // mid-trip
    if (mas_visible)  lv_obj_clear_flag(mas_img, LV_OBJ_FLAG_HIDDEN);
    mas_anim = &sleep_def;
    mas_frame = 0;
    mas_mode = MAS_ASLEEP;
    mas_mode_started = millis();
    mas_x = mas_slot_x;
    mas_face = +1;
    mas_render(&sleep_def, 0, false, &mas_dsc, mas_buf, mas_img,
               sleep_slot_cell(mas_cell), mas_x, mas_feet_y);
}

lv_obj_t* splash_mascot_create(lv_obj_t *parent, int slot_x, int feet_y, int cell) {
    mas_cell = cell;
    mas_slot_x = slot_x;
    mas_feet_y = feet_y;
    mas_screen_w = board_caps().width;
    // Buffer for the largest act bbox (pointing, 28×21 cells).
    const size_t mas_bytes = (size_t)(28 * cell) * (21 * cell) * 3;
    const splash_anim_def_t *lurk = anim_by_name("lurking");
    const BoardCaps& c = board_caps();
    int mind = (c.width < c.height) ? c.width : c.height;
    mas_lurk_cell = mind / SPLASH_GRID;
    if (mas_lurk_cell < 1) mas_lurk_cell = 1;
    const size_t lurk_bytes = lurk ?
        (size_t)(lurk->w * mas_lurk_cell) * (lurk->h * mas_lurk_cell) * 3 : 0;
    mas_buf      = (uint8_t*)heap_caps_malloc(mas_bytes,  MALLOC_CAP_SPIRAM);
    mas_lurk_buf = lurk_bytes ? (uint8_t*)heap_caps_malloc(lurk_bytes, MALLOC_CAP_SPIRAM) : NULL;
    if (!mas_buf) return NULL;
    mas_img = lv_image_create(parent);
    if (mas_lurk_buf) {
        mas_lurk_img = lv_image_create(parent);
        lv_obj_add_flag(mas_lurk_img, LV_OBJ_FLAG_HIDDEN);
    }
    mas_show_still();
    return mas_img;
}

void splash_mascot_set_visible(bool v) {
    mas_visible = v;
    if (!mas_img) return;
    if (v) {
        lv_obj_clear_flag(mas_img, LV_OBJ_FLAG_HIDDEN);
        // The mascot walks over everything — keep him above later-created
        // siblings (battery icon, labels) whenever he's shown.
        lv_obj_move_foreground(mas_img);
        if (mas_lurk_img) lv_obj_move_foreground(mas_lurk_img);
        if (s_asleep && sleep_ok) mas_show_asleep();
        else                   mas_show_still();   // restart clean at the slot
    } else {
        lv_obj_add_flag(mas_img, LV_OBJ_FLAG_HIDDEN);
        if (mas_lurk_img) lv_obj_add_flag(mas_lurk_img, LV_OBJ_FLAG_HIDDEN);
    }
}

void splash_mascot_tick(void) {
    if (!mas_img || !mas_visible || !mas_anim) return;
    if (mas_mode == MAS_ASLEEP) return;
    const uint32_t now = millis();

    if (mas_mode == MAS_STILL) {
        int g = usage_rate_group();
        if (g < 0 || g > 3) g = 0;
        if (now - mas_mode_started < MAS_STILL_MS_BY_RATE[g]) return;
        uint8_t count = 0;
        while (count < 4 && MAS_ACTS_BY_RATE[g][count]) count++;
        if (count == 0) { mas_mode_started = now; return; }
        const char *act = MAS_ACTS_BY_RATE[g][mas_act_idx++ % count];
        mas_frame = 0;
        mas_frame_started = now;
        mas_from_loop = false;
        if (strcmp(act, "lurking") == 0 && mas_lurk_img) {   // the lurk trip
            mas_anim = anim_by_name("walking");
            mas_face = -1;
            mas_mode = MAS_WALK_OFF;
        } else {
            const splash_anim_def_t *a = anim_by_name(act);
            if (!a) { mas_mode_started = now; return; }
            mas_anim = a;
            mas_face = +1;
            mas_mode = MAS_ACT;
        }
        return;
    }

    const splash_anim_def_t *a = mas_anim;
    if (now - mas_frame_started < a->holds[mas_frame]) return;
    mas_frame_started = now;

    uint16_t next = mas_frame + 1;
    const bool walking_mode = (mas_mode == MAS_WALK_OFF || mas_mode == MAS_WALK_IN);
    if (walking_mode && mas_frame == a->loop_end)
        next = a->loop_start;                       // walk: hold the gait loop

    if (next >= a->frame_count) {                   // act / lurk finished
        if (mas_mode == MAS_LURK) {
            lv_obj_add_flag(mas_lurk_img, LV_OBJ_FLAG_HIDDEN);
            mas_anim = anim_by_name("walking");
            mas_frame = 0;
            mas_from_loop = false;
            mas_face = -1;                          // he lurked on the right,
            mas_x = mas_screen_w;                   // so he re-enters from it
            mas_mode = MAS_WALK_IN;
            lv_obj_clear_flag(mas_img, LV_OBJ_FLAG_HIDDEN);
            return;
        }
        mas_show_still();                           // acts end on the idle pose
        return;
    }

    const bool from_loop = mas_from_loop;
    mas_frame = next;
    mas_from_loop = walking_mode &&
        mas_frame >= a->loop_start && mas_frame <= a->loop_end;

    if (walking_mode && mas_from_loop) {
        const int step = walk_gait_cells_k(WALK_FRONT, mas_frame, from_loop) * mas_cell;
        // Walk-off always exits left; walk-in heads toward the slot from
        // whichever side he's on (right, after the lurk trip).
        const int dir = (mas_mode == MAS_WALK_OFF) ? -1
                        : (mas_x < mas_slot_x ? +1 : -1);
        mas_face = (mas_mode == MAS_WALK_OFF) ? -1 : dir;
        mas_x += dir * step;
        if (mas_mode == MAS_WALK_OFF && mas_x <= -a->w * mas_cell) {
            // Fully off: hide the corner sprite, run the full-size lurk.
            lv_obj_add_flag(mas_img, LV_OBJ_FLAG_HIDDEN);
            const splash_anim_def_t *lurk = anim_by_name("lurking");
            if (lurk && mas_lurk_img && mas_lurk_buf) {
                mas_anim = lurk;
                mas_frame = 0;
                mas_mode = MAS_LURK;
                lv_obj_clear_flag(mas_lurk_img, LV_OBJ_FLAG_HIDDEN);
                lv_obj_move_foreground(mas_lurk_img);
                // He left stage left, so he peeks in from the RIGHT edge —
                // mirrored at render time (the art is authored left-edge).
                mas_render(lurk, 0, true, &mas_lurk_dsc, mas_lurk_buf,
                           mas_lurk_img, mas_lurk_cell,
                           mas_screen_w - lurk->w * mas_lurk_cell,
                           (STAGE_ANCHOR_Y + lurk->oy + lurk->h) * mas_lurk_cell);
            } else {
                mas_mode = MAS_WALK_IN;             // no lurk asset: turn back
                mas_face = +1;
            }
            return;
        }
        if (mas_mode == MAS_WALK_IN &&
            ((dir > 0 && mas_x >= mas_slot_x) || (dir < 0 && mas_x <= mas_slot_x))) {
            mas_show_still();                       // arrived: settle in the slot
            return;
        }
    }

    if (mas_mode == MAS_LURK) {
        mas_render(a, mas_frame, true, &mas_lurk_dsc, mas_lurk_buf, mas_lurk_img,
                   mas_lurk_cell, mas_screen_w - a->w * mas_lurk_cell,
                   (STAGE_ANCHOR_Y + a->oy + a->h) * mas_lurk_cell);
    } else {
        mas_render(a, mas_frame, mas_face < 0, &mas_dsc, mas_buf, mas_img,
                   mas_cell, mas_x, mas_feet_y);
    }
}

static void show_placeholder() {
    // Solid dark background + centered status label. On the direct-draw path
    // there's no canvas; the black container is the background and the LVGL
    // label shows over it.
#if !SPLASH_DIRECT_DRAW
    if (canvas_buf) {
        for (int i = 0; i < canvas_w * canvas_h; i++) canvas_buf[i] = COL_EMPTY;
    }
    if (canvas) lv_obj_invalidate(canvas);
#endif
    if (label_status) lv_obj_clear_flag(label_status, LV_OBJ_FLAG_HIDDEN);
}

void splash_init(lv_obj_t *parent) {
    const BoardCaps& c = board_caps();

    // Shared full-screen black container — the splash background.
    splash_container = lv_obj_create(parent);
    lv_obj_set_size(splash_container, c.width, c.height);
    lv_obj_set_pos(splash_container, 0, 0);
    lv_obj_set_style_bg_color(splash_container, THEME_BG, 0);
    lv_obj_set_style_bg_opa(splash_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(splash_container, 0, 0);
    lv_obj_set_style_pad_all(splash_container, 0, 0);
    lv_obj_clear_flag(splash_container, LV_OBJ_FLAG_SCROLLABLE);

#if SPLASH_DIRECT_DRAW
    // Direct-to-panel path (no PSRAM): no LVGL canvas. Compute on-screen cell
    // size + centering, and a scratch band buffer sized for one grid-row strip
    // across the square art (GRID*scr_cell × scr_cell). On the C6 that's
    // 480×24×2 ≈ 23 KB of internal SRAM.
    int mind = (c.width < c.height) ? c.width : c.height;
    scr_cell = mind / GRID;
    int side = GRID * scr_cell;
    scr_offx = (c.width  - side) / 2;
    scr_offy = (c.height - side) / 2;
    strip_buf = (uint16_t*)heap_caps_malloc((size_t)side * scr_cell * 2,
                                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!strip_buf) {
        Serial.println("splash: strip buffer alloc failed");
        return;
    }
#else
    // PSRAM path: render into an LVGL canvas at native size (no transform).
    SplashGeometry geo = splash_compute_geometry(c.width, c.height, true);
    cell                = geo.cell;
    canvas_w            = geo.canvas_dim;
    canvas_h            = geo.canvas_dim;
    const int img_scale = geo.scale;

    canvas_buf = (uint16_t*)heap_caps_malloc(canvas_w * canvas_h * 2, MALLOC_CAP_SPIRAM);
    row_buf    = (uint16_t*)heap_caps_malloc(canvas_w * 2,            MALLOC_CAP_SPIRAM);
    if (!canvas_buf || !row_buf) {
        Serial.println("splash: failed to alloc canvas buffer");
        return;
    }

    canvas = lv_canvas_create(splash_container);
    lv_canvas_set_buffer(canvas, canvas_buf, canvas_w, canvas_h, LV_COLOR_FORMAT_RGB565);
    if (img_scale != SPLASH_SCALE_UNITY) {
        lv_image_set_antialias(canvas, false);
        lv_image_set_pivot(canvas, canvas_w / 2, canvas_h / 2);
        lv_image_set_scale(canvas, img_scale);
    }
    lv_obj_center(canvas);
#endif

    // Placeholder label (visible only when no animations are loaded)
    label_status = lv_label_create(splash_container);
    lv_label_set_text(label_status,
        "no animations loaded\n\n"
        "run tools/convert_official_clawd.js");
    lv_obj_set_style_text_font(label_status, &font_styrene_28, 0);
    lv_obj_set_style_text_color(label_status, lv_color_hex(0xb0aea5), 0);
    lv_obj_set_style_text_align(label_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label_status);

    resolve_group_lists();
    build_sleep_pose();

    if (SPLASH_ANIM_COUNT == 0) {
        show_placeholder();
    } else {
        lv_obj_add_flag(label_status, LV_OBJ_FLAG_HIDDEN);
#if !SPLASH_DIRECT_DRAW
        // PSRAM path pre-renders frame 0 into the canvas buffer. The direct
        // path draws nothing here — render_frame() bails while inactive, so the
        // splash never paints to the panel before it's actually shown.
        const splash_anim_def_t *a = &splash_anims[0];
        render_frame(compose_stage(a, 0), a->palette);
#endif
        frame_started_ms = millis();
    }

    lv_obj_add_flag(splash_container, LV_OBJ_FLAG_HIDDEN);
}

void splash_tick(void) {
    if (!active || SPLASH_ANIM_COUNT == 0) return;
    const uint32_t now = millis();

    if (s_asleep && sleep_ok) {        // asleep: only the dream advances
#if SPLASH_DIRECT_DRAW
        if (force_full) dr_dirty = true;       // repaint after a (re)show
#endif
        dream_tick(now);
        return;
    }

#if SPLASH_DIRECT_DRAW
    // Deferred full repaint after a (re)show — runs now that LVGL has drawn the
    // black background this loop iteration.
    if (force_full) {
        const splash_anim_def_t *fa = &splash_anims[cur_anim];
        if (fa->frame_count) render_frame(compose_stage(fa, cur_frame), fa->palette);
    }
#endif

    const splash_anim_def_t *a = &splash_anims[cur_anim];
    if (a->frame_count == 0) return;

    if (walk_active) walk_choreo(a);

    // Scenes: hold the loop for SCENE_LOOP_MS, then let the outro play.
    if (!walk_active && in_loop && !loop_release &&
        now - loop_entered_ms >= SCENE_LOOP_MS)
        loop_release = true;

    // Auto-rotate — never a hard cut. Walkers switch only while standing at
    // home; everything else releases its loop and switches after the outro.
    if (now - last_pick_ms >= SPLASH_ROTATE_INTERVAL_MS) {
        if (walk_active) {
            if (walk_phase == 0 && pb_done) splash_pick_for_current_rate();
        } else {
            loop_release = true;
            pending_pick = true;
            last_pick_ms = now;    // don't re-fire while the outro plays
        }
    }

    if (pb_done) return;                       // holding the idle frame
    if (now - frame_started_ms < a->holds[cur_frame]) return;

    // Advance one frame through intro → loop → outro.
    const bool from_loop = in_loop;
    uint16_t next = cur_frame + 1;
    if (cur_frame == a->loop_end && !loop_release)
        next = a->loop_start;

    if (next >= a->frame_count) {              // completed the file
        if (pending_pick) {
            pending_pick = false;
            splash_pick_for_current_rate();
            return;
        }
        if (walk_active) {                     // walk finished: stand
            cur_frame = 0;
            frame_started_ms = now;
            pb_done = true;
            render_frame(compose_stage(a, 0), a->palette);
            return;
        }
        next = 0;                              // replay from the intro
        loop_release = false;
    }

    cur_frame = next;
    frame_started_ms = now;
    const bool now_in = cur_frame >= a->loop_start && cur_frame <= a->loop_end;
    if (now_in && !from_loop) loop_entered_ms = now;
    in_loop = now_in;

    // Walk translation, locked to gait frames; clamp to land exactly on the
    // target, then release the loop so the gait exits.
    if (walk_active && walk_dir != 0 && in_loop) {
        walk_x += walk_dir * walk_gait_cells(cur_frame, from_loop);
        if ((walk_dir > 0 && walk_x >= walk_target) ||
            (walk_dir < 0 && walk_x <= walk_target)) {
            walk_x = walk_target;
            walk_dir = 0;
            loop_release = true;
        }
    }

    render_frame(compose_stage(a, cur_frame), a->palette);
}

void splash_next(void) {
    if (SPLASH_ANIM_COUNT == 0) return;
    if (s_asleep && sleep_ok) {                // asleep: skip to the next dream
        if (dr_beat >= 0) dr_story = (dr_story + 1) % DREAM_STORIES;
        dr_beat = -1;
        dream_start_beat(0);
        return;
    }
    cur_anim = (cur_anim + 1) % SPLASH_ANIM_COUNT;
    cur_frame = 0;
    frame_started_ms = millis();
    last_pick_ms = frame_started_ms;
    const splash_anim_def_t *a = &splash_anims[cur_anim];
    anim_reset(a);
    render_first(a);
    Serial.printf("splash: -> %s\n", a->name);
}

void splash_pick_for_current_rate(void) {
    if (SPLASH_ANIM_COUNT == 0) return;
    int g = usage_rate_group();
    if (g < 0 || g >= GROUP_COUNT) g = 0;
    if (group_size[g] == 0) return;

    uint8_t slot = group_rotation[g] % group_size[g];
    group_rotation[g]++;
    int8_t idx = group_lists[g][slot];
    if (idx < 0) return;

    cur_anim = (uint16_t)idx;
    cur_frame = 0;
    frame_started_ms = millis();
    last_pick_ms = frame_started_ms;
    const splash_anim_def_t *a = &splash_anims[cur_anim];
    anim_reset(a);
    render_first(a);
}

void splash_set_asleep(bool asleep) {
    if (asleep == s_asleep) return;
    s_asleep = asleep;
    if (!sleep_ok) return;
    if (asleep) {
        dr_beat = -1;                            // doze off, then dream
        dr_anim = NULL;
        dr_dots = 0;
        dr_story_started = millis();
        dream_set_palette(NULL);
        dream_force_repaint();
        dr_dirty = true;                         // splash_tick paints him
        if (mas_img) mas_show_asleep();
    } else {
        if (active) splash_pick_for_current_rate();   // wake up
        if (mas_img) mas_show_still();
    }
}

const lv_image_dsc_t* splash_asleep_icon(int px_per_cell) {
    static lv_image_dsc_t dsc;
    static uint8_t *buf = NULL;
    static int built_cell = 0;
    if (!sleep_ok || px_per_cell < 1) return NULL;
    if (buf && built_cell == px_per_cell) return &dsc;
    // Same size as the standing still icon; he lies at its bottom left.
    const int w = still_cols * px_per_cell, h = still_rows * px_per_cell;
    const int pc = sleep_slot_cell(px_per_cell);
    const int top = h - sleep_def.h * pc;
    if (top < 0 || sleep_def.w * pc > w) return NULL;
    if (buf) { heap_caps_free(buf); buf = NULL; }
    buf = (uint8_t*)heap_caps_malloc((size_t)w * h * 3, MALLOC_CAP_8BIT);
    if (!buf) return NULL;
    uint16_t *color = (uint16_t*)buf;
    uint8_t  *alpha = buf + (size_t)w * h * 2;
    memset(buf, 0, (size_t)w * h * 3);
    for (int gy = 0; gy < sleep_def.h; gy++) {
        for (int gx = 0; gx < sleep_def.w; gx++) {
            uint8_t code = sleep_cells[gy * sleep_def.w + gx];
            if (!code) continue;
            uint16_t c = (code < SPLASH_PALETTE_SIZE) ? sleep_def.palette[code] : 0;
            for (int dy = 0; dy < pc; dy++)
                for (int dx = 0; dx < pc; dx++) {
                    const int i = (top + gy * pc + dy) * w + gx * pc + dx;
                    color[i] = c;
                    alpha[i] = 255;
                }
        }
    }
    memset(&dsc, 0, sizeof(dsc));
    dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc.header.w = w;
    dsc.header.h = h;
    dsc.header.cf = LV_COLOR_FORMAT_RGB565A8;
    dsc.header.stride = w * 2;
    dsc.data = buf;
    dsc.data_size = (size_t)w * h * 3;
    built_cell = px_per_cell;
    return &dsc;
}

bool splash_is_active(void) { return active; }

void splash_show(void) {
    splash_pick_for_current_rate();   // select animation; direct path defers the draw
    if (splash_container) lv_obj_clear_flag(splash_container, LV_OBJ_FLAG_HIDDEN);
    active = true;
#if SPLASH_DIRECT_DRAW
    // LVGL fills the container black once on unhide; that would erase a creature
    // drawn now. Defer the full repaint to the next splash_tick(), which runs
    // after lv_timer_handler() in the main loop.
    force_full = true;
#endif
}

void splash_set_covered(bool c) {
#if SPLASH_DIRECT_DRAW
    if (covered && !c) force_full = true;   // LVGL redraws black first; repaint after
    covered = c;
#else
    (void)c;                                 // canvas path: LVGL layers it correctly
#endif
}

void splash_hide(void) {
    if (splash_container) lv_obj_add_flag(splash_container, LV_OBJ_FLAG_HIDDEN);
    active = false;
}

lv_obj_t* splash_get_root(void) {
    return splash_container;
}

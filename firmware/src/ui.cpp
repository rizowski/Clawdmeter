#include "ui.h"
#include "splash.h"
#include <lvgl.h>
#include <time.h>
#include "logo.h"
#include "clawd_still.h"
#include "icons.h"
#include "hal/board_caps.h"
#include "usage_rate.h"

// Custom fonts (scaled for 314 PPI, ~1.9x from original 165 PPI)
LV_FONT_DECLARE(font_tiempos_56);
LV_FONT_DECLARE(font_tiempos_34);
LV_FONT_DECLARE(font_styrene_48);
LV_FONT_DECLARE(font_styrene_28);
LV_FONT_DECLARE(font_styrene_24);
LV_FONT_DECLARE(font_styrene_20);
LV_FONT_DECLARE(font_styrene_16);
LV_FONT_DECLARE(font_styrene_14);
LV_FONT_DECLARE(font_styrene_12);
LV_FONT_DECLARE(font_mono_32);
LV_FONT_DECLARE(font_mono_18);

// Layout values computed from the active board's geometry. Populated once
// in ui_init() and treated as const for the rest of the program. Adding a
// new display size means extending compute_layout() with another
// breakpoint — never editing the screen-builder functions below.
struct Layout {
    int16_t scr_w, scr_h;
    int16_t margin;
    int16_t title_y;
    int16_t content_y;
    int16_t content_w;

    // Usage screen — top row (Current + Status, side by side) and a
    // full-width bottom row (Weekly), so the two heights/offsets diverge.
    int16_t usage_top_h;
    int16_t usage_bottom_h;
    int16_t usage_panel_gap;      // gap between top tiles, and between rows
    int16_t usage_top_bar_y;
    int16_t usage_top_reset_y;
    int16_t usage_bottom_bar_y;
    int16_t usage_bottom_reset_y;
    int16_t bar_h;
    int16_t panel_pad_x, panel_pad_y;
    int16_t pill_pad_x, pill_pad_y;
    const lv_font_t* title_font;     // screen title / clock
    const lv_font_t* pct_font;       // big percentage number
    const lv_font_t* ent_pct_font;   // enterprise spending number
    const lv_font_t* pill_font;      // "Current" / "Weekly" pill
    const lv_font_t* reset_font;     // "Resets in ..." line
    const lv_font_t* pace_font;      // enterprise "Under/On/Over pace" line
    const lv_font_t* anim_font;      // animated status line
    int16_t anim_y;                  // status line offset from bottom
    bool    small_icons;             // 40px logo + 24px battery (vs 80/48) on small screens
    int16_t title_nudge;             // title x-shift balancing the corner logo
    int16_t logo_y;                  // logo top edge
    int16_t batt_y;                  // battery icon top edge
    int16_t batt_w;                  // battery icon width, for position math

    // Pairing hint / idle screen
    int16_t pair_y1, pair_y2, pair_y3;
    int16_t idle_px;                 // sleeping-creature size on the idle screen

    // Bluetooth screen
    int16_t bt_info_panel_h;
    int16_t bt_reset_zone_h;
    const lv_font_t* bt_title_font;
    const lv_font_t* bt_status_font;
    const lv_font_t* bt_device_font;
    const lv_font_t* bt_credit_1_font;
    const lv_font_t* bt_credit_2_font;
};
static Layout L = {};

// Pick layout values from the active board's pixel dimensions. The two
// existing boards happen to land on the two breakpoints below; new ports
// inherit the closer one — visually OK, may need a polish pass for
// pixel-perfect alignment but never blocks the port from booting.
static void compute_layout(const BoardCaps& c) {
    L.scr_w = c.width;
    L.scr_h = c.height;
    L.margin = 20;
    L.title_y = 30;

    // Values shared by the two original breakpoints; the small branch below
    // overrides them wholesale.
    L.bar_h = 24;
    L.panel_pad_x = 16;
    L.panel_pad_y = 12;
    L.pill_pad_x = 18;
    L.pill_pad_y = 6;
    L.title_font   = &font_tiempos_56;
    L.pct_font     = &font_styrene_48;
    L.ent_pct_font = &font_tiempos_56;
    L.pill_font    = &font_styrene_28;
    L.reset_font   = &font_styrene_28;
    L.pace_font    = &font_styrene_16;
    L.anim_font    = &font_mono_32;
    L.anim_y = -15;
    L.small_icons = false;
    L.title_nudge = 16;
    L.logo_y = L.title_y - 10;
    L.batt_y = L.title_y;
    L.batt_w = ICON_BATTERY_W;
    L.pair_y1 = 40;
    L.pair_y2 = 120;
    L.pair_y3 = 160;
    L.idle_px = 160;

    if (c.height >= 460) {
        // Large layout — tuned for 480x480 (AMOLED-2.16).
        L.content_y = 100;
        L.usage_top_h = 130;
        L.usage_bottom_h = 170;
        L.usage_panel_gap = 16;
        L.usage_top_bar_y = 47;
        L.usage_top_reset_y = 78;
        L.usage_bottom_bar_y = 65;
        L.usage_bottom_reset_y = 109;
        L.bt_info_panel_h = 160;
        L.bt_reset_zone_h = 110;
        L.bt_title_font    = &font_tiempos_56;
        L.bt_status_font   = &font_styrene_48;
        L.bt_device_font   = &font_styrene_28;
        L.bt_credit_1_font = &font_styrene_24;
        L.bt_credit_2_font = &font_styrene_20;
    } else if (c.height >= 300) {
        // Compact layout — tuned for 368x448 (AMOLED-1.8).
        L.content_y = 85;
        // Same fonts as large, so the top tile needs large's vertical rhythm:
        // styrene_48 digits end at y 40, bar (+4 px marker) from 47, reset row
        // (styrene_28, 30 px) from 78 → 108 + 2×12 padding.
        L.usage_top_h = 132;
        L.usage_bottom_h = 160;
        L.usage_panel_gap = 12;
        L.usage_top_bar_y = 47;
        L.usage_top_reset_y = 78;
        L.usage_bottom_bar_y = 62;
        L.usage_bottom_reset_y = 100;
        L.bt_info_panel_h = 140;
        L.bt_reset_zone_h = 90;
        // 368 px is too narrow for styrene_28 rows: "On pace - Resets Aug 15"
        // clipped and the elapsed "/75%" ran into the pill.
        L.pill_font  = &font_styrene_24;
        L.reset_font = &font_styrene_24;
        L.bt_title_font    = &font_tiempos_34;
        L.bt_status_font   = &font_styrene_28;
        L.bt_device_font   = &font_styrene_20;
        L.bt_credit_1_font = &font_styrene_16;
        L.bt_credit_2_font = &font_styrene_14;
    } else {
        // Small layout — tuned for 240x240 (LCD-1.54 and similar square TFTs).
        // Everything shrinks: fonts two steps down, panels ~half height, and
        // the corner logo/battery switch to the 40px/24px small assets.
        L.margin = 8;
        L.title_y = 4;
        L.content_y = 44;
        // Top tile stacks pct (styrene_24, 25 px) → bar (12 px, marker
        // overhangs 4 px each side) → reset/rate row (14 px), no overlap.
        L.usage_top_h = 68;
        L.usage_bottom_h = 92;
        L.usage_panel_gap = 6;
        L.usage_top_bar_y = 24;
        L.usage_top_reset_y = 41;
        L.usage_bottom_bar_y = 39;
        L.usage_bottom_reset_y = 59;
        L.bar_h = 12;
        L.panel_pad_x = 10;
        L.panel_pad_y = 6;
        L.pill_pad_x = 8;
        L.pill_pad_y = 2;
        L.title_font   = &font_tiempos_34;
        L.pct_font     = &font_styrene_24;
        // Tiempos 34's digits reach y 28 and would sit on the bar at y 24;
        // Styrene 24 (the normal pct font) clears it.
        L.ent_pct_font = &font_styrene_24;
        L.pill_font    = &font_styrene_14;
        L.reset_font   = &font_styrene_14;
        L.pace_font    = &font_styrene_12;
        L.anim_font    = &font_mono_18;
        // Center the status line in the strip below the weekly panel; flush
        // against the bottom edge it reads as unevenly spaced.
        L.anim_y = -10;
        L.small_icons = true;
        L.title_nudge = 8;
        L.logo_y = 2;
        L.batt_y = 10;
        L.batt_w = ICON_BATTERY_SMALL_W;
        L.pair_y1 = 12;
        L.pair_y2 = 56;
        L.pair_y3 = 80;
        L.idle_px = 96;
        L.bt_info_panel_h = 90;
        L.bt_reset_zone_h = 60;
        L.bt_title_font    = &font_tiempos_34;
        L.bt_status_font   = &font_styrene_20;
        L.bt_device_font   = &font_styrene_14;
        L.bt_credit_1_font = &font_styrene_12;
        L.bt_credit_2_font = &font_styrene_12;
    }

    L.content_w = L.scr_w - 2 * L.margin;
}

// Anthropic brand palette — design tokens live in theme.h
#include "theme.h"
#define COL_BG        THEME_BG
#define COL_PANEL     THEME_PANEL
#define COL_TEXT      THEME_TEXT
#define COL_DIM       THEME_DIM
#define COL_ACCENT    THEME_ACCENT
#define COL_GREEN     THEME_GREEN
#define COL_AMBER     THEME_AMBER
#define COL_RED       THEME_RED
#define COL_BAR_BG    THEME_BAR_BG

// ---- Usage screen widgets (single non-splash view) ----
static lv_obj_t* usage_container;
static lv_obj_t* lbl_title;
// Clock fed by the daemon: base epoch (local wall-clock seconds) + the lv_tick at
// which it landed, so the title ticks forward locally between 60s payloads.
static long     clock_base_epoch = 0;
static uint32_t clock_base_ms = 0;
static int      clock_fmt = 24;   // 12 or 24, set from the daemon payload
static int      clock_last_min = -1;   // last rendered minute; avoids redrawing the title every tick
static lv_obj_t* usage_group;   // the two usage panels — shown when connected
static lv_obj_t* pair_group;    // pairing hint — shown when disconnected
static lv_obj_t* bar_session;
static lv_obj_t* lbl_session_pct;
static lv_obj_t* lbl_session_label;
static lv_obj_t* lbl_session_reset;
static lv_obj_t* bar_weekly;
static lv_obj_t* lbl_weekly_pct;
static lv_obj_t* lbl_weekly_label;
static lv_obj_t* lbl_weekly_reset;
static lv_obj_t* panel_session = nullptr;
static lv_obj_t* panel_weekly = nullptr;
static lv_obj_t* lbl_rate = nullptr;         // "Active 0.25 %/min", in the Current tile

// Dim "/62%" — how far into the window you are, as a number. The tick on the
// bar is the same value; the statusline shows only the number, so show both.
static lv_obj_t* lbl_session_elapsed = nullptr;
static lv_obj_t* lbl_weekly_elapsed = nullptr;

// Pace markers — a tick on each bar at the "you should be here" position.
static lv_obj_t* marker_session = nullptr;
static lv_obj_t* marker_weekly = nullptr;
static int       bar_session_w = 0;   // inner bar widths, cached for marker math
static int       bar_weekly_w = 0;

// Window lengths are Anthropic product constants, not fields in the BLE
// payload — the daemon only sends the countdown to the next reset.
#define SESSION_WINDOW_MINS   300     // 5h
#define WEEKLY_WINDOW_MINS   10080    // 7d

// Rainbow mode — mirrors ~/bin/claude-statusline: each window rainbows
// independently through its own final stretch, stepping one fixed color per
// second rather than sweeping hue. Text gets a per-character gradient, the
// same shape the script draws with ANSI codes.
#define RAINBOW_5H_MINS     60      // 5h window: last hour  (script: near=3600s)
#define RAINBOW_7D_MINS     1440    // 7d window: last day   (script: near=86400s)
#define RB_COUNT            6
// ANSI bright red, yellow, green, cyan, blue, magenta — the script's order.
static const uint32_t RB_COLORS[RB_COUNT] = {
    0xff5555, 0xffff55, 0x55ff55, 0x55ffff, 0x5555ff, 0xff55ff
};
static bool     rainbow_session = false;
static bool     rainbow_weekly = false;
static uint8_t  rainbow_phase = 0;    // advances once per second
// The plain text and color each label falls back to when its window is not
// rainbowing. Cached because the rainbow overwrites the label text itself,
// and because enterprise picks the weekly color from pace rather than pct.
static lv_color_t base_bar_session_col;
static lv_color_t base_bar_weekly_col;
static char       txt_session_pct[16];
static char       txt_session_reset[24];
static char       txt_weekly_pct[16];
static char       txt_weekly_reset[96];   // enterprise packs recolor markup in here
// Enterprise-only widgets inside panel_session
static lv_obj_t* lbl_session_pct_sym = nullptr;  // "%" in smaller font
static lv_obj_t* lbl_spending_desc = nullptr;     // "of monthly budget"
static lv_obj_t* lbl_anim;      // status line: connection state + whimsical idle

// ---- Battery indicator (shared, on top) ----
static lv_obj_t* battery_img;
static lv_obj_t* logo_img;
static lv_image_dsc_t battery_dscs[5];  // empty, low, medium, full, charging

// ---- Live-data freshness → which usage sub-view to show ----
// usage panels when data is flowing, an idle "Zzz" screen when the host is
// connected but no usage update landed within DATA_FRESH_MS, the pairing hint
// when BLE is down. Re-evaluated every loop in ui_tick_anim().
static lv_obj_t* idle_group;            // the "Zzz" idle screen
static uint32_t  last_data_ms = 0;      // lv_tick when the last valid usage update landed
static bool      data_received = false; // any valid update since boot
static bool      data_ok = true;        // last payload's ok flag; a {"ok":false} beat = "no fresh data"
static int       view_state = -1;       // -1 unknown / 0 pair / 1 idle / 2 usage
static const uint32_t DATA_FRESH_MS = 90000;  // usage counts as "live" within this window (daemon sends ~60s)

// ---- Shared ----
static lv_image_dsc_t logo_dsc;
static screen_t current_screen = SCREEN_USAGE;
static bool     s_ble_connected = false;   // cached BLE connection state
static uint32_t connected_at_ms = 0;       // when we last entered CONNECTED ("Connected" dwell)

// Animation state
static uint32_t anim_last_ms = 0;
static uint8_t anim_spinner_idx = 0;
static uint8_t anim_phase = 0;
static uint8_t anim_msg_idx = 0;
static uint32_t anim_msg_start = 0;
#define ANIM_MSG_MS     4000

static const char* const spinner_frames[] = {
    "\xC2\xB7", "\xE2\x9C\xBB", "\xE2\x9C\xBD",
    "\xE2\x9C\xB6", "\xE2\x9C\xB3", "\xE2\x9C\xA2",
};
#define SPINNER_COUNT 6
#define SPINNER_PHASES (2 * (SPINNER_COUNT - 1))  // 10: ping-pong 0..5..0

static const uint16_t spinner_ms[SPINNER_COUNT] = {
    260, 130, 130, 130, 130, 260,
};

static const char* const anim_messages[] = {
    "Accomplishing", "Elucidating", "Perusing",
    "Actioning", "Enchanting", "Philosophising",
    "Actualizing", "Envisioning", "Pondering",
    "Baking", "Finagling", "Pontificating",
    "Booping", "Flibbertigibbeting", "Processing",
    "Brewing", "Forging", "Puttering",
    "Calculating", "Forming", "Puzzling",
    "Cerebrating", "Frolicking", "Reticulating",
    "Channelling", "Generating", "Ruminating",
    "Churning", "Germinating", "Scheming",
    "Clauding", "Hatching", "Schlepping",
    "Coalescing", "Herding", "Shimmying",
    "Cogitating", "Honking", "Shucking",
    "Combobulating", "Hustling", "Simmering",
    "Computing", "Ideating", "Smooshing",
    "Concocting", "Imagining", "Spelunking",
    "Conjuring", "Incubating", "Spinning",
    "Considering", "Inferring", "Stewing",
    "Contemplating", "Jiving", "Sussing",
    "Cooking", "Manifesting", "Synthesizing",
    "Crafting", "Marinating", "Thinking",
    "Creating", "Meandering", "Tinkering",
    "Crunching", "Moseying", "Transmuting",
    "Deciphering", "Mulling", "Unfurling",
    "Deliberating", "Mustering", "Unravelling",
    "Determining", "Musing", "Vibing",
    "Discombobulating", "Noodling", "Wandering",
    "Divining", "Percolating", "Whirring",
    "Doing", "Wibbling",
    "Effecting", "Wizarding",
    "Working", "Wrangling",
};
#define ANIM_MSG_COUNT (sizeof(anim_messages) / sizeof(anim_messages[0]))

// Fraction of a usage window already elapsed, 0-100. Returns -1 when there is
// no active window: the daemon's reset_minutes() sends 0 once the reset
// timestamp is in the past (board idle overnight), and -1 for enterprise,
// which has no 5h window at all. Callers render -1 as "no marker".
static int elapsed_pct(int reset_mins, int window_mins) {
    if (reset_mins <= 0 || reset_mins > window_mins) return -1;
    return (int)(100.0f * (float)(window_mins - reset_mins) / (float)window_mins + 0.5f);
}

// Absolute level. Only used where there is no window to pace against.
static lv_color_t pct_color(float pct) {
    if (pct >= 80.0f) return COL_RED;
    if (pct >= 50.0f) return COL_AMBER;
    return COL_GREEN;
}

// Pace ratio, matching color_pace() in ~/bin/claude-statusline: the color says
// whether you are burning faster than the window is elapsing, not how full the
// bucket is. 88% used at 92% elapsed is green; 88% at 40% elapsed is red.
// Falls back to the absolute scale when there is no active window to compare
// against, the same way render_window() does without a resets_at.
static lv_color_t window_pace_color(float used_pct, int elapsed) {
    if (elapsed < 0)  return pct_color(used_pct);
    // elapsed_pct() rounds to 0 for the first ~1% of a window (1.5 min of
    // 5h, ~50 min of 7d); floor at 1 so heavy early use still reads hot.
    if (elapsed == 0) elapsed = 1;
    int ratio = (int)(used_pct * 100.0f / (float)elapsed);
    if (ratio >= 200) return COL_RED;
    if (ratio > 100)  return COL_AMBER;
    return COL_GREEN;
}

static void format_reset_time(int mins, char* buf, size_t len) {
    if (mins < 0) {
        snprintf(buf, len, "---");
    } else if (mins < 60) {
        snprintf(buf, len, "Resets in %dm", mins);
    } else if (mins < 1440) {
        snprintf(buf, len, "Resets in %dh %dm", mins / 60, mins % 60);
    } else {
        snprintf(buf, len, "Resets in %dd %dh", mins / 1440, (mins % 1440) / 60);
    }
}

// Same durations, no "Resets in " prefix — fits the narrow top-row tiles.
static void format_reset_time_short(int mins, char* buf, size_t len) {
    if (mins < 0) {
        snprintf(buf, len, "---");
    } else if (mins < 60) {
        snprintf(buf, len, "%dm", mins);
    } else if (mins < 1440) {
        snprintf(buf, len, "%dh %dm", mins / 60, mins % 60);
    } else {
        snprintf(buf, len, "%dd %dh", mins / 1440, (mins % 1440) / 60);
    }
}

// Forward decls — callbacks defined near ui_show_screen below
static void global_click_cb(lv_event_t* e);

static lv_obj_t* make_panel(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_style_bg_color(panel, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_left(panel, L.panel_pad_x, 0);
    lv_obj_set_style_pad_right(panel, L.panel_pad_x, 0);
    lv_obj_set_style_pad_top(panel, L.panel_pad_y, 0);
    lv_obj_set_style_pad_bottom(panel, L.panel_pad_y, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_EVENT_BUBBLE);
    return panel;
}

static lv_obj_t* make_bar(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* bar = lv_bar_create(parent);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, w, h);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, COL_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, COL_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 6, LV_PART_INDICATOR);
    return bar;
}

// Small labels share the reset row with the much larger countdown. Aligning
// their boxes to the same y sits them high; offset by the difference in
// baseline position so the text lines up optically instead.
static int baseline_drop(const lv_font_t* big, const lv_font_t* small) {
    int b = big->line_height - big->base_line;
    int s = small->line_height - small->base_line;
    return (b - s) > 0 ? (b - s) : 0;
}

// Pace tick: a thin vertical line drawn over a bar at the "you should be here"
// position for how far into the window you are. Taller than the bar so it
// overhangs both edges and reads as a marker rather than a gap in the fill.
#define MARKER_W 3
static lv_obj_t* make_marker(lv_obj_t* parent, int bar_y, int bar_h) {
    lv_obj_t* m = lv_obj_create(parent);
    lv_obj_set_size(m, MARKER_W, bar_h + 8);
    lv_obj_set_pos(m, 0, bar_y - 4);
    lv_obj_set_style_bg_color(m, COL_TEXT, 0);
    lv_obj_set_style_bg_opa(m, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(m, 2, 0);
    lv_obj_set_style_border_width(m, 0, 0);
    lv_obj_set_style_pad_all(m, 0, 0);
    lv_obj_clear_flag(m, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(m, LV_OBJ_FLAG_HIDDEN);
    return m;
}

// Position the tick at `pct` along a `bar_w`-wide bar; pct < 0 hides it.
static void place_marker(lv_obj_t* m, int pct, int bar_w, int bar_y) {
    if (!m) return;
    if (pct < 0) {
        lv_obj_add_flag(m, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    int max_x = bar_w - MARKER_W;
    if (max_x < 0) max_x = 0;
    int x = pct * max_x / 100;
    if (x < 0) x = 0;
    if (x > max_x) x = max_x;
    lv_obj_set_pos(m, x, bar_y - 4);
    lv_obj_clear_flag(m, LV_OBJ_FLAG_HIDDEN);
}

static void init_icon_dsc_rgb565a8(lv_image_dsc_t* dsc, int w, int h, const uint8_t* data) {
    dsc->header.w = w;
    dsc->header.h = h;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565A8;
    dsc->header.stride = w * 2;
    dsc->data = data;
    dsc->data_size = w * h * 3;
}

static lv_obj_t* make_pill(lv_obj_t* parent, const char* text,
                           const lv_font_t* font, int pad_x, int pad_y) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, COL_TEXT, 0);
    lv_obj_set_style_bg_color(lbl, COL_BAR_BG, 0);
    lv_obj_set_style_bg_opa(lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(lbl, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_left(lbl, pad_x, 0);
    lv_obj_set_style_pad_right(lbl, pad_x, 0);
    lv_obj_set_style_pad_top(lbl, pad_y, 0);
    lv_obj_set_style_pad_bottom(lbl, pad_y, 0);
    return lbl;
}

static void init_battery_icons(void) {
    if (L.small_icons) {
        init_icon_dsc_rgb565a8(&battery_dscs[0], ICON_BATTERY_SMALL_W, ICON_BATTERY_SMALL_H, icon_battery_small_data);
        init_icon_dsc_rgb565a8(&battery_dscs[1], ICON_BATTERY_LOW_SMALL_W, ICON_BATTERY_LOW_SMALL_H, icon_battery_low_small_data);
        init_icon_dsc_rgb565a8(&battery_dscs[2], ICON_BATTERY_MEDIUM_SMALL_W, ICON_BATTERY_MEDIUM_SMALL_H, icon_battery_medium_small_data);
        init_icon_dsc_rgb565a8(&battery_dscs[3], ICON_BATTERY_FULL_SMALL_W, ICON_BATTERY_FULL_SMALL_H, icon_battery_full_small_data);
        init_icon_dsc_rgb565a8(&battery_dscs[4], ICON_BATTERY_CHARGING_SMALL_W, ICON_BATTERY_CHARGING_SMALL_H, icon_battery_charging_small_data);
        return;
    }
    init_icon_dsc_rgb565a8(&battery_dscs[0], ICON_BATTERY_W, ICON_BATTERY_H, icon_battery_data);
    init_icon_dsc_rgb565a8(&battery_dscs[1], ICON_BATTERY_LOW_W, ICON_BATTERY_LOW_H, icon_battery_low_data);
    init_icon_dsc_rgb565a8(&battery_dscs[2], ICON_BATTERY_MEDIUM_W, ICON_BATTERY_MEDIUM_H, icon_battery_medium_data);
    init_icon_dsc_rgb565a8(&battery_dscs[3], ICON_BATTERY_FULL_W, ICON_BATTERY_FULL_H, icon_battery_full_data);
    init_icon_dsc_rgb565a8(&battery_dscs[4], ICON_BATTERY_CHARGING_W, ICON_BATTERY_CHARGING_H, icon_battery_charging_data);
}

// ======== Usage Screen ========

static lv_obj_t* make_usage_panel(lv_obj_t* parent, int x, int y, int w, int h,
                                  int bar_y, int reset_y, const char* pill_text,
                                  const lv_font_t* pill_font, int pill_pad_x, int pill_pad_y,
                                  lv_obj_t** out_pct, lv_obj_t** out_pill,
                                  lv_obj_t** out_bar, lv_obj_t** out_reset,
                                  lv_obj_t** out_marker, int* out_bar_w,
                                  lv_obj_t** out_elapsed) {
    lv_obj_t* panel = make_panel(parent, x, y, w, h);

    *out_pct = lv_label_create(panel);
    // Recolor markup is how the rainbow tints these per character.
    lv_label_set_recolor(*out_pct, true);
    lv_label_set_text(*out_pct, "---%");
    lv_obj_set_style_text_font(*out_pct, L.pct_font, 0);
    lv_obj_set_style_text_color(*out_pct, COL_TEXT, 0);
    lv_obj_set_pos(*out_pct, 0, 0);

    *out_pill = make_pill(panel, pill_text, pill_font, pill_pad_x, pill_pad_y);
    lv_obj_align(*out_pill, LV_ALIGN_TOP_RIGHT, 0, 1);

    int bar_w = w - 2 * L.panel_pad_x;
    *out_bar = make_bar(panel, 0, bar_y, bar_w, L.bar_h);
    // After the bar, so the tick draws on top of the fill.
    *out_marker = make_marker(panel, bar_y, L.bar_h);
    *out_bar_w = bar_w;

    *out_reset = lv_label_create(panel);
    lv_label_set_recolor(*out_reset, true);
    lv_label_set_text(*out_reset, "---");
    lv_obj_set_style_text_font(*out_reset, L.reset_font, 0);
    lv_obj_set_style_text_color(*out_reset, COL_DIM, 0);
    lv_obj_set_pos(*out_reset, 0, reset_y);

    // Elapsed-window number, small and dim, sitting on the big percentage's
    // baseline the way the statusline prints "42%/50%". Positioned in
    // realign_elapsed() because the anchor's width changes with the value.
    *out_elapsed = lv_label_create(panel);
    lv_label_set_text(*out_elapsed, "");
    lv_obj_set_style_text_font(*out_elapsed, L.pace_font, 0);
    lv_obj_set_style_text_color(*out_elapsed, COL_DIM, 0);

    return panel;
}

// Pairing hint — shown when disconnected so the screen isn't empty and the
// user knows how to (re)pair. Wording matches the 3-second release gesture.
static void build_pair_group(lv_obj_t* parent) {
    pair_group = lv_obj_create(parent);
    lv_obj_set_size(pair_group, L.scr_w, L.scr_h - L.content_y);
    lv_obj_set_pos(pair_group, 0, L.content_y);
    lv_obj_set_style_bg_opa(pair_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pair_group, 0, 0);
    lv_obj_set_style_pad_all(pair_group, 0, 0);
    lv_obj_clear_flag(pair_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t* l1 = lv_label_create(pair_group);
    lv_label_set_text(l1, "To pair");
    lv_obj_set_style_text_font(l1, L.bt_status_font, 0);
    lv_obj_set_style_text_color(l1, COL_TEXT, 0);
    lv_obj_align(l1, LV_ALIGN_TOP_MID, 0, L.pair_y1);

    lv_obj_t* l2 = lv_label_create(pair_group);
    lv_label_set_text(l2, "hold the power button");
    lv_obj_set_style_text_font(l2, L.bt_device_font, 0);
    lv_obj_set_style_text_color(l2, COL_DIM, 0);
    lv_obj_align(l2, LV_ALIGN_TOP_MID, 0, L.pair_y2);

    lv_obj_t* l3 = lv_label_create(pair_group);
    lv_label_set_text(l3, "for 3 seconds, then release");
    lv_obj_set_style_text_font(l3, L.bt_device_font, 0);
    lv_obj_set_style_text_color(l3, COL_DIM, 0);
    lv_obj_align(l3, LV_ALIGN_TOP_MID, 0, L.pair_y3);

    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_HIDDEN);  // ui_update_ble_status decides
}

// Idle "Zzz" screen — shown when the host is connected but no usage update has
// landed recently (token expired, daemon down, host asleep…). Full-screen, like
// the pairing hint, so we never render hours-old numbers as if they were live.
static void build_idle_group(lv_obj_t* parent) {
    idle_group = lv_obj_create(parent);
    lv_obj_set_size(idle_group, L.scr_w, L.scr_h - L.content_y);
    lv_obj_set_pos(idle_group, 0, L.content_y);
    lv_obj_set_style_bg_opa(idle_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(idle_group, 0, 0);
    lv_obj_set_style_pad_all(idle_group, 0, 0);
    lv_obj_clear_flag(idle_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(idle_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    // A shrunk-down resting creature (the official cloud-ride animation)
    // sits between the header and the status line; the animated "Listening…"
    // status line carries the words, so no extra text is needed here.
    lv_obj_t* creature = splash_mini_create(idle_group, "cloud", L.idle_px);
    if (creature) lv_obj_align(creature, LV_ALIGN_CENTER, 0, -20);

    lv_obj_add_flag(idle_group, LV_OBJ_FLAG_HIDDEN);  // update_view_state decides
}

static void init_usage_screen(lv_obj_t* scr) {
    usage_container = lv_obj_create(scr);
    lv_obj_set_size(usage_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(usage_container, 0, 0);
    lv_obj_set_style_bg_opa(usage_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(usage_container, 0, 0);
    lv_obj_set_style_pad_all(usage_container, 0, 0);
    lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(usage_container, global_click_cb, LV_EVENT_CLICKED, NULL);

    lbl_title = lv_label_create(usage_container);
    lv_label_set_text(lbl_title, "Usage");
    lv_obj_set_style_text_font(lbl_title, L.title_font, 0);
    lv_obj_set_style_text_color(lbl_title, COL_TEXT, 0);
    // The nudge balances the corner logo on the left; smaller on small
    // screens where the logo is 40px and the battery icon sits closer.
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, L.title_nudge, L.title_y);

    // Usage panels (shown when connected) live in a transparent full-size group
    // so they can be toggled against the pairing hint as one unit.
    usage_group = lv_obj_create(usage_container);
    lv_obj_set_size(usage_group, L.scr_w, L.scr_h);
    lv_obj_set_pos(usage_group, 0, 0);
    lv_obj_set_style_bg_opa(usage_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(usage_group, 0, 0);
    lv_obj_set_style_pad_all(usage_group, 0, 0);
    lv_obj_clear_flag(usage_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(usage_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    panel_session = make_usage_panel(usage_group, L.margin, L.content_y,
                     L.content_w, L.usage_top_h,
                     L.usage_top_bar_y, L.usage_top_reset_y, "Current",
                     L.pill_font, L.pill_pad_x, L.pill_pad_y,
                     &lbl_session_pct, &lbl_session_label,
                     &bar_session, &lbl_session_reset,
                     &marker_session, &bar_session_w, &lbl_session_elapsed);

    // Burn rate sits between the countdown and the elapsed number on the
    // Current tile's bottom row. It is not part of the pace signal, so it
    // keeps its tier color and never joins the rainbow.
    lbl_rate = lv_label_create(panel_session);
    lv_label_set_text(lbl_rate, "");
    lv_obj_set_style_text_font(lbl_rate, L.pace_font, 0);
    lv_obj_set_style_text_color(lbl_rate, COL_DIM, 0);
    lv_obj_align(lbl_rate, LV_ALIGN_TOP_MID, 0,
                 L.usage_top_reset_y + baseline_drop(L.reset_font, L.pace_font));

    // Enterprise-only overlays inside panel_session — hidden until enterprise data arrives
    lbl_session_pct_sym = lv_label_create(panel_session);
    lv_label_set_text(lbl_session_pct_sym, "%");
    lv_obj_set_style_text_font(lbl_session_pct_sym, L.reset_font, 0);
    lv_obj_set_style_text_color(lbl_session_pct_sym, COL_TEXT, 0);
    lv_obj_add_flag(lbl_session_pct_sym, LV_OBJ_FLAG_HIDDEN);

    lbl_spending_desc = lv_label_create(panel_session);
    lv_label_set_text(lbl_spending_desc, "of monthly budget");   // fits 368 wide
    lv_obj_set_style_text_font(lbl_spending_desc, L.reset_font, 0);
    lv_obj_set_style_text_color(lbl_spending_desc, COL_DIM, 0);
    lv_obj_set_pos(lbl_spending_desc, 0, L.usage_top_reset_y);
    lv_obj_add_flag(lbl_spending_desc, LV_OBJ_FLAG_HIDDEN);

    panel_weekly = make_usage_panel(usage_group, L.margin,
                     L.content_y + L.usage_top_h + L.usage_panel_gap, L.content_w, L.usage_bottom_h,
                     L.usage_bottom_bar_y, L.usage_bottom_reset_y, "Weekly",
                     L.pill_font, L.pill_pad_x, L.pill_pad_y,
                     &lbl_weekly_pct, &lbl_weekly_label,
                     &bar_weekly, &lbl_weekly_reset,
                     &marker_weekly, &bar_weekly_w, &lbl_weekly_elapsed);

    build_pair_group(usage_container);
    build_idle_group(usage_container);

    // Status line — always visible on the usage view. Driven by ui_tick_anim().
    lbl_anim = lv_label_create(usage_container);
    lv_label_set_text(lbl_anim, "");
    lv_obj_set_style_text_font(lbl_anim, L.anim_font, 0);
    lv_obj_set_style_text_color(lbl_anim, COL_ACCENT, 0);
    lv_obj_align(lbl_anim, LV_ALIGN_BOTTOM_MID, 0, L.anim_y);
}

// ======== Public API ========

void ui_init(void) {
    compute_layout(board_caps());
    base_bar_session_col = COL_GREEN;
    base_bar_weekly_col  = COL_GREEN;

    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

#ifndef BOARD_HAS_PSRAM
    // Static corner mascot (see clawd_still.h) — the animated one needs PSRAM.
    if (L.small_icons) init_icon_dsc_rgb565a8(&logo_dsc, CLAWD_STILL_SMALL_W, CLAWD_STILL_SMALL_H, clawd_still_small_data);
    else               init_icon_dsc_rgb565a8(&logo_dsc, CLAWD_STILL_W, CLAWD_STILL_H, clawd_still_data);
#endif
    init_battery_icons();

    init_usage_screen(scr);
    splash_init(scr);

    if (splash_get_root()) {
        lv_obj_add_event_cb(splash_get_root(), global_click_cb, LV_EVENT_CLICKED, NULL);
    }

    // Corner mascot in the old logo slot. The still Clawd is shorter than the
    // 80/40 px slot the spark logo used; center it vertically in that slot.
    {
        const int slot  = L.small_icons ? LOGO_SMALL_HEIGHT : LOGO_HEIGHT;
        const int art_h = L.small_icons ? CLAWD_STILL_SMALL_H : CLAWD_STILL_H;
        const int top   = L.logo_y + (slot - art_h) / 2;
#ifdef BOARD_HAS_PSRAM
        // Animated: idles, does acts, and takes walk-off/lurk trips.
        splash_mascot_create(scr, L.margin, top + art_h, L.small_icons ? 2 : 3);
#else
        logo_img = lv_image_create(scr);
        lv_image_set_src(logo_img, &logo_dsc);
        lv_obj_set_pos(logo_img, L.margin, top);
#endif
    }

    battery_img = lv_image_create(scr);
    lv_image_set_src(battery_img, &battery_dscs[0]);
    lv_obj_set_pos(battery_img, L.scr_w - L.batt_w - L.margin, L.batt_y);
    // Boards without battery telemetry never show the indicator (per the HAL
    // contract; previously every board drew the empty-battery glyph).
    if (!board_caps().has_battery) {
        lv_obj_del(battery_img);
        battery_img = nullptr;
    }
}

// Per-character gradient via LVGL recolor markup, matching rainbow() in the
// statusline: character i takes palette slot (i + phase). Spaces are left
// uncolored — a "#RRGGBB  #" run confuses the recolor parser. The label must
// have lv_label_set_recolor(lbl, true).
static void set_rainbow_text(lv_obj_t* lbl, const char* src, uint8_t phase) {
    char out[320];
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 12 < sizeof(out); i++) {
        if (src[i] == ' ') { out[o++] = ' '; continue; }
        o += snprintf(out + o, sizeof(out) - o, "#%06x %c#",
                      (unsigned)RB_COLORS[(i + phase) % RB_COUNT], src[i]);
    }
    out[o] = 0;
    lv_label_set_text(lbl, out);
}

// Single owner of every color that rainbow mode touches. Called from both
// ui_update (fresh payload) and ui_tick_anim (hue step) — split ownership
// would let the tick clobber the payload's bar colors, and would leave the
// rainbow colors stuck on screen for up to a minute after it deactivates.
// Recolors pill *text*, not pill backgrounds: a saturated hue behind the
// near-white COL_TEXT is unreadable through much of the cycle.
// The elapsed labels hang off the right edge of the percentage labels, whose
// width changes with the value ("9%" vs "100%"), so re-anchor them whenever
// that text is rewritten. The y-offset drops them from the percentage's box
// top onto its baseline.
static void realign_elapsed(void) {
    int drop = baseline_drop(L.pct_font, L.pace_font);
    if (lbl_session_elapsed && lbl_session_pct)
        lv_obj_align_to(lbl_session_elapsed, lbl_session_pct, LV_ALIGN_OUT_RIGHT_TOP, 6, drop);
    if (lbl_weekly_elapsed && lbl_weekly_pct)
        lv_obj_align_to(lbl_weekly_elapsed, lbl_weekly_pct, LV_ALIGN_OUT_RIGHT_TOP, 6, drop);
}

static void apply_colors(void) {
    if (!bar_session || !bar_weekly) return;

    lv_color_t rb = lv_color_hex(RB_COLORS[rainbow_phase % RB_COUNT]);

    if (rainbow_session) {
        lv_obj_set_style_bg_color(bar_session, rb, LV_PART_INDICATOR);
        set_rainbow_text(lbl_session_pct,   txt_session_pct,   rainbow_phase);
        set_rainbow_text(lbl_session_reset, txt_session_reset, rainbow_phase);
    } else {
        lv_obj_set_style_bg_color(bar_session, base_bar_session_col, LV_PART_INDICATOR);
        lv_label_set_text(lbl_session_pct,   txt_session_pct);
        lv_label_set_text(lbl_session_reset, txt_session_reset);
    }

    if (rainbow_weekly) {
        lv_obj_set_style_bg_color(bar_weekly, rb, LV_PART_INDICATOR);
        set_rainbow_text(lbl_weekly_pct,   txt_weekly_pct,   rainbow_phase);
        set_rainbow_text(lbl_weekly_reset, txt_weekly_reset, rainbow_phase);
    } else {
        lv_obj_set_style_bg_color(bar_weekly, base_bar_weekly_col, LV_PART_INDICATOR);
        lv_label_set_text(lbl_weekly_pct,   txt_weekly_pct);
        lv_label_set_text(lbl_weekly_reset, txt_weekly_reset);
    }

    realign_elapsed();
}

void ui_update(const UsageData* data) {
    if (!data->valid) return;
    data_ok = data->ok;
    if (!data->ok) return;          // a {"ok":false} "no data" beat → fall through to idle, keep last numbers
    last_data_ms = lv_tick_get();   // a real usage update just landed
    data_received = true;

    if (data->clock_epoch > 0) {    // daemon supplied wall-clock time → drive the title clock
        clock_base_epoch = data->clock_epoch;
        clock_base_ms = last_data_ms;
        clock_fmt = data->clock_fmt;
    } else if (clock_base_epoch != 0) {   // clock turned off daemon-side → revert title to "Usage"
        clock_base_epoch = 0;
        clock_last_min = -1;
        lv_label_set_text(lbl_title, "Usage");
    }

    int s_pct = (int)(data->session_pct + 0.5f);

    // Rate tile. usage_rate_sample() is fed in main's BLE poll just before this
    // call, so the value is current. It reports a negative rate for the first
    // ~4 minutes after boot (and again after every session reset) while its
    // ring buffer refills — "Waiting" rather than a misleading "Idle".
    if (lbl_rate) {
        float rate = usage_rate_pct_per_min();
        if (rate < 0.0f) {
            lv_label_set_text(lbl_rate, "Waiting");
            lv_obj_set_style_text_color(lbl_rate, COL_DIM, 0);
        } else {
            static const char* const RATE_WORDS[] = { "Idle", "Normal", "Active", "Heavy" };
            const lv_color_t RATE_COLORS[] = { COL_GREEN, COL_GREEN, COL_AMBER, COL_RED };
            int g = usage_rate_group();
            // LVGL's lv_snprintf drops floats unless LV_SPRINTF_USE_FLOAT is
            // set, so format with the C library and set the text directly.
            char rbuf[32];
            snprintf(rbuf, sizeof(rbuf), "%s  %.2f %%/min", RATE_WORDS[g], rate);
            lv_label_set_text(lbl_rate, rbuf);
            lv_obj_set_style_text_color(lbl_rate, RATE_COLORS[g], 0);
        }
    }

    // Enterprise has no 5h/7d windows: the period bar's own value is time_pct,
    // so a pace tick would sit on the fill tip and there is nothing to rainbow.
    int elapsed_s = data->enterprise ? -1
                  : elapsed_pct(data->session_reset_mins, SESSION_WINDOW_MINS);
    int elapsed_w = data->enterprise ? -1
                  : elapsed_pct(data->weekly_reset_mins, WEEKLY_WINDOW_MINS);

    // Each window rainbows through its own final stretch, independently.
    rainbow_session = elapsed_s >= 0 && data->session_reset_mins <= RAINBOW_5H_MINS;
    rainbow_weekly  = elapsed_w >= 0 && data->weekly_reset_mins  <= RAINBOW_7D_MINS;

    place_marker(marker_session, elapsed_s, bar_session_w, L.usage_top_bar_y);
    place_marker(marker_weekly,  elapsed_w, bar_weekly_w,  L.usage_bottom_bar_y);

    if (elapsed_s >= 0) lv_label_set_text_fmt(lbl_session_elapsed, "/%d%%", elapsed_s);
    else                lv_label_set_text(lbl_session_elapsed, "");
    if (elapsed_w >= 0) lv_label_set_text_fmt(lbl_weekly_elapsed, "/%d%%", elapsed_w);
    else                lv_label_set_text(lbl_weekly_elapsed, "");

    if (data->enterprise) {
        // Spending box: big number-only label + small "%" symbol + desc + pace
        lv_obj_set_style_text_font(lbl_session_pct, L.ent_pct_font, 0);
        lv_label_set_text(lbl_session_label, "Spending");
        lv_obj_add_flag(lbl_session_reset, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_session_pct_sym, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_spending_desc,   LV_OBJ_FLAG_HIDDEN);
        if (lbl_rate) lv_obj_add_flag(lbl_rate, LV_OBJ_FLAG_HIDDEN);
        if (panel_weekly) lv_obj_clear_flag(panel_weekly, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_set_style_text_font(lbl_session_pct, L.pct_font, 0);
        lv_label_set_text(lbl_session_label, "Current");
        lv_obj_clear_flag(lbl_session_reset, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_session_pct_sym, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_spending_desc,   LV_OBJ_FLAG_HIDDEN);
        if (lbl_rate) lv_obj_clear_flag(lbl_rate, LV_OBJ_FLAG_HIDDEN);
        if (panel_weekly) lv_obj_clear_flag(panel_weekly, LV_OBJ_FLAG_HIDDEN);
    }

    char buf[48];

    // Pace vars used in both enterprise blocks below
    const char* pace_text = "Under pace";
    lv_color_t  pace_color = COL_GREEN;
    const char* pace_hex   = "788c5d";   // matches THEME_GREEN
    if (data->session_pct > (float)data->time_pct + 15.0f) {
        pace_text = "Over pace";  pace_color = COL_RED;   pace_hex = "c0392b";
    } else if (data->session_pct > (float)data->time_pct - 15.0f) {
        pace_text = "On pace";    pace_color = COL_AMBER; pace_hex = "d97757";
    }

    if (data->enterprise) {
        snprintf(txt_session_pct, sizeof(txt_session_pct), "%d", s_pct);
        txt_session_reset[0] = 0;
        lv_label_set_text(lbl_session_pct, txt_session_pct);
        lv_obj_align_to(lbl_session_pct_sym, lbl_session_pct,
                        LV_ALIGN_OUT_RIGHT_TOP, 4, 12);
    } else {
        snprintf(txt_session_pct, sizeof(txt_session_pct), "%d%%", s_pct);
        format_reset_time_short(data->session_reset_mins, txt_session_reset,
                                sizeof(txt_session_reset));
    }

    lv_bar_set_value(bar_session, s_pct, LV_ANIM_ON);
    base_bar_session_col = window_pace_color(data->session_pct, elapsed_s);

    if (data->enterprise) {
        // Period box: time % + dynamic pace color + "Resets <date>" label
        lv_label_set_text(lbl_weekly_label, "Period");
        snprintf(txt_weekly_pct, sizeof(txt_weekly_pct), "%d%%", data->time_pct);
        lv_bar_set_value(bar_weekly, data->time_pct, LV_ANIM_ON);
        base_bar_weekly_col = (data->session_pct <= (float)data->time_pct) ? COL_GREEN :
                              (data->session_pct <= (float)data->time_pct + 15.0f) ? COL_AMBER :
                              COL_RED;
        snprintf(txt_weekly_reset, sizeof(txt_weekly_reset),
                 "#%s %s# - #faf9f5 Resets %s#",
                 pace_hex, pace_text, data->reset_date);
    } else {
        int w_pct = (int)(data->weekly_pct + 0.5f);
        snprintf(txt_weekly_pct, sizeof(txt_weekly_pct), "%d%%", w_pct);
        lv_bar_set_value(bar_weekly, w_pct, LV_ANIM_ON);
        base_bar_weekly_col = window_pace_color(data->weekly_pct, elapsed_w);
        format_reset_time(data->weekly_reset_mins, txt_weekly_reset,
                          sizeof(txt_weekly_reset));
    }

    apply_colors();
}

// Pick the usage-view sub-screen: pairing hint (BLE down), the idle "Zzz" screen
// (connected but data has gone stale), or the live usage panels. Only re-lays-out
// on an actual change. The animated status line stays visible everywhere — it
// reads "Listening…" on the idle screen, keeping it alive rather than frozen.
static void update_view_state(void) {
    if (!usage_group || !pair_group || !idle_group) return;
    int v;
    if (!s_ble_connected) {
        v = 0;  // pairing hint
    } else if (data_received && data_ok && (lv_tick_get() - last_data_ms) < DATA_FRESH_MS) {
        v = 2;  // live usage
    } else {
        v = 1;  // idle / Zzz
    }
    if (v == view_state) return;
    view_state = v;
    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(idle_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(usage_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(v == 0 ? pair_group : v == 1 ? idle_group : usage_group,
                      LV_OBJ_FLAG_HIDDEN);
}

void ui_tick_anim(void) {
    if (current_screen != SCREEN_USAGE) return;
    update_view_state();
    if (view_state == 1) splash_mini_tick();   // animate the sleeping creature on the idle screen

    uint32_t now = lv_tick_get();

    // Rainbow steps one palette slot per second, like the statusline's
    // `now % 6`. Only redraws on the second boundary, and only on view_state 2
    // (the live usage panels) — the bars and numbers are hidden otherwise.
    if ((rainbow_session || rainbow_weekly) && view_state == 2) {
        uint8_t phase = (uint8_t)((now / 1000) % RB_COUNT);
        if (phase != rainbow_phase) {
            rainbow_phase = phase;
            apply_colors();
        }
    }

    // Title clock: once the daemon has sent wall-clock time, replace "Usage" with
    // the live time, advanced locally so it ticks every minute between payloads.
    if (clock_base_epoch > 0) {
        time_t cur = (time_t)(clock_base_epoch + (now - clock_base_ms) / 1000);
        struct tm tmv;
        gmtime_r(&cur, &tmv);   // epoch is already local wall-clock → gmtime keeps it as-is
        if (tmv.tm_min != clock_last_min) {   // only rewrite the title when the minute changes
            clock_last_min = tmv.tm_min;
            char tbuf[12];
            if (clock_fmt == 12) {
                int h12 = tmv.tm_hour % 12;
                if (h12 == 0) h12 = 12;
                snprintf(tbuf, sizeof(tbuf), "%d:%02d %s", h12, tmv.tm_min,
                         tmv.tm_hour < 12 ? "AM" : "PM");
            } else {
                snprintf(tbuf, sizeof(tbuf), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
            }
            lv_label_set_text(lbl_title, tbuf);
        }
    }

    if (now - anim_msg_start >= ANIM_MSG_MS) {
        anim_msg_idx = (anim_msg_idx + 1) % ANIM_MSG_COUNT;
        anim_msg_start = now;
    }

    if (now - anim_last_ms < spinner_ms[anim_spinner_idx]) return;
    anim_last_ms = now;
    anim_phase = (anim_phase + 1) % SPINNER_PHASES;
    anim_spinner_idx = (anim_phase < SPINNER_COUNT) ? anim_phase
                                                    : (SPINNER_PHASES - anim_phase);

    // Status text by priority. Whimsical messages only when connected & settled.
    const char* text;
    if (!s_ble_connected) {
        text = "Waiting";              // advertising / waiting for a host connection
    } else if (view_state == 1) {      // idle — alternate so it reads as alive AND data-less
        text = (anim_msg_idx & 1) ? "No data" : "Listening";
    } else if (now - connected_at_ms < 5000) {
        text = "Connected";
    } else {
        text = anim_messages[anim_msg_idx];
    }

    // All states share the whimsical style: "<glyph> <Title-case word>…"
    static char buf[80];
    snprintf(buf, sizeof(buf), "%s %s\xE2\x80\xA6",
             spinner_frames[anim_spinner_idx], text);
    lv_label_set_text(lbl_anim, buf);
}

static screen_t prev_non_splash_screen = SCREEN_USAGE;
static void apply_battery_visibility(void) {
    if (!battery_img) return;
    if (current_screen == SCREEN_SPLASH) lv_obj_add_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
    else                                  lv_obj_clear_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
}

static void global_click_cb(lv_event_t* e) {
    (void)e;
    if (current_screen == SCREEN_SPLASH) ui_show_screen(prev_non_splash_screen);
    else                                  ui_show_screen(SCREEN_SPLASH);
}

void ui_show_screen(screen_t screen) {
    lv_obj_add_flag(usage_container, LV_OBJ_FLAG_HIDDEN);
    splash_hide();

    switch (screen) {
    case SCREEN_SPLASH:  splash_show(); break;
    case SCREEN_USAGE:   lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_HIDDEN); break;
    default: break;
    }

    splash_mascot_set_visible(screen != SCREEN_SPLASH);
    if (logo_img) {
        if (screen == SCREEN_SPLASH) lv_obj_add_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
        else                          lv_obj_clear_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
    }

    if (screen != SCREEN_SPLASH) prev_non_splash_screen = screen;
    current_screen = screen;
    apply_battery_visibility();
}

void ui_toggle_splash(void) {
    if (current_screen == SCREEN_SPLASH) ui_show_screen(prev_non_splash_screen);
    else                                  ui_show_screen(SCREEN_SPLASH);
}

screen_t ui_get_current_screen(void) {
    return current_screen;
}

void ui_update_ble_status(ble_state_t state, const char* name, const char* mac) {
    (void)name; (void)mac;
    bool was_connected = s_ble_connected;
    s_ble_connected = (state == BLE_STATE_CONNECTED);

    if (s_ble_connected && !was_connected) connected_at_ms = lv_tick_get();
    // pair / idle / usage — picked from connection + data freshness.
    update_view_state();
}

void ui_update_battery(int percent, bool charging) {
    if (!battery_img) return;
    int idx;
    if (charging) {
        idx = 4;
    } else if (percent < 0) {
        idx = 0;
    } else if (percent <= 10) {
        idx = 0;
    } else if (percent <= 35) {
        idx = 1;
    } else if (percent <= 75) {
        idx = 2;
    } else {
        idx = 3;
    }
    lv_image_set_src(battery_img, &battery_dscs[idx]);
    apply_battery_visibility();
}

#pragma once
#include <stdint.h>
#include <lvgl.h>

// Initialize splash module. Creates the canvas widget inside `parent` and
// allocates the 480x480 pixel buffer (PSRAM).
void splash_init(lv_obj_t *parent);

// Advance animation frame if hold time elapsed. Call from main loop.
void splash_tick(void);

// Cycle to the next animation in the catalog.
void splash_next(void);

// Show/hide the splash container.
void splash_show(void);
void splash_hide(void);

// Pick the next animation matching the current usage-rate group.
// Called automatically by splash_show(); also exposed so other modules can
// trigger a re-pick when the rate group changes mid-display.
void splash_pick_for_current_rate(void);

// True when splash is currently rendering (used to gate re-picks).
bool splash_is_active(void);

// Something on lv_layer_top (the setup-code overlay) covers the splash. The
// direct-draw path (no PSRAM) writes to the panel behind LVGL's back, so it
// must stop while covered and repaint in full once uncovered.
void splash_set_covered(bool covered);

// Root container (so ui.cpp can attach a click event).
lv_obj_t* splash_get_root(void);

// Mini animated creature for embedding elsewhere (e.g. the idle screen).
// Renders the named official animation (e.g. "cloud") at ~px×px
// inside `parent`; returns the canvas object (position it with lv_obj_align) or
// NULL if the animation isn't found / allocation fails. Drive it with
// splash_mini_tick(). One mini creature at a time.
lv_obj_t* splash_mini_create(lv_obj_t *parent, const char *anim_name, int px);
void splash_mini_tick(void);

// Corner mascot (usage screen, PSRAM boards): the still Clawd idles in the
// logo slot, does occasional acts, and takes walk-off/lurk/walk-back trips.
// feet_y = px of the art's ground line; cell = px per art cell in the corner.
lv_obj_t* splash_mascot_create(lv_obj_t *parent, int slot_x, int feet_y, int cell);
void splash_mascot_tick(void);
void splash_mascot_set_visible(bool v);

// Usage at its limit: Clawd lies down asleep (an official frame laid on his
// back, face up, eyes shut) until this is cleared. On the splash he dreams: three stories of
// official clips, about a minute each, play above him in turn. The corner
// mascot shows the sleeping pose only.
void splash_set_asleep(bool asleep);

// The sleeping pose as a static RGB565A8 image, for boards whose corner mascot
// is the static clawd_still.h icon (no PSRAM). Same size as that icon at the
// same px per cell. NULL when the pose or the buffer is unavailable.
const lv_image_dsc_t* splash_asleep_icon(int px_per_cell);

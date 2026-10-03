#pragma once

// Tracks short-term rate of change in session_pct (%/min) so the UI can react
// to *how heavily* Claude is being used right now, not just the current bucket
// level. Returns one of 4 group indices for the splash to pick animations from.

// Feed in the latest session percentage every time fresh BLE data arrives.
// Returns true when this sample is a session reset (pct dropped substantially
// vs the previous sample) — the caller uses this to chime the buzzer. Never
// true on the first sample after boot (no prior sample to compare against).
bool usage_rate_sample(float session_pct);

// 0 = idle, 1 = normal, 2 = active, 3 = heavy.
// Defaults to 0 when the buffer doesn't have enough samples yet.
int usage_rate_group(void);

// Smoothed rate of change in session_pct, in %/min. Returns a negative value
// while the ring buffer is still warming up (fewer than 2 samples, or less
// than MIN_WINDOW_MS of accumulated history), so callers can tell "genuinely
// idle" apart from "don't know yet" — usage_rate_group() collapses both to 0.
float usage_rate_pct_per_min(void);

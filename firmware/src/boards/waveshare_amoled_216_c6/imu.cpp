#include "../../hal/imu_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Wire.h>
#include <SensorQMI8658.hpp>

// Poll and hysteresis timing
#define IMU_POLL_MS       100    // ~10 Hz
#define STABLE_TIME_MS    300    // orientation must hold this long before rotating
#define TILT_THRESHOLD    0.5f   // ~30° from axis (sin 30° ≈ 0.5)

// Upright (ay = -1g) renders as quadrant 2, so that's the default until the
// IMU says otherwise — and the permanent answer when it's flat on a desk
// (ambiguous reading) or failed to init.
#define UPRIGHT_ROTATION 2

static SensorQMI8658 imu;
static uint8_t  current_rotation   = UPRIGHT_ROTATION;
static uint8_t  candidate_rotation = UPRIGHT_ROTATION;
static uint32_t candidate_since    = 0;
static uint32_t last_poll_ms       = 0;
static bool     imu_ok             = false;

// The QMI8658 is mounted differently here than on the S3 2.16 board this
// logic came from: X is mirrored, and the whole frame sits a quarter turn
// off. The mirrored X is taken out by the ax mapping below (2/0 rather than
// 0/2) — without it the display rotates the wrong way and every tilt reads
// as a 180° flip. ROT_OFFSET then lines the frame up with MADCTL 0x30's
// base orientation: measured on hardware, upright is ay = -1g, raw
// quadrant 1, which must render as quadrant 2.
#define ROT_OFFSET 1


static uint8_t accel_to_rotation(float ax, float ay) {
    float abs_ax = fabsf(ax);
    float abs_ay = fabsf(ay);
    if (abs_ax < TILT_THRESHOLD && abs_ay < TILT_THRESHOLD) {
        return 255;  // ambiguous (face-up/down)
    }
    uint8_t q;
    if (abs_ay > abs_ax) q = (ay > 0) ? 3 : 1;
    else                 q = (ax > 0) ? 2 : 0;   // X mirrored on this carrier
    return (uint8_t)((q + ROT_OFFSET) % 4);
}

void imu_hal_init(void) {
    if (!imu.begin(Wire, QMI8658_L_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
        Serial.println("QMI8658 init failed");
        return;
    }
    Serial.println("QMI8658 init OK");
    imu.configAccelerometer(
        SensorQMI8658::ACC_RANGE_4G,
        SensorQMI8658::ACC_ODR_LOWPOWER_21Hz,
        SensorQMI8658::LPF_MODE_3);
    imu.enableAccelerometer();
    imu_ok = true;

    // Adopt the boot orientation right away (no 300 ms debounce) so the
    // first frame is drawn the right way up.
    float ax, ay, az;
    if (imu.getAccelerometer(ax, ay, az)) {
        uint8_t r = accel_to_rotation(ax, ay);
        if (r != 255) current_rotation = candidate_rotation = r;
    }
}

void imu_hal_tick(void) {
    if (!imu_ok) return;
    uint32_t now = millis();
    if (now - last_poll_ms < IMU_POLL_MS) return;
    last_poll_ms = now;

    float ax, ay, az;
    if (!imu.getAccelerometer(ax, ay, az)) return;

    uint8_t target = accel_to_rotation(ax, ay);
    if (target == 255 || target == current_rotation) {
        candidate_rotation = current_rotation;
        return;
    }
    if (target != candidate_rotation) {
        candidate_rotation = target;
        candidate_since = now;
    } else if (now - candidate_since >= STABLE_TIME_MS) {
        current_rotation = target;
        Serial.printf("Rotation: %d\n", current_rotation);
    }
}

uint8_t imu_hal_rotation_quadrant(void) { return current_rotation; }

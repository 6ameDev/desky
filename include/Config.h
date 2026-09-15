#pragma once

// --- Motor GPIO Pin Mapping ---
#define PIN_IN1 25
#define PIN_IN2 26
#define PIN_IN3 18
#define PIN_IN4 19
#define PIN_FAULT 27

// --- PWM Settings ---
#define PWM_FREQ 20000
#define PWM_RESOLUTION 8
#define MIN_MOTOR_PWM 65

// --- Max Power Setting ---
#define DEFAULT_MAX_POWER_PERCENT 50
#define MIN_MAX_POWER_PERCENT 10
#define MAX_MAX_POWER_PERCENT 100

// --- Safety & Watchdog Constants ---
#define DEFAULT_CLIFF_LIMIT_MM 200
#define COMMAND_TIMEOUT_MS 300
#define TELEMETRY_INTERVAL_MS 1000
#define TELEMETRY_IDLE_INTERVAL_MS 5000
#define TELEMETRY_DISTANCE_EPSILON_MM 15

// --- MPU6050 IMU ---
#define MPU_I2C_ADDR_PRIMARY 0x68
#define MPU_I2C_ADDR_FALLBACK 0x69
#define MPU_READ_TICK_OFFSET 2
#define PICKED_UP_ACCEL_G 1.5f
#define PICKED_UP_TILT_DEG 45.0f
#define MPU_REPROBE_INTERVAL_MS 5000

// --- Wiggle motion primitive (1 wiggle = one left+right sway pair) ---
// Direction base speeds: FORWARD > 0, IN_PLACE 0, BACKWARD < 0.
// Total time is derived (pairs * 2 * half-period), never a tunable.
#define WIGGLE_FORWARD_SPEED 15
#define WIGGLE_REVERSE_SPEED -15
#define WIGGLE_SWAY_DELTA 130
#define WIGGLE_HALF_PERIOD_MS 180

// --- Cliff response (backwards wiggle when driving forward off an edge) ---
#define CLIFF_WIGGLE_PAIRS 2

// --- ToF Sensor Fault Handling ---
#define I2C_TIMEOUT_MS 20
#define TOF_READ_EVERY_N_TICKS 5
#define TOF_MAX_CONSECUTIVE_ERRORS 5
#define TOF_VALID_MAX_MM 4000
#define TOF_XSHUT_PIN 4
#define TOF_XSHUT_SHUTDOWN_MS 20
#define TOF_XSHUT_BOOT_MS 50

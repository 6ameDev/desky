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

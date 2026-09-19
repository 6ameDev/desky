#pragma once
// desky v2 global config — no hardcoded pins here.
// All board specifics come from the active MCU definition.

#include "mcu/active_mcu.h"

// Motor PWM (mirrors MCU defaults, tunable per behavior)
#define CFG_PWM_FREQ_HZ MCU_PWM_FREQ_HZ
#define CFG_PWM_RES_BITS MCU_PWM_RESOLUTION_BITS
#define CFG_PWM_MIN_DUTY MCU_PWM_MIN_DUTY

// I2C bus
#define CFG_I2C_SDA MCU_I2C_SDA
#define CFG_I2C_SCL MCU_I2C_SCL
#define CFG_I2C_FREQ_HZ MCU_I2C_FREQ_HZ
#define CFG_I2C_TIMEOUT_MS MCU_I2C_TIMEOUT_MS

// Task rates (v2 architecture target)
#define CFG_MOTOR_LOOP_HZ 100
#define CFG_MPU_RATE_HZ 50
#define CFG_TOF_RATE_HZ 20
#define CFG_DISPLAY_FPS 25

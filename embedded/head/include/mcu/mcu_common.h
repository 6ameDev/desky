#pragma once
// MCU capability flags — shared contract for all MCU definitions.
// To swap MCU later: add a new <name>.h in this folder and point
// include/mcu/active_mcu.h at it. No driver/fusion/behavior changes.

#include <stdint.h>

// Each MCU header must define:
//   MCU_NAME (string), MCU_NUM_CORES, MCU_CPU_FREQ_MHZ,
//   MCU_FLASH_SIZE_MB, MCU_HAS_PSRAM, MCU_HAS_NATIVE_USB, MCU_HAS_CAMERA,
//   plus pin/bus maps and constraint comments.

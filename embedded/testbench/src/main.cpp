#include <Arduino.h>
#include <Wire.h>

#include "config.h"

// desky-testbench: stripped-down POC rig on the ESP32 DevKit v1 (30-pin).
// Boot contract: Serial banner -> I2C scan -> LED breathe. An empty I2C
// bus is a VALID result (the part under test is absent or DOA) — scan
// failures never block boot or the breathe.
//
// Per-part workflow: add a temporary src/hal/<part>_poc.h + the pinned
// lib_dep it needs, hook it into loop() behind a clearly-marked block,
// prove the part, then DELETE the POC file so the rig returns to this
// baseline. This file stays lean on purpose.

// GPIO2 doubles as a strapping pin: PWM-driving it as an LED output is
// fine, but never attach external pulldowns/pullups to it (boot-sampled).
static void scanBus();
// Breathe state: triangle ramp 0 -> CFG_LED_MAX_DUTY (quarter power) -> 0.
static int s_duty = 0;
static int s_dir = 1;
static uint32_t s_breathCount = 0;

static void stepBreathe() {
  ledcWrite(MCU_LED_PIN, s_duty);
  s_duty += s_dir;
  if (s_duty >= CFG_LED_MAX_DUTY) {
    s_duty = CFG_LED_MAX_DUTY;
    s_dir = -1;
  } else if (s_duty <= 0) {
    s_duty = 0;
    s_dir = 1;
    ++s_breathCount;
    Serial.printf("TESTBENCH alive #%lu duty=0\r\n", (unsigned long)s_breathCount);
    if (s_breathCount % CFG_SCAN_EVERY_N_BREATHS == 0) {
      scanBus();
    }
  }
}

static void scanBus() {
  uint8_t found = 0;
  Serial.printf("I2C scan SDA=%d SCL=%d @%luHz:\r\n", CFG_I2C_SDA, CFG_I2C_SCL, (unsigned long)CFG_I2C_FREQ_HZ);
  for (uint8_t addr = 0x03; addr <= 0x77; ++addr) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  found 0x%02X\r\n", addr);
      ++found;
    }
  }
  Serial.printf("I2C scan done: %u device(s)\r\n", found);
}

void setup() {
  Serial.begin(CFG_CLI_BAUD);
  // Brief settle so the first banner lines survive the USB-UART reset noise.
  delay(200);
  Serial.printf("\r\ndesky-testbench alive | MCU=%s cores=%d flash=%dMB psram=%d\r\n", MCU_NAME, MCU_NUM_CORES,
                MCU_FLASH_SIZE_MB, MCU_HAS_PSRAM);
  Serial.printf("reset=%d sdk=%s rev=%d cpu=%dMHz flash_chip=%uB\r\n", (int)esp_reset_reason(), ESP.getSdkVersion(),
                (int)ESP.getChipRevision(), (int)ESP.getCpuFreqMHz(), (unsigned int)ESP.getFlashChipSize());
  Wire.begin(CFG_I2C_SDA, CFG_I2C_SCL, CFG_I2C_FREQ_HZ);
  scanBus();
  const bool ledOk = ledcAttach(MCU_LED_PIN, CFG_LED_FREQ_HZ, CFG_LED_RES_BITS);
  ledcWrite(MCU_LED_PIN, 0);
  Serial.printf("pins i2c=%d,%d led=%d pwm=%luHz res=%ubit max_duty=%d %s\r\n", MCU_I2C_SDA, MCU_I2C_SCL, MCU_LED_PIN,
                (unsigned long)CFG_LED_FREQ_HZ, CFG_LED_RES_BITS, CFG_LED_MAX_DUTY, ledOk ? "ok" : "ATTACH-FAILED");
}

void loop() {
  stepBreathe();
  delay(CFG_LED_STEP_MS);
}

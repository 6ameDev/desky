#include <Arduino.h>
#include <Wire.h>

#include "config.h"
#include "hal/mic_poc.h"
#include "hal/tcrt_poc.h"

// desky-testbench: stripped-down POC rig on the ESP32 DevKit v1 (30-pin).
// Boot contract: Serial banner -> I2C scan -> LED breathe + mic meter. An
// empty I2C bus or a dead mic are VALID results (part absent or DOA) —
// neither blocks boot, the breathe, or the surviving meter.
//
// Per-part workflow: add a temporary src/hal/<part>_poc.h + the pinned
// lib_dep it needs, hook it into loop() behind a clearly-marked block,
// prove the part, then DELETE the POC file so the rig returns to this
// baseline. This file stays lean on purpose.

// GPIO2 doubles as a strapping pin: PWM-driving it as an LED output is
// fine, but never attach external pulldowns/pullups to it (boot-sampled).
static void scanBus();
static uint32_t s_breathCount;  // zero-initialized; stepped by stepBreathe below
// STAGE 1 POC block (temporary — delete with src/hal/mic_poc.h after the POC).
namespace {
MicRx g_mic(CFG_I2S_WS, CFG_I2S_BCLK, CFG_I2S_DIN, CFG_I2S_DOUT, CFG_AUDIO_RATE_HZ);
uint32_t g_micBuf[2 * CFG_MIC_BATCH_FRAMES];
uint32_t s_lastStepMs = 0;
uint32_t s_lastMeterMs = 0;
uint32_t s_lastTcrtMs = 0;   // TCRT POC: raw-count calibration logger
bool s_tcrtSurface = false;  // hysteresis rail verdict (see src/hal/tcrt_poc.h)
#if !CFG_DEMO_MODE
uint32_t s_lastBeepMs = 0;
bool s_beepHigh = false;
#else
uint32_t s_peakL = 0;  // demo peak-hold: verdict latches once a slot proves itself
uint32_t s_peakR = 0;
#endif
}  // namespace

#if CFG_DEMO_MODE
// Render one 16-char bar into out (out must hold 17 bytes).
static void renderBar(uint32_t rms, char* out) {
  const uint32_t chars = demopoc::barChars(rms);
  for (uint32_t i = 0; i < demopoc::kBarWidth; ++i) {
    out[i] = i < chars ? '#' : '.';
  }
  out[demopoc::kBarWidth] = '\0';
}
#endif

// One meter tick: drain a 16ms batch, print per-slot RMS. Stage 1 reads the
// verdict by eye — energy in ONE slot only (which slot = the L/R tie); both
// flat zero = dead board (not "quiet").
static void meterTick() {
  const size_t got = g_mic.readFrames(g_micBuf, CFG_MIC_BATCH_FRAMES, 100);
  if (got == 0) {
    Serial.printf("MIC read timeout (0 frames)\r\n");
    return;
  }
  const uint32_t rmsL = micpoc::slotRms(g_micBuf, got, micpoc::kLeftSlot);
  const uint32_t rmsR = micpoc::slotRms(g_micBuf, got, micpoc::kRightSlot);
#if CFG_DEMO_MODE
  if (rmsL > s_peakL) {
    s_peakL = rmsL;
  }
  if (rmsR > s_peakR) {
    s_peakR = rmsR;
  }
  char barL[17], barR[17];
  renderBar(rmsL, barL);
  renderBar(rmsR, barR);
  // Single-line in-place meter: a bare carriage return rewrites the same
  // terminal line — no ANSI codes, so every serial monitor handles it.
  // Fixed widths everywhere and no trailing newline, so the line persists
  // until the next tick overwrites it.
  const bool liveL = demopoc::slotLive(s_peakL);
  const bool liveR = demopoc::slotLive(s_peakR);
  Serial.printf("L [%s] %7lu %s | R [%s] %7lu %s | up %lus\r", barL, (unsigned long)rmsL, liveL ? "LIVE  " : "SILENT",
                barR, (unsigned long)rmsR, liveR ? "LIVE  " : "SILENT", (unsigned long)(millis() / 1000u));
#else
  Serial.printf("MIC rmsL=%lu rmsR=%lu frames=%u\r\n", (unsigned long)rmsL, (unsigned long)rmsR, (unsigned)got);
#endif
}

#if !CFG_DEMO_MODE
// STAGE S2 POC: one quiet beep, chunked (256-frame synth -> pack -> write so
// no second buffer and no heap). Blocking ~CFG_SPK_BEEP_MS; the breathe LED
// simply steps on return (invisible pause).
static void playBeep(uint16_t freqHz) {
  Serial.printf("BEEP %uHz\r\n", freqHz);
  const uint32_t total = (CFG_AUDIO_RATE_HZ * CFG_SPK_BEEP_MS) / 1000u;
  int16_t mono[256];
  int32_t stereo[2 * 256];
  uint32_t pos = 0;
  while (pos < total) {
    uint32_t n = total - pos;
    if (n > 256) {
      n = 256;
    }
    for (uint32_t i = 0; i < n; ++i) {
      mono[i] = spkpoc::synthSine(pos + i, total, freqHz, CFG_AUDIO_RATE_HZ, CFG_SPK_PEAK);
    }
    spkpoc::packMono32(mono, stereo, n);
    if (g_mic.writeFrames(stereo, n, 100) == 0) {
      Serial.printf("BEEP write timeout\r\n");
      return;
    }
    pos += n;
  }
}
#endif
// Breathe state: triangle ramp 0 -> CFG_LED_MAX_DUTY (quarter power) -> 0.
static int s_duty = 0;
static int s_dir = 1;

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
#if !CFG_DEMO_MODE
    Serial.printf("TESTBENCH alive #%lu duty=0\r\n", (unsigned long)s_breathCount);
    if (s_breathCount % CFG_SCAN_EVERY_N_BREATHS == 0) {
      scanBus();
    }
#endif
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
  // TCRT POC: explicit ADC setup (12-bit, 11dB ~= full 3V3 swing) so the
  // calibration counts never depend on framework defaults. Drain tap parked
  // HIGH (emitter off; LOW = on).
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  pinMode(CFG_TCRT_DRAIN_PIN, OUTPUT);
  digitalWrite(CFG_TCRT_DRAIN_PIN, HIGH);
  Serial.printf("tcrt tap=GPIO%d drain=GPIO%d pairs=%u settle=%uus (LOW=on)\r\n", CFG_TCRT_PIN, CFG_TCRT_DRAIN_PIN,
                CFG_TCRT_PAIRS, CFG_TCRT_SETTLE_US);
  const bool ledOk = ledcAttach(MCU_LED_PIN, CFG_LED_FREQ_HZ, CFG_LED_RES_BITS);
  ledcWrite(MCU_LED_PIN, 0);
  Serial.printf("pins i2c=%d,%d led=%d pwm=%luHz res=%ubit max_duty=%d %s\r\n", MCU_I2C_SDA, MCU_I2C_SCL, MCU_LED_PIN,
                (unsigned long)CFG_LED_FREQ_HZ, CFG_LED_RES_BITS, CFG_LED_MAX_DUTY, ledOk ? "ok" : "ATTACH-FAILED");
  // STAGE 1 POC: single INMP441 alive + slot check (see src/hal/mic_poc.h).
  // Never asserted: a silent/dead mic is the result being tested for.
  const bool micOk = g_mic.init();
  Serial.printf("mic i2s ws=%d sck=%d sd=%d rate=%luHz %s\r\n", CFG_I2S_WS, CFG_I2S_BCLK, CFG_I2S_DIN,
                (unsigned long)CFG_AUDIO_RATE_HZ, micOk ? "ok" : "INIT-FAILED (check wiring/power)");
  Serial.printf("spk i2s dout=%d %s\r\n", CFG_I2S_DOUT, g_mic.txReady() ? "tx-ready" : "tx-off (mic meter unaffected)");
#if CFG_DEMO_MODE
  Serial.printf("DEMO: mic A = L/R->GND (LEFT slot), mic B = L/R->3V3 (RIGHT slot)\r\n");
  Serial.printf("DEMO: rub the acoustic port ~3s per mic — the meter line below updates in place\r\n");
#else
  if (micOk) {
    Serial.printf("mic: one board only — expect energy in ONE slot (L/R tie decides which)\r\n");
  }
  if (g_mic.txReady()) {
    Serial.printf("spk: quiet beeps alternate %d/%dHz every %ums (Vin 3V3, ~5%% FS)\r\n", CFG_SPK_FREQ_A_HZ,
                  CFG_SPK_FREQ_B_HZ, CFG_SPK_EVERY_MS);
  }
#endif
}

void loop() {
  const uint32_t now = millis();
  if (now - s_lastStepMs >= CFG_LED_STEP_MS) {
    s_lastStepMs = now;
    stepBreathe();
  }
  if (g_mic.isReady() && now - s_lastMeterMs >= CFG_MIC_METER_MS) {
    s_lastMeterMs = now;
    meterTick();
  }
  // TCRT POC, modulated: on/off pair reads minus ambient. The rail runs on
  // signal-space (reflective = HIGH — polarity flipped vs raw counts).
  if (now - s_lastTcrtMs >= CFG_TCRT_READ_MS) {
    s_lastTcrtMs = now;
    const tcrtpoc::ModSample s =
        tcrtpoc::readModulated(CFG_TCRT_DRAIN_PIN, CFG_TCRT_PIN, CFG_TCRT_SETTLE_US, CFG_TCRT_PAIRS);
    s_tcrtSurface =
        tcrtpoc::railUpdate(s_tcrtSurface, s.signal, CFG_TCRT_THRESHOLD, CFG_TCRT_HYSTERESIS, CFG_TCRT_ASSERT_HIGH);
    Serial.printf("TCRT on=%u off=%u sig=%u %s\r\n", s.on, s.off, s.signal, s_tcrtSurface ? "SURFACE" : "VOID");
  }
#if !CFG_DEMO_MODE
  if (g_mic.txReady() && now - s_lastBeepMs >= CFG_SPK_EVERY_MS) {
    s_lastBeepMs = now;
    s_beepHigh = !s_beepHigh;
    playBeep(s_beepHigh ? CFG_SPK_FREQ_B_HZ : CFG_SPK_FREQ_A_HZ);
  }
#endif
}

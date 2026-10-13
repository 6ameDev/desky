#pragma once
// desky-testbench TCRT POC rail (discrete pair, COMPONENTS.md §5 circuit) —
// TEMPORARY POC file: prove the part, then DELETE so the rig returns to the
// lean baseline (see src/main.cpp workflow note).
//
// Hysteresis rail over raw ADC counts. Bands BENCH-MEASURED on this rig's
// pair (DevKit V1, GPIO33, 12-bit @11dB, 220Ω emitter): white ~20-170,
// void ~4095. Threshold/hysteresis below encode that split — re-pin them if
// the floor material or ride height changes. Logic adapted from core's
// tcrt::railUpdate (already host-tested there); the POC owns its copy.
//
// Polarity (confirmed on the bench, not assumed): the collector pull-up
// tap sinks with reflection, so surface present <=> LOW counts.

#include <stdint.h>

namespace tcrtpoc {

// Symmetric hysteresis rail around threshold (ADC counts). assertHigh=false
// (our circuit — tap sinks with reflection): a cleared rail asserts surface
// at <= threshold-hysteresis, an asserted rail clears at >=
// threshold+hysteresis. Everything between holds. int32 arithmetic: a
// hysteresis wider than the threshold saturates, never wraps the uint16
// band edges (which would freeze the rail).
inline bool railUpdate(bool level, uint16_t counts, uint16_t threshold, uint16_t hyst, bool assertHigh = false) {
  const int32_t hi = static_cast<int32_t>(threshold) + static_cast<int32_t>(hyst);
  const int32_t lo = static_cast<int32_t>(threshold) - static_cast<int32_t>(hyst);
  const int32_t c = static_cast<int32_t>(counts);
  if (assertHigh) {
    if (!level && c >= hi) {
      return true;
    }
    if (level && c <= lo) {
      return false;
    }
    return level;
  }
  if (!level && c <= lo) {
    return true;
  }
  if (level && c >= hi) {
    return false;
  }
  return level;
}

// Ambient subtraction core (pure: averages in, signal out). The tap sinks
// with reflection, so emitter-on reads LOWER than emitter-off: signal =
// off - on, clamped at zero (a negative difference is noise, not light).
inline uint16_t subtractSignal(uint32_t avgOn, uint32_t avgOff) {
  return static_cast<uint16_t>(avgOff > avgOn ? avgOff - avgOn : 0u);
}

}  // namespace tcrtpoc

#ifdef ARDUINO
// ── Firmware: modulated pair reader ─────────────────────────────────────────
// Emitter on (drain LOW) -> settle -> read; emitter off (drain HIGH) ->
// settle -> read; pairs averaged, then ambient subtracted. The detector tap
// sinks with reflection, so signal+ambient reads LOWER than ambient alone:
// signal = off - on (clamped at zero), i.e. reflected-light counts with the
// room subtracted out. Callers pinMode the drain pin OUTPUT once (HIGH = off
// default) before the first read.
#include <Arduino.h>

namespace tcrtpoc {

struct ModSample {
  uint16_t on = 0;      // emitter-on average (signal + ambient)
  uint16_t off = 0;     // emitter-off average (ambient alone)
  uint16_t signal = 0;  // reflected-light counts, ambient cancelled
};

inline ModSample readModulated(int drainPin, int adcPin, uint32_t settleUs, uint32_t pairs) {
  uint32_t accOn = 0;
  uint32_t accOff = 0;
  const uint32_t n = pairs > 0 ? pairs : 1u;
  for (uint32_t i = 0; i < n; ++i) {
    digitalWrite(drainPin, LOW);  // emitter on
    delayMicroseconds(settleUs);
    accOn += static_cast<uint32_t>(analogRead(adcPin));
    digitalWrite(drainPin, HIGH);  // emitter off
    delayMicroseconds(settleUs);
    accOff += static_cast<uint32_t>(analogRead(adcPin));
  }
  ModSample s;
  s.on = static_cast<uint16_t>(accOn / n);
  s.off = static_cast<uint16_t>(accOff / n);
  s.signal = subtractSignal(accOn / n, accOff / n);
  return s;
}

}  // namespace tcrtpoc

#endif  // ARDUINO

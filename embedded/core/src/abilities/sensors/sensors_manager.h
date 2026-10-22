#pragma once
// Sensors manager — abilities/sensors.
//
// Owns the production Fusion instance built from fusion::kBoardMounting plus
// a thin evaluate(snapshot, state) entry point shared by the SensorTask and
// the telemetry poll. Pure logic only: zero sockets, zero NVS, zero RNG.

#include "sensors_codec.h"

namespace sensors {

// Single production Fusion instance (function-local static). Built once from
// the canonical board mounting; stateless, safe to call from any task.
inline const fusion::Fusion& productionFusion() {
  static const fusion::Fusion instance(fusion::kBoardMounting);
  return instance;
}

// Thin production entry point: evaluate a snapshot into state via the shared
// instance above.
inline void evaluate(const fusion::SensorSnapshot& snap, SystemState& state) {
  productionFusion().evaluate(snap, state);
}

}  // namespace sensors

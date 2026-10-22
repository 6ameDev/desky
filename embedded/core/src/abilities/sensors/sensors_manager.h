#pragma once
// desky v2 sensors manager — abilities/sensors (ability layer).
//
// Owns the production Fusion instance built from fusion::kBoardMounting plus
// a thin evaluate(snapshot, state) entry point wrapping it, so the two
// current call sites (SensorTask bottom half, services/TelemetryTask::poll)
// share one home instead of each constructing Fusion locally. Pure logic only:
// zero sockets, zero NVS, zero RNG. Fusion math + edge helpers live in
// sensors_codec.h; the FreeRTOS binder lives in sensors_task.h.
//
// Transitional (Slice 3): both call sites still construct
// fusion::Fusion{fusion::kBoardMounting} locally, verbatim from the
// middleware move — they switch to sensors::evaluate in a later slice.

#include "sensors_codec.h"

namespace sensors {

// Single production Fusion instance (function-local static: one instance
// program-wide for inline functions). Built once from the canonical board
// mounting; stateless, safe to call from any task context.
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

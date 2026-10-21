#pragma once
// desky-head face renderer — thin compatibility shim (Task 3 split).
//
// The face ability now lives as a codec/manager/task triple:
//   src/abilities/face/face_codec.h   (pure geometry/state, Arduino-free)
//   src/abilities/face/face_manager.h (overlay state machine, no sockets/GFX)
//   src/abilities/face/face_task.h    (FaceRenderer RTOS render binding)
// Existing host tests include this path; it stays until Task 6 deletes it.
// New code includes the abilities path directly.

#include "abilities/face/face_codec.h"
#include "abilities/face/face_manager.h"
#include "abilities/face/face_task.h"

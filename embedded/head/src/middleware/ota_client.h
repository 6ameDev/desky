#pragma once
// desky-head OTA client — MIDDLEWARE compatibility shim (Task 5 split,
// deleted in Task 6).
//
// The OTA ability now lives as a codec/manager/task triple under
// abilities/ota/ (pure URL/version/magic/sha/state in ota_codec.h, lifecycle
// policy in ota_manager.h, the WiFiClient/Update pump binding in
// ota_task.h). This shim re-exports those headers — including the OtaUpdater
// class name — so existing host tests including this path pass with zero
// test changes. New code includes abilities/ota/ota_task.h directly.
//
// STANDING RULE — do NOT ship a head image without this client: once the
// single USB session ends the head has no wired door (the MB board occupies
// every pin; field units expose no USB), so an image without the OTA path is
// unflashable except by re-opening the unit. main.cpp references this rule
// where the updater is constructed.

#include "abilities/ota/ota_codec.h"
#include "abilities/ota/ota_manager.h"
#include "abilities/ota/ota_task.h"

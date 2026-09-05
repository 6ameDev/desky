#include "RobotState.h"
#include <Preferences.h>

static Preferences s_prefs;
static bool s_prefsBegun = false;

static void ensurePrefs() {
    if (!s_prefsBegun) {
        s_prefs.begin("desky", false);
        s_prefsBegun = true;
    }
}

RobotStateStore::RobotStateStore() {
    _mutex = xSemaphoreCreateMutex();
    ensurePrefs();
    int saved = (int)s_prefs.getInt("maxPower", DEFAULT_MAX_POWER_PERCENT);
    _state.maxPowerPercent = constrain(saved, MIN_MAX_POWER_PERCENT, MAX_MAX_POWER_PERCENT);
}

ControlState RobotStateStore::getState() {
    ControlState copy;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    copy = _state;
    xSemaphoreGive(_mutex);
    return copy;
}

void RobotStateStore::updateDriveCommand(int8_t x, int8_t y) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    // Scale the full-scale D-pad vector (-100..100 -> -255..255) by the
    // configured max-power ceiling, e.g. 50% caps output at ~127.
    float ceiling = constrain(_state.maxPowerPercent, MIN_MAX_POWER_PERCENT, MAX_MAX_POWER_PERCENT) / 100.0f;
    _state.targetLeftSpeed = constrain((y + x) * 2.55f * ceiling, -255, 255);
    _state.targetRightSpeed = constrain((y - x) * 2.55f * ceiling, -255, 255);
    _state.lastCommandTime = millis();
    xSemaphoreGive(_mutex);
}

void RobotStateStore::toggleEBrake() {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _state.isEBrake = !_state.isEBrake;
    _state.targetLeftSpeed = 0;
    _state.targetRightSpeed = 0;
    xSemaphoreGive(_mutex);
}

void RobotStateStore::setCliffThreshold(int thresholdMM) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _state.cliffThresholdMM = thresholdMM;
    xSemaphoreGive(_mutex);
}

void RobotStateStore::setMaxPower(int percent) {
    percent = constrain(percent, MIN_MAX_POWER_PERCENT, MAX_MAX_POWER_PERCENT);
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _state.maxPowerPercent = percent;
    xSemaphoreGive(_mutex);
    ensurePrefs();
    s_prefs.putInt("maxPower", percent);
}

void RobotStateStore::updateTelemetry(int distanceMM, bool isCliff, bool isFault, const String& status) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _state.currentDistanceMM = distanceMM;
    _state.isCliff = isCliff;
    _state.isFault = isFault;
    _state.status = status;
    xSemaphoreGive(_mutex);
}

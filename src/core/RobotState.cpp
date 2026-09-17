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
}

void RobotStateStore::begin() {
    ensurePrefs();
    int saved = (int)s_prefs.getInt("maxPower", DEFAULT_MAX_POWER_PERCENT);
    int orient = (int)s_prefs.getInt("imuOrient", 0);
    float pitchOff = s_prefs.getFloat("imuPitchOff", 0);
    float rollOff = s_prefs.getFloat("imuRollOff", 0);
    bool mpuEn = s_prefs.getBool("mpuEn", (bool)MPU_ENABLED_DEFAULT);
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _state.maxPowerPercent = constrain(saved, MIN_MAX_POWER_PERCENT, MAX_MAX_POWER_PERCENT);
    _state.imuOrientation = constrain(orient, 0, 3);
    _state.imuPitchOffset = constrain(pitchOff, -45.0f, 45.0f);
    _state.imuRollOffset = constrain(rollOff, -45.0f, 45.0f);
    _state.mpuEnabled = mpuEn;
    xSemaphoreGive(_mutex);
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

void RobotStateStore::requestWiggle(int dir, int pairs) {
    if (dir < 0 || dir > 2) {
        return;
    }
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _state.wiggleRequestDir = dir;
    _state.wiggleRequestPairs = constrain(pairs, 1, 6);
    xSemaphoreGive(_mutex);
}

bool RobotStateStore::takeWiggleRequest(int& dir, int& pairs) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    dir = _state.wiggleRequestDir;
    pairs = _state.wiggleRequestPairs;
    _state.wiggleRequestDir = -1;
    _state.wiggleRequestPairs = 0;
    xSemaphoreGive(_mutex);
    return dir >= 0;
}

void RobotStateStore::setDisplayWorried(uint32_t untilMs) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _state.displayWorriedUntilMs = untilMs;
    xSemaphoreGive(_mutex);
}

void RobotStateStore::toggleDisplayDebug() {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _state.displayDebugOn = !_state.displayDebugOn;
    // Debug wins: clear forced mood so eyes don't clash
    if (_state.displayDebugOn) _state.displayMoodOverride = -1;
    xSemaphoreGive(_mutex);
}

void RobotStateStore::setDisplayMood(int mood) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    if (mood < 0 || mood > 3) mood = -1;
    if (_state.displayMoodOverride == mood) _state.displayMoodOverride = -1;
    else _state.displayMoodOverride = mood;
    if (mood >= 0) _state.displayDebugOn = false;
    xSemaphoreGive(_mutex);
}

void RobotStateStore::requestDisplayAnim(int anim) {
    if (anim < 1 || anim > 4) return;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _state.displayAnimRequest = anim;
    xSemaphoreGive(_mutex);
}

bool RobotStateStore::takeDisplayAnim(int &anim) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    anim = _state.displayAnimRequest;
    _state.displayAnimRequest = 0;
    xSemaphoreGive(_mutex);
    return anim != 0;
}

void RobotStateStore::setMpuEnabled(bool enabled) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _state.mpuEnabled = enabled;
    xSemaphoreGive(_mutex);
    ensurePrefs();
    s_prefs.putBool("mpuEn", enabled);
}

void RobotStateStore::toggleMpuEnabled() {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _state.mpuEnabled = !_state.mpuEnabled;
    bool en = _state.mpuEnabled;
    xSemaphoreGive(_mutex);
    ensurePrefs();
    s_prefs.putBool("mpuEn", en);
}

void RobotStateStore::updateTelemetry(int distanceMM, bool isCliff, bool isFault, bool tofFault, const String& status) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _state.currentDistanceMM = distanceMM;
    _state.isCliff = isCliff;
    _state.isFault = isFault;
    _state.tofFault = tofFault;
    _state.status = status;
    xSemaphoreGive(_mutex);
}

void RobotStateStore::requestTofRecovery(int mode) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _state.tofRecoveryRequest = mode;
    xSemaphoreGive(_mutex);
}

int RobotStateStore::takeTofRecoveryRequest() {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    int req = _state.tofRecoveryRequest;
    _state.tofRecoveryRequest = 0;
    xSemaphoreGive(_mutex);
    return req;
}

void RobotStateStore::cycleImuOrientation() {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _state.imuOrientation = (_state.imuOrientation + 1) % 4;
    _state.imuPitchOffset = 0;
    _state.imuRollOffset = 0;
    int orient = _state.imuOrientation;
    xSemaphoreGive(_mutex);
    ensurePrefs();
    s_prefs.putInt("imuOrient", orient);
    s_prefs.putFloat("imuPitchOff", 0);
    s_prefs.putFloat("imuRollOff", 0);
}

void RobotStateStore::requestImuCalibrate() {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _state.imuCalibrateRequested = true;
    xSemaphoreGive(_mutex);
}

bool RobotStateStore::takeImuCalibrateRequest() {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    bool req = _state.imuCalibrateRequested;
    _state.imuCalibrateRequested = false;
    xSemaphoreGive(_mutex);
    return req;
}

void RobotStateStore::setImuOffsets(float pitchOffset, float rollOffset) {
    pitchOffset = constrain(pitchOffset, -45.0f, 45.0f);
    rollOffset = constrain(rollOffset, -45.0f, 45.0f);
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _state.imuPitchOffset = pitchOffset;
    _state.imuRollOffset = rollOffset;
    xSemaphoreGive(_mutex);
    ensurePrefs();
    s_prefs.putFloat("imuPitchOff", pitchOffset);
    s_prefs.putFloat("imuRollOff", rollOffset);
}

void RobotStateStore::updateImu(const ImuReading& reading) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _state.imu = reading;
    xSemaphoreGive(_mutex);
}

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
    float shakeGG = s_prefs.getFloat("shakeGG", SHAKE_GENTLE_G);
    float shakeGgyro = s_prefs.getFloat("shakeGGyro", SHAKE_GENTLE_GYRO_DPS);
    float shakeAG = s_prefs.getFloat("shakeAG", SHAKE_ANGRY_G);
    float shakeAgyro = s_prefs.getFloat("shakeAGyro", SHAKE_ANGRY_GYRO_DPS);
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _state.maxPowerPercent = constrain(saved, MIN_MAX_POWER_PERCENT, MAX_MAX_POWER_PERCENT);
    _state.imuOrientation = constrain(orient, 0, 3);
    _state.imuPitchOffset = constrain(pitchOff, -45.0f, 45.0f);
    _state.imuRollOffset = constrain(rollOff, -45.0f, 45.0f);
    _state.mpuEnabled = mpuEn;
    _state.shakeGentleG = constrain(shakeGG, 1.01f, 2.5f);
    _state.shakeGentleGyro = constrain(shakeGgyro, 1.0f, 150.0f);
    _state.shakeAngryG = constrain(shakeAG, 1.01f, 3.5f);
    _state.shakeAngryGyro = constrain(shakeAgyro, 1.0f, 350.0f);
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

void RobotStateStore::setDisplayAngry(uint32_t untilMs) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _state.displayAngryUntilMs = untilMs;
    if (untilMs != 0) _state.displayDebugOn = false;
    xSemaphoreGive(_mutex);
}

void RobotStateStore::setDisplayHappy(uint32_t untilMs) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _state.displayHappyUntilMs = untilMs;
    if (untilMs != 0) _state.displayDebugOn = false;
    xSemaphoreGive(_mutex);
}

void RobotStateStore::wakeFromSleep() {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _state.displayMoodOverride = -1;
    _state.displayDebugOn = false;
    _state.displayWakeResetMs = millis();
    xSemaphoreGive(_mutex);
}

void RobotStateStore::setShakeThresholds(float gentleG, float gentleGyro, float angryG, float angryGyro) {
    gentleG = constrain(gentleG, 1.01f, 2.5f);
    gentleGyro = constrain(gentleGyro, 1.0f, 150.0f);
    angryG = constrain(angryG, 1.01f, 3.5f);
    angryGyro = constrain(angryGyro, 1.0f, 350.0f);
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _state.shakeGentleG = gentleG;
    _state.shakeGentleGyro = gentleGyro;
    _state.shakeAngryG = angryG;
    _state.shakeAngryGyro = angryGyro;
    xSemaphoreGive(_mutex);
    ensurePrefs();
    s_prefs.putFloat("shakeGG", gentleG);
    s_prefs.putFloat("shakeGGyro", gentleGyro);
    s_prefs.putFloat("shakeAG", angryG);
    s_prefs.putFloat("shakeAGyro", angryGyro);
}

void RobotStateStore::setShakeGentle(float g, float gyro) {
    g = constrain(g, 1.01f, 2.5f);
    gyro = constrain(gyro, 1.0f, 150.0f);
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _state.shakeGentleG = g;
    _state.shakeGentleGyro = gyro;
    xSemaphoreGive(_mutex);
    ensurePrefs();
    s_prefs.putFloat("shakeGG", g);
    s_prefs.putFloat("shakeGGyro", gyro);
}

void RobotStateStore::setShakeAngry(float g, float gyro) {
    g = constrain(g, 1.01f, 3.5f);
    gyro = constrain(gyro, 1.0f, 350.0f);
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _state.shakeAngryG = g;
    _state.shakeAngryGyro = gyro;
    xSemaphoreGive(_mutex);
    ensurePrefs();
    s_prefs.putFloat("shakeAG", g);
    s_prefs.putFloat("shakeAGyro", gyro);
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
    if (mood < 0 || mood > 5) mood = -1;
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

void RobotStateStore::updateTelemetryMotion(const String& status) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    bool moving = (status == "DRIVING" || status == "WIGGLE!" || status == "CLIFF WIGGLE!");
    if (moving) _state.lastMotionMs = millis();
    xSemaphoreGive(_mutex);
}

void RobotStateStore::announceInput(InputType type, float accelMag, float gyroZ, float pitch, float roll) {
    if (type == INPUT_NONE) return;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    // Per-input cooldown already checked in detector, but keep single pending slot: drop if pending not consumed
    if (_state.pendingInput.type != INPUT_NONE) {
        xSemaphoreGive(_mutex);
        return;
    }
    _state.pendingInput.type = type;
    _state.pendingInput.timeMs = millis();
    _state.pendingInput.accelMag = accelMag;
    _state.pendingInput.gyroZ = gyroZ;
    _state.pendingInput.pitch = pitch;
    _state.pendingInput.roll = roll;
    if (type == INPUT_NUDGED) _state.lastNudgedMs = millis();
    else if (type == INPUT_SHAKEN) _state.lastShakenMs = millis();
    xSemaphoreGive(_mutex);
}

bool RobotStateStore::takeInput(InputEvent &out) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    out = _state.pendingInput;
    bool has = out.type != INPUT_NONE;
    _state.pendingInput.type = INPUT_NONE;
    xSemaphoreGive(_mutex);
    return has;
}

void RobotStateStore::markDirectCommand() {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _state.lastDirectMs = millis();
    _state.lastCommandTime = millis();
    xSemaphoreGive(_mutex);
}

bool RobotStateStore::isMotionCooldownActive() const {
    auto *self = const_cast<RobotStateStore*>(this);
    xSemaphoreTake(self->_mutex, portMAX_DELAY);
    bool active = (millis() - self->_state.lastMotionMs < (unsigned long)MOTION_COOLDOWN_MS);
    bool moving = (self->_state.status == "DRIVING" || self->_state.status == "WIGGLE!" || self->_state.status == "CLIFF WIGGLE!");
    bool cool = moving || active;
    xSemaphoreGive(self->_mutex);
    return cool;
}

bool RobotStateStore::isDirectActive(unsigned long windowMs) const {
    // const method but needs mutex - cast away const for lock
    auto *self = const_cast<RobotStateStore*>(this);
    xSemaphoreTake(self->_mutex, portMAX_DELAY);
    bool active = (millis() - self->_state.lastDirectMs < windowMs);
    xSemaphoreGive(self->_mutex);
    return active;
}

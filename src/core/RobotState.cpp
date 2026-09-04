#include "RobotState.h"

RobotStateStore::RobotStateStore() {
    _mutex = xSemaphoreCreateMutex();
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
    _state.targetLeftSpeed = constrain((y + x) * 2.55, -255, 255);
    _state.targetRightSpeed = constrain((y - x) * 2.55, -255, 255);
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

void RobotStateStore::updateTelemetry(int distanceMM, bool isCliff, bool isFault, const String& status) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _state.currentDistanceMM = distanceMM;
    _state.isCliff = isCliff;
    _state.isFault = isFault;
    _state.status = status;
    xSemaphoreGive(_mutex);
}

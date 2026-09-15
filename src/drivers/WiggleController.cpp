#include "WiggleController.h"
#include "Config.h"

WiggleController::WiggleController(MotorDriver& motors) : _motors(motors) {}

void WiggleController::start(WiggleDirection dir, int pairs) {
    if (_active) {
        return;
    }
    if (pairs < 1) {
        pairs = 1;
    }
    _dir = dir;
    _totalPhases = pairs * 2;
    _baseSpeed = (dir == WiggleDirection::FORWARD) ? WIGGLE_FORWARD_SPEED
               : (dir == WiggleDirection::BACKWARD) ? WIGGLE_REVERSE_SPEED : 0;
    _openLeft = !_openLeft;
    _startMs = millis();
    _active = true;
}

bool WiggleController::update() {
    if (!_active) {
        return true;
    }
    unsigned long elapsed = millis() - _startMs;
    unsigned long phase = elapsed / WIGGLE_HALF_PERIOD_MS;
    if (phase >= (unsigned long)_totalPhases) {
        _active = false;
        return true;
    }
    bool phaseLeft = ((phase % 2) == 0) == _openLeft;
    _motors.driveWiggle(_baseSpeed, WIGGLE_SWAY_DELTA, phaseLeft);
    return false;
}

void WiggleController::abort() {
    _active = false;
}

bool WiggleController::isActive() const {
    return _active;
}

#pragma once
#include <Arduino.h>
#include "Config.h"

struct ControlState {
    int targetLeftSpeed = 0;
    int targetRightSpeed = 0;
    int currentDistanceMM = 0;
    int cliffThresholdMM = DEFAULT_CLIFF_LIMIT_MM;
    bool isCliff = false;
    bool isFault = false;
    bool isEBrake = false;
    String status = "STOPPED";
    unsigned long lastCommandTime = 0;
};

class RobotStateStore {
public:
    RobotStateStore();
    ControlState getState();
    
    void updateDriveCommand(int8_t x, int8_t y);
    void toggleEBrake();
    void setCliffThreshold(int thresholdMM);
    void updateTelemetry(int distanceMM, bool isCliff, bool isFault, const String& status);

private:
    ControlState _state;
    SemaphoreHandle_t _mutex;
};

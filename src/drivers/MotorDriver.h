#pragma once
#include <Arduino.h>

class MotorDriver {
public:
    MotorDriver(int in1, int in2, int in3, int in4, int faultPin);
    void begin();
    void drive(int leftSpeed, int rightSpeed);
    void applyBrake();
    bool isFaultActive();

private:
    int _in1, _in2, _in3, _in4, _faultPin;
    int applyFloor(int speed);
};

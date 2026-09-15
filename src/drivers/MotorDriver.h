#pragma once
#include <Arduino.h>

class MotorDriver {
public:
    MotorDriver(int in1, int in2, int in3, int in4, int faultPin);
    void begin();
    void drive(int leftSpeed, int rightSpeed);
    // Wiggle motion primitive: drives at baseSpeed with an alternating
    // left/right sway bias. baseSpeed > 0 wiggles forward, 0 wiggles
    // in place, < 0 wiggles backward. phaseLeft selects the sway side;
    // flip it on a timer (see WIGGLE_HALF_PERIOD_MS) for butt-wiggle.
    void driveWiggle(int baseSpeed, int swayDelta, bool phaseLeft);
    void applyBrake();
    bool isFaultActive();

private:
    int _in1, _in2, _in3, _in4, _faultPin;
    int applyFloor(int speed);
};

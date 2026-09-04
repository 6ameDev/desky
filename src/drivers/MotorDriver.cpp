#include "MotorDriver.h"
#include "Config.h"

MotorDriver::MotorDriver(int in1, int in2, int in3, int in4, int faultPin)
    : _in1(in1), _in2(in2), _in3(in3), _in4(in4), _faultPin(faultPin) {}

void MotorDriver::begin() {
    pinMode(_faultPin, INPUT_PULLUP);
    ledcAttach(_in1, PWM_FREQ, PWM_RESOLUTION);
    ledcAttach(_in2, PWM_FREQ, PWM_RESOLUTION);
    ledcAttach(_in3, PWM_FREQ, PWM_RESOLUTION);
    ledcAttach(_in4, PWM_FREQ, PWM_RESOLUTION);
    applyBrake();
}

bool MotorDriver::isFaultActive() {
    return (digitalRead(_faultPin) == LOW);
}

int MotorDriver::applyFloor(int speed) {
    if (abs(speed) < 10) return 0;
    int mag = map(abs(speed), 10, 255, MIN_MOTOR_PWM, 255);
    return (speed > 0) ? mag : -mag;
}

void MotorDriver::drive(int leftSpeed, int rightSpeed) {
    leftSpeed = applyFloor(leftSpeed);
    rightSpeed = applyFloor(rightSpeed);

    if (leftSpeed > 0) { ledcWrite(_in1, constrain(leftSpeed, 0, 255)); ledcWrite(_in2, 0); }
    else if (leftSpeed < 0) { ledcWrite(_in1, 0); ledcWrite(_in2, constrain(-leftSpeed, 0, 255)); }
    else { ledcWrite(_in1, 0); ledcWrite(_in2, 0); }

    if (rightSpeed > 0) { ledcWrite(_in3, constrain(rightSpeed, 0, 255)); ledcWrite(_in4, 0); }
    else if (rightSpeed < 0) { ledcWrite(_in3, 0); ledcWrite(_in4, constrain(-rightSpeed, 0, 255)); }
    else { ledcWrite(_in3, 0); ledcWrite(_in4, 0); }
}

void MotorDriver::applyBrake() {
    ledcWrite(_in1, 255); ledcWrite(_in2, 255);
    ledcWrite(_in3, 255); ledcWrite(_in4, 255);
}

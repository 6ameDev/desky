#pragma once
#include <Arduino.h>
// Vendored from playfultechnology/esp32-eyes EyeConfig.h (GPL-3.0)
// Original: https://github.com/playfultechnology/esp32-eyes
struct EyeConfig {
    int16_t OffsetX = 0;
    int16_t OffsetY = 0;
    int16_t Height = 34;
    int16_t Width = 34;
    float Slope_Top = 0;
    float Slope_Bottom = 0;
    int16_t Radius_Top = 10;
    int16_t Radius_Bottom = 10;
    int16_t Inverse_Radius_Top = 0;
    int16_t Inverse_Radius_Bottom = 0;
    int16_t Inverse_Offset_Top = 0;
    int16_t Inverse_Offset_Bottom = 0;
};

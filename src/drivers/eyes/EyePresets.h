#pragma once
#include "EyeConfig.h"
// Focused preset from playfultechnology/esp32-eyes EyePresets.h (GPL-3.0) — other presets pruned (unused)
// Original: https://github.com/playfultechnology/esp32-eyes/blob/main/EyePresets.h

static const EyeConfig Preset_Focused = {
    .OffsetX = 0, .OffsetY = 0,
    .Height = 14, .Width = 40,
    .Slope_Top = 0.2f, .Slope_Bottom = 0,
    .Radius_Top = 3, .Radius_Bottom = 1,
    .Inverse_Radius_Top = 0, .Inverse_Radius_Bottom = 0,
    .Inverse_Offset_Top = 0, .Inverse_Offset_Bottom = 0
};


// Neutral already exists as default 34x34 r10; kept for reference
static const EyeConfig Preset_Normal = {
    .OffsetX = 0, .OffsetY = 0,
    .Height = 34, .Width = 34,
    .Slope_Top = 0, .Slope_Bottom = 0,
    .Radius_Top = 10, .Radius_Bottom = 10,
    .Inverse_Radius_Top = 0, .Inverse_Radius_Bottom = 0,
    .Inverse_Offset_Top = 0, .Inverse_Offset_Bottom = 0
};
static const EyeConfig Preset_Sleeping = {
    .OffsetX = 0, .OffsetY = 0,
    .Height = 1, .Width = 34,
    .Slope_Top = 0, .Slope_Bottom = 0,
    .Radius_Top = 0, .Radius_Bottom = 0,
    .Inverse_Radius_Top = 0, .Inverse_Radius_Bottom = 0,
    .Inverse_Offset_Top = 0, .Inverse_Offset_Bottom = 0
};

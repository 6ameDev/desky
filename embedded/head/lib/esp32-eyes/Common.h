#ifndef COMMON_h
#define COMMON_h

#include <U8g2lib.h>

// Single-U8G2 adaptation: Ssd1306Display owns the only instance (bus-mutex push); bound at renderer init.
extern U8G2* eyesU8g2;

#endif
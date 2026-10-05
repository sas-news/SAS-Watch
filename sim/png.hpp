#pragma once

#include <cstdint>

// RGB565 (little-endian uint16) のフレームを PNG (RGB888) で書く。
bool write_png_rgb565(const char* path, const uint16_t* fb, int w, int h);

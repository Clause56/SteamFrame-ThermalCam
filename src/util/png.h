// Minimal dependency-free PNG writer (uncompressed deflate blocks).
#pragma once

#include <cstdint>
#include <string>

bool writePng(const std::string& path, const uint8_t* rgba, int width, int height);

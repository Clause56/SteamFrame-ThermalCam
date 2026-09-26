// Tiny logger: writes to stderr and, once opened, to a log file that is
// flushed to disk line by line, so the last steps before a crash survive.
#pragma once

#include <string>

void logOpen(const std::string& path);
void logMsg(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

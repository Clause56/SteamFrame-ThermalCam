#include "log.h"

#include <unistd.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace {
FILE* gFile = nullptr;
std::mutex gMutex;
const auto gStart = std::chrono::steady_clock::now();
}  // namespace

void logOpen(const std::string& path) {
    std::lock_guard<std::mutex> l(gMutex);
    if (gFile) fclose(gFile);
    gFile = fopen(path.c_str(), "w");
}

void logMsg(const char* fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - gStart).count();
    std::lock_guard<std::mutex> l(gMutex);
    fprintf(stderr, "%s\n", msg);
    if (gFile) {
        fprintf(gFile, "[%8.3f] %s\n", t, msg);
        fflush(gFile);
        fsync(fileno(gFile));
    }
}

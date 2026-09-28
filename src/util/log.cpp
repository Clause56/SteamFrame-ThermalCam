#include "log.h"

#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace {
FILE* gFile = nullptr;
std::mutex gMutex;
const auto gStart = std::chrono::steady_clock::now();
}  // namespace

void logOpen(const std::string& path) {
    std::lock_guard<std::mutex> l(gMutex);
    if (gFile) fclose(gFile);
    // Keep earlier runs (a relaunch right after a crash or Quit is exactly
    // what we want to see), but roll over once the file gets large.
    struct stat st;
    if (stat(path.c_str(), &st) == 0 && st.st_size > 1024 * 1024)
        rename(path.c_str(), (path + ".old").c_str());
    gFile = fopen(path.c_str(), "a");
    if (gFile) {
        char when[64];
        time_t t = time(nullptr);
        strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", localtime(&t));
        fprintf(gFile, "\n===== %s  pid %d =====\n", when, int(getpid()));
        fflush(gFile);
    }
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
        fprintf(gFile, "[%d %8.3f] %s\n", int(getpid()), t, msg);
        fflush(gFile);
        fsync(fileno(gFile));
    }
}

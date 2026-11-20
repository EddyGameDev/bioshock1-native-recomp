#pragma once

// Mirrors log lines into a file the launcher can share via EXPORT LOGS.
// The path is published by the Java side with nativeSetLogFile() via setenv().
#include <android/log.h>
#include <cstdio>
#include <cstdlib>

namespace bs1::renderer::gles3 {

inline void Bs1FileLogLine(const char* message) {
  const char* path = std::getenv("BS1_LOG_FILE");
  if (path == nullptr || path[0] == '\0') return;
  std::FILE* f = std::fopen(path, "a");
  if (f == nullptr) return;
  std::fprintf(f, "%s\n", message);
  std::fclose(f);
}

}  // namespace bs1::renderer::gles3

#define BS1_GLES_LOG_IMPL(priority, ...)                                          \
  do {                                                                           \
    char bs1_log_buf[2048];                                                      \
    std::snprintf(bs1_log_buf, sizeof(bs1_log_buf), __VA_ARGS__);                 \
    __android_log_print(priority, "BS1-GLES3", "%s", bs1_log_buf);                \
    ::bs1::renderer::gles3::Bs1FileLogLine(bs1_log_buf);                          \
  } while (0)

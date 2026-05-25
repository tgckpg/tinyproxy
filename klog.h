#ifndef KLOG_H
#define KLOG_H

void log_at(char sev, const char *file, int line, const char *fmt, ...);

#define LOG_INFO(...)  log_at('I', __FILE__, __LINE__, __VA_ARGS__)
#define LOG_WARN(...)  log_at('W', __FILE__, __LINE__, __VA_ARGS__)
#define LOG_ERROR(...) log_at('E', __FILE__, __LINE__, __VA_ARGS__)

#endif

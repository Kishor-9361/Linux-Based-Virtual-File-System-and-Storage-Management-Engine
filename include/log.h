#ifndef MINIFS_LOG_H
#define MINIFS_LOG_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef enum {
    LOG_LEVEL_NONE  = 0,
    LOG_LEVEL_ERROR = 1,
    LOG_LEVEL_WARN  = 2,
    LOG_LEVEL_INFO  = 3,
    LOG_LEVEL_DEBUG = 4
} log_level_t;

static inline log_level_t minifs_get_log_level(void) {
    const char *env = getenv("MINIFS_LOG");
    if (!env) return LOG_LEVEL_ERROR;
    if (strcasecmp(env, "DEBUG") == 0) return LOG_LEVEL_DEBUG;
    if (strcasecmp(env, "INFO") == 0)  return LOG_LEVEL_INFO;
    if (strcasecmp(env, "WARN") == 0)  return LOG_LEVEL_WARN;
    if (strcasecmp(env, "ERROR") == 0) return LOG_LEVEL_ERROR;
    if (strcasecmp(env, "NONE") == 0)  return LOG_LEVEL_NONE;
    int num = atoi(env);
    if (num >= 0 && num <= 4) return (log_level_t)num;
    return LOG_LEVEL_ERROR;
}

#define LOG_ERROR(fmt, ...) do { \
    if (minifs_get_log_level() >= LOG_LEVEL_ERROR) { \
        fprintf(stderr, "[ERROR] %s:%d: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
    } \
} while(0)

#define LOG_WARN(fmt, ...) do { \
    if (minifs_get_log_level() >= LOG_LEVEL_WARN) { \
        fprintf(stderr, "[WARN] %s:%d: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
    } \
} while(0)

#define LOG_INFO(fmt, ...) do { \
    if (minifs_get_log_level() >= LOG_LEVEL_INFO) { \
        fprintf(stderr, "[INFO] " fmt "\n", ##__VA_ARGS__); \
    } \
} while(0)

#define LOG_DEBUG(fmt, ...) do { \
    if (minifs_get_log_level() >= LOG_LEVEL_DEBUG) { \
        fprintf(stderr, "[DEBUG] %s:%d: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
    } \
} while(0)

#endif /* MINIFS_LOG_H */

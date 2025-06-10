#ifndef PLAYER_H
#define PLAYER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LOG_LEVEL_ERROR = 0,
    LOG_LEVEL_INFO  = 1,
    LOG_LEVEL_DEBUG = 2
} log_level_t;

extern int CURRENT_LOG_LEVEL;

#define LOG_ERROR(fmt, ...) \
    do { \
        if (CURRENT_LOG_LEVEL >= LOG_LEVEL_ERROR) \
            fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__); \
    } while (0)

#define LOG_INFO(fmt, ...) \
    do { \
        if (CURRENT_LOG_LEVEL >= LOG_LEVEL_INFO) \
            printf("[INFO] " fmt "\n", ##__VA_ARGS__); \
    } while (0)

#define LOG_DEBUG(fmt, ...) \
    do { \
        if (CURRENT_LOG_LEVEL >= LOG_LEVEL_DEBUG) \
            printf("[DEBUG] " fmt "\n", ##__VA_ARGS__); \
    } while (0)

int player_init(void);

int player_play(const char* uri);

int player_pause(void);

int player_resume(void);

int player_stop(void);

int player_seek(int seconds);

int player_get_position(int* current_sec, int* total_sec);

int player_get_volume(void);

int player_set_volume(int volume);

int player_is_playing(void);

int player_deinit(void);

#ifdef __cplusplus
}
#endif

#endif // PLAYER_H

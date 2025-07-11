#ifndef PLAYER_H
#define PLAYER_H

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <glib.h>
#include <gst/gst.h>
#include <glib/gprintf.h>
#include <alsa/asoundlib.h>

#include <pthread.h>
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

int player_set_mute(int mute);

int player_get_mute(int *mute);

int player_set_volume(int volume);

int player_is_playing(void);

int player_deinit(void);

int run_main_loop(void);

int set_hw_volume_from_gst(double volume, const char *card, const char *selem_name);

void list_mixer_controls(const char *card);

GOptionGroup* player_get_option_group(void);
#ifdef __cplusplus
}
#endif

#endif // PLAYER_H

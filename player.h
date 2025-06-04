#ifndef PLAYER_H
#define PLAYER_H

#include <stdint.h>
#include <alsa/asoundlib.h>

#ifdef __cplusplus
extern "C" {
#endif

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

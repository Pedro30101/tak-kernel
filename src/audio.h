#ifndef AUDIO_H
#define AUDIO_H
#include "tak.h"

int audio_init(void);
int audio_present(void);
void audio_start(void);
void audio_pause(int on);
void audio_stop(void);
void audio_poll(void);
int audio_space(void);
void audio_put(const i16 *frames, int n);
int audio_buffered(void);
void audio_volume(int pct);
extern char audio_name[48];
#endif

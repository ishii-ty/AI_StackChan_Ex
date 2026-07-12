#ifndef _PLAY_WAV_H
#define _PLAY_WAV_H

#include <Arduino.h>
#include <AudioGeneratorWAV.h>

extern AudioGeneratorWAV *wav;

extern void wav_init(void);
extern bool playWavHttp(const String& url);

#endif

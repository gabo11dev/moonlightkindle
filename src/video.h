#pragma once
#include <Limelight.h>
#include <stdint.h>
extern DECODER_RENDERER_CALLBACKS kindle_decoder;
/* Caller owns returned RGB pixels. A single latest frame bounds latency/memory. */
uint8_t *video_take_frame(int *width, int *height);
void video_discard_frame(void);
void video_set_grayscale(int enabled);
unsigned long video_decoded_count(void);

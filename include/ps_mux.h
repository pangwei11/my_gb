#pragma once
#include "globals.h"

int generate_ps_packet(uint8_t *ps_buffer, int buf_size,
                       const uint8_t *h264_data, int h264_size,
                       int64_t pts_90k, int is_keyframe,
                       int *ps_size);

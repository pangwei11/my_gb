#pragma once
#include "globals.h"

void frame_queue_init(FrameQueue *q);
void frame_queue_clean(FrameQueue *q);
int  frame_queue_get_count(FrameQueue *q);
int  frame_queue_push(FrameQueue *q, const uint8_t *data, int size, int64_t pts, int is_keyframe);
int  frame_queue_pop(FrameQueue *q, FrameData *out, int timeout_ms);
void frame_queue_clear(FrameQueue *q);

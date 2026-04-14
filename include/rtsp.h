#pragma once
#include "globals.h"

void rtsp_preheat_init(void);
void rtsp_preheat_cleanup(void);

void* rtsp_pull_thread(void *arg);

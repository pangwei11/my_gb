#pragma once
#include "globals.h"

void* sip_thread(void *arg);
int   parse_sdp_ip_port(const char *sdp, SdpParseResult *result);

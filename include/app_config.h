#pragma once

#include <stdbool.h>

// Runtime configuration loaded from config.json (with config.h as defaults).

typedef enum {
    TS_MONOTONIC = 0,
    TS_FIXED_STEP = 1,
} TsMode;

typedef enum {
    TRANS_UDP = 0,
    TRANS_TCP = 1,
} TransportMode;

typedef enum {
    TCP_PASSIVE = 0,
    TCP_ACTIVE = 1,
} TcpMode;

typedef struct {
    char device_id[32];
    char channel_id[32];
    char password[64];
    char realm[64];

    char local_ip[64];
    int  local_sip_port;
    int  local_rtp_port; // 0=random

    // 200OK SDP o=/c= address. Allow 0.0.0.0 for WVP compatibility.
    char sdp_ip[64];
} DeviceConfig;

typedef struct {
    char sip_ip[64];
    int  sip_port;
    char platform_id[32];
} PlatformConfig;

typedef struct {
    char rtsp_url[256];
    int  rtp_payload_type;
    int  rtp_max_packet_size;
    TransportMode transport;
    TcpMode tcp_mode;
} MediaConfig;

typedef struct {
    bool wait_for_ack;
    TsMode ts_mode;
    bool enable_filler_idr;
    bool dump_sip_raw;
} WvpCompatConfig;

typedef struct {
    int level; // 0=debug 1=info 2=warn 3=error
    char file[256];
} LogConfig;

typedef struct {
    DeviceConfig device;
    PlatformConfig platform;
    MediaConfig media;
    WvpCompatConfig wvp;
    LogConfig log;
} AppConfig;

// Fill defaults from include/config.h
void app_config_set_defaults(AppConfig *cfg);

// Load JSON and override defaults. Missing fields keep defaults.
// Returns 0 on success, <0 on error (file missing/parse error).
int app_config_load_json(AppConfig *cfg, const char *path);

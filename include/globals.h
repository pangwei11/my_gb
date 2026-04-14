#pragma once
#include "common.h"
#include "app_config.h"

typedef struct {
    atomic_int running;
    atomic_int streaming;
    atomic_int waiting_for_ack;
    atomic_int need_idr;
    atomic_int call_ended;
    // Whether we have already answered a BYE for the current dialog (or the stack already did).
    // Used to avoid duplicate 200 OK on retransmissions / event reordering.
    atomic_int bye_answered;
    atomic_int connection_broken;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} ProgramState;

typedef struct {
    uint16_t seq;
    uint32_t ssrc;
    int sock;
    struct sockaddr_in dst_addr;
    pthread_mutex_t lock;
    int local_port;
    int payload_type;
    int is_tcp;
    int tcp_connected;
    int tcp_passive_mode;
    int tcp_listen_sock;

    char tcp_target_ip[32];
    int tcp_target_port;
    atomic_int tcp_connect_failed;

    atomic_ullong last_rtp_send_ts;
    atomic_ullong last_rtp_attempt_ts;
    atomic_int rtp_drop_count;
    atomic_int rtp_packets_sent;
    atomic_int rtp_send_errors;
} RtpContext;

typedef struct {
    uint8_t *data;
    int size;
    int64_t pts;       // 90k
    int is_keyframe;
} FrameData;

void state_init(void);


typedef struct {
    FrameData frames[MAX_FRAME_QUEUE];
    int front;
    int rear;
    int count;
    int drop_count;
    int drop_until_keyframe;
    int push_delay_us;
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
} FrameQueue;

typedef struct {
    char ip[32];
    int port;
    char ssrc_str[16];
    uint32_t ssrc;
    int has_ssrc;
    int payload_type;

    char origin_ip[32];
    int has_origin_ip;

    int transport_tcp;
    int has_setup;
    int setup_passive;
    int is_actpass;
} SdpParseResult;

typedef struct {
    uint64_t last_send_time_ms;
    int last_sn;
    int retry_count;
    atomic_int pending_response;
} KeepaliveState;

// ---- globals (defined in globals.c) ----
extern ProgramState g_state;
extern RtpContext g_rtp_ctx;
extern FrameQueue g_frame_queue;

extern struct eXosip_t *g_exosip_ctx;
extern pthread_mutex_t g_call_id_mutex;
extern int g_dialog_id;
extern int g_call_id;

extern AVBSFContext *g_bsf_ctx;

extern atomic_int g_need_send_bye;
extern atomic_int g_tcp_connecting;

extern uint64_t g_wait_ack_start_ms;
extern uint64_t g_invite_start_ms;
extern uint64_t g_first_frame_sent_ms;

extern KeepaliveState g_keepalive_state;
extern atomic_int g_registered;
extern uint64_t g_last_reg_success_ms;
extern uint64_t g_last_reg_attempt_ms;
extern uint64_t g_next_reg_retry_ms;
extern int g_register_retries;
extern int g_reg_failure_count;
extern int g_reg_id;

extern int g_sn_counter;
extern int g_keepalive_sn;
extern uint64_t g_keepalive_send_time_ms;
extern char g_last_keepalive_call_id[128];

extern char g_civil_code[7];

extern int g_last_psm_sent;

// Runtime config (defaults from config.h, overrides from config.json)
extern AppConfig g_cfg;
extern int64_t g_pts_offset;
extern int64_t g_first_rtp_pts;

extern FILE *g_log_file;
extern pthread_mutex_t g_log_mutex;

extern volatile sig_atomic_t g_signal_received;

// TCP connect timeout control
#define TCP_CONNECT_TIMEOUT_MS 10000
extern uint64_t g_tcp_wait_start_ms;

// no RTP monitor
extern int g_no_rtp_count;

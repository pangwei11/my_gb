#include "globals.h"

AppConfig g_cfg; // runtime config

ProgramState g_state = {
    .running = ATOMIC_VAR_INIT(1),
    .streaming = ATOMIC_VAR_INIT(0)
};

// 运行时初始化锁和条件变量
void state_init(void) {
    // 留空即可
}


RtpContext g_rtp_ctx;
FrameQueue g_frame_queue;

struct eXosip_t *g_exosip_ctx = NULL;
pthread_mutex_t g_call_id_mutex = PTHREAD_MUTEX_INITIALIZER;
int g_call_id = -1;
int g_dialog_id = -1;

AVBSFContext *g_bsf_ctx = NULL;

atomic_int g_need_send_bye = ATOMIC_VAR_INIT(0);
atomic_int g_tcp_connecting = ATOMIC_VAR_INIT(0);

uint64_t g_wait_ack_start_ms = 0;
uint64_t g_invite_start_ms = 0;
uint64_t g_first_frame_sent_ms = 0;

KeepaliveState g_keepalive_state = {0, 0, 0, ATOMIC_VAR_INIT(0)};
atomic_int g_registered = ATOMIC_VAR_INIT(0);
uint64_t g_last_reg_success_ms = 0;
uint64_t g_last_reg_attempt_ms = 0;
uint64_t g_next_reg_retry_ms = 0;
int g_register_retries = 0;
int g_reg_failure_count = 0;
int g_reg_id = 0;

int g_sn_counter = 0;
int g_keepalive_sn = 0;
uint64_t g_keepalive_send_time_ms = 0;
char g_last_keepalive_call_id[128] = {0};

char g_civil_code[7] = {0};

int g_last_psm_sent = 0;
int64_t g_pts_offset = 0;
int64_t g_first_rtp_pts = 0;

FILE *g_log_file = NULL;
pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

volatile sig_atomic_t g_signal_received = 0;

uint64_t g_tcp_wait_start_ms = 0;

int g_no_rtp_count = 0;

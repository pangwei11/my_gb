#include "app_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"  // defaults
#include "jsmn.h"

static int read_file_all(const char *path, char **out_buf, size_t *out_len) {
    *out_buf = NULL;
    *out_len = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 1024 * 1024) { fclose(f); return -2; }
    char *buf = (char*)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return -3; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return -4; }
    fclose(f);
    buf[n] = 0;
    *out_buf = buf;
    *out_len = (size_t)n;
    return 0;
}

static int jsoneq(const char *json, const jsmntok_t *tok, const char *s) {
    if (tok->type != JSMN_STRING) return 0;
    int len = tok->end - tok->start;
    return (int)strlen(s) == len && strncmp(json + tok->start, s, (size_t)len) == 0;
}

static void tok_to_str(char *dst, size_t cap, const char *json, const jsmntok_t *tok) {
    if (!dst || cap == 0) return;
    if (!tok || tok->start < 0 || tok->end < tok->start) return;
    size_t n = (size_t)(tok->end - tok->start);
    if (n >= cap) n = cap - 1;
    memcpy(dst, json + tok->start, n);
    dst[n] = 0;
}

static int tok_to_int(const char *json, const jsmntok_t *tok, int *out) {
    if (!tok || tok->type != JSMN_PRIMITIVE) return -1;
    char tmp[64] = {0};
    tok_to_str(tmp, sizeof(tmp), json, tok);
    *out = atoi(tmp);
    return 0;
}

static int tok_to_bool(const char *json, const jsmntok_t *tok, int *out01) {
    if (!tok || tok->type != JSMN_PRIMITIVE) return -1;
    int len = tok->end - tok->start;
    if (len == 4 && strncmp(json + tok->start, "true", 4) == 0) { *out01 = 1; return 0; }
    if (len == 5 && strncmp(json + tok->start, "false", 5) == 0) { *out01 = 0; return 0; }
    return -2;
}

// Find value token index of key inside an object token.
// Returns token index of the value, or -1 if not found.
static int obj_get(const char *json, const jsmntok_t *toks, int tok_count, int obj_i, const char *key) {
    if (obj_i < 0 || obj_i >= tok_count) return -1;
    if (toks[obj_i].type != JSMN_OBJECT) return -1;
    // In jsmn, object children are: key, value, key, value...
    int i = obj_i + 1;
    int pairs = toks[obj_i].size;
    for (int p = 0; p < pairs; p++) {
        if (i >= tok_count) return -1;
        const jsmntok_t *k = &toks[i];
        const jsmntok_t *v = &toks[i + 1];
        if (jsoneq(json, k, key)) return i + 1;
        // skip key+value; if value is object/array, need to skip its subtree
        i += 2;
        // Skip nested tokens belonging to v
        if (v->type == JSMN_OBJECT || v->type == JSMN_ARRAY) {
            int end = v->end;
            while (i < tok_count && toks[i].start < end) i++;
        }
    }
    return -1;
}

static int root_get_obj(const char *json, const jsmntok_t *toks, int tok_count, const char *key) {
    int v = obj_get(json, toks, tok_count, 0, key);
    if (v < 0) return -1;
    if (toks[v].type != JSMN_OBJECT) return -1;
    return v;
}

static TransportMode parse_transport(const char *s) {
    if (!s || !s[0]) return TRANS_UDP;
    return (strcmp(s, "tcp") == 0) ? TRANS_TCP : TRANS_UDP;
}
static TcpMode parse_tcp_mode(const char *s) {
    if (!s || !s[0]) return TCP_PASSIVE;
    return (strcmp(s, "active") == 0) ? TCP_ACTIVE : TCP_PASSIVE;
}
static TsMode parse_ts_mode(const char *s) {
    if (!s || !s[0]) return TS_MONOTONIC;
    return (strcmp(s, "fixed_step") == 0) ? TS_FIXED_STEP : TS_MONOTONIC;
}
static int parse_log_level(const char *s, int defv) {
    if (!s || !s[0]) return defv;
    if (strcmp(s, "debug") == 0) return 0;
    if (strcmp(s, "info") == 0) return 1;
    if (strcmp(s, "warn") == 0) return 2;
    if (strcmp(s, "error") == 0) return 3;
    return defv;
}

void app_config_set_defaults(AppConfig *cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));

    //device 模块（设备自身配置）
    snprintf(cfg->device.device_id, sizeof(cfg->device.device_id), "%s", DEVICE_ID);        // 设备ID
    snprintf(cfg->device.channel_id, sizeof(cfg->device.channel_id), "%s", CHANNEL_ID);     // 通道ID
    snprintf(cfg->device.password, sizeof(cfg->device.password), "%s", SIP_AUTH_PASSWORD);  // SIP认证密码
    snprintf(cfg->device.realm, sizeof(cfg->device.realm), "%s", PLATFORM_REALM);           // 认证域
    snprintf(cfg->device.local_ip, sizeof(cfg->device.local_ip), "%s", LOCAL_IP);           // 本地IP
    cfg->device.local_sip_port = LOCAL_SIP_PORT;                                            // 本地SIP端口
    cfg->device.local_rtp_port = LOCAL_RTP_PORT;                                            // 本地RTP端口
    snprintf(cfg->device.sdp_ip, sizeof(cfg->device.sdp_ip), "%s", SDP_IP);                 // SDP消息携带的IP

    //platform 模块（国标平台对接配置）
    snprintf(cfg->platform.sip_ip, sizeof(cfg->platform.sip_ip), "%s", PLATFORM_SIP_IP);    // 平台SIP IP
    cfg->platform.sip_port = PLATFORM_SIP_PORT;                                             // 平台SIP端口
    snprintf(cfg->platform.platform_id, sizeof(cfg->platform.platform_id), "%s", PLATFORM_ID);  // 平台ID

    //media 模块（媒体流传输配置）
    snprintf(cfg->media.rtsp_url, sizeof(cfg->media.rtsp_url), "%s", RTSP_URL);             // 拉流RTSP地址
    cfg->media.rtp_payload_type = RTP_PAYLOAD_TYPE;                                         // RTP载荷类型
    cfg->media.rtp_max_packet_size = RTP_MAX_PACKET_SIZE;                                   // RTP最大包大小
    cfg->media.transport = (SIP_TRANSPORT_UDP ? TRANS_UDP : TRANS_TCP);                     // 传输协议
    cfg->media.tcp_mode = TCP_PASSIVE;                                                      // TCP模式：默认被动模式

    // wvp 模块（WVP 平台兼容配置）
    cfg->wvp.wait_for_ack = (WAIT_FOR_ACK ? true : false);                                  // 是否等待ACK
#if defined(WVP_TS_MODE_FIXED_STEP) && WVP_TS_MODE_FIXED_STEP                               // 条件编译：控制时间戳模式默认值
    cfg->wvp.ts_mode = TS_FIXED_STEP;                                                       // 定义并启用宏：默认固定步长时间戳
#else
    cfg->wvp.ts_mode = TS_MONOTONIC;                                                        // 未定义/未启用：默认单调递增时间戳
#endif
    cfg->wvp.enable_filler_idr = (WVP_ENABLE_FILLER_IDR ? true : false);                    // 启用填充IDR帧
    cfg->wvp.dump_sip_raw = (WVP_DUMP_SIP_RAW ? true : false);                              // 打印原始SIP消息

    //log 模块（日志配置）
    cfg->log.level = 1;                                                                     //// 日志级别默认1,对应info
    snprintf(cfg->log.file, sizeof(cfg->log.file), "%s", "/tmp/gb28181_wvp.log");           // 日志文件路径：默认/tmp/gb28181_wvp.lo
}

int app_config_load_json(AppConfig *cfg, const char *path) {
    if (!cfg || !path) return -1;

    char *json = NULL;
    size_t json_len = 0;
    int rr = read_file_all(path, &json, &json_len);                                           //调用read_file_all将整个 JSON 文件读取到内存缓冲区
    if (rr != 0) return -2;

    // token count: adjust if your config grows
    // 初始化解析器
    jsmn_parser p;
    jsmn_init(&p);
    const int MAX_TOK = 512;
    jsmntok_t *toks = (jsmntok_t*)calloc((size_t)MAX_TOK, sizeof(jsmntok_t));               //// 分配token数组（calloc自动置0）
    if (!toks) { free(json); return -3; }

    // 解析JSON为token数组
    int tok_count = jsmn_parse(&p, json, json_len, toks, (unsigned int)MAX_TOK);
    if (tok_count < 0) { free(toks); free(json); return -4; }
    if (tok_count < 1 || toks[0].type != JSMN_OBJECT) { free(toks); free(json); return -5; }

    //分模块解析 JSON 配置（核心逻辑）
    // device
    int dev = root_get_obj(json, toks, tok_count, "device");            //// 找根对象下的"device"子对象
    if (dev >= 0) {
        int v;
        // 字符串字段（device_id/channel_id等）：安全拷贝到结构体（自动截断超长内容）
        if ((v = obj_get(json, toks, tok_count, dev, "device_id")) >= 0 && toks[v].type == JSMN_STRING)
            tok_to_str(cfg->device.device_id, sizeof(cfg->device.device_id), json, &toks[v]);
        if ((v = obj_get(json, toks, tok_count, dev, "channel_id")) >= 0 && toks[v].type == JSMN_STRING)
            tok_to_str(cfg->device.channel_id, sizeof(cfg->device.channel_id), json, &toks[v]);
        if ((v = obj_get(json, toks, tok_count, dev, "password")) >= 0 && toks[v].type == JSMN_STRING)
            tok_to_str(cfg->device.password, sizeof(cfg->device.password), json, &toks[v]);
        if ((v = obj_get(json, toks, tok_count, dev, "realm")) >= 0 && toks[v].type == JSMN_STRING)
            tok_to_str(cfg->device.realm, sizeof(cfg->device.realm), json, &toks[v]);
        if ((v = obj_get(json, toks, tok_count, dev, "local_ip")) >= 0 && toks[v].type == JSMN_STRING)
            tok_to_str(cfg->device.local_ip, sizeof(cfg->device.local_ip), json, &toks[v]);
        if ((v = obj_get(json, toks, tok_count, dev, "sdp_ip")) >= 0 && toks[v].type == JSMN_STRING)
            tok_to_str(cfg->device.sdp_ip, sizeof(cfg->device.sdp_ip), json, &toks[v]);
        if ((v = obj_get(json, toks, tok_count, dev, "local_sip_port")) >= 0)
            tok_to_int(json, &toks[v], &cfg->device.local_sip_port);
        if ((v = obj_get(json, toks, tok_count, dev, "local_rtp_port")) >= 0)
            tok_to_int(json, &toks[v], &cfg->device.local_rtp_port);
    }

    // platform
    int pf = root_get_obj(json, toks, tok_count, "platform");
    if (pf >= 0) {
        int v;
        if ((v = obj_get(json, toks, tok_count, pf, "sip_ip")) >= 0 && toks[v].type == JSMN_STRING)
            tok_to_str(cfg->platform.sip_ip, sizeof(cfg->platform.sip_ip), json, &toks[v]);
        if ((v = obj_get(json, toks, tok_count, pf, "sip_port")) >= 0)
            tok_to_int(json, &toks[v], &cfg->platform.sip_port);
        if ((v = obj_get(json, toks, tok_count, pf, "platform_id")) >= 0 && toks[v].type == JSMN_STRING)
            tok_to_str(cfg->platform.platform_id, sizeof(cfg->platform.platform_id), json, &toks[v]);
    }

    // media
    int m = root_get_obj(json, toks, tok_count, "media");
    if (m >= 0) {
        int v;
        if ((v = obj_get(json, toks, tok_count, m, "rtsp_url")) >= 0 && toks[v].type == JSMN_STRING)
            tok_to_str(cfg->media.rtsp_url, sizeof(cfg->media.rtsp_url), json, &toks[v]);
        if ((v = obj_get(json, toks, tok_count, m, "rtp_payload_type")) >= 0)
            tok_to_int(json, &toks[v], &cfg->media.rtp_payload_type);
        if ((v = obj_get(json, toks, tok_count, m, "rtp_max_packet_size")) >= 0)
            tok_to_int(json, &toks[v], &cfg->media.rtp_max_packet_size);

        if ((v = obj_get(json, toks, tok_count, m, "transport")) >= 0 && toks[v].type == JSMN_STRING) {
            char tmp[16] = {0};
            tok_to_str(tmp, sizeof(tmp), json, &toks[v]);
            cfg->media.transport = parse_transport(tmp);
        }
        if ((v = obj_get(json, toks, tok_count, m, "tcp_mode")) >= 0 && toks[v].type == JSMN_STRING) {
            char tmp[16] = {0};
            tok_to_str(tmp, sizeof(tmp), json, &toks[v]);
            cfg->media.tcp_mode = parse_tcp_mode(tmp);
        }
    }

    // wvp_compat
    int w = root_get_obj(json, toks, tok_count, "wvp_compat");
    if (w >= 0) {
        int v, b;
        if ((v = obj_get(json, toks, tok_count, w, "wait_for_ack")) >= 0 && tok_to_bool(json, &toks[v], &b) == 0)
            cfg->wvp.wait_for_ack = (b != 0);
        if ((v = obj_get(json, toks, tok_count, w, "enable_filler_idr")) >= 0 && tok_to_bool(json, &toks[v], &b) == 0)
            cfg->wvp.enable_filler_idr = (b != 0);
        if ((v = obj_get(json, toks, tok_count, w, "dump_sip_raw")) >= 0 && tok_to_bool(json, &toks[v], &b) == 0)
            cfg->wvp.dump_sip_raw = (b != 0);
        if ((v = obj_get(json, toks, tok_count, w, "ts_mode")) >= 0 && toks[v].type == JSMN_STRING) {
            char tmp[24] = {0};
            tok_to_str(tmp, sizeof(tmp), json, &toks[v]);
            cfg->wvp.ts_mode = parse_ts_mode(tmp);
        }
    }

    // log
    int lg = root_get_obj(json, toks, tok_count, "log");
    if (lg >= 0) {
        int v;
        if ((v = obj_get(json, toks, tok_count, lg, "file")) >= 0 && toks[v].type == JSMN_STRING)
            tok_to_str(cfg->log.file, sizeof(cfg->log.file), json, &toks[v]);
        if ((v = obj_get(json, toks, tok_count, lg, "level")) >= 0) {
            if (toks[v].type == JSMN_STRING) {
                char tmp[16] = {0};
                tok_to_str(tmp, sizeof(tmp), json, &toks[v]);
                cfg->log.level = parse_log_level(tmp, cfg->log.level);
            } else {
                tok_to_int(json, &toks[v], &cfg->log.level);
            }
        }
    }

    free(toks);
    free(json);
    return 0;
}

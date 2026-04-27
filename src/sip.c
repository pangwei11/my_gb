#include <bits/types/struct_tm.h>
#include <time.h>
#include "sip.h"
#include "rtp.h"
#include "frame_queue.h"

static int hex_string_to_bytes(const char *hex_str, uint8_t *out_bytes, int max_len) {
    if (!hex_str || !out_bytes) return -1;
    
    int len = strlen(hex_str);
    if (len % 2 != 0) return -1; // 必须是偶数个字符
    
    int byte_count = len / 2;
    if (byte_count > max_len) byte_count = max_len; // 防止溢出
    
    for (int i = 0; i < byte_count; i++) {
        char hex_byte[3] = { hex_str[i*2], hex_str[i*2+1], '\0' };
        // 跳过字符串中可能存在的空格（有些平台发来的指令带空格，如 "A5 00 00 10"）
        if (hex_byte[0] == ' ') {
            hex_byte[0] = hex_byte[1];
            hex_byte[1] = hex_str[i*2+2];
            hex_byte[2] = '\0';
            i++; // 多跳过一个字符
        }
        out_bytes[i] = (uint8_t)strtol(hex_byte, NULL, 16);
    }
    return byte_count;
}

// 核心函数：解析变倍、变焦、光圈指令 (统一处理)
static void parse_and_execute_ptz_cmd(const char *xml_body) {
    char ptz_cmd_str[64] = {0};
    
    // 1. 从 XML 中提取 <PTZCmd> 的值
    if (xml_get_value(xml_body, "PTZCmd", ptz_cmd_str, sizeof(ptz_cmd_str)) != 0) {
        LOG_WARN("收到DeviceControl,但未找到PTZCmd标签");
        return;
    }
    
    // 2. 将十六进制字符串转为字节数组
    uint8_t cmd_bytes[8] = {0};
    int byte_len = hex_string_to_bytes(ptz_cmd_str, cmd_bytes, 8);
    
    // 3. 校验指令合法性 
    if (byte_len != 8 || cmd_bytes[0] != 0xA5) {
        LOG_WARN("PTZ指令长度不符或首字节非0xA5 (长度=%d, 首字节=0x%02X)", byte_len, cmd_bytes[0]);
        return;
    }
    
    uint8_t byte4 = cmd_bytes[3]; // 控制动作的核心字节

        // 5. 根据Bit6区分指令类型
    int is_fi_cmd = (byte4 >> 6) & 0x01; // Bit6=1为FI指令，Bit6=0为PTZ指令

        if (is_fi_cmd) {
        // ==================== FI指令处理 ====================
        // 变焦控制：Bit1(近)、Bit0(远)
        int is_focus_near = (byte4 >> 1) & 0x01;
        int is_focus_far = byte4 & 0x01;
        if (is_focus_far || is_focus_near) {
            int focus_speed = cmd_bytes[4]; // 字节5：聚焦速度
            if (is_focus_far) {
                LOG_INFO(">>> 执行变焦指令: 【变焦远】, 速度=%d", focus_speed);
                // TODO: camera_focus_far(focus_speed);
            } else if (is_focus_near) {
                LOG_INFO(">>> 执行变焦指令: 【变焦近】, 速度=%d", focus_speed);
                // TODO: camera_focus_near(focus_speed);
            }
        }
        
        // 光圈控制：Bit3(缩小)、Bit2(放大)
        int is_iris_close = (byte4 >> 3) & 0x01;
        int is_iris_open = (byte4 >> 2) & 0x01;
        if (is_iris_close || is_iris_open) {
            int iris_speed = cmd_bytes[5]; // 字节6：光圈速度
            if (is_iris_close) {
                LOG_INFO(">>> 执行光圈指令: 【缩小】, 速度=%d", iris_speed);
                // TODO: camera_iris_close(iris_speed);
            } else if (is_iris_open) {
                LOG_INFO(">>> 执行光圈指令: 【放大】, 速度=%d", iris_speed);
                // TODO: camera_iris_open(iris_speed);
            }
        }
    } else {
        // ==================== PTZ指令处理 ====================
        // 变倍控制：Bit5(缩小)、Bit4(放大)
        int is_zoom_out = (byte4 >> 5) & 0x01;
        int is_zoom_in = (byte4 >> 4) & 0x01;
        if (is_zoom_out || is_zoom_in) {
            int zoom_speed = (cmd_bytes[6] >> 4) & 0x0F; // 字节7高4位：变倍速度
            if (is_zoom_out) {
                LOG_INFO(">>> 执行变倍指令: 【缩小】, 速度=%d", zoom_speed);
                // TODO: camera_zoom_out(zoom_speed);
            } else if (is_zoom_in) {
                LOG_INFO(">>> 执行变倍指令: 【放大】, 速度=%d", zoom_speed);
                // TODO: camera_zoom_in(zoom_speed);
            }
        }
    }
}




// ------- SDP parse -------
int parse_sdp_ip_port(const char *sdp, SdpParseResult *result) {
    if (!sdp || !result) return -1;
    memset(result, 0, sizeof(*result));
    result->port = -1;
    result->payload_type = g_cfg.media.rtp_payload_type;

    char *copy = strdup(sdp);
    if (!copy) return -1;

    char *save=NULL;
    for (char *line=strtok_r(copy, "\r\n", &save); line; line=strtok_r(NULL, "\r\n", &save)) {
        if (!strncmp(line, "c=IN IP4 ", 9)) {
            strncpy(result->ip, line+9, sizeof(result->ip)-1);
        } else if (!strncmp(line, "o=", 2)) {
            char *p = strstr(line, "IN IP4 ");
            if (p) {
                p += strlen("IN IP4 ");
                char *sp = strchr(p, ' ');
                if (sp) *sp = '\0';
                strncpy(result->origin_ip, p, sizeof(result->origin_ip)-1);
                result->has_origin_ip = 1;
            }
        } else if (!strncmp(line, "m=video ", 8)) {
            int port=0; char proto[64]={0}; char fmts[256]={0};
            if (sscanf(line, "m=video %d %63s %255[^\r\n]", &port, proto, fmts) >= 2) {
                result->port = port;
                if (strstr(proto, "TCP/RTP/AVP")) result->transport_tcp = 1;
                else result->transport_tcp = 0;
                // first payload
                char *sp = strchr(fmts, ' '); if (sp) *sp='\0';
                int pt = atoi(fmts);
                if (pt>0) result->payload_type = pt;
            }
        } else if (!strncmp(line, "a=setup:", 8)) {
            result->has_setup = 1;
            if (strstr(line, "passive")) result->setup_passive = 1;
            else if (strstr(line, "actpass")) { result->is_actpass = 1; result->setup_passive = 0; }
            else result->setup_passive = 0; // active
        } else if (!strncmp(line, "y=", 2)) {
            strncpy(result->ssrc_str, line+2, sizeof(result->ssrc_str)-1);
            result->ssrc = (uint32_t)strtoul(result->ssrc_str, NULL, 10);
            result->has_ssrc = 1;
        } else if (strstr(line, "PS/90000")) {
            char *colon = strchr(line, ':');
            if (colon) {
                int pt = atoi(colon+1);
                if (pt>0) result->payload_type = pt;
            }
        }
    }
    free(copy);

    if (result->port <= 0) return -1;
    if (result->ip[0]=='\0' || !strcmp(result->ip,"0.0.0.0")) {
        if (result->has_origin_ip && result->origin_ip[0] && strcmp(result->origin_ip,"0.0.0.0")) {
            strncpy(result->ip, result->origin_ip, sizeof(result->ip)-1);
        } else {
            strncpy(result->ip, g_cfg.platform.sip_ip, sizeof(result->ip)-1);
        }
    }
    return 0;
}

// ------- keepalive message build -------
static int extract_platform_username(eXosip_event_t *event, char *out, size_t out_sz) {
    if (!event || !event->request || !event->request->from || !event->request->from->url) return -1;
    osip_uri_t *u = event->request->from->url;
    if (u->username && u->username[0]) { strncpy(out, u->username, out_sz-1); out[out_sz-1]=0; return 0; }
    strncpy(out, g_cfg.platform.platform_id, out_sz-1); out[out_sz-1]=0; return 1;
}

static void sip_send_response_message(const char *to, const char *xml_body) {
    if (!g_exosip_ctx || !to || !xml_body) return;
    char from[256]; snprintf(from, sizeof(from), "sip:%s@%s", g_cfg.device.device_id, g_cfg.device.realm);
    osip_message_t *msg=NULL;
    eXosip_lock(g_exosip_ctx);
    if (eXosip_message_build_request(g_exosip_ctx, &msg, "MESSAGE", to, from, NULL)==0 && msg) {
        osip_message_set_body(msg, xml_body, strlen(xml_body));
        osip_message_set_content_type(msg, "Application/MANSCDP+xml");
        eXosip_message_send_request(g_exosip_ctx, msg);
    }
    eXosip_unlock(g_exosip_ctx);
}

static void sip_send_keepalive(void) {
    if (!g_exosip_ctx) return;

    g_sn_counter++;
    if (g_sn_counter > 999999) g_sn_counter = 1;
    g_keepalive_sn = g_sn_counter;
    g_keepalive_send_time_ms = get_timestamp_ms();

    char xml_body[512];
    snprintf(xml_body, sizeof(xml_body),
        "<?xml version=\"1.0\"?>\r\n"
        "<Notify>\r\n"
        "<CmdType>Keepalive</CmdType>\r\n"
        "<SN>%d</SN>\r\n"
        "<DeviceID>%s</DeviceID>\r\n"
        "<Status>OK</Status>\r\n"
        "</Notify>\r\n",
        g_sn_counter, g_cfg.device.device_id);

    char to[256];
    snprintf(to, sizeof(to), "sip:%s@%s:%d", g_cfg.platform.platform_id, g_cfg.platform.sip_ip, g_cfg.platform.sip_port);
    char from[256];
    snprintf(from, sizeof(from), "sip:%s@%s", g_cfg.device.device_id, g_cfg.device.realm);

    osip_message_t *message=NULL;
    eXosip_lock(g_exosip_ctx);
    if (eXosip_message_build_request(g_exosip_ctx, &message, "MESSAGE", to, from, NULL)==0 && message) {
        osip_message_set_body(message, xml_body, strlen(xml_body));
        osip_message_set_content_type(message, "Application/MANSCDP+xml");

        osip_call_id_t *cid = osip_message_get_call_id(message);
        if (cid && cid->number) {
            strncpy(g_last_keepalive_call_id, cid->number, sizeof(g_last_keepalive_call_id)-1);
            g_last_keepalive_call_id[sizeof(g_last_keepalive_call_id)-1]=0;
        }

        eXosip_message_send_request(g_exosip_ctx, message);

        g_keepalive_state.last_send_time_ms = get_timestamp_ms();
        g_keepalive_state.last_sn = g_sn_counter;
        atomic_store(&g_keepalive_state.pending_response, 0); // fire-and-forget keepalive

    }
    eXosip_unlock(g_exosip_ctx);

    // IMPORTANT:
    // Keepalive is fire-and-forget for compatibility with platforms that don't
    // send application-level replies. Do not treat missing replies as offline.
}

static void sip_send_device_status(void) {
    // Optional WVP compatibility: periodic DeviceStatus report
    if (!g_exosip_ctx) return;

    char time_buf[64] = {0};
    time_t now_time = time(NULL);
    struct tm tm_info;
    if (localtime_r(&now_time, &tm_info)) {
        snprintf(time_buf, sizeof(time_buf), "%04d-%02d-%02dT%02d:%02d:%02d",
                 tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
                 tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
    } else {
        strncpy(time_buf, "1970-01-01T00:00:00", sizeof(time_buf)-1);
    }

    g_sn_counter++;
    if (g_sn_counter > 999999) g_sn_counter = 1;

    char xml_body[768];
    snprintf(xml_body, sizeof(xml_body),
        "<?xml version=\"1.0\"?>\r\n"
        "<Notify>\r\n"
        "<CmdType>DeviceStatus</CmdType>\r\n"
        "<SN>%d</SN>\r\n"
        "<DeviceID>%s</DeviceID>\r\n"
        "<Result>OK</Result>\r\n"
        "<Online>ONLINE</Online>\r\n"
        "<Status>OK</Status>\r\n"
        "<Encode>ON</Encode>\r\n"
        "<Record>OFF</Record>\r\n"
        "<DeviceTime>%s</DeviceTime>\r\n"
        "</Notify>\r\n",
        g_sn_counter, g_cfg.device.device_id, time_buf);

    char to[256];
    snprintf(to, sizeof(to), "sip:%s@%s:%d", g_cfg.platform.platform_id, g_cfg.platform.sip_ip, g_cfg.platform.sip_port);
    sip_send_response_message(to, xml_body);
}

static void sip_check_keepalive(void) {
    // NOTE:
    // Many GB28181 platforms (incl. WVP) do NOT send an application-level reply to our Keepalive MESSAGE,
    // and eXosip may only deliver a SIP 200 OK (transaction response) which we are not matching here.
    // If we treat "no reply" as failure, we'll falsely mark device offline and WVP channel list will disappear.
    // So keepalive here is FIRE-AND-FORGET: send periodically, never flip g_registered based on keepalive timeout.
    if (!atomic_load(&g_registered)) return;

    uint64_t now = get_timestamp_ms();
    if (now - g_keepalive_state.last_send_time_ms < (uint64_t)SIP_KEEPALIVE_INTERVAL * 1000ULL) return;

    // Clear pending state to avoid "timeout->retry->offline" cascade.
    atomic_store(&g_keepalive_state.pending_response, 0);
    g_keepalive_state.retry_count = 0;

    sip_send_keepalive();

#if WVP_COMPATIBILITY
    // WVP兼容：每分钟额外上报一次设备状态（平台有时依赖该消息刷新在线状态）
    static uint64_t last_device_status_ms = 0;
    if (now - last_device_status_ms >= 60000) {
        sip_send_device_status();
        last_device_status_ms = now;
    }
#endif
}

// 注册状态诊断：用于定位“注册看似成功，但一段时间后被平台 BYE/下线”的问题
static void check_registration_status(void) {
    static int last_reg_state = -1;
    static uint64_t unregistered_since = 0;
    int current = atomic_load(&g_registered);

    if (current != last_reg_state) {
        LOG_INFO("注册状态变化: %s -> %s", (last_reg_state==1) ? "已注册" : "未注册", current ? "已注册" : "未注册");
        last_reg_state = current;
    }

    uint64_t now = get_timestamp_ms();
    if (!current) {
        if (unregistered_since==0) unregistered_since = now;
        else if (now - unregistered_since > 30000) {
            LOG_WARN("已连续%.1f秒未注册成功 (last_attempt=%.1fs ago, fail_cnt=%d)",
                     (now - unregistered_since)/1000.0,
                     g_last_reg_attempt_ms ? (now - g_last_reg_attempt_ms)/1000.0 : -1.0,
                     g_reg_failure_count);
            unregistered_since = now - 20000; // throttle
        }
    } else {
        unregistered_since = 0;
    }
}

static void exosip_send_register(int expires) {
    if (!g_exosip_ctx) return;

    char from[256], proxy[256], contact[256];
    snprintf(from, sizeof(from), "sip:%s@%s", g_cfg.device.device_id, g_cfg.device.realm);
    snprintf(proxy, sizeof(proxy), "sip:%s@%s:%d", g_cfg.platform.platform_id, g_cfg.platform.sip_ip, g_cfg.platform.sip_port);
    if (g_cfg.media.transport == TRANS_UDP)
        snprintf(contact, sizeof(contact), "sip:%s@%s:%d", g_cfg.device.device_id, g_cfg.device.local_ip, g_cfg.device.local_sip_port);
    else
        snprintf(contact, sizeof(contact), "sip:%s@%s:%d;transport=tcp", g_cfg.device.device_id, g_cfg.device.local_ip, g_cfg.device.local_sip_port);

    osip_message_t *reg=NULL;
    eXosip_lock(g_exosip_ctx);
    if (expires==0) {
        if (g_reg_id>0) {
            eXosip_register_build_register(g_exosip_ctx, g_reg_id, 0, &reg);
            if (reg) eXosip_register_send_register(g_exosip_ctx, g_reg_id, reg);
        }
    } else {
        if (g_reg_id<=0) {
            g_reg_id = eXosip_register_build_initial_register(g_exosip_ctx, from, proxy, contact, expires, &reg);
            if (g_reg_id>0 && reg) eXosip_register_send_register(g_exosip_ctx, g_reg_id, reg);
            else g_reg_id = 0;
        } else {
            eXosip_register_build_register(g_exosip_ctx, g_reg_id, expires, &reg);
            if (reg) eXosip_register_send_register(g_exosip_ctx, g_reg_id, reg);
            else g_reg_id = 0;
        }
    }
    eXosip_unlock(g_exosip_ctx);
    g_last_reg_attempt_ms = get_timestamp_ms();
}

static void handle_registration_response(eXosip_event_t *event) {
    if (!event->response) return;
    int status = event->response->status_code;      //// 提取SIP响应状态码
    LOG_INFO("收到注册响应: %d %s", status, event->response->reason_phrase ? event->response->reason_phrase : "");

    //成功状态
    if (status==200) {                                  
        atomic_store(&g_registered, 1);                 //原子操作标记“已注册”（多线程安全）
        g_last_reg_success_ms = get_timestamp_ms();     // 记录最后一次注册成功的时间戳
        g_next_reg_retry_ms = 0;                        // 清空下次重试时间（成功后无需重试）
        g_register_retries = 0;                         // 重置重试次数
        g_reg_failure_count = 0;                        // 重置连续失败计数
        LOG_INFO("注册成功");
        return;
    }

    //注册失败逻辑（非 200 状态码）
    atomic_store(&g_registered, 0);                     // 标记“未注册”
    g_register_retries++;                               // 全局重试次数+1
    g_reg_failure_count++;                              // 连续失败计数+1

    // 失败重试的“退避算法”：避免频繁重试被平台拉黑，同时适配不同失败类型
    int backoff = 10 * g_register_retries;              // 基础退避：10秒×重试次数（线性退避）
    if (status==403) backoff = 30 * (1 << (g_register_retries-1));      //// 403特殊处理：指数退避
    if (backoff > 300) backoff = 300;
    g_next_reg_retry_ms = get_timestamp_ms() + (uint64_t)backoff*1000ULL;       //// 计算下次重试的时间戳（当前时间 + 退避秒数×1000转毫秒）

    if (g_reg_failure_count >= 3) {
        LOG_WARN("连续注册失败，重置reg_id");
        g_reg_id = 0;
        g_reg_failure_count = 0;
    }
}

// ------- streaming state -------
static void sip_start_streaming(void) {
    int do_init = 0;
    pthread_mutex_lock(&g_state.mutex);
    if (!atomic_load(&g_state.streaming)) {
        atomic_store(&g_state.streaming, 1);
        atomic_store(&g_state.waiting_for_ack, 0);
        atomic_store(&g_state.need_idr, 1);
        atomic_store(&g_state.call_ended, 0);
        atomic_store(&g_state.bye_answered, 0);
        atomic_store(&g_state.connection_broken, 0);
        do_init = 1;
    }
    pthread_mutex_unlock(&g_state.mutex);

    if (do_init) {
        g_last_psm_sent = 0;
        g_first_rtp_pts = 0;
        g_no_rtp_count = 0;
        frame_queue_clean(&g_frame_queue);
        atomic_store(&g_rtp_ctx.rtp_packets_sent, 0);
        atomic_store(&g_rtp_ctx.rtp_send_errors, 0);
        atomic_store(&g_rtp_ctx.last_rtp_send_ts, 0);
        LOG_INFO("开始推流：清空队列，等待IDR");
        frame_queue_clear(&g_frame_queue);
        extern void venc_trigger_wait_idr(void); 
        venc_trigger_wait_idr(); 
    }
}

static void sip_stop_streaming(void) {
    atomic_store(&g_state.streaming, 0);
    atomic_store(&g_state.waiting_for_ack, 0);
    atomic_store(&g_state.need_idr, 1);
    rtp_socket_close();
    atomic_store(&g_tcp_connecting, 0);
    g_first_rtp_pts = 0;
    g_no_rtp_count = 0;
    g_invite_start_ms = 0;
    g_first_frame_sent_ms = 0;
    atomic_store(&g_state.bye_answered, 0);

    LOG_INFO("停止推流");
}

static void sip_check_ack_timeout(void) {
    if (!atomic_load(&g_state.waiting_for_ack)) return;
    uint64_t now = get_timestamp_ms();
    if (now - g_wait_ack_start_ms > (uint64_t)ACK_TIMEOUT_MS) {
        LOG_WARN("ACK等待超时(%dms)，强制开始推流", ACK_TIMEOUT_MS);
        atomic_store(&g_state.waiting_for_ack, 0);
        sip_start_streaming();
        maybe_start_tcp_connect_after_ack();
    }
}

// ------- MESSAGE handler -------
static void sip_handle_message(eXosip_event_t *event) {
    osip_body_t *body=NULL;
    osip_message_get_body(event->request, 0, &body);
    if (!body || !body->body) return;

    const char *xml = body->body;

    char cmd[32]={0}, sn[32]={0}, devid[32]={0}, config_type[32]={0};
    if (xml_get_value(xml, "CmdType", cmd, sizeof(cmd))<0) cmd[0]=0;
    xml_get_value(xml, "SN", sn, sizeof(sn));
    xml_get_value(xml, "DeviceID", devid, sizeof(devid));
    xml_get_value(xml, "ConfigType", config_type, sizeof(config_type));

    // answer 200 for MESSAGE
    eXosip_lock(g_exosip_ctx);
    eXosip_message_send_answer(g_exosip_ctx, event->tid, 200, NULL);
    eXosip_unlock(g_exosip_ctx);

    // platform->device keepalive query: reply
    if (!strcmp(cmd,"Keepalive")) {
        atomic_store(&g_keepalive_state.pending_response, 0);
        g_keepalive_state.retry_count = 0;

        char platform_user[64];
        if (extract_platform_username(event, platform_user, sizeof(platform_user))<0) strncpy(platform_user, g_cfg.platform.platform_id, sizeof(platform_user)-1);

        char to_uri[256];
        snprintf(to_uri, sizeof(to_uri), "sip:%s@%s:%d", platform_user, g_cfg.platform.sip_ip, g_cfg.platform.sip_port);

        char resp[512];
        snprintf(resp, sizeof(resp),
            "<?xml version=\"1.0\"?>\r\n"
            "<Response>\r\n"
            "<CmdType>Keepalive</CmdType>\r\n"
            "<SN>%s</SN>\r\n"
            "<DeviceID>%s</DeviceID>\r\n"
            "<Result>OK</Result>\r\n"
            "</Response>\r\n",
            sn[0]?sn:"0", devid[0]?devid:g_cfg.device.device_id);
        sip_send_response_message(to_uri, resp);
        return;
    }

    char platform_user[64];
    if (extract_platform_username(event, platform_user, sizeof(platform_user))<0) strncpy(platform_user, g_cfg.platform.platform_id, sizeof(platform_user)-1);
    char to_uri[256];
    snprintf(to_uri, sizeof(to_uri), "sip:%s@%s:%d", platform_user, g_cfg.platform.sip_ip, g_cfg.platform.sip_port);

    // WVP 常用：Catalog / DeviceInfo / DeviceStatus / ConfigDownload / DeviceControl
    if (!strcmp(cmd,"Catalog")) {
        char resp[4096];
        snprintf(resp, sizeof(resp),
            "<?xml version=\"1.0\" encoding=\"GB2312\"?>\r\n"
            "<Response>\r\n"
            "<CmdType>Catalog</CmdType>\r\n"
            "<SN>%s</SN>\r\n"
            "<DeviceID>%s</DeviceID>\r\n"
            "<SumNum>1</SumNum>\r\n"
            "<DeviceList Num=\"1\">\r\n"
            "<Item>\r\n"
            "<DeviceID>%s</DeviceID>\r\n"
            "<Name>GB28181 Camera</Name>\r\n"
            "<Manufacturer>Generic</Manufacturer>\r\n"
            "<Model>IPC</Model>\r\n"
            "<Owner>Owner</Owner>\r\n"
            "<CivilCode>%s</CivilCode>\r\n"
            "<Address>Address</Address>\r\n"
            "<Parental>0</Parental>\r\n"
            "<ParentID>%s</ParentID>\r\n"
            "<SafetyWay>0</SafetyWay>\r\n"
            "<RegisterWay>1</RegisterWay>\r\n"
            "<Secrecy>0</Secrecy>\r\n"
            "<Status>ON</Status>\r\n"
            "<Longitude>0.000000</Longitude>\r\n"
            "<Latitude>0.000000</Latitude>\r\n"
            "<Info><PTZType>0</PTZType></Info>\r\n"
            "</Item>\r\n"
            "</DeviceList>\r\n"
            "</Response>\r\n",
            sn[0]?sn:"0", g_cfg.device.device_id, g_cfg.device.channel_id, g_civil_code, g_cfg.device.device_id);
        sip_send_response_message(to_uri, resp);
        return;
    }

    if (!strcmp(cmd,"DeviceInfo")) {
        char resp[1024];
        snprintf(resp, sizeof(resp),
            "<?xml version=\"1.0\" encoding=\"GB2312\"?>\r\n"
            "<Response>\r\n"
            "<CmdType>DeviceInfo</CmdType>\r\n"
            "<SN>%s</SN>\r\n"
            "<DeviceID>%s</DeviceID>\r\n"
            "<Result>OK</Result>\r\n"
            "<DeviceName>GB28181 Camera</DeviceName>\r\n"
            "<Manufacturer>Generic</Manufacturer>\r\n"
            "<Model>IPC</Model>\r\n"
            "<Firmware>V1.0</Firmware>\r\n"
            "<SerialNumber>%s</SerialNumber>\r\n"
            "<Hardware>1.0</Hardware>\r\n"
            "<Software>1.0</Software>\r\n"
            "<MaxCamera>1</MaxCamera>\r\n"
            "<MaxAlarm>0</MaxAlarm>\r\n"
            "<Channel>1</Channel>\r\n"
            "</Response>\r\n",
            sn[0]?sn:"0", g_cfg.device.device_id, g_cfg.device.device_id);
        sip_send_response_message(to_uri, resp);
        return;
    }

    if (!strcmp(cmd,"DeviceStatus")) {
        time_t now_time = time(NULL);
        struct tm tm_info;
        char time_buf[64];
        if (localtime_r(&now_time, &tm_info)) {
            snprintf(time_buf, sizeof(time_buf), "%04d-%02d-%02dT%02d:%02d:%02d",
                     tm_info.tm_year+1900, tm_info.tm_mon+1, tm_info.tm_mday,
                     tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
        } else snprintf(time_buf, sizeof(time_buf), "2024-01-01T00:00:00");

        char resp[1024];
        snprintf(resp, sizeof(resp),
            "<?xml version=\"1.0\"?>\r\n"
            "<Response>\r\n"
            "<CmdType>DeviceStatus</CmdType>\r\n"
            "<SN>%s</SN>\r\n"
            "<DeviceID>%s</DeviceID>\r\n"
            "<Result>OK</Result>\r\n"
            "<Online>ONLINE</Online>\r\n"
            "<Status>OK</Status>\r\n"
            "<Encode>ON</Encode>\r\n"
            "<Record>OFF</Record>\r\n"
            "<DeviceTime>%s</DeviceTime>\r\n"
            "</Response>\r\n",
            sn[0]?sn:"0", g_cfg.device.device_id, time_buf);
        sip_send_response_message(to_uri, resp);
        return;
    }

    if (!strcmp(cmd,"ConfigDownload")) {
        char resp[4096];
        if (!strcmp(config_type, "BasicParam")) {
            snprintf(resp, sizeof(resp),
                "<?xml version=\"1.0\" encoding=\"GB2312\"?>\r\n"
                "<Response>\r\n"
                "<CmdType>ConfigDownload</CmdType>\r\n"
                "<SN>%s</SN>\r\n"
                "<DeviceID>%s</DeviceID>\r\n"
                "<Result>OK</Result>\r\n"
                "<BasicParam>\r\n"
                "<Name>GB28181 Camera</Name>\r\n"
                "<DeviceID>%s</DeviceID>\r\n"
                "<DeviceType>IPC</DeviceType>\r\n"
                "<Manufacturer>Generic</Manufacturer>\r\n"
                "<Model>IPC</Model>\r\n"
                "<Firmware>V1.0</Firmware>\r\n"
                "<Software>GB28181 Streamer</Software>\r\n"
                "<MaxCamera>1</MaxCamera>\r\n"
                "<MaxAlarm>0</MaxAlarm>\r\n"
                "<CivilCode>%s</CivilCode>\r\n"
                "</BasicParam>\r\n"
                "</Response>\r\n",
                sn[0]?sn:"0", g_cfg.device.device_id, g_cfg.device.device_id, g_civil_code);
        } else if (!strcmp(config_type, "VideoParam")) {
            snprintf(resp, sizeof(resp),
                "<?xml version=\"1.0\" encoding=\"GB2312\"?>\r\n"
                "<Response>\r\n"
                "<CmdType>ConfigDownload</CmdType>\r\n"
                "<SN>%s</SN>\r\n"
                "<DeviceID>%s</DeviceID>\r\n"
                "<Result>OK</Result>\r\n"
                "<VideoParam>\r\n"
                "<VideoEncoding>H264</VideoEncoding>\r\n"
                "<VideoResolution>%dx%d</VideoResolution>\r\n"
                "<VideoBitrate>%d</VideoBitrate>\r\n"
                "<VideoFrameRate>%d</VideoFrameRate>\r\n"
                "<VideoKeyFrameInterval>%d</VideoKeyFrameInterval>\r\n"
                "<Profile>Main</Profile>\r\n"
                "</VideoParam>\r\n"
                "</Response>\r\n",
                sn[0]?sn:"0", g_cfg.device.device_id, VIDEO_WIDTH, VIDEO_HEIGHT, VIDEO_BITRATE, VIDEO_FPS, VIDEO_GOP);
        } else {
            snprintf(resp, sizeof(resp),
                "<?xml version=\"1.0\" encoding=\"GB2312\"?>\r\n"
                "<Response>\r\n"
                "<CmdType>ConfigDownload</CmdType>\r\n"
                "<SN>%s</SN>\r\n"
                "<DeviceID>%s</DeviceID>\r\n"
                "<Result>OK</Result>\r\n"
                "</Response>\r\n",
                sn[0]?sn:"0", g_cfg.device.device_id);
        }
        sip_send_response_message(to_uri, resp);
        return;
    }

    if (!strcmp(cmd,"DeviceControl")) {
        char resp[512];
        snprintf(resp, sizeof(resp),
            "<?xml version=\"1.0\" encoding=\"GB2312\"?>\r\n"
            "<Response>\r\n"
            "<CmdType>DeviceControl</CmdType>\r\n"
            "<SN>%s</SN>\r\n"
            "<DeviceID>%s</DeviceID>\r\n"
            "<Result>OK</Result>\r\n"
            "</Response>\r\n",
            sn[0]?sn:"0", g_cfg.device.device_id);
        sip_send_response_message(to_uri, resp);


        // ==================== 在这里加入调用 ====================
        // 将收到的 xml 字符串传给我们的解析函数
        parse_and_execute_ptz_cmd(xml); 
        // ======================================================


        return;
    }

    LOG_WARN("未知CmdType: %s", cmd);
}

// ------- INVITE handler -------
//平台视频点播请求
static int sip_handle_invite(eXosip_event_t *event) {
    LOG_INFO("收到INVITE");
    g_invite_start_ms = get_timestamp_ms();     //// 记录点播开始时间（用于后续统计）
    g_first_frame_sent_ms = 0;                  // 初始化首帧发送时间

    osip_body_t *body=NULL;
    // 2. 从INVITE请求中提取第0个消息体（SIP消息通常仅1个body，对应SDP）
// event->request：指向收到的SIP INVITE请求消息结构体
// 0：取第一个（也是唯一）的消息体
// &body：输出参数，指向提取到的SDP体（若存在）
    osip_message_get_body(event->request, 0, &body);    //// 3. 合法性校验：无SDP体 或 SDP体内容为空
    if (!body || !body->body) {
        eXosip_lock(g_exosip_ctx);
        eXosip_call_send_answer(g_exosip_ctx, event->tid, 400, NULL);       //// 3.2 发送400 Bad Request响应（SIP协议规范：请求格式错误/必要内容缺失）
        eXosip_unlock(g_exosip_ctx);
        return -1;
    }

    SdpParseResult sdp;             //定义SDP解析结果结构体变量，用于存储解析后的SDP关键参数
    if (parse_sdp_ip_port(body->body, &sdp) < 0) {      //调用自定义函数parse_sdp_ip_port解析SDP内容，提取核心传输参数
        eXosip_lock(g_exosip_ctx);
        eXosip_call_send_answer(g_exosip_ctx, event->tid, 488, NULL);   //发送488错误响应：SIP协议码，语义为“Not Acceptable Here”（请求的媒体参数不可接受）
        eXosip_unlock(g_exosip_ctx);
        return -1;
    }

    int local_rtp_port=0;       //// 本地RTP端口
    int tcp_passive=0;          // TCP模式下的角色标记：0=主动(active)，1=被动(passive)

    //TCP 传输协议分支（SDP 指定 TCP）
    if (sdp.transport_tcp) {    
        if (sdp.has_setup) {    //确定TCP主动/被动模式
            // 对端 passive -> 我 active；对端 active -> 我 passive
            if (sdp.setup_passive || sdp.is_actpass) tcp_passive = 0;
            else tcp_passive = 1;
        } else tcp_passive = 0;

        //// TCP被动模式（本端监听，等待平台主动连接）
        if (tcp_passive) {
            // 创建TCP监听套接字，自动分配可用端口并赋值给local_rtp_port
            int ls = rtp_tcp_server_create(&local_rtp_port);
            if (ls<0) {
                eXosip_lock(g_exosip_ctx);
                eXosip_call_send_answer(g_exosip_ctx, event->tid, 500, NULL);
                eXosip_unlock(g_exosip_ctx);
                return -1;
            }
            // 保护全局RTP上下文，更新TCP被动模式的核心参数
            pthread_mutex_lock(&g_rtp_ctx.lock);
            g_rtp_ctx.tcp_listen_sock = ls;
            g_rtp_ctx.local_port = local_rtp_port;
            g_rtp_ctx.is_tcp = 1;
            g_rtp_ctx.tcp_passive_mode = 1;
            g_rtp_ctx.tcp_connected = 0;
            pthread_mutex_unlock(&g_rtp_ctx.lock);

            // 创建独立线程处理TCP连接接入（detach后无需主线程join）
            pthread_t t;
            if (pthread_create(&t, NULL, rtp_tcp_accept_thread, NULL)==0) pthread_detach(t);
        }
        //TCP主动模式（本端主动连接平台）
        else {
            // 创建TCP套接字（AF_INET=IPv4，SOCK_STREAM=TCP）
            int sock = socket(AF_INET, SOCK_STREAM, 0);
            if (sock<0) {
                eXosip_lock(g_exosip_ctx);
                eXosip_call_send_answer(g_exosip_ctx, event->tid, 500, NULL);
                eXosip_unlock(g_exosip_ctx);
                return -1;
            }
            // 优化TCP传输参数（针对视频流低延迟、高吞吐需求）
            int nodelay=1; setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
            int flags=fcntl(sock,F_GETFL,0); fcntl(sock,F_SETFL, flags|O_NONBLOCK);
            struct timeval to={.tv_sec=3,.tv_usec=0}; setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof(to));
            int snd=1024*1024; setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &snd, sizeof(snd));

            // 更新全局RTP上下文（主动模式参数）
            pthread_mutex_lock(&g_rtp_ctx.lock);
            g_rtp_ctx.sock = sock;
            g_rtp_ctx.local_port = 0; // active 不绑定端口
            g_rtp_ctx.is_tcp = 1;
            g_rtp_ctx.tcp_passive_mode = 0;
            g_rtp_ctx.tcp_connected = 0;
            strncpy(g_rtp_ctx.tcp_target_ip, sdp.ip, sizeof(g_rtp_ctx.tcp_target_ip)-1);
            g_rtp_ctx.tcp_target_port = sdp.port;
            pthread_mutex_unlock(&g_rtp_ctx.lock);
        }
    } else {    //UDP 传输协议分支（SDP 指定 UDP）
        if (rtp_udp_socket_create(sdp.ip, sdp.port, &local_rtp_port) < 0) {
            eXosip_lock(g_exosip_ctx);
            eXosip_call_send_answer(g_exosip_ctx, event->tid, 500, NULL);
            eXosip_unlock(g_exosip_ctx);
            return -1;
        }
        // 从全局RTP上下文同步本地端口（rtp_udp_socket_create内部已赋值给g_rtp_ctx.local_port
        local_rtp_port = g_rtp_ctx.local_port;
    }

    //RTP 核心参数初始化
    pthread_mutex_lock(&g_rtp_ctx.lock);
    g_rtp_ctx.ssrc = sdp.has_ssrc ? sdp.ssrc : generate_stable_ssrc(g_cfg.device.device_id);
    g_rtp_ctx.payload_type = sdp.payload_type>0 ? sdp.payload_type : g_cfg.media.rtp_payload_type;
    g_rtp_ctx.seq = (uint16_t)(rand() & 0xFFFF);
    pthread_mutex_unlock(&g_rtp_ctx.lock);

    //SDP 响应构建的前置变量定义
    // build 200OK SDP
    char sdp_resp[1024];
    const char *our_setup = tcp_passive ? "passive" : "active";
    int m_port = (sdp.transport_tcp && !tcp_passive) ? 0 : local_rtp_port; // active => 0 per RFC practice

    //200 OK 响应的 SDP 内容构建核心逻辑
    int n = 0;
    if (sdp.transport_tcp) {    //TCP 传输模式的 SDP 构建（sdp.transport_tcp 为真）
        n = snprintf(sdp_resp, sizeof(sdp_resp),
            "v=0\r\n"
            "o=%s 0 0 IN IP4 %s\r\n"
            "s=Play\r\n"
            "c=IN IP4 %s\r\n"
            "t=0 0\r\n"
            "m=video %d TCP/RTP/AVP %d\r\n"
            "a=setup:%s\r\n"
            "a=connection:new\r\n"
            "a=rtpmap:%d PS/90000\r\n"
            "a=sendonly\r\n"
            "y=%010u\r\n",
            g_cfg.device.device_id, g_cfg.device.sdp_ip, g_cfg.device.sdp_ip, m_port, g_rtp_ctx.payload_type, our_setup, g_rtp_ctx.payload_type, g_rtp_ctx.ssrc);
    } else {    //UDP 传输模式的 SDP 构建（sdp.transport_tcp 为假）
        n = snprintf(sdp_resp, sizeof(sdp_resp),
            "v=0\r\n"
            "o=%s 0 0 IN IP4 %s\r\n"
            "s=Play\r\n"
            "c=IN IP4 %s\r\n"
            "t=0 0\r\n"
            "m=video %d RTP/AVP %d\r\n"
            "a=rtpmap:%d PS/90000\r\n"
            "a=sendonly\r\n"
            "y=%010u\r\n",
            g_cfg.device.device_id, g_cfg.device.sdp_ip, g_cfg.device.sdp_ip, local_rtp_port, g_rtp_ctx.payload_type, g_rtp_ctx.payload_type, g_rtp_ctx.ssrc);
    }

    eXosip_lock(g_exosip_ctx);
    osip_message_t *ans=NULL;
    if (eXosip_call_build_answer(g_exosip_ctx, event->tid, 200, &ans)==0 && ans) {      //构建 200 OK 响应消息
        osip_message_set_body(ans, sdp_resp, n);                                       //填充 SDP 内容到 200 OK 响应
        osip_message_set_content_type(ans, "application/sdp");
        eXosip_call_send_answer(g_exosip_ctx, event->tid, 200, ans);            //发送 200 OK 响应
        pthread_mutex_lock(&g_call_id_mutex);
        g_call_id = event->cid;                     //记录会话关键 ID
        g_dialog_id = event->did;
        pthread_mutex_unlock(&g_call_id_mutex);
    }
    eXosip_unlock(g_exosip_ctx);

    if (g_cfg.wvp.wait_for_ack) {               //判断是否等待 ACK 后再启动推流
        atomic_store(&g_state.waiting_for_ack, 1);
        g_wait_ack_start_ms = get_timestamp_ms();
        LOG_INFO("等待ACK... 超时=%dms", ACK_TIMEOUT_MS);
    } else {
        sip_start_streaming();
        if (sdp.transport_tcp && !tcp_passive) maybe_start_tcp_connect_after_ack();
    }
    return 0;
}

// expose accept thread symbol for rtp.c
extern void* rtp_tcp_accept_thread(void *arg);

// ------- sip thread -------
void* sip_thread(void *arg) {
    (void)arg;
    LOG_INFO("SIP线程启动");

    g_exosip_ctx = eXosip_malloc();     //分配 eXosip 上下文对象（struct eXosip_t）的内存；
    if (!g_exosip_ctx) return NULL;
    if (eXosip_init(g_exosip_ctx)!=0) { osip_free(g_exosip_ctx); g_exosip_ctx=NULL; return NULL; }      //调用 eXosip_init() 初始化已分配的 eXosip 上下文

    if (strlen(g_cfg.device.device_id)>0 && strlen(g_cfg.device.password)>0) {
        eXosip_add_authentication_info(g_exosip_ctx,                //向 eXosip 上下文添加 SIP 认证信息
                                       g_cfg.device.device_id,
                                       g_cfg.device.device_id,
                                       g_cfg.device.password,
                                       NULL, NULL);
    }

    //根据配置的传输协议（UDP/TCP），调用 eXosip 库的监听接口绑定本地 SIP 端口；若监听失败，清理资源并退出 SIP 线程
    int listen_ret = (g_cfg.media.transport == TRANS_UDP)
        ? eXosip_listen_addr(g_exosip_ctx, IPPROTO_UDP, g_cfg.device.local_ip, g_cfg.device.local_sip_port, AF_INET, 0)
        : eXosip_listen_addr(g_exosip_ctx, IPPROTO_TCP, g_cfg.device.local_ip, g_cfg.device.local_sip_port, AF_INET, 0);
    if (listen_ret!=0) {
        LOG_ERROR("SIP监听失败 %s:%d", g_cfg.device.local_ip, g_cfg.device.local_sip_port);
        eXosip_quit(g_exosip_ctx); osip_free(g_exosip_ctx); g_exosip_ctx=NULL;
        return NULL;
    }

    exosip_send_register(SIP_REGISTER_INTERVAL);        //设备向平台发起 / 刷新 SIP 注册（REGISTER）请求 的核心调用

    uint64_t last_keepalive_check = get_timestamp_ms();
    uint64_t last_reg_check = get_timestamp_ms();

    while (atomic_load(&g_state.running)) {
        eXosip_event_t *ev = eXosip_event_wait(g_exosip_ctx, 0, 100);       //有事件时返回 eXosip_event_t 事件结构体（如注册结果、点播请求），无事件时返回 NULL。
        if (ev) {
            switch (ev->type) {
            case EXOSIP_REGISTRATION_SUCCESS:
            case EXOSIP_REGISTRATION_FAILURE:
                handle_registration_response(ev);           //注册结果事件
                break;

            case EXOSIP_CALL_INVITE:                        //点播请求事件
                sip_handle_invite(ev);
                break;

            case EXOSIP_CALL_ACK:                           //通话确认事件
                LOG_INFO("收到ACK");
                sip_start_streaming();
                maybe_start_tcp_connect_after_ack();
                break;

            //CALL_CLOSED/RELEASED / CALL_MESSAGE_NEW(BYE)：处理平台挂断，包含防重传和详细的 RTP 统计日志打印
            case EXOSIP_CALL_CLOSED:
            case EXOSIP_CALL_RELEASED:
                LOG_INFO("通话结束");

                /* Some stacks may deliver BYE inside CALL_CLOSED/RELEASED. Dump & try to answer once. */
                if (ev->request && ev->request->sip_method && !strcmp(ev->request->sip_method, "BYE")) {
                    int already_answered = atomic_load(&g_state.bye_answered);
                    if (already_answered) {
                        LOG_INFO("CALL_CLOSED/RELEASED 携带BYE(可能延迟/重传)，但本地已应答/结束，跳过再次200OK");
                    } else {
                        LOG_INFO("CALL_CLOSED/RELEASED 携带BYE请求(可能延迟/重传)，记录并尝试应答");
                    }
                    if (g_cfg.wvp.dump_sip_raw && ev->request) {
                        char *raw = NULL;
                        size_t raw_len = 0;
                        if (osip_message_to_str(ev->request, &raw, &raw_len) == 0 && raw) {
                            LOG_INFO("BYE原始报文(长度=%u):\n%.*s", (unsigned)raw_len, (int)raw_len, raw);
                            osip_free(raw);
                        }
                    }
                    if (!already_answered) {
                        eXosip_lock(g_exosip_ctx);
                        int rc = eXosip_call_send_answer(g_exosip_ctx, ev->tid, 200, NULL);
                        eXosip_unlock(g_exosip_ctx);
                        // rc=-3 usually means already answered/transaction closed by stack.
                        if (rc == 0 || rc == -3) {
                            atomic_store(&g_state.bye_answered, 1);
                        } else {
                            LOG_WARN("BYE 200OK 发送失败: rc=%d", rc);
                        }
                    }
                }

                atomic_store(&g_state.call_ended, 1);
                sip_stop_streaming();
                break;

            case EXOSIP_CALL_MESSAGE_NEW:
                if (ev->request && ev->request->sip_method && !strcmp(ev->request->sip_method, "BYE")) {
                    // WVP 平台主动断开（常见：5分钟超时、媒体流异常、无RTP等）
                    int already_ended = atomic_load(&g_state.call_ended);
                    if (!already_ended) {
                        atomic_store(&g_state.call_ended, 1);
                        LOG_INFO("收到平台发送的BYE请求");
                    } else {
                        LOG_WARN("收到BYE，但本地已标记通话结束(可能重传/顺序乱)，仅记录不再停止/应答");
                    }

                    if (g_cfg.wvp.dump_sip_raw && ev->request) {
                        char *raw = NULL;
                        size_t raw_len = 0;
                        if (osip_message_to_str(ev->request, &raw, &raw_len) == 0 && raw) {
                            // 注意：raw 可能较长，仅在调试版打印
                            LOG_INFO("BYE原始报文(长度=%u):\n%.*s", (unsigned)raw_len, (int)raw_len, raw);
                            osip_free(raw);
                        }
                    }

                    // 打印关键头（Reason/User-Agent 等）
                    if (ev->request) {
                        osip_header_t *hdr = NULL;
                        int pos = 0;
                        while (osip_message_get_header(ev->request, pos, &hdr) == 0) {
                            if (hdr && hdr->hname && hdr->hvalue) {
                                if (strcasestr(hdr->hname, "Reason") ||
                                    strcasestr(hdr->hname, "User-Agent") ||
                                    strcasestr(hdr->hname, "Warning") ||
                                    strcasestr(hdr->hname, "Session-Expires") ||
                                    strcasestr(hdr->hname, "Min-SE") ||
                                    strcasestr(hdr->hname, "Supported") ||
                                    strcasestr(hdr->hname, "Require")) {
                                    LOG_INFO("BYE头: %s: %s", hdr->hname, hdr->hvalue);
                                }
                            }
                            pos++;
                        }
                    }

                    uint64_t now_ms = get_timestamp_ms();
                    if (g_invite_start_ms) {
                        LOG_INFO("点播持续时长: %.2f秒", (now_ms - g_invite_start_ms) / 1000.0);
                    }
                    if (g_first_frame_sent_ms) {
                        LOG_INFO("从首帧发送起持续: %.2f秒", (now_ms - g_first_frame_sent_ms) / 1000.0);
                    }
                    int sent = atomic_load(&g_rtp_ctx.rtp_packets_sent);
                    int errors = atomic_load(&g_rtp_ctx.rtp_send_errors);
                    int drops = atomic_load(&g_rtp_ctx.rtp_drop_count);
                    uint64_t last_ok = atomic_load(&g_rtp_ctx.last_rtp_send_ts);
                    uint64_t last_try = atomic_load(&g_rtp_ctx.last_rtp_attempt_ts);
                    LOG_INFO("RTP统计: 发送包=%d 错误=%d 丢弃=%d last_ok_age=%llums last_try_age=%llums", sent, errors, drops,
                             (unsigned long long)(last_ok ? (get_monotonic_ms() - last_ok) : 0),
                             (unsigned long long)(last_try ? (get_monotonic_ms() - last_try) : 0));

                    if (!already_ended) {
                        sip_stop_streaming();

                        // Answer BYE once. rc=-3 usually means already answered/transaction closed.
                        eXosip_lock(g_exosip_ctx);
                        int rc = eXosip_call_send_answer(g_exosip_ctx, ev->tid, 200, NULL);
                        eXosip_unlock(g_exosip_ctx);
                        if (rc == 0 || rc == -3) {
                            atomic_store(&g_state.bye_answered, 1);
                        } else {
                            LOG_WARN("BYE 200OK 发送失败: rc=%d", rc);
                        }
                    }
                } else if (ev->request && ev->request->sip_method && !strcmp(ev->request->sip_method, "MESSAGE")) {
                    sip_handle_message(ev);
                } else if (ev->request && ev->request->sip_method && !strcmp(ev->request->sip_method, "UPDATE")) {
                    eXosip_lock(g_exosip_ctx);
                    eXosip_call_send_answer(g_exosip_ctx, ev->tid, 200, NULL);
                    eXosip_unlock(g_exosip_ctx);
                }
                break;

            case EXOSIP_MESSAGE_NEW:
                sip_handle_message(ev);
                break;

            case EXOSIP_MESSAGE_ANSWERED:
                if (ev->response && ev->response->status_code==200) {
                    if (ev->response->call_id && g_last_keepalive_call_id[0] &&
                        ev->response->call_id->number &&
                        !strcmp(ev->response->call_id->number, g_last_keepalive_call_id)) {
                        atomic_store(&g_keepalive_state.pending_response, 0);
                        g_keepalive_state.retry_count = 0;
                        g_last_keepalive_call_id[0] = '\0';
                    }
                }
                break;

            default:
                break;
            }
            eXosip_event_free(ev);
        }

        check_and_handle_bye_request();     // 兜底检查BYE请求（避免漏处理）
        sip_check_ack_timeout();            // 检查ACK超时（若等待ACK推流，超时则处理）

        uint64_t now = get_timestamp_ms();
        if (now - last_keepalive_check >= 1000) {   //每秒钟检查一次是否需要发生心跳
            sip_check_keepalive();                  // 心跳是20秒发一次
            check_registration_status();            // 检查注册状态
            last_keepalive_check = now;
        }

        //五秒检查一次是否注册状态
        if (now - last_reg_check >= 5000) {
            if (atomic_load(&g_registered)) {
                uint64_t elapsed = now - g_last_reg_success_ms;
                int refresh = SIP_REGISTER_INTERVAL;
                if (refresh < 300) { refresh -= 30; if (refresh < 60) refresh = 60; }
                else refresh -= 300;
                if (elapsed >= (uint64_t)refresh*1000ULL) exosip_send_register(SIP_REGISTER_INTERVAL);
            } else {
                if (g_next_reg_retry_ms==0) {
                    if (now - g_last_reg_attempt_ms >= 30000) exosip_send_register(SIP_REGISTER_INTERVAL);
                } else if (now >= g_next_reg_retry_ms) {
                    exosip_send_register(SIP_REGISTER_INTERVAL);
                }
            }
            last_reg_check = now;
        }

        eXosip_automatic_action(g_exosip_ctx);
        usleep(10000);
    }

    if (g_exosip_ctx && atomic_load(&g_registered)) {
        exosip_send_register(0);
        sleep(1);
    }
    if (g_exosip_ctx) {
        eXosip_quit(g_exosip_ctx);
        osip_free(g_exosip_ctx);
        g_exosip_ctx=NULL;
    }
    LOG_INFO("SIP线程退出");
    return NULL;
}

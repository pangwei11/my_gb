#include "globals.h"
#include "frame_queue.h"
#include "rtsp.h"
#include "sip.h"
#include "rtp.h"
#include "ps_mux.h"

// ===================== WVP 90kHz 时间戳：基于 CLOCK_MONOTONIC，确保单调递增 =====================
typedef struct {
    int initialized;
    uint64_t last_mono_ms;
    int64_t stable_ts_90k;
    uint32_t last_rtp_ts;
    int64_t src_base_pts; // 仅用于对齐/调试
} WvpTsCtx;

static void wvp_ts_reset(WvpTsCtx *c) {
    if (!c) return;
    memset(c, 0, sizeof(*c));
}

static void wvp_ts_tick(WvpTsCtx *c, TsMode mode, int64_t src_pts, int fps, int64_t *out_pts_90k, uint32_t *out_rtp_ts) {
    if (!c || !out_pts_90k || !out_rtp_ts) return;

    int64_t expected = 90000 / (fps > 0 ? fps : 15);
    if (expected <= 0) expected = 6000;

    uint64_t now_ms = get_monotonic_ms();

    if (!c->initialized) {
        c->initialized = 1;
        c->last_mono_ms = now_ms;
        c->stable_ts_90k = 0;
        c->last_rtp_ts = 0;
        c->src_base_pts = src_pts;
    } else {
        uint64_t delta_ms = (now_ms >= c->last_mono_ms) ? (now_ms - c->last_mono_ms) : 0;
        // 防止偶发的系统时间抖动影响节奏
        if (delta_ms > 200) delta_ms = 200;
        int64_t inc;
        if (mode == TS_FIXED_STEP) {
            // 强制固定步进：每帧严格 +expected，适配 WVP 对时间戳敏感的场景
            inc = expected;
        } else {
            // MONOTONIC：用系统 CLOCK_MONOTONIC 推导节奏（对变帧率更友好）
            inc = (int64_t)(delta_ms * 90); // 90kHz: 90 ticks per ms
            // 若系统调度导致 delta_ms 太小，用期望步进兜底，保持稳定帧率
            if (inc < expected / 2) inc = expected;
            // 若 delta_ms 太大，避免大跳变
            if (inc > expected * 3) inc = expected;
        }
        c->stable_ts_90k += inc;
        c->last_mono_ms = now_ms;
    }

    uint32_t ts = (uint32_t)(c->stable_ts_90k & 0xFFFFFFFF);
    if (c->last_rtp_ts != 0 && ts <= c->last_rtp_ts) {
        ts = c->last_rtp_ts + (uint32_t)expected;
    }
    c->last_rtp_ts = ts;

    *out_pts_90k = c->stable_ts_90k;
    *out_rtp_ts = ts;
}

static void* ps_rtp_thread(void *arg) {
    (void)arg;
    LOG_INFO("PS/RTP线程启动");

    uint8_t *ps_buf = (uint8_t*)malloc(PS_BUFFER_SIZE);
    if (!ps_buf) return NULL;

    FrameData frame = {0};
    int need_idr = 1;
    uint64_t wait_idr_start = 0;
    uint64_t last_rtp_check = get_timestamp_ms();

    // WVP：使用 MONOTONIC 推导 90kHz，并保证 RTP timestamp 单调递增
    WvpTsCtx ts_ctx;
    wvp_ts_reset(&ts_ctx);

    // 用于“无新帧”时的保活：缓存最近一次IDR
    uint8_t *last_idr = NULL;
    int last_idr_size = 0;
    uint64_t last_real_frame_ms = 0;
    uint64_t last_filler_ms = 0;
    int64_t last_src_pts = 0;
    int warned_wait_idr = 0;


    while (atomic_load(&g_state.running)) {
        if (!atomic_load(&g_state.streaming)) {
            usleep(100000);
            need_idr = 1;
            wait_idr_start = 0;
            g_first_rtp_pts = 0;
            wvp_ts_reset(&ts_ctx);
            last_real_frame_ms = 0;
            last_filler_ms = 0;
            last_src_pts = 0;
            warned_wait_idr = 0;
            if (last_idr) {
                free(last_idr);
                last_idr = NULL;
                last_idr_size = 0;
            }
            g_no_rtp_count = 0;
            continue;
        }

        // TCP连接超时
        pthread_mutex_lock(&g_rtp_ctx.lock);
        int is_tcp = g_rtp_ctx.is_tcp;
        int tcp_connected = g_rtp_ctx.tcp_connected;
        pthread_mutex_unlock(&g_rtp_ctx.lock);

        if (is_tcp && !tcp_connected) {
            if (g_tcp_wait_start_ms) {
                uint64_t now = get_monotonic_ms();
                if (now - g_tcp_wait_start_ms > TCP_CONNECT_TIMEOUT_MS) {
                    LOG_ERROR("TCP连接等待超时，触发BYE");
                    atomic_store(&g_need_send_bye, 1);
                    atomic_store(&g_tcp_connecting, 0);
                }
            }
            usleep(10000);
            continue;
        }

        // 无RTP发送检测：每5秒检查一次，连续3次判定断链
        uint64_t now = get_monotonic_ms();
        if (now - last_rtp_check >= 5000) {
            uint64_t last_ts = atomic_load(&g_rtp_ctx.last_rtp_send_ts);
            uint64_t last_try = atomic_load(&g_rtp_ctx.last_rtp_attempt_ts);
            int err_now = atomic_load(&g_rtp_ctx.rtp_send_errors);
            int drop_now = atomic_load(&g_rtp_ctx.rtp_drop_count);
            if (last_try && (now - last_try) > 5000 && last_ts && (now - last_ts) > 5000) {
                g_no_rtp_count++;
                LOG_WARN("无RTP检测: now=%llu last_ok=%llu last_try=%llu delta_ok=%llums delta_try=%llums err=%d drop=%d count=%d",
                         (unsigned long long)now,
                         (unsigned long long)last_ts,
                         (unsigned long long)last_try,
                         (unsigned long long)(now - last_ts),
                         (unsigned long long)(now - last_try),
                         err_now,
                         drop_now,
                         g_no_rtp_count);
                LOG_WARN("连续%d次检测到5秒无RTP发送", g_no_rtp_count);
                if (g_no_rtp_count >= 3) {
                    LOG_ERROR("连续15秒无RTP发送，触发BYE");
                    atomic_store(&g_need_send_bye, 1);
                    g_no_rtp_count = 0;
                }
            } else {
                g_no_rtp_count = 0;
            }
            last_rtp_check = now;
        }

        // Try to pop a frame. If RTSP stalls briefly, optionally resend last IDR to keep the media path alive.
        if (frame_queue_pop(&g_frame_queue, &frame, 100) < 0) {
            uint64_t nowms = get_monotonic_ms();
            if (last_real_frame_ms == 0) last_real_frame_ms = nowms;

            if (g_cfg.wvp.enable_filler_idr &&
                !need_idr && last_idr && last_idr_size > 0 &&
                nowms - last_real_frame_ms >= (uint64_t)WVP_FILLER_IDLE_MS &&
                nowms - last_filler_ms >= (uint64_t)WVP_FILLER_INTERVAL_MS) {

                int64_t media_pts_90k = 0;
                uint32_t rtp_ts = 0;
                wvp_ts_tick(&ts_ctx, g_cfg.wvp.ts_mode, last_src_pts, VIDEO_FPS, &media_pts_90k, &rtp_ts);

                int ps_size = 0;
                if (generate_ps_packet(ps_buf, PS_BUFFER_SIZE, last_idr, last_idr_size,
                                       media_pts_90k, /*is_keyframe*/1, &ps_size) == 0) {
                    int off = 0;
                    while (off < ps_size) {
                        int chunk = ps_size - off;
                        int maxpkt = g_cfg.media.rtp_max_packet_size;
                        if (maxpkt <= 0) maxpkt = 1400;
                        if (chunk > maxpkt) chunk = maxpkt;
                        if (chunk > 1400) chunk = 1400;
                        int marker = (off + chunk >= ps_size) ? 1 : 0;
                        send_rtp_packet(&g_rtp_ctx, ps_buf + off, chunk, rtp_ts, marker);
                        off += chunk;
                    }
                    last_filler_ms = nowms;
                }
            }
            continue;
        }

        // Track last real frame time and cache last IDR (for filler)
        last_real_frame_ms = get_monotonic_ms();
        last_src_pts = frame.pts;
        if (frame.is_keyframe && frame.data && frame.size > 0) {
            uint8_t *newbuf = (uint8_t*)malloc((size_t)frame.size);
            if (newbuf) {
                memcpy(newbuf, frame.data, (size_t)frame.size);
                free(last_idr);
                last_idr = newbuf;
                last_idr_size = frame.size;
            }
        }

        if (need_idr && !frame.is_keyframe) {
            // Prefer to start with an IDR, but many RTSP sources send IDR slowly.
            // If we drop all P-frames for too long, WVP/ZLM may timeout the media path.
            uint64_t t = get_monotonic_ms();
            if (!wait_idr_start) wait_idr_start = t;

            if (t - wait_idr_start < (uint64_t)WVP_MAX_WAIT_IDR_MS) {
                free(frame.data); frame.data = NULL;
                continue;
            }

            if (!warned_wait_idr) {
                LOG_WARN("等待IDR超过%ums，先发送P帧保活，直到收到IDR再恢复正常显示", (unsigned)WVP_MAX_WAIT_IDR_MS);
                warned_wait_idr = 1;
            }
            // fallthrough: send this P-frame
        }

        if (need_idr && frame.is_keyframe) {
            LOG_INFO("收到IDR，开始发送");
            need_idr = 0;
            atomic_store(&g_state.need_idr, 0);
            wait_idr_start = 0;
        }

        if (!g_first_rtp_pts) g_first_rtp_pts = frame.pts;

#if defined(WVP_DIAG) && WVP_DIAG
        // 每秒输出一次关键运行指标（用于定位“播放几分钟后被BYE”的问题）
        static uint64_t last_stat_ms = 0;
        static int last_sent = 0;
        static int last_err = 0;
        static int last_drop = 0;
        uint64_t stat_now = get_monotonic_ms();
        if (stat_now - last_stat_ms >= (uint64_t)WVP_RTP_STAT_INTERVAL_MS) {
            int q = frame_queue_get_count(&g_frame_queue);
            int sent = atomic_load(&g_rtp_ctx.rtp_packets_sent);
            int err = atomic_load(&g_rtp_ctx.rtp_send_errors);
            int drop = atomic_load(&g_rtp_ctx.rtp_drop_count);
            uint64_t last_ok = atomic_load(&g_rtp_ctx.last_rtp_send_ts);
            uint64_t last_try = atomic_load(&g_rtp_ctx.last_rtp_attempt_ts);
            LOG_DEBUG("[WVP诊断] 队列=%d/%d RTP=%d(+%d/s) err=%d(+%d) drop=%d(+%d) last_ok_age=%llums last_try_age=%llums",
                     q, MAX_FRAME_QUEUE,
                     sent, sent - last_sent,
                     err, err - last_err,
                     drop, drop - last_drop,
                     (unsigned long long)(last_ok ? (stat_now - last_ok) : 0),
                     (unsigned long long)(last_try ? (stat_now - last_try) : 0));
            last_stat_ms = stat_now;
            last_sent = sent;
            last_err = err;
            last_drop = drop;
        }
#endif

#if ENABLE_PTS_PACING
        int backlog = frame_queue_get_count(&g_frame_queue);
        static uint64_t pacing_start_us = 0;
        static int64_t pacing_start_pts = 0;
        if (!pacing_start_us) { pacing_start_us = get_monotonic_us(); pacing_start_pts = frame.pts; }
        if (backlog < PACING_BACKLOG_THRESHOLD) {
            uint64_t target = pacing_start_us + (uint64_t)((frame.pts - pacing_start_pts) * 1000000LL / 90000LL);
            uint64_t nowus = get_monotonic_us();
            if (target > nowus) {
                uint64_t sleep_us = target - nowus;
                if (sleep_us > 200000) sleep_us = 200000;
                usleep((useconds_t)sleep_us);
            }
        } else {
            pacing_start_us = 0; pacing_start_pts = 0;
        }
#endif

        // ===================== WVP：时间戳策略（非常关键） =====================
        // 说明：WVP/JS 解码器对时间戳跳变非常敏感。
        // 如果 RTSP 源 pts 抖动/跳变，建议使用固定步进 90kHz 生成媒体时间戳。
        int64_t media_pts_90k = frame.pts;
        uint32_t ts = 0;
        // 90kHz PTS/RTP timestamp: derive from CLOCK_MONOTONIC and keep monotonic.
        wvp_ts_tick(&ts_ctx, g_cfg.wvp.ts_mode, frame.pts, VIDEO_FPS, &media_pts_90k, &ts);

        int ps_size=0;
        if (generate_ps_packet(ps_buf, PS_BUFFER_SIZE, frame.data, frame.size, media_pts_90k, frame.is_keyframe, &ps_size) < 0) {
            free(frame.data); frame.data=NULL;
            continue;
        }

        int off=0;
        while (off < ps_size) {
            int chunk = ps_size - off;
            int maxpkt = g_cfg.media.rtp_max_packet_size;
            if (maxpkt <= 0) maxpkt = 1400;
            if (chunk > maxpkt) chunk = maxpkt;
            if (chunk > 1400) chunk = 1400;
            int marker = (off + chunk >= ps_size) ? 1 : 0;

            if (send_rtp_packet(&g_rtp_ctx, ps_buf+off, chunk, ts, marker) < 0) usleep(10000);
            off += chunk;
        }

free(frame.data); frame.data=NULL;
    }

    if (frame.data) free(frame.data);
    if (last_idr) free(last_idr);
    free(ps_buf);
    LOG_INFO("PS/RTP线程退出");
    return NULL;
}

int main(int argc, char *argv[]) {
    state_init();
    const char *cfg_path = "/root/project/config/gb28181_config.json";
    if (argc >= 2 && argv[1] && argv[1][0]) cfg_path = argv[1];

    LOG_INFO("========== GB28181推流程序启动 ==========");
    LOG_INFO("版本: split-1.0-stable18");

    // Load runtime config (defaults from include/config.h, overrides from config.json)
    app_config_set_defaults(&g_cfg);    // GB28181 推流程序的配置默认值初始化核心函数
    int cfg_rc = app_config_load_json(&g_cfg, cfg_path);    //从 JSON 配置文件加载配置并覆盖默认值的核心函数
    if (cfg_rc == 0) {
        LOG_INFO("已加载配置: %s", cfg_path);
    } else {
        LOG_WARN("配置加载失败(%d)，使用默认编译参数。path=%s", cfg_rc, cfg_path);
    }

        // ========== 自动检测IP逻辑 ==========
    // 如果 local_ip 配置为 "auto" 或 "0.0.0.0"，则自动获取
    if (!strcmp(g_cfg.device.local_ip, "auto") || !strcmp(g_cfg.device.local_ip, "0.0.0.0")) {
        char detected_ip[64] = {0};
        if (auto_detect_local_ip(detected_ip, sizeof(detected_ip)) == 0) {
            strncpy(g_cfg.device.local_ip, detected_ip, sizeof(g_cfg.device.local_ip) - 1);
            LOG_INFO("local_ip 已自动设置为: %s", g_cfg.device.local_ip);
        } else {
            LOG_ERROR("自动检测 local_ip 失败，请检查网络连接！");
            return -1;
        }
    }

        // 【修复隐患】如果 sdp_ip 配置为 "0.0.0.0"、"auto" 或为空，自动等同于 local_ip
    if (!strcmp(g_cfg.device.sdp_ip, "0.0.0.0") || !strcmp(g_cfg.device.sdp_ip, "auto") || g_cfg.device.sdp_ip[0] == '\0') {
        strncpy(g_cfg.device.sdp_ip, g_cfg.device.local_ip, sizeof(g_cfg.device.sdp_ip) - 1);
        LOG_INFO("sdp_ip 未明确配置，已自动同步为 local_ip: %s", g_cfg.device.sdp_ip);
    }

    ignore_sigpipe();                   //忽略SIGPIPE 信号
    install_signal_handlers();

    if (!strcmp(g_cfg.device.local_ip, "0.0.0.0") || !strcmp(g_cfg.device.local_ip, "127.0.0.1")) {
        LOG_ERROR("local_ip不能是0.0.0.0或127.0.0.1：%s", g_cfg.device.local_ip);
        return -1;
    }
    if (!strcmp(g_cfg.device.sdp_ip, "127.0.0.1")) {
        LOG_ERROR("sdp_ip不能是127.0.0.1：%s", g_cfg.device.sdp_ip);
        return -1;
    }

    check_device_id();


    avformat_network_init();    ///*这行代码是 FFmpeg 网络模块的初始化入口,*该程序的核心流程是「RTSP 拉流 → PS 封装 → RTP/SIP 推流」，这行代码的作用直接服务于 RTSP 拉流环节*/

    frame_queue_init(&g_frame_queue);   //帧队列初始化函数,g_frame_queue 是 RTSP 拉流线程（生产视频帧）和 PS/RTP 发送线程（消费视频帧）的核心数据通道
    memset(&g_rtp_ctx, 0, sizeof(g_rtp_ctx));   //将全局 RTP 传输上下文结构体 g_rtp_ctx 的所有成员变量（如 socket 句柄、TCP 状态、计数器等）强制初始化为 0；
    pthread_mutex_init(&g_rtp_ctx.lock, NULL);
    g_rtp_ctx.sock = -1;                                        //RTP 核心 socket 句柄初始化
    g_rtp_ctx.tcp_listen_sock = -1;                             //TCP 监听 socket 句柄初始化
    g_rtp_ctx.payload_type = g_cfg.media.rtp_payload_type;      //RTP 负载类型赋值
    atomic_store(&g_rtp_ctx.tcp_connect_failed, 0);             //通过原子操作将 TCP 连接失败标记置为 0
    atomic_store(&g_rtp_ctx.rtp_packets_sent, 0);               //RTP 发送成功计数器初始化
    atomic_store(&g_rtp_ctx.rtp_send_errors, 0);                //RTP 发送错误计数器初始化
    atomic_store(&g_rtp_ctx.last_rtp_send_ts, 0);               //最后一次 RTP 发送时间戳初始化

    rtsp_preheat_init();                                        // RTSP 预热线程的初始化入口函数

    pthread_t sip_tid, rtsp_tid, ps_tid;
    if (pthread_create(&sip_tid, NULL, sip_thread, NULL)!=0) { LOG_ERROR("创建SIP线程失败"); return -1; }           //处理 GB28181 协议的 SIP 信令
    sleep(2);
    if (pthread_create(&rtsp_tid, NULL, rtsp_pull_thread, NULL)!=0) { LOG_ERROR("创建RTSP线程失败"); return -1; }   //从前端摄像头 / 流媒体服务器拉取 RTSP 视频流（获取原始视频帧数据）
    //将拉取的视频帧封装为 PS 格式（GB28181 要求的媒体封装格式），并通过 RTP 协议发送给 GB28181 平台（如 WVP/ZLMediaKit 等）。
    if (pthread_create(&ps_tid, NULL, ps_rtp_thread, NULL)!=0) { LOG_ERROR("创建PS线程失败"); return -1; }

    int heartbeat=0;                            //heartbeat=0 初始化心跳计数器
    while (atomic_load(&g_state.running)) {
        sleep(1);
        check_and_log_signals();                //检查程序是否收到系统信号
        if (++heartbeat >= 30) {                //每 30 秒输出心跳日志
            int q = frame_queue_get_count(&g_frame_queue);      //获取帧队列积压数

            //加锁读取RTP上下文
            pthread_mutex_lock(&g_rtp_ctx.lock);
            int tcp_mode = g_rtp_ctx.is_tcp ? (g_rtp_ctx.tcp_passive_mode ? 2 : 1) : 0;
            int tcp_conn = g_rtp_ctx.tcp_connected;
            int sock_valid = (g_rtp_ctx.sock >= 0);
            uint32_t ssrc = g_rtp_ctx.ssrc;
            int pt = g_rtp_ctx.payload_type;
            pthread_mutex_unlock(&g_rtp_ctx.lock);

            //构造传输模式/TCP连接状态字符串
            const char *mode = (tcp_mode==0)?"UDP":(tcp_mode==1)?"TCP(active)":"TCP(passive)";
            const char *conn = (tcp_mode==0)?"N/A":(tcp_conn?"已连接":"未连接");
            int sent = atomic_load(&g_rtp_ctx.rtp_packets_sent);
            int err  = atomic_load(&g_rtp_ctx.rtp_send_errors);
            LOG_INFO("心跳: 注册=%s 推流=%s 传输=%s TCP=%s sock=%s SSRC=%u PT=%d 队列=%d/%d RTP=%d err=%d",
                     atomic_load(&g_registered)?"是":"否",
                     atomic_load(&g_state.streaming)?"是":"否",
                     mode, conn, sock_valid?"有效":"无效",
                     ssrc, pt, q, MAX_FRAME_QUEUE, sent, err);
            heartbeat=0;
        }
    }

    LOG_INFO("退出中...");
    pthread_join(sip_tid, NULL);
    pthread_join(rtsp_tid, NULL);
    pthread_join(ps_tid, NULL);

    rtsp_preheat_cleanup();
    rtp_socket_close();
    frame_queue_clean(&g_frame_queue);
    if (g_bsf_ctx) { av_bsf_free(&g_bsf_ctx); g_bsf_ctx=NULL; }

    avformat_network_deinit();
    close_log_file();
    pthread_mutex_destroy(&g_rtp_ctx.lock);

    LOG_INFO("========== 程序退出 ==========");
    return 0;
}

#include "rtsp.h"
#include "frame_queue.h"

#ifdef USE_LOCAL_VENC
// VENC 模式下，不需要 RTSP 预热
void rtsp_preheat_init(void) { LOG_INFO("VENC模式，跳过RTSP预热"); }
void rtsp_preheat_cleanup(void) { }
#else
// ================= 下面是原来的 RTSP 预热代码 =================
static atomic_int g_rtsp_preheated = ATOMIC_VAR_INIT(0);

static void* rtsp_preheat_thread(void *arg) {
    (void)arg;
    AVFormatContext *fmt = NULL;            //FFmpeg 用于封装音视频格式（此处为 RTSP）的核心结构体

    while (atomic_load(&g_state.running)) {     //原子操作读取全局运行状态（0/1）,只要程序未退出（g_state.running=1），就持续尝试预热 / 维持预热连接
        if (atomic_load(&g_rtsp_preheated)) { usleep(1000000); continue; }      //若已完成预热，线程休眠 1 秒（1000000 微秒）后重新检查，避免空循环占用 CPU；若未预热，则进入建联逻辑。

        //RTSP 连接参数配置（FFmpeg 选项）
        AVDictionary *opt=NULL;
        const char *transport = (g_cfg.media.transport == TRANS_UDP) ? "udp" : "tcp";
        av_dict_set(&opt, "rtsp_transport", transport, 0);
        //av_dict_set(&opt, "rtsp_transport", "tcp", 0);      // 强制RTSP over TCP（国标常用）
        av_dict_set(&opt, "stimeout", "10000000", 0);       // RTSP超时时间：10秒（微秒）
        av_dict_set(&opt, "max_delay", "1000000", 0);       // 最大延迟：1秒（微秒）
        //av_dict_set(&opt, "buffer_size", "131072", 0);      // 接收缓冲区大小：128KB
        av_dict_set(&opt, "buffer_size", "1048576", 0);   // 1 MB
        av_dict_set(&opt, "rtsp_flags", "prefer_tcp", 0);   // 优先使用TCP传输（兜底配置）

        //RTSP 建联与流信息获取
        int ret = avformat_open_input(&fmt, g_cfg.media.rtsp_url, NULL, &opt);
        av_dict_free(&opt);                                 //// 释放参数字典（用完即释放，避免内存泄漏）
        if (ret<0) { usleep(5000000); continue; }           // 建联失败：休眠5秒后重试
        if (avformat_find_stream_info(fmt, NULL) < 0) { avformat_close_input(&fmt); fmt=NULL; usleep(5000000); continue; }    // 获取流信息（解析SDP、码率、编码格式等）

        LOG_INFO("RTSP预热成功，保持连接");
        atomic_store(&g_rtsp_preheated, 1);              // 原子操作标记“预热完成”

        // 保活循环：程序运行中且预热状态未被重置时，定期读帧维持连接
        while (atomic_load(&g_state.running) && atomic_load(&g_rtsp_preheated)) {
            AVPacket *pkt = av_packet_alloc();          // 分配数据包内存
            if (!pkt) break;
            int rr = av_read_frame(fmt, pkt);           // 读取一帧数据（触发RTSP心跳，维持连接）
            av_packet_free(&pkt);                       // 释放数据包（仅读帧保活，不处理数据）
            if (rr<0) break;
            usleep(30000000); // 30s
        }

        // 关闭RTSP连接，清理上下文
        if (fmt) { avformat_close_input(&fmt); fmt=NULL; }
        atomic_store(&g_rtsp_preheated, 0);      // 标记“预热失效”，外层循环会重新尝试建联
    }

    if (fmt) avformat_close_input(&fmt);
    return NULL;
}

void rtsp_preheat_init(void) {
    pthread_t t;        //声明 POSIX 线程 ID 变量 t：用于存储 pthread_create 创建线程后返回的线程唯一标识符，后续通过该 ID 操作线程（如设置分离态）。
    if (pthread_create(&t, NULL, rtsp_preheat_thread, NULL)==0) pthread_detach(t);      //pthread_detach(t)：将创建的线程设置为「分离态（detached）」
}

void rtsp_preheat_cleanup(void) {
    atomic_store(&g_rtsp_preheated, 0);
}
#endif // 👈【重点】在这里结束封印

#ifndef USE_LOCAL_VENC
static int init_h264_bsf(const AVCodecParameters *codecpar) {
    if (g_bsf_ctx) { av_bsf_free(&g_bsf_ctx); g_bsf_ctx=NULL; }
    const AVBitStreamFilter *bsf = av_bsf_get_by_name("h264_mp4toannexb");
    if (!bsf) return -1;
    if (av_bsf_alloc(bsf, &g_bsf_ctx) < 0) return -1;
    if (avcodec_parameters_copy(g_bsf_ctx->par_in, codecpar) < 0) { av_bsf_free(&g_bsf_ctx); g_bsf_ctx=NULL; return -1; }
    if (av_bsf_init(g_bsf_ctx) < 0) { av_bsf_free(&g_bsf_ctx); g_bsf_ctx=NULL; return -1; }
    return 0;
}


static int process_frame(AVFormatContext *fmt, AVPacket *pkt, int vs_idx, int *frame_count, int64_t *last_pts_90k, int *no_key) {
    int64_t pts_90k=0;
    if (pkt->pts == AV_NOPTS_VALUE) {
        if (g_pts_offset==0) g_pts_offset = (int64_t)(get_timestamp_ms() * 90);
        pts_90k = g_pts_offset + (int64_t)((*frame_count) * 90000 / VIDEO_FPS);
    } else {
        pts_90k = av_rescale_q(pkt->pts, fmt->streams[vs_idx]->time_base, (AVRational){1,90000});
    }
    if (*last_pts_90k && pts_90k <= *last_pts_90k) pts_90k = *last_pts_90k + (90000/VIDEO_FPS);
    *last_pts_90k = pts_90k;

    int is_key = (pkt->flags & AV_PKT_FLAG_KEY) ? 1 : is_h264_idr_frame(pkt->data, pkt->size);
    if (is_key) *no_key = 0;
    else {
        (*no_key)++;
        if (*no_key > 300) { *no_key = 0; return 1; } // need reconnect
    }

    frame_queue_push(&g_frame_queue, pkt->data, pkt->size, pts_90k, is_key);
    (*frame_count)++;
    return 0;
}
#endif // 👈【结束封印】

void* rtsp_pull_thread(void *arg) {
    (void)arg;

#ifdef USE_LOCAL_VENC
    LOG_INFO("启动本地 VENC 硬件拉流模式");
    
    // 1. 初始化硬件
    if (venc_local_init() != 0) {
        LOG_ERROR("VENC 初始化失败");
        return NULL;
    }
    
    // 2. 启动采集和编码线程
    venc_local_start();
    LOG_INFO("VENC 线程已启动，正在硬件采集编码...");

    // 3. 阻塞等待，直到 GB28181 程序收到退出信号 (Ctrl+C 等)
    // 真正的推流动作在 venc_get_stream_thread 里通过 frame_queue_push 完成了
    while (atomic_load(&g_state.running)) {
        sleep(1);
    }

    // 4. 收到退出信号，清理硬件资源
    venc_local_deinit();
    LOG_INFO("本地 VENC 拉流线程退出");
    return NULL;

#else
    LOG_INFO("RTSP拉流线程启动: %s", g_cfg.media.rtsp_url);

    AVFormatContext *fmt=NULL;              ////FFmpeg 核心上下文：管理RTSP流的格式、连接、流信息等
    AVPacket *pkt = av_packet_alloc();      //分配音视频数据包结构体：用于存储从RTSP流读取的单帧数据
    if (!pkt) return NULL;

    int vs_idx=-1;                          ///视频流索引：定位RTSP流中的视频轨道
    int frame_count=0;                      //已处理视频帧计数：辅助计算时间戳、统计帧数量
    int64_t last_pts_90k=0;                 //上一帧的PTS时间戳（90kHz基准）
    int no_key=0;                            //连续非关键帧计数

    while (atomic_load(&g_state.running)) {                                 ////线程主循环：原子操作读取全局运行状态
        if (!atomic_load(&g_state.streaming)) {                             //检查“拉流使能”状态：0=停止拉流，1=正常拉流
            if (fmt) { avformat_close_input(&fmt); fmt=NULL; }
            if (g_bsf_ctx) { av_bsf_free(&g_bsf_ctx); g_bsf_ctx=NULL; }
            vs_idx=-1; frame_count=0; last_pts_90k=0; no_key=0;             //重置所有状态变量为初始值
            usleep(1000000);
            continue;
        }

        // TCP模式且未连接：不拉流，避免队列撑爆
        pthread_mutex_lock(&g_rtp_ctx.lock);
        int is_tcp = g_rtp_ctx.is_tcp;
        int tcp_connected = g_rtp_ctx.tcp_connected;
        pthread_mutex_unlock(&g_rtp_ctx.lock);
        if (is_tcp && !tcp_connected) { usleep(100000); continue; }

        if (!fmt) {         //FFmpeg RTSP 参数配置
            AVDictionary *opt=NULL;
            av_dict_set(&opt, "rtsp_transport", "tcp", 0);
            av_dict_set(&opt, "stimeout", "10000000", 0);
            av_dict_set(&opt, "max_delay", "1000000", 0);
            av_dict_set(&opt, "buffer_size", "131072", 0);
            av_dict_set(&opt, "rtsp_flags", "prefer_tcp", 0);
            av_dict_set(&opt, "analyzeduration", "500000", 0);
            av_dict_set(&opt, "probesize", "250000", 0);

            int ret = avformat_open_input(&fmt, g_cfg.media.rtsp_url, NULL, &opt);      //创建 RTSP 连接，初始化fmt上下文，传入配置的 RTSP 地址
            av_dict_free(&opt);
            if (ret<0) { usleep(2000000); continue; }
            if (avformat_find_stream_info(fmt, NULL) < 0) { avformat_close_input(&fmt); fmt=NULL; usleep(2000000); continue; }      //// 解析流信息（SDP/编码/分辨率）

            //筛选视频流轨道,RTSP 流可能包含音频、视频、字幕等多个轨道，此处仅筛选视频流轨道
            vs_idx=-1;
            for (unsigned i=0;i<fmt->nb_streams;i++) if (fmt->streams[i]->codecpar->codec_type==AVMEDIA_TYPE_VIDEO) { vs_idx=(int)i; break; }
            if (vs_idx<0) { avformat_close_input(&fmt); fmt=NULL; usleep(2000000); continue; }

            AVCodecParameters *cp = fmt->streams[vs_idx]->codecpar;
            if (cp->codec_id != AV_CODEC_ID_H264) { avformat_close_input(&fmt); fmt=NULL; usleep(2000000); continue; }      //校验 H264 编码格式
            init_h264_bsf(cp);          //初始化 H264 过滤器 + 重置状态
            g_pts_offset = 0;
            g_last_psm_sent = 0;
            frame_count=0; last_pts_90k=0; no_key=0;

            LOG_INFO("RTSP连接成功: %dx%d H264", cp->width, cp->height);
        }

        av_packet_unref(pkt);       //重置AVPacket结构体,每次读新帧前必须unref，避免旧数据污染新帧
        int rr = av_read_frame(fmt, pkt);       //取 RTSP 帧并处理读帧失败
        if (rr<0) {
            avformat_close_input(&fmt); fmt=NULL;
            if (g_bsf_ctx) { av_bsf_free(&g_bsf_ctx); g_bsf_ctx=NULL; }
            usleep(1000000);
            continue;
        }
        if (pkt->stream_index != vs_idx) { av_packet_unref(pkt); continue; }    //筛选视频流帧,仅处理视频帧，过滤音频 / 字幕等无关轨道

        //初始化重连标记 + H264 格式转换
        int need_reconnect = 0;             //// 初始化：默认不需要重连
        if (g_bsf_ctx) {                    // 存在H264码流过滤器（已初始化)
            if (av_bsf_send_packet(g_bsf_ctx, pkt)==0) {    //// 把原始帧送入过滤器
                while (av_bsf_receive_packet(g_bsf_ctx, pkt)==0) {      // 循环接收转换后的帧（过滤器可能拆分/合并帧）
                    need_reconnect = process_frame(fmt, pkt, vs_idx, &frame_count, &last_pts_90k, &no_key);     // // 处理帧（入队、时间戳校正、关键帧检查）
                    av_packet_unref(pkt);
                    if (need_reconnect) break;
                }
            } else {    //// 过滤器发送失败，降级处理原始帧
                need_reconnect = process_frame(fmt, pkt, vs_idx, &frame_count, &last_pts_90k, &no_key);
            }
        } else {      //// 无过滤器，直接处理原始帧（兜底逻辑）
            need_reconnect = process_frame(fmt, pkt, vs_idx, &frame_count, &last_pts_90k, &no_key);
        }
        av_packet_unref(pkt);

        if (need_reconnect) {
            LOG_WARN("RTSP无关键帧超时，触发重连");
            avformat_close_input(&fmt); fmt=NULL;
            if (g_bsf_ctx) { av_bsf_free(&g_bsf_ctx); g_bsf_ctx=NULL; }
            usleep(1000000);
        }
    }

    if (fmt) avformat_close_input(&fmt);
    if (g_bsf_ctx) { av_bsf_free(&g_bsf_ctx); g_bsf_ctx=NULL; }
    av_packet_free(&pkt);
    LOG_INFO("RTSP拉流线程退出");
    return NULL;

#endif
}

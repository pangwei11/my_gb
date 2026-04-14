#include "rtp.h"

#define RTP_PACING_US 2000  // 2ms 基础节流；EAGAIN 时退避会更大

#define TCP_SNDTIMEO_MS 20  // TCP send timeout to avoid long stalls


static void rtp_tcp_set_sock_opts(int sock)
{
    // Avoid Nagle delay for realtime stream
    int one = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    // Set send timeout so send() won't block forever when uplink/platform is congested
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = TCP_SNDTIMEO_MS * 1000;
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static _Atomic int g_udp_backoff_us = 12000; // ~14ms @800kbps for 1412B pkt

static _Atomic int g_udp_target_kbps = 800; // default pacing target; can override with env RTP_TARGET_KBPS

static inline int udp_target_kbps(void){
    int v = atomic_load(&g_udp_target_kbps);
    return v > 0 ? v : 800;
}

static inline void udp_init_target_kbps_once(void){
    static int inited = 0;
    if (inited) return;
    inited = 1;
    const char* s = getenv("RTP_TARGET_KBPS");
    if (s && *s){
        int v = atoi(s);
        if (v >= 200 && v <= 5000){
            atomic_store(&g_udp_target_kbps, v);
        }
    }
}

// Compute minimal pacing interval based on packet size and target bitrate.
// interval_us = pkt_bits / (kbps*1000) seconds.
static inline int udp_pace_us(int pkt_size){
    int kbps = udp_target_kbps();
    long long bits = (long long)pkt_size * 8;
    long long us = (bits * 1000000LL) / ( (long long)kbps * 1000LL );
    if (us < 2000) us = 2000;      // don't spin too fast
    if (us > 20000) us = 20000;    // cap to 20ms to avoid excessive latency
    return (int)us;
}

static inline int udp_get_backoff_us(int pkt_size){ udp_init_target_kbps_once();
    int v = atomic_load(&g_udp_backoff_us);
    int pace = udp_pace_us(pkt_size);
    if (v < pace) v = pace;
    return v > 0 ? v : pace; }
static inline void udp_backoff_on_ok(void){ int v = atomic_load(&g_udp_backoff_us); if (v > RTP_PACING_US) atomic_store(&g_udp_backoff_us, v - 200); }
static inline void udp_backoff_on_eagain(void){ int v = atomic_load(&g_udp_backoff_us); int nv = v<1000?1000:v*2; if (nv>20000) nv=20000; atomic_store(&g_udp_backoff_us, nv); }

// 兼容：有些版本里调用的是 udp_backoff_on_fail()。
// 这里把它映射为与 EAGAIN 同样的退避策略，避免出现 undefined reference。
static inline void udp_backoff_on_fail(void){ udp_backoff_on_eagain(); }
  // reduce burst, help avoid UDP stalls and platform jitter
#include "frame_queue.h"



static int send_rtp_packet_tcp(int sock, const uint8_t* rtp_packet, int packet_size)
{
    // GB28181 TCP-RTP usually uses RFC4571 framing: 2-byte length prefix + RTP packet
    if (sock < 0 || !rtp_packet || packet_size <= 0) return -1;

    uint16_t nlen = htons((uint16_t)packet_size);
    uint8_t prefix[2];
    memcpy(prefix, &nlen, 2);

    // send prefix + payload with partial-send support
    const uint8_t* bufs[2] = { prefix, rtp_packet };
    int lens[2] = { 2, packet_size };

    for (int part = 0; part < 2; part++) {
        int sent = 0;
        while (sent < lens[part]) {
            int ret = send(sock, bufs[part] + sent, lens[part] - sent, MSG_NOSIGNAL);
            if (ret > 0) {
                sent += ret;
                continue;
            }
            if (ret == 0) {
                errno = ECONNRESET;
                return -1;
            }

            int e = errno;
            if (e == EINTR) continue;

            if (e == EAGAIN || e == EWOULDBLOCK) {
                // congested: for realtime, drop this RTP packet instead of blocking the whole pipeline
                //usleep(RTP_PACING_US * 10);
                return -2;
            }

            return -1;
        }
    }

    // light pacing
    usleep(RTP_PACING_US);
    return 0;
}



int rtp_socket_close(void) {
    int sock=-1, ls=-1;
    pthread_mutex_lock(&g_rtp_ctx.lock);
    if (g_rtp_ctx.sock>=0) { sock=g_rtp_ctx.sock; g_rtp_ctx.sock=-1; }
    if (g_rtp_ctx.tcp_listen_sock>=0) { ls=g_rtp_ctx.tcp_listen_sock; g_rtp_ctx.tcp_listen_sock=-1; }
    g_rtp_ctx.local_port = 0;
    g_rtp_ctx.tcp_connected = 0;
    g_rtp_ctx.is_tcp = 0;
    g_rtp_ctx.tcp_passive_mode = 0;
    atomic_store(&g_rtp_ctx.tcp_connect_failed, 0);
    pthread_mutex_unlock(&g_rtp_ctx.lock);
    if (sock>=0) close(sock);
    if (ls>=0) close(ls);
    return 0;
}

int rtp_udp_socket_create(const char *dest_ip, int dest_port, int *out_local_port) {
    rtp_socket_close();

    int new_sock=-1, local_port=0;
    int bind_ok=0;
    for (int attempt=0; attempt<5 && !bind_ok; attempt++) {
        new_sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (new_sock<0) return -1;
        int opt=1;
        setsockopt(new_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        // UDP socket use non-blocking to avoid long stalls; combine with pacing and realtime drop strategy
        int flags=fcntl(new_sock, F_GETFL, 0);
        fcntl(new_sock, F_SETFL, flags | O_NONBLOCK);
struct sockaddr_in la; memset(&la,0,sizeof(la));
        la.sin_family=AF_INET;
        la.sin_addr.s_addr=INADDR_ANY;
        int cfg_port = g_cfg.device.local_rtp_port;
        la.sin_port = htons((attempt==0 && cfg_port!=0) ? cfg_port : 0);

        if (bind(new_sock, (struct sockaddr*)&la, sizeof(la))<0) { close(new_sock); continue; }
        socklen_t alen=sizeof(la);
        getsockname(new_sock, (struct sockaddr*)&la, &alen);
        local_port = ntohs(la.sin_port);
        if ((local_port & 1)==0) bind_ok=1;
        else { close(new_sock); new_sock=-1; }
    }
    if (!bind_ok) return -1;

    int snd = 4*1024*1024;
    setsockopt(new_sock, SOL_SOCKET, SO_SNDBUF, &snd, sizeof(snd));
    struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 200000; // 200ms
    setsockopt(new_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int snd_actual=0; socklen_t snd_len=sizeof(snd_actual);
    if (getsockopt(new_sock, SOL_SOCKET, SO_SNDBUF, &snd_actual, &snd_len)==0) {
        LOG_INFO("UDP发送缓冲区(SO_SNDBUF)=%d bytes", snd_actual);
    }

    pthread_mutex_lock(&g_rtp_ctx.lock);
    g_rtp_ctx.sock = new_sock;
    g_rtp_ctx.local_port = local_port;
    g_rtp_ctx.is_tcp = 0;
    memset(&g_rtp_ctx.dst_addr,0,sizeof(g_rtp_ctx.dst_addr));
    g_rtp_ctx.dst_addr.sin_family=AF_INET;
    g_rtp_ctx.dst_addr.sin_port=htons(dest_port);
    g_rtp_ctx.dst_addr.sin_addr.s_addr=inet_addr(dest_ip);
    if (g_rtp_ctx.payload_type==0) g_rtp_ctx.payload_type=g_cfg.media.rtp_payload_type;
    g_rtp_ctx.seq = (uint16_t)(rand() & 0xFFFF);
    atomic_store(&g_rtp_ctx.rtp_packets_sent, 0);
    atomic_store(&g_rtp_ctx.rtp_send_errors, 0);
    atomic_store(&g_rtp_ctx.last_rtp_send_ts, 0);
    pthread_mutex_unlock(&g_rtp_ctx.lock);

    if (out_local_port) *out_local_port = local_port;
    LOG_INFO("RTP UDP socket创建: 本地0.0.0.0:%d -> %s:%d PT=%d",
             local_port, dest_ip, dest_port, g_rtp_ctx.payload_type);
    return 0;
}

void trigger_connection_broken(void) {
    if (atomic_exchange(&g_state.connection_broken, 1) == 0) {
        LOG_ERROR("RTP连接断开，触发断链处理");
        // Only request BYE when we have an established dialog and streaming is active.
        if (atomic_load(&g_state.streaming) && !atomic_load(&g_state.call_ended) && g_call_id != -1 && g_dialog_id != -1) {
            atomic_store(&g_need_send_bye, 1);
        } else {
            LOG_WARN("RTP断链但未建立对话或已结束: streaming=%d ended=%d call_id=%d dialog_id=%d", 
                     atomic_load(&g_state.streaming), atomic_load(&g_state.call_ended), g_call_id, g_dialog_id);
        }
    }
}

// ----- TCP passive accept thread -----
void* rtp_tcp_accept_thread(void *arg) {
    (void)arg;
    int listen_sock=-1;
    pthread_mutex_lock(&g_rtp_ctx.lock);
    listen_sock = g_rtp_ctx.tcp_listen_sock;
    pthread_mutex_unlock(&g_rtp_ctx.lock);
    if (listen_sock<0) return NULL;

    int wait_seconds = 30;
    for (int i=0;i<wait_seconds && atomic_load(&g_state.running); i++) {
        pthread_mutex_lock(&g_rtp_ctx.lock);
        int connected = g_rtp_ctx.tcp_connected;
        pthread_mutex_unlock(&g_rtp_ctx.lock);
        if (connected) return NULL;

        struct pollfd pfd = {.fd=listen_sock, .events=POLLIN};
        int pr = poll(&pfd,1,1000);
        if (pr>0 && (pfd.revents & POLLIN)) {
            struct sockaddr_in ca; socklen_t clen=sizeof(ca);
            int cs = accept(listen_sock, (struct sockaddr*)&ca, &clen);
            if (cs>=0) {
                int nodelay=1; setsockopt(cs, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
                int flags=fcntl(cs,F_GETFL,0); fcntl(cs,F_SETFL, flags | O_NONBLOCK);
                struct timeval to={.tv_sec=3,.tv_usec=0}; setsockopt(cs, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof(to));
                int snd=1024*1024; setsockopt(cs, SOL_SOCKET, SO_SNDBUF, &snd, sizeof(snd));

                pthread_mutex_lock(&g_rtp_ctx.lock);
                g_rtp_ctx.sock = cs;
                g_rtp_ctx.tcp_connected = 1;
                if (g_rtp_ctx.tcp_listen_sock>=0) { close(g_rtp_ctx.tcp_listen_sock); g_rtp_ctx.tcp_listen_sock=-1; }
                pthread_mutex_unlock(&g_rtp_ctx.lock);

                LOG_INFO("TCP RTP passive 已建立连接，开始推流");
                return NULL;
            }
        }
    }
    LOG_ERROR("TCP passive 等待连接超时，准备BYE");
    atomic_store(&g_need_send_bye, 1);
    return NULL;
}

int rtp_tcp_server_create(int *out_local_port) {
    int sock=-1, local_port=0, bind_ok=0;
    for (int attempt=0; attempt<5 && !bind_ok; attempt++) {
        sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock<0) return -1;
        int opt=1; setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        struct sockaddr_in sa; memset(&sa,0,sizeof(sa));
        sa.sin_family=AF_INET;
        sa.sin_addr.s_addr=INADDR_ANY;
        int cfg_port = g_cfg.device.local_rtp_port;
        sa.sin_port = htons((attempt==0 && cfg_port!=0) ? cfg_port : 0);
        if (bind(sock, (struct sockaddr*)&sa, sizeof(sa))<0) { close(sock); continue; }
        socklen_t alen=sizeof(sa);
        getsockname(sock, (struct sockaddr*)&sa, &alen);
        local_port = ntohs(sa.sin_port);
        if ((local_port & 1)==0) bind_ok=1;
        else { close(sock); sock=-1; }
    }
    if (!bind_ok) return -1;
    if (listen(sock, 1) < 0) { close(sock); return -1; }
    int flags=fcntl(sock, F_GETFL,0); fcntl(sock, F_SETFL, flags|O_NONBLOCK);
    if (out_local_port) *out_local_port = local_port;
    return sock;
}

// ----- TCP active connect -----
static int tcp_connect_on_socket(int sock, const char *ip, int port, int max_retries) {
    int orig = fcntl(sock, F_GETFL, 0);
    int backoff_ms[] = {200,500,1000,2000,5000};
    int backoff_n = (int)(sizeof(backoff_ms)/sizeof(backoff_ms[0]));

    for (int r=0; r<max_retries && atomic_load(&g_state.running); r++) {
        if (r>0) {
            int w = backoff_ms[(r-1<backoff_n)?(r-1):(backoff_n-1)];
            usleep((useconds_t)w*1000);
        }
        fcntl(sock, F_SETFL, orig | O_NONBLOCK);

        struct sockaddr_in sa; memset(&sa,0,sizeof(sa));
        sa.sin_family=AF_INET;
        sa.sin_port=htons(port);
        sa.sin_addr.s_addr=inet_addr(ip);

        int cr = connect(sock, (struct sockaddr*)&sa, sizeof(sa));
        if (cr<0 && errno!=EINPROGRESS) { fcntl(sock, F_SETFL, orig); continue; }

        struct pollfd pfd={.fd=sock,.events=POLLOUT};
        int pr = poll(&pfd,1,5000);
        if (pr<=0 || (pfd.revents&(POLLERR|POLLHUP|POLLNVAL))) { fcntl(sock, F_SETFL, orig); continue; }

        int so_error=0; socklen_t len=sizeof(so_error);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &len);
        if (so_error!=0) { fcntl(sock, F_SETFL, orig); continue; }

        struct timeval to={.tv_sec=3,.tv_usec=0};
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof(to));
        return 0;
    }
    fcntl(sock, F_SETFL, orig);
    return -1;
}

static void* rtp_tcp_connect_thread(void *arg) {
    (void)arg;
    // 等待streaming打开（最多5秒）
    int wait=0;
    while (!atomic_load(&g_state.streaming) && wait<500 && atomic_load(&g_state.running)) {
        usleep(10000); wait++;
    }
    if (!atomic_load(&g_state.running)) { atomic_store(&g_tcp_connecting, 0); return NULL; }

    char ip[32]; int port=0; int sock=-1;
    pthread_mutex_lock(&g_rtp_ctx.lock);
    strncpy(ip, g_rtp_ctx.tcp_target_ip, sizeof(ip));
    port = g_rtp_ctx.tcp_target_port;
    sock = g_rtp_ctx.sock;
    pthread_mutex_unlock(&g_rtp_ctx.lock);

    if (sock<0 || ip[0]==0 || port<=0) { atomic_store(&g_tcp_connecting, 0); return NULL; }

    usleep(500000); // 给平台准备时间

    int ret = tcp_connect_on_socket(sock, ip, port, 5);
    if (ret<0) {
        LOG_ERROR("TCP active 连接失败，触发BYE");
        pthread_mutex_lock(&g_rtp_ctx.lock);
        int to_close = g_rtp_ctx.sock;
        g_rtp_ctx.sock = -1;
        atomic_store(&g_rtp_ctx.tcp_connect_failed, 1);
        pthread_mutex_unlock(&g_rtp_ctx.lock);
        if (to_close>=0) close(to_close);
        atomic_store(&g_need_send_bye, 1);
        atomic_store(&g_tcp_connecting, 0);
        return NULL;
    }

    pthread_mutex_lock(&g_rtp_ctx.lock);
    g_rtp_ctx.tcp_connected = 1;
    atomic_store(&g_rtp_ctx.tcp_connect_failed, 0);
    pthread_mutex_unlock(&g_rtp_ctx.lock);

    LOG_INFO("TCP active 连接建立成功");
        rtp_tcp_set_sock_opts(sock);
atomic_store(&g_tcp_connecting, 0);
    return NULL;
}

void maybe_start_tcp_connect_after_ack(void) {
    pthread_mutex_lock(&g_rtp_ctx.lock);
    int need = (g_rtp_ctx.is_tcp &&
                !g_rtp_ctx.tcp_passive_mode &&
                !g_rtp_ctx.tcp_connected &&
                g_rtp_ctx.sock>=0 &&
                g_rtp_ctx.tcp_target_ip[0] &&
                g_rtp_ctx.tcp_target_port>0);
    pthread_mutex_unlock(&g_rtp_ctx.lock);
    if (!need) return;

    int expected=0;
    if (!atomic_compare_exchange_strong(&g_tcp_connecting, &expected, 1)) return;
    g_tcp_wait_start_ms = get_monotonic_ms();

    pthread_t t;
    if (pthread_create(&t, NULL, rtp_tcp_connect_thread, NULL)==0) pthread_detach(t);
    else atomic_store(&g_tcp_connecting, 0);
}

int send_rtp_packet(RtpContext *ctx, const uint8_t *payload, int payload_size,
                    uint32_t timestamp, int marker) {
    uint8_t packet[1500];
    int hdr = 12;
    if (payload_size > (int)sizeof(packet)-hdr) return -1;

    int sock=-1, is_tcp=0, pt=g_cfg.media.rtp_payload_type;
    struct sockaddr_in dst; memset(&dst,0,sizeof(dst));
    uint16_t seq=0; uint32_t ssrc=0;

    pthread_mutex_lock(&ctx->lock);
    if (ctx->sock<0) { pthread_mutex_unlock(&ctx->lock); return -1; }
    sock = ctx->sock;
    is_tcp = ctx->is_tcp;
    if (!is_tcp) dst = ctx->dst_addr;
    seq = ctx->seq++;
    ssrc = ctx->ssrc;
    pt = ctx->payload_type;
    pthread_mutex_unlock(&ctx->lock);

    packet[0] = 0x80;
    packet[1] = (uint8_t)((pt & 0x7F) | (marker?0x80:0x00));
    packet[2] = (seq>>8)&0xFF; packet[3]=(seq)&0xFF;
    packet[4] = (timestamp>>24)&0xFF; packet[5]=(timestamp>>16)&0xFF;
    packet[6] = (timestamp>>8)&0xFF;  packet[7]=(timestamp)&0xFF;
    packet[8] = (ssrc>>24)&0xFF; packet[9]=(ssrc>>16)&0xFF;
    packet[10]=(ssrc>>8)&0xFF;  packet[11]=(ssrc)&0xFF;
    memcpy(packet+hdr, payload, (size_t)payload_size);
    int pkt_size = hdr + payload_size;

    // mark attempt timestamp even if send fails (used to avoid false disconnect)
    atomic_store(&ctx->last_rtp_attempt_ts, (unsigned long long)get_monotonic_ms());

    int ret=0;
    if (is_tcp) {
        ret = send_rtp_packet_tcp(sock, packet, pkt_size);
        // send_rtp_packet_tcp(): 0=ok, -2=EAGAIN/timeout (treat as drop), -1=fatal
        if (ret == -2) {
            // TCP 临时阻塞：不要认为断链，也不要发 BYE。
            // 直接丢包（播放器会看到轻微卡顿，但不会黑屏退出）。
            atomic_fetch_add(&ctx->rtp_drop_count, 1);
            return 0;
        }
        if (ret < 0) trigger_connection_broken();
    } else {
        // UDP：基础节流 + 退避（EAGAIN/ENOBUFS）
        // 目的：避免 WiFi 场景瞬间把内核发送队列打满，导致连续 EAGAIN，播放器一直转圈。
        usleep(udp_get_backoff_us(pkt_size));

        ret = sendto(sock, packet, pkt_size, 0, (struct sockaddr*)&dst, sizeof(dst));
        if (ret >= 0) {
            udp_backoff_on_ok();
        } else if (errno==EAGAIN || errno==EWOULDBLOCK || errno==ENOBUFS) {
            udp_backoff_on_fail();
            // 轻微重试 1 次（避免阻塞太久）；仍失败就让上层丢弃该包
            usleep(udp_get_backoff_us(pkt_size));
            ret = sendto(sock, packet, pkt_size, 0, (struct sockaddr*)&dst, sizeof(dst));
            if (ret >= 0) udp_backoff_on_ok();
        }
    }

    if (ret<0) {
        int e = errno;
        if ((e==EBADF || e==ENOTCONN) &&
            (!atomic_load(&g_state.streaming) || atomic_load(&g_state.call_ended) || !atomic_load(&g_state.running))) {
            // Socket is closed during shutdown/stop; do not count as an error burst.
            return -1;
        }
        // For blocking UDP, errors usually mean ENOBUFS or network issue
        atomic_fetch_add(&ctx->rtp_send_errors, 1);
        // Rate-limited error logging (once per second, or on errno change)
        static uint64_t last_err_log_ms = 0;
        static int last_errno = 0;
        static int burst = 0;
        burst++;
        uint64_t now_ms = get_monotonic_ms();
        if (now_ms - last_err_log_ms >= 1000 || e != last_errno) {
            char ip[64] = {0};
            int dport = 0;
            if (is_tcp) {
                strncpy(ip, ctx->tcp_target_ip, sizeof(ip)-1);
                dport = ctx->tcp_target_port;
            } else {
                inet_ntop(AF_INET, &dst.sin_addr, ip, sizeof(ip));
                dport = ntohs(dst.sin_port);
            }
            int err_cnt = atomic_load(&ctx->rtp_send_errors);
            LOG_WARN("RTP发送失败: errno=%d(%s) pkt=%d seq=%u ts=%u ssrc=%u sock=%d -> %s:%d err=%d burst=%d",
                     e, strerror(e), pkt_size, seq, timestamp, ssrc, sock, ip, dport, err_cnt, burst);
            last_err_log_ms = now_ms;
            last_errno = e;
            burst = 0;
        }
        return -1;
    }

    atomic_store(&ctx->last_rtp_send_ts, get_monotonic_ms());
    atomic_fetch_add(&ctx->rtp_packets_sent, 1);
    return 0;
}

void check_and_handle_bye_request(void) {
    if (!atomic_load(&g_need_send_bye)) return;

    LOG_INFO("检测到需要发送BYE，开始处理");
    pthread_mutex_lock(&g_call_id_mutex);
    if (g_exosip_ctx && g_call_id >= 0) {
        eXosip_lock(g_exosip_ctx);
        eXosip_call_terminate(g_exosip_ctx, g_call_id, g_dialog_id);
        eXosip_unlock(g_exosip_ctx);
        g_call_id = -1;
        g_dialog_id = -1;
        LOG_INFO("已发送BYE");
    }
    pthread_mutex_unlock(&g_call_id_mutex);

    // stop streaming
    atomic_store(&g_state.streaming, 0);
    atomic_store(&g_state.waiting_for_ack, 0);
    atomic_store(&g_state.need_idr, 1);

    rtp_socket_close();
    atomic_store(&g_need_send_bye, 0);
    // keep connection_broken until next session
}

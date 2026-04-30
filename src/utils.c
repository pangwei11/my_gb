#include "common.h"
#include "globals.h"
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <bits/types/struct_tm.h>

/**
 * 自动检测当前活跃网络的IP（通过默认网关判断）
 */
int auto_detect_local_ip(char *out_ip, int out_size) {
    if (!out_ip || out_size <= 0) return -1;
    out_ip[0] = '\0';

    FILE *fp = fopen("/proc/net/route", "r");
    if (!fp) return -1;
    
    char line[256];
    char iface[IFNAMSIZ] = {0};
    int found = 0;
    
    // 跳过标题行
    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return -1;
    }
    
    // 查找默认网关（Dest为00000000，Gateway非0）
    while (fgets(line, sizeof(line), fp)) {
        unsigned int dest, gw;
        char ifname[IFNAMSIZ];
        if (sscanf(line, "%31s %x %x", ifname, &dest, &gw) >= 3) {
            if (dest == 0 && gw != 0) {
                strncpy(iface, ifname, sizeof(iface) - 1);
                found = 1;
                break;
            }
        }
    }
    fclose(fp);
    
    if (!found) return -1;
    
    // 获取该接口的IP
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return -1;
    
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);
    
    if (ioctl(sock, SIOCGIFADDR, &ifr) < 0) {
        close(sock);
        return -1;
    }
    close(sock);
    
    struct sockaddr_in *addr = (struct sockaddr_in *)&ifr.ifr_addr;
    strncpy(out_ip, inet_ntoa(addr->sin_addr), out_size - 1);
    
    LOG_INFO("自动检测网络接口 [%s] 的IP为: %s", iface, out_ip);
    return 0;
}

uint64_t get_timestamp_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

uint64_t get_monotonic_us(void) {
#if defined(CLOCK_MONOTONIC)
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
    }
#endif
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

uint64_t get_monotonic_ms(void) {
    return get_monotonic_us() / 1000ULL;
}

void log_print(const char *level, const char *fmt, ...) {
    // Runtime log filtering (loaded from config.json). If config not loaded yet,
    // g_cfg fields may be zero; treat that as "print all".
    int lvl = 1;
    if (level) {
        if (!strcmp(level, "DEBUG")) lvl = 0;
        else if (!strcmp(level, "INFO")) lvl = 1;
        else if (!strcmp(level, "WARN")) lvl = 2;
        else if (!strcmp(level, "ERROR")) lvl = 3;
    }
    int cfg_lvl = g_cfg.log.level;
    if (cfg_lvl < 0 || cfg_lvl > 3) cfg_lvl = 0; // default: no filter
    if (lvl < cfg_lvl) return;

    char buffer[768];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    struct timeval tv;
    gettimeofday(&tv, NULL);

    // 格式化时间为人类可读的 年-月-日 时:分:秒.微秒
    struct tm tm_info;
    char ts_buf[64];
    if (localtime_r(&tv.tv_sec, &tm_info)) {
        snprintf(ts_buf, sizeof(ts_buf), "%04d-%02d-%02d %02d:%02d:%02d.%06ld", 
                tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday, 
                tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec, (long)tv.tv_usec);
    } else {
        snprintf(ts_buf, sizeof(ts_buf), "%ld.%06ld", (long)tv.tv_sec, (long)tv.tv_usec);
    }
    printf("[%s] [%s] %s\n", ts_buf, level, buffer);

    pthread_mutex_lock(&g_log_mutex);
    if (!g_log_file) {
        const char *path = (g_cfg.log.file[0] ? g_cfg.log.file : "/root/project/log/gb28181.log");
        g_log_file = fopen(path, "a");
        if (g_log_file) setvbuf(g_log_file, NULL, _IOLBF, 0);
    }
    if (g_log_file) {
        struct timeval tv2;
        gettimeofday(&tv2, NULL);
        // 格式化时间为人类可读的 年-月-日 时:分:秒.微秒
        struct tm tm_info2;
        char ts_buf2[64];
        if (localtime_r(&tv2.tv_sec, &tm_info2)) {
            snprintf(ts_buf2, sizeof(ts_buf2), "%04d-%02d-%02d %02d:%02d:%02d.%06ld", 
                    tm_info2.tm_year + 1900, tm_info2.tm_mon + 1, tm_info2.tm_mday, 
                    tm_info2.tm_hour, tm_info2.tm_min, tm_info2.tm_sec, (long)tv2.tv_usec);
        } else {
            snprintf(ts_buf2, sizeof(ts_buf2), "%ld.%06ld", (long)tv2.tv_sec, (long)tv2.tv_usec);
        }
        fprintf(g_log_file, "[%s] [%s] %s\n", ts_buf2, level, buffer);

        fflush(g_log_file);
    }
    pthread_mutex_unlock(&g_log_mutex);
}

void close_log_file(void) {
    pthread_mutex_lock(&g_log_mutex);
    if (g_log_file) { fclose(g_log_file); g_log_file = NULL; }
    pthread_mutex_unlock(&g_log_mutex);
}

void ignore_sigpipe(void) { signal(SIGPIPE, SIG_IGN); }

static void signal_handler(int sig) { g_signal_received = sig; }

/*这段代码是 Unix/Linux 环境下的信号处理器安装函数，核心作用是为程序注册 SIGINT（终端中断）和 SIGTERM（进程终止）信号的自定义处理逻辑，
让 GB28181 推流程序能优雅退出（而非强制崩溃），同时保障网络 / IO 系统调用的稳定性。*/
void install_signal_handlers(void) {
    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, NULL);       // 注册 Ctrl+C 信号处理
    sigaction(SIGTERM, &sa, NULL);      // 注册 kill 命令信号处理
}

void check_and_log_signals(void) {
    static int last_signal_logged = 0;
    int sig = g_signal_received;
    if (sig && sig != last_signal_logged) {
        LOG_INFO("收到退出信号 %d", sig);
        atomic_store(&g_state.running, 0);
        last_signal_logged = sig;
    }
}

int is_all_digits(const char *s) {
    for (; *s; ++s) if (*s < '0' || *s > '9') return 0;
    return 1;
}

void check_device_id(void) {
    const char *devid = g_cfg.device.device_id;
    const char *chid  = g_cfg.device.channel_id;
    if (strlen(devid) != 20) {
        LOG_ERROR("DEVICE_ID长度错误(应为20位): %s (长度=%zu)", devid, strlen(devid));
        exit(1);
    }
    if (!is_all_digits(devid)) {
        LOG_ERROR("DEVICE_ID包含非数字字符: %s", devid);
        exit(1);
    }
    if (strlen(devid) >= 6) { memcpy(g_civil_code, devid, 6); g_civil_code[6] = '\0'; }
    LOG_INFO("设备ID校验通过: %s", devid);
    LOG_INFO("通道ID: %s (长度=%zu)", chid, strlen(chid));
    LOG_INFO("行政区码: %s", g_civil_code);
}

int xml_get_value(const char* xml, const char* tag, char* out, int out_sz) {
    char start_tag[64], end_tag[64];
    snprintf(start_tag, sizeof(start_tag), "<%s>", tag);
    snprintf(end_tag, sizeof(end_tag), "</%s>", tag);
    const char *start = strstr(xml, start_tag);
    if (!start) return -1;
    const char *end = strstr(start, end_tag);
    if (!end) return -1;
    int len = (int)(end - (start + strlen(start_tag)));
    if (len >= out_sz) len = out_sz - 1;
    memcpy(out, start + strlen(start_tag), (size_t)len);
    out[len] = '\0';
    return 0;
}

int is_h264_idr_frame(const uint8_t *data, int size) {
    if (!data || size < 5) return 0;
    for (int i = 0; i < size - 4; i++) {
        if (data[i]==0x00 && data[i+1]==0x00 && data[i+2]==0x01) {
            uint8_t t = data[i+3] & 0x1F;
            if (t==5) return 1;
        } else if (i+4 < size && data[i]==0x00 && data[i+1]==0x00 && data[i+2]==0x00 && data[i+3]==0x01) {
            uint8_t t = data[i+4] & 0x1F;
            if (t==5) return 1;
        }
    }
    return 0;
}

uint32_t generate_stable_ssrc(const char *device_id) {
    uint32_t hash = 0;
    for (const char *p = device_id; *p; ++p) hash = (hash << 5) + hash + (uint8_t)(*p);
    if (hash == 0) hash = 0x12345678;
    return hash & 0x7FFFFFFF;
}

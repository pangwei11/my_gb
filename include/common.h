#pragma once

#include <time.h>
//#include "uclibc_compat.h"
#include "config.h"


// ---------------- system headers ----------------
#include <time.h>
#include <sys/time.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <endian.h>
#include <sched.h>
#include <fcntl.h>
#include <sys/types.h>
#include <poll.h>
#include <ctype.h>
#include <stdatomic.h>


#include <openssl/md5.h>

// ---------------- third party ----------------
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/time.h>
#include <libavcodec/bsf.h>

#include <osip2/osip.h>
#include <eXosip2/eXosip.h>

// ---------------- common utilities ----------------
uint64_t get_timestamp_ms(void);
uint64_t get_monotonic_us(void);
uint64_t get_monotonic_ms(void);

void log_print(const char *level, const char *fmt, ...);

#define LOG_INFO(fmt, ...)  log_print("INFO",  fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  log_print("WARN",  fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) log_print("ERROR", fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) do { if (ENABLE_DEBUG_LOG) log_print("DEBUG", fmt, ##__VA_ARGS__); } while (0)

void close_log_file(void);
void ignore_sigpipe(void);
void install_signal_handlers(void);
void check_and_log_signals(void);

int is_all_digits(const char *s);
void check_device_id(void);

// simple xml helpers
int xml_get_value(const char* xml, const char* tag, char* out, int out_sz);

// h264
int is_h264_idr_frame(const uint8_t *data, int size);
uint32_t generate_stable_ssrc(const char *device_id);

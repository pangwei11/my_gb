#pragma once
// // uClibc 某些 sysroot 下 <time.h> 可能在 __need_time_t 等场景缺类型
// // 仅 32位 uClibc 环境启用兼容定义，64位aarch64自动跳过
// #if defined(__UCLIBC__) && !defined(__aarch64__)
// // uClibc 某些 sysroot 下 <time.h> 可能在 __need_time_t 等场景缺类型
// typedef long long time_t;
// struct timespec { time_t tv_sec; long tv_nsec; };
// struct tm {
//     int tm_sec;
//     int tm_min;
//     int tm_hour;
//     int tm_mday;
//     int tm_mon;
//     int tm_year;
//     int tm_wday;
//     int tm_yday;
//     int tm_isdst;
// };

// #endif
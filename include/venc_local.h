#ifndef VENC_LOCAL_H
#define VENC_LOCAL_H

// 初始化 V4L2 和 RK MPP 硬件编码器
int venc_local_init(void);

// 启动采集和获取码流的线程
int venc_local_start(void);

// 停止线程并清理所有硬件资源
void venc_local_deinit(void);

#endif

#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

#include "venc_local.h"
#include "frame_queue.h"
#include "globals.h"  // 引入 g_state

// 瑞芯微 MPP 接口 (保留)
#include "rk_debug.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_venc.h"

// 标准库 (保留)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <time.h>
#include "rk_debug.h"

// ====================== 核心配置 (保留) ======================
#define V4L2_DEVICE "/dev/video23"
// 注意：这里建议用宏读取 GB28181 的 config.h，或者直接写死
#define VIDEO_WIDTH 1920
#define VIDEO_HEIGHT 1080
#define VENC_BITRATE_KBPS 2048
#define VENC_GOP_SIZE 15
#define VENC_CHANNEL 0


void venc_trigger_wait_idr(void);

// ====================== 全局变量 (精简) ======================
static int g_v4l2_idx = 0;
static int g_v4l2_fd = -1;
static void *g_v4l2_buf = NULL;
static pthread_t g_send_th, g_get_th;

typedef struct {
    RK_U32 width;
    RK_U32 height;
    MB_POOL pool;
} VENC_CTX_S;

static VENC_CTX_S g_venc_ctx;

static volatile int g_wait_idr_flag = 0;

// 供 sip.c 调用，触发等待 I 帧
void venc_trigger_wait_idr(void) {
    g_wait_idr_flag = 1;
}






// ====================== V4L2 采集模块 (原封不动保留) ======================
// ====================== V4L2 采集模块 (保持不变) ======================
static int v4l2_init(int width, int height)
{
    struct v4l2_format fmt;
    struct v4l2_requestbuffers req;
    int ret;

    g_v4l2_fd = open(V4L2_DEVICE, O_RDWR);
    if (g_v4l2_fd < 0) {
        RK_LOGE("open video23 failed");
        return -1;
    }

    //设置摄像头采集格式
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width = width;
    fmt.fmt.pix_mp.height = height;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix_mp.num_planes = 1;

    ret = ioctl(g_v4l2_fd, VIDIOC_S_FMT, &fmt);
    if (ret < 0) {
        RK_LOGE("V4L2 设置格式失败");
        return -1;
    }

    //向内核申请视频缓冲区
    memset(&req, 0, sizeof(req));
    req.count = 1;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    ioctl(g_v4l2_fd, VIDIOC_REQBUFS, &req);

    //查询缓冲区的参数
    struct v4l2_buffer buf;
    struct v4l2_plane planes;
    memset(&buf, 0, sizeof(buf));
    memset(&planes, 0, sizeof(planes));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = 0;
    buf.m.planes = &planes;
    buf.length = 1;
    ioctl(g_v4l2_fd, VIDIOC_QUERYBUF, &buf);

    //内存映射（mmap）
    g_v4l2_buf = mmap(
        NULL,                // 系统自动分配虚拟地址
        planes.length,       // 缓冲区大小（从查询结果获取）
        PROT_READ | PROT_WRITE, // 权限：可读可写
        MAP_SHARED,          // 共享内存（内核和程序共用）
        g_v4l2_fd,           // 摄像头文件描述符
        planes.m.mem_offset  // 内核缓冲区偏移量（查询结果）
    );

    //启动视频流 + 缓冲区入队
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    ioctl(g_v4l2_fd, VIDIOC_STREAMON, &type);
    ioctl(g_v4l2_fd, VIDIOC_QBUF, &buf);        //把空缓冲区放入驱动队列

    RK_LOGI("V4L2 初始化完成");
    return 0;
}

static void *v4l2_grab_frame(RK_U32 *len) {
    struct v4l2_buffer buf;
    struct v4l2_plane planes;
    memset(&buf, 0, sizeof(buf));
    memset(&planes, 0, sizeof(planes));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.m.planes = &planes;
    buf.length = 1;
    
    if (ioctl(g_v4l2_fd, VIDIOC_DQBUF, &buf) < 0) {     //获取一帧画面
        *len = 0;
        return NULL;
    }
    
    g_v4l2_idx = buf.index;
    *len = planes.bytesused;
    return g_v4l2_buf;
}


// ====================== 码流获取线程 (消费者) ======================
void *venc_get_stream_thread(void *arg) {
    VENC_STREAM_S stStream;
    memset(&stStream, 0, sizeof(stStream));
    
    stStream.pstPack = malloc(sizeof(VENC_PACK_S));
    if (!stStream.pstPack) return NULL;

    int64_t last_pts_90k = 0; 
    int waiting_for_idr = 0;

    while (atomic_load(&g_state.running)) {
        stStream.u32PackCount = 0;
        RK_S32 s32Ret = RK_MPI_VENC_GetStream(VENC_CHANNEL, &stStream, 100);
        if (s32Ret != RK_SUCCESS) continue;

        // 状态1：未推流 或 刚收到点播指令
        if (!atomic_load(&g_state.streaming) || g_wait_idr_flag) {
            if (g_wait_idr_flag && atomic_load(&g_state.streaming)) {
                LOG_INFO("[VENC] 收到点播指令，等待下一个IDR...");
                last_pts_90k = 0;
                waiting_for_idr = 1;
                g_wait_idr_flag = 0;
                RK_MPI_VENC_RequestIDR(VENC_CHANNEL, RK_TRUE); 
            }
            RK_MPI_VENC_ReleaseStream(VENC_CHANNEL, &stStream);
            continue;
        }

        // 状态2：等待 IDR 帧到达
        if (waiting_for_idr) {
            // 👇【终极真理】2 是带 SPS/PPS 的 I 帧，5 是纯 IDR，都是关键帧！
            int is_idr = (stStream.pstPack[0].DataType.enH264EType == H264E_NALU_ISLICE || 
                          stStream.pstPack[0].DataType.enH264EType == H264E_NALU_IDRSLICE);
            if (!is_idr) {
                RK_MPI_VENC_ReleaseStream(VENC_CHANNEL, &stStream);
                continue;
            } else {
                waiting_for_idr = 0;
                LOG_INFO("[VENC] 捕获到IDR帧，开始正式推流");
            }
        }

        // --- 正常推流逻辑 ---
        int64_t pts_90k = (int64_t)(stStream.pstPack[0].u64PTS * 9 / 100);
        int64_t expected_inc = 90000 / 15; 
        if (last_pts_90k > 0 && pts_90k <= last_pts_90k) {
            pts_90k = last_pts_90k + expected_inc;
        }
        last_pts_90k = pts_90k;

        // 👇【终极真理】同上，判断是否关键帧
        int is_key = (stStream.pstPack[0].DataType.enH264EType == H264E_NALU_ISLICE || 
                      stStream.pstPack[0].DataType.enH264EType == H264E_NALU_IDRSLICE);

        void *pData = RK_MPI_MB_Handle2VirAddr(stStream.pstPack[0].pMbBlk);
        
        uint32_t data_len = stStream.pstPack[0].u32Len;
        frame_queue_push(&g_frame_queue, pData, data_len, pts_90k, is_key);
        
        // 正常的日志：每 10 秒打印一次（150帧）
        static int log_count = 0;
        if ((log_count++ % 150) == 0) {
            LOG_INFO("[VENC] 推流中... len=%d, pts=%ld, is_key=%d", data_len, pts_90k, is_key);
        }

        RK_MPI_VENC_ReleaseStream(VENC_CHANNEL, &stStream);
    }
    free(stStream.pstPack);
    return NULL;
}


// ====================== 原始帧发送线程 (生产者) (保持不变) ======================
void *venc_send_frame_thread(void *arg) {
    VENC_CTX_S *ctx = (VENC_CTX_S *)arg;
    VIDEO_FRAME_INFO_S stFrame;     // 瑞芯微视频帧结构体（发给编码器的数据包)
    RK_S32 s32Ret;                  // 函数返回值
    void *pViraddr;                 // 摄像头数据的虚拟地址
    RK_U32 u32Len;                  // 摄像头一帧数据的长度
    MB_BLK mbBlk = NULL;            // DMA内存块句柄
    RK_U32 frame_size = ctx->width * ctx->height * 3 / 2;       // NV12格式一帧大小：宽×高×1.5

    while (atomic_load(&g_state.running)) {
          // 如果拿不到数据/数据长度为0，休眠1毫秒，继续重试
        pViraddr = v4l2_grab_frame(&u32Len);
        if (!pViraddr || u32Len == 0) {
            usleep(1000);
            continue;
        }

        //// 异常处理：数据长度超过预期，直接丢弃这一帧
        if (u32Len > frame_size) {
             struct v4l2_buffer buf;
             struct v4l2_plane planes;
             memset(&buf, 0, sizeof(buf));
             memset(&planes, 0, sizeof(planes));
             buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
             buf.memory = V4L2_MEMORY_MMAP;
             buf.index = g_v4l2_idx;
             buf.m.planes = &planes;
             buf.length = 1;
             ioctl(g_v4l2_fd, VIDIOC_QBUF, &buf);
             continue;
        }

        // 从编码器的内存池里，拿一块空的DMA内存
        mbBlk = RK_MPI_MB_GetMB(ctx->pool, frame_size, RK_TRUE);
        if (mbBlk == NULL) {
            RK_LOGE("RK_MPI_MB_GetMB failed");
            continue;
        }

        // 把DMA内存句柄转为程序可读写的虚拟地址
        void *mb_ptr = RK_MPI_MB_Handle2VirAddr(mbBlk);
        if (mb_ptr) {
            memcpy(mb_ptr, pViraddr, u32Len);   //// 把摄像头的画面数据 → 拷贝到 DMA内存中
        }

        //组装视频帧
        memset(&stFrame, 0, sizeof(VIDEO_FRAME_INFO_S));
        stFrame.stVFrame.pMbBlk = mbBlk;
        stFrame.stVFrame.u32Width = ctx->width;
        stFrame.stVFrame.u32Height = ctx->height;
        stFrame.stVFrame.u32VirWidth = ctx->width;
        stFrame.stVFrame.u32VirHeight = ctx->height;
        stFrame.stVFrame.enPixelFormat = RK_FMT_YUV420SP;
        
        //// 获取系统时间戳（PTS）
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        stFrame.stVFrame.u64PTS = (RK_U64)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;

        //把帧发给硬件编码器
        s32Ret = RK_MPI_VENC_SendFrame(VENC_CHANNEL, &stFrame, 100);
        if (s32Ret != RK_SUCCESS) {
            RK_LOGE("RK_MPI_VENC_SendFrame failed %x", s32Ret);
        }

        RK_MPI_MB_ReleaseMB(mbBlk);     //// 释放DMA内存块（放回内存池，循环使用）
        mbBlk = NULL;

        // 【V4L2核心操作】把用完的缓冲区重新放回摄像头队列
        // 告诉摄像头：这个空篮子我用完了，你可以装下一帧画面了！
        struct v4l2_buffer buf;
        struct v4l2_plane planes;
        memset(&buf, 0, sizeof(buf));
        memset(&planes, 0, sizeof(planes));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = g_v4l2_idx;
        buf.m.planes = &planes;
        buf.length = 1;
        ioctl(g_v4l2_fd, VIDIOC_QBUF, &buf);
    }
    return NULL;
}

// ====================== 硬件编码初始化 (原封不动保留) ======================
static int venc_init(VENC_CTX_S *ctx)
{
    VENC_CHN_ATTR_S attr;               //// 编码器通道属性
    VENC_RECV_PIC_PARAM_S recv_param;   // 编码器接收图像参数
    MB_POOL_CONFIG_S pool_cfg;          // 瑞芯微媒体内存池配置

    //创建硬件编码专用内存池
    memset(&pool_cfg, 0, sizeof(pool_cfg));
    pool_cfg.u64MBSize = ctx->width * ctx->height * 3 / 2;      //单块内存大小
    pool_cfg.u32MBCnt = 4;                                      //一共创建4块内存
    pool_cfg.enAllocType = MB_ALLOC_TYPE_DMA;                   //内存类型：DMA内存
    pool_cfg.bPreAlloc = RK_TRUE;                               //预分配内存
    ctx->pool = RK_MPI_MB_CreatePool(&pool_cfg);                //调用瑞芯微接口，创建内存池，保存到上下文ctx中

    // ========== 码率控制配置（决定视频画质、体积、流畅度） ==========
    memset(&attr, 0, sizeof(attr)); 
    
    // 👇【保留这行】解决 profile 0 警告，同时解决由它引发的 bitstream overflow
    attr.stVencAttr.u32Profile = 100;               
    
    attr.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;               // 编码模式：H.264 恒定码率
    attr.stRcAttr.stH264Cbr.u32SrcFrameRateNum = 15;            //// 源帧率：摄像头输入 15帧/秒
    attr.stRcAttr.stH264Cbr.u32SrcFrameRateDen = 1;
    attr.stRcAttr.stH264Cbr.fr32DstFrameRateNum = 15;           //// 目标帧率：编码输出 15帧/秒
    attr.stRcAttr.stH264Cbr.fr32DstFrameRateDen = 1;
    attr.stRcAttr.stH264Cbr.u32BitRate = VENC_BITRATE_KBPS;     // 编码码率
    attr.stRcAttr.stH264Cbr.u32Gop = VENC_GOP_SIZE;             // GOP大小：关键帧间隔

    // ========== 编码基础配置 ==========
    attr.stVencAttr.enType = RK_VIDEO_ID_AVC;               //编码类型：AVC
    attr.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;        //// 像素格式：YUV420SP = NV12
    attr.stVencAttr.u32PicWidth = ctx->width;
    attr.stVencAttr.u32PicHeight = ctx->height;
    attr.stVencAttr.u32VirWidth = ctx->width;
    attr.stVencAttr.u32VirHeight = ctx->height;

    //创建硬件编码器通道
    RK_MPI_VENC_CreateChn(VENC_CHANNEL, &attr);

    memset(&recv_param, 0, sizeof(recv_param));
    recv_param.s32RecvPicNum = -1;          //// -1 = 无限接收图像（持续编码，不停止）
    RK_MPI_VENC_StartRecvFrame(VENC_CHANNEL, &recv_param);      // 启动编码器接收帧

    RK_LOGI("硬件H.264编码器初始化完成");
    return 0;
}


// ====================== 对外接口实现 【拼装起来】 ======================
int venc_local_init(void) {
    memset(&g_venc_ctx, 0, sizeof(g_venc_ctx));
    g_venc_ctx.width = VIDEO_WIDTH;
    g_venc_ctx.height = VIDEO_HEIGHT;

    RK_MPI_SYS_Init();
    if (v4l2_init(VIDEO_WIDTH, VIDEO_HEIGHT) < 0) return -1;
    if (venc_init(&g_venc_ctx) < 0) return -1;
    
    return 0;
}

int venc_local_start(void) {
    pthread_create(&g_send_th, NULL, venc_send_frame_thread, &g_venc_ctx);
    pthread_create(&g_get_th, NULL, venc_get_stream_thread, NULL);
    return 0;
}

void venc_local_deinit(void) {
    // 等待线程退出 (g_state.running 变为 0 时线程会自动退出)
    pthread_join(g_send_th, NULL);
    pthread_join(g_get_th, NULL);

    // 清理 V4L2 资源 (从原 main 函数搬过来)
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    ioctl(g_v4l2_fd, VIDIOC_STREAMOFF, &type);
    if (g_v4l2_buf) munmap(g_v4l2_buf, VIDEO_WIDTH * VIDEO_HEIGHT * 3 / 2);
    close(g_v4l2_fd);

    // 清理 MPP 资源 (从原 main 函数搬过来)
    RK_MPI_VENC_StopRecvFrame(VENC_CHANNEL);
    RK_MPI_VENC_DestroyChn(VENC_CHANNEL);
    RK_MPI_MB_DestroyPool(g_venc_ctx.pool);
    RK_MPI_SYS_Exit();
}

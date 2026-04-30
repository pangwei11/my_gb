#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

#include "venc_local.h"
#include "frame_queue.h"
#include "globals.h"

// 瑞芯微 MPP 接口
#include "rk_debug.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_venc.h"
#include "rk_mpi_vi.h"      // 【新增】VI 模块头文件
#include "rk_comm_vi.h"     // 【新增】VI 属性结构体

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

// ====================== 核心配置 ======================
#define VIDEO_WIDTH 1920
#define VIDEO_HEIGHT 1080
#define VENC_BITRATE_KBPS 2048
#define VENC_GOP_SIZE 15
#define VENC_CHANNEL 0

// 【关键配置】VI 参数，需根据你的实际设备节点修改！
// 常见的选择："rkispp_scale0" (摄像头), "rkvpss_scale0/1" (HDMI或虚拟节点)
#define VI_DEV_ID       1
#define VI_PIPE_ID      VI_DEV_ID
#define VI_CHN_ID       2
#define VI_ENTITY_NAME  "rkvpss_scale1"  // 对应你之前用的 /dev/video46

// ====================== 全局变量 ======================
static pthread_t g_get_th;
static volatile int g_wait_idr_flag = 0;

void venc_trigger_wait_idr(void) {
    g_wait_idr_flag = 1;
}

// ====================== VI 初始化模块 (从 vi_test.c 移植) ======================
static int vi_init(void) {
    VI_DEV_ATTR_S stDevAttr;
    VI_DEV_BIND_PIPE_S stBindPipe;
    VI_CHN_ATTR_S stChnAttr;
    RK_S32 s32Ret;

    // 1. 获取并设置 Dev 属性
    memset(&stDevAttr, 0, sizeof(stDevAttr));
    s32Ret = RK_MPI_VI_GetDevAttr(VI_DEV_ID, &stDevAttr);
    if (s32Ret == RK_ERR_VI_NOT_CONFIG) {
        s32Ret = RK_MPI_VI_SetDevAttr(VI_DEV_ID, &stDevAttr);
        if (s32Ret != RK_SUCCESS) {
            LOG_ERROR("RK_MPI_VI_SetDevAttr fail %d", s32Ret);
            return -1;
        }
    }

    // 2. 使能 Dev 并绑定 Pipe
    s32Ret = RK_MPI_VI_GetDevIsEnable(VI_DEV_ID);
    if (s32Ret != RK_SUCCESS) {
        s32Ret = RK_MPI_VI_EnableDev(VI_DEV_ID);
        if (s32Ret != RK_SUCCESS) {
            LOG_ERROR("RK_MPI_VI_EnableDev fail %d", s32Ret);
            return -1;
        }

        stBindPipe.u32Num = 1;
        stBindPipe.PipeId[0] = VI_PIPE_ID;
        stBindPipe.bDataOffline = RK_FALSE;
        stBindPipe.bUserStartPipe[0] = RK_FALSE;
        s32Ret = RK_MPI_VI_SetDevBindPipe(VI_DEV_ID, &stBindPipe);
        if (s32Ret != RK_SUCCESS) {
            LOG_ERROR("RK_MPI_VI_SetDevBindPipe fail %d", s32Ret);
            return -1;
        }
    }

    // 3. 设置 Chn 属性并使能
    memset(&stChnAttr, 0, sizeof(stChnAttr));
    stChnAttr.stSize.u32Width = VIDEO_WIDTH;
    stChnAttr.stSize.u32Height = VIDEO_HEIGHT;
    stChnAttr.enCompressMode = COMPRESS_MODE_NONE; // 不压缩直出
    stChnAttr.stIspOpt.bNoUseLibV4L2 = RK_TRUE;   // 不走 libv4l2
    stChnAttr.stIspOpt.u32BufCount = 3;
    stChnAttr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
    stChnAttr.stIspOpt.enCaptureType = VI_V4L2_CAPTURE_TYPE_VIDEO_CAPTURE;
    stChnAttr.u32Depth = 0; // 绑定模式下必须设为 0
    stChnAttr.enPixelFormat = RK_FMT_YUV420SP;
    stChnAttr.stFrameRate.s32SrcFrameRate = -1;
    stChnAttr.stFrameRate.s32DstFrameRate = -1;
    
    // 绑定实际的硬件实体
    memcpy(stChnAttr.stIspOpt.aEntityName, VI_ENTITY_NAME, strlen(VI_ENTITY_NAME));

    s32Ret = RK_MPI_VI_SetChnAttr(VI_PIPE_ID, VI_CHN_ID, &stChnAttr);
    if (s32Ret != RK_SUCCESS) {
        LOG_ERROR("RK_MPI_VI_SetChnAttr fail %d", s32Ret);
        return -1;
    }

    s32Ret = RK_MPI_VI_EnableChn(VI_PIPE_ID, VI_CHN_ID);
    if (s32Ret != RK_SUCCESS) {
        LOG_ERROR("RK_MPI_VI_EnableChn fail %d", s32Ret);
        return -1;
    }

    LOG_INFO("VI 初始化完成");
    return 0;
}

// ====================== 码流获取线程 (消费者) ======================
void *venc_get_stream_thread(void *arg) {
    VENC_STREAM_S stStream;
    memset(&stStream, 0, sizeof(stStream));
    
    // 瑞芯微一帧可能有多个 Pack（例如 Pack0=SPS/PPS, Pack1=IDR 数据）
    // 为了安全，分配一个能容纳多个 Pack 指针的空间
    RK_U32 max_pack = 8; 
    stStream.pstPack = (VENC_PACK_S *)malloc(sizeof(VENC_PACK_S) * max_pack);
    if (!stStream.pstPack) return NULL;

    // 用于拼接一帧内的多个 Pack
    uint8_t *frame_buf = (uint8_t *)malloc(VIDEO_WIDTH * VIDEO_HEIGHT * 3 / 2);
    if (!frame_buf) {
        free(stStream.pstPack);
        return NULL;
    }

    int64_t last_pts_90k = 0;
    int waiting_for_idr = 0;

    while (atomic_load(&g_state.running)) {
        stStream.u32PackCount = max_pack;
        // 阻塞获取码流，超时时间 100ms
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

        int is_key = (stStream.pstPack[0].DataType.enH264EType == H264E_NALU_ISLICE || 
                      stStream.pstPack[0].DataType.enH264EType == H264E_NALU_IDRSLICE);

        // 【重点优化】将一帧的所有 Pack 拼接成一个完整的 NAL 单元再推入队列
        uint32_t total_len = 0;
        for (RK_U32 i = 0; i < stStream.u32PackCount; i++) {
            if (total_len + stStream.pstPack[i].u32Len < (VIDEO_WIDTH * VIDEO_HEIGHT * 3 / 2)) {
                void *pData = RK_MPI_MB_Handle2VirAddr(stStream.pstPack[i].pMbBlk);
                memcpy(frame_buf + total_len, pData, stStream.pstPack[i].u32Len);
                total_len += stStream.pstPack[i].u32Len;
            }
        }

        // 一次性推入你的 GB28181 帧队列
        frame_queue_push(&g_frame_queue, frame_buf, total_len, pts_90k, is_key);

        // 正常的日志：每 10 秒打印一次
        static int log_count = 0;
        if ((log_count++ % 150) == 0) {
            LOG_INFO("[VENC] 推流中... len=%d, pts=%ld, is_key=%d, packs=%d", total_len, pts_90k, is_key, stStream.u32PackCount);
        }

        RK_MPI_VENC_ReleaseStream(VENC_CHANNEL, &stStream);
    }

    free(frame_buf);
    free(stStream.pstPack);
    return NULL;
}

// ====================== 硬件编码初始化 ======================
static int venc_init(void) {
    VENC_CHN_ATTR_S attr;
    VENC_RECV_PIC_PARAM_S recv_param;

    memset(&attr, 0, sizeof(attr));
    
    // 【必须】解决 profile 0 警告及绑定失败问题 (从 vi_test.c 吸取的教训)
    attr.stVencAttr.u32Profile = 100; 
    
    attr.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
    attr.stRcAttr.stH264Cbr.u32SrcFrameRateNum = 15;
    attr.stRcAttr.stH264Cbr.u32SrcFrameRateDen = 1;
    attr.stRcAttr.stH264Cbr.fr32DstFrameRateNum = 15;
    attr.stRcAttr.stH264Cbr.fr32DstFrameRateDen = 1;
    attr.stRcAttr.stH264Cbr.u32BitRate = VENC_BITRATE_KBPS;
    attr.stRcAttr.stH264Cbr.u32Gop = VENC_GOP_SIZE;

    attr.stVencAttr.enType = RK_VIDEO_ID_AVC;
    attr.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
    attr.stVencAttr.u32PicWidth = VIDEO_WIDTH;
    attr.stVencAttr.u32PicHeight = VIDEO_HEIGHT;
    attr.stVencAttr.u32VirWidth = VIDEO_WIDTH;
    attr.stVencAttr.u32VirHeight = VIDEO_HEIGHT;
    
    // 【重要】绑定模式下配置内部缓冲区
    attr.stVencAttr.u32StreamBufCnt = 5; 
    attr.stVencAttr.u32BufSize = VIDEO_WIDTH * VIDEO_HEIGHT * 3 / 2;

    RK_MPI_VENC_CreateChn(VENC_CHANNEL, &attr);

    memset(&recv_param, 0, sizeof(recv_param));
    recv_param.s32RecvPicNum = -1; // 持续接收
    RK_MPI_VENC_StartRecvFrame(VENC_CHANNEL, &recv_param);

    LOG_INFO("硬件H.264编码器初始化完成");
    return 0;
}

// ====================== 对外接口实现 ======================
int venc_local_init(void) {
    // 1. 系统 MPP 初始化
    RK_MPI_SYS_Init();

    // 2. 初始化 VI (替代原来的 v4l2_init)
    if (vi_init() < 0) {
        LOG_ERROR("VI 初始化失败");
        return -1;
    }

    // 3. 初始化 VENC
    if (venc_init() < 0) {
        LOG_ERROR("VENC 初始化失败");
        return -1;
    }

    // 4. 【核心】执行 VI 到 VENC 的零拷贝绑定
    MPP_CHN_S stSrcChn, stDestChn;
    stSrcChn.enModId = RK_ID_VI;
    stSrcChn.s32DevId = VI_DEV_ID;
    stSrcChn.s32ChnId = VI_CHN_ID;

    stDestChn.enModId = RK_ID_VENC;
    stDestChn.s32DevId = 0;
    stDestChn.s32ChnId = VENC_CHANNEL;

    RK_S32 s32Ret = RK_MPI_SYS_Bind(&stSrcChn, &stDestChn);
    if (s32Ret != RK_SUCCESS) {
        LOG_ERROR("VI 绑定 VENC 失败: %d", s32Ret);
        return -1;
    }
    LOG_INFO("VI -> VENC 零拷贝绑定成功");

    return 0;
}

int venc_local_start(void) {
    // 注意：这里不再需要启动 venc_send_frame_thread (生产者) 了！
    // 硬件绑定后，数据会自动流转，我们只需要启动消费者线程拿结果。
    if (pthread_create(&g_get_th, NULL, venc_get_stream_thread, NULL) != 0) {
        LOG_ERROR("创建 VENC 获取线程失败");
        return -1;
    }
    return 0;
}

void venc_local_deinit(void) {
    // 1. 停止获取线程
    pthread_join(g_get_th, NULL);

    // 2. 解绑 (非常重要！必须先解绑再销毁通道)
    MPP_CHN_S stSrcChn, stDestChn;
    stSrcChn.enModId = RK_ID_VI;
    stSrcChn.s32DevId = VI_DEV_ID;
    stSrcChn.s32ChnId = VI_CHN_ID;
    stDestChn.enModId = RK_ID_VENC;
    stDestChn.s32DevId = 0;
    stDestChn.s32ChnId = VENC_CHANNEL;
    RK_MPI_SYS_UnBind(&stSrcChn, &stDestChn);

    // 3. 销毁 VENC
    RK_MPI_VENC_StopRecvFrame(VENC_CHANNEL);
    RK_MPI_VENC_DestroyChn(VENC_CHANNEL);

    // 4. 关闭 VI
    RK_MPI_VI_DisableChn(VI_PIPE_ID, VI_CHN_ID);
    RK_MPI_VI_DisableDev(VI_DEV_ID);

    // 5. 退出 MPP 系统
    RK_MPI_SYS_Exit();
    
    LOG_INFO("本地 VENC 资源清理完成");
}

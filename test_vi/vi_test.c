#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include "rk_defines.h"
#include "rk_debug.h"
#include "rk_mpi_vi.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_venc.h"
#include "rk_comm_vi.h"

#define TEST_VENC_MAX 2

typedef struct _rkTestVencCfg {
    RK_BOOL bOutDebugCfg;
    VENC_CHN_ATTR_S stAttr;
    RK_CHAR dstFilePath[128];
    RK_CHAR dstFileName[128];
    RK_S32 s32ChnId;
    FILE *fp;
    RK_S32 selectFd;
} TEST_VENC_CFG;

typedef enum rkTestVIMODE_E {
    TEST_VI_MODE_VI_FRAME_ONLY = 0,
    TEST_VI_MODE_BIND_VENC = 1,
    TEST_VI_MODE_BIND_VENC_MULTI = 2,
} TEST_VI_MODE_E;

typedef struct _rkMpiVICtx {
    RK_S32 width;
    RK_S32 height;
    RK_S32 devId;
    RK_S32 pipeId;
    RK_S32 channelId;
    RK_S32 loopCountSet;
    RK_BOOL bFreeze;
    RK_BOOL bNoUseLibv4l2;
    COMPRESS_MODE_E enCompressMode;
    VI_DEV_ATTR_S stDevAttr;
    VI_DEV_BIND_PIPE_S stBindPipe;
    VI_CHN_ATTR_S stChnAttr;
    VI_SAVE_FILE_INFO_S stDebugFile;
    TEST_VI_MODE_E enMode;
    const char *aEntityName;
    TEST_VENC_CFG stVencCfg[TEST_VENC_MAX];
    VENC_STREAM_S stFrame[TEST_VENC_MAX];
} TEST_VI_CTX_S;

// ================= Helper Functions =================

static RK_S32 create_venc(TEST_VI_CTX_S *ctx, RK_U32 u32Ch) {
    VENC_RECV_PIC_PARAM_S stRecvParam;
    stRecvParam.s32RecvPicNum = ctx->loopCountSet;
    RK_MPI_VENC_CreateChn(u32Ch, &ctx->stVencCfg[u32Ch].stAttr);
    RK_MPI_VENC_StartRecvFrame(u32Ch, &stRecvParam);
    return RK_SUCCESS;
}

void init_venc_cfg(TEST_VI_CTX_S *ctx, RK_U32 u32Ch, RK_CODEC_ID_E enType) {
    ctx->stVencCfg[u32Ch].stAttr.stVencAttr.enType = enType;
    ctx->stVencCfg[u32Ch].s32ChnId = u32Ch;
    ctx->stVencCfg[u32Ch].stAttr.stVencAttr.enPixelFormat = ctx->stChnAttr.enPixelFormat;
    ctx->stVencCfg[u32Ch].stAttr.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
    ctx->stVencCfg[u32Ch].stAttr.stRcAttr.stH264Cbr.u32Gop = 60;
    ctx->stVencCfg[u32Ch].stAttr.stRcAttr.stH264Cbr.u32BitRate = 4 * 1024 * 1024;
    ctx->stVencCfg[u32Ch].stAttr.stVencAttr.u32PicWidth = ctx->width;
    ctx->stVencCfg[u32Ch].stAttr.stVencAttr.u32PicHeight = ctx->height;
    ctx->stVencCfg[u32Ch].stAttr.stVencAttr.u32VirWidth = ctx->width;
    ctx->stVencCfg[u32Ch].stAttr.stVencAttr.u32VirHeight = ctx->height;
    
    // 【必须增加这一行】解决 profile 0 导致的绑定失败
    ctx->stVencCfg[u32Ch].stAttr.stVencAttr.u32Profile = 100; 
    
    ctx->stVencCfg[u32Ch].stAttr.stVencAttr.u32StreamBufCnt = 5;
    ctx->stVencCfg[u32Ch].stAttr.stVencAttr.u32BufSize = ctx->width * ctx->height * 3 / 2;
}






static RK_S32 test_vi_init(TEST_VI_CTX_S *ctx) {
    RK_S32 s32Ret = RK_FAILURE;

    // 0. Get and Set Dev Attr
    s32Ret = RK_MPI_VI_GetDevAttr(ctx->devId, &ctx->stDevAttr);
    if (s32Ret == RK_ERR_VI_NOT_CONFIG) {
        s32Ret = RK_MPI_VI_SetDevAttr(ctx->devId, &ctx->stDevAttr);
        if (s32Ret != RK_SUCCESS) {
            RK_LOGE("RK_MPI_VI_SetDevAttr fail %x", s32Ret);
            return s32Ret;
        }
    }

    // 1. Enable Dev and Bind Pipe
    s32Ret = RK_MPI_VI_GetDevIsEnable(ctx->devId);
    if (s32Ret != RK_SUCCESS) {
        //1-2.enable dev
        s32Ret = RK_MPI_VI_EnableDev(ctx->devId);
        if (s32Ret != RK_SUCCESS) {
            RK_LOGE("RK_MPI_VI_EnableDev fail %x", s32Ret);
            return s32Ret;
        }
        //1-3.bind dev/pipe
        ctx->stBindPipe.u32Num = 1;             //该 VI Dev 所绑定的 PIPE 数目，取值范围[1，VI_MAX_PIPE_NUM]。
        ctx->stBindPipe.PipeId[0] = ctx->pipeId;        //该 VI Dev 绑定的 PIPE 号
        ctx->stBindPipe.bDataOffline = RK_FALSE; //dev work online
        ctx->stBindPipe.bUserStartPipe[0] = RK_FALSE ;          //定义在绑定时是否启动pipe，RK_FALSE: vi启动pipe，RK_TRUE：用户启动pipe，默认RK_FALSE
        s32Ret = RK_MPI_VI_SetDevBindPipe(ctx->devId, &ctx->stBindPipe);
        if (s32Ret != RK_SUCCESS) {
            RK_LOGE("RK_MPI_VI_SetDevBindPipe fail %x", s32Ret);
            return s32Ret;
        }
    }

    // 2. Set Chn Attr and Enable Chn
    ctx->stChnAttr.stSize.u32Width = ctx->width;
    ctx->stChnAttr.stSize.u32Height = ctx->height;
    ctx->stChnAttr.enCompressMode = ctx->enCompressMode;        //目标图像压缩格式
    ctx->stChnAttr.stIspOpt.bNoUseLibV4L2 = ctx->bNoUseLibv4l2;     //获取图像为isp处理/直通数据的参数设置
    if (ctx->aEntityName != RK_NULL) {
        memcpy(ctx->stChnAttr.stIspOpt.aEntityName, ctx->aEntityName, strlen(ctx->aEntityName));
    }
    s32Ret = RK_MPI_VI_SetChnAttr(ctx->pipeId, ctx->channelId, &ctx->stChnAttr);
    if (s32Ret != RK_SUCCESS) {
        RK_LOGE("RK_MPI_VI_SetChnAttr fail %x", s32Ret);
        return s32Ret;
    }

    //3.enable channel
    s32Ret = RK_MPI_VI_EnableChn(ctx->pipeId, ctx->channelId);
    if (s32Ret != RK_SUCCESS) {
        RK_LOGE("RK_MPI_VI_EnableChn fail %x", s32Ret);
        return s32Ret;
    }

    //因为后续把VI绑定到venc了，所以这个接口获取不到数据
    // //4.save debug file
    // if (ctx->stDebugFile.bCfg) {
    //     s32Ret = RK_MPI_VI_ChnSaveFile(ctx->pipeId, ctx->channelId, &ctx->stDebugFile);
    //     RK_LOGE("RK_MPI_VI_ChnSaveFile %x", s32Ret);
    //     if (s32Ret != RK_SUCCESS) {
    //         RK_LOGE("Save VI debug file failed, check /data path permission!");
    //     }
    // }

    return RK_SUCCESS;
}

// ================= Core Test Loop =================

static RK_S32 test_vi_bind_venc_loop(TEST_VI_CTX_S *ctx) {
    MPP_CHN_S stSrcChn, stDestChn;
    RK_S32 loopCount = 0;
    void *pData = RK_NULL;
    RK_S32 s32Ret = RK_FAILURE;
    RK_U32 i;
    RK_U32 u32DstCount = 1; 
    RK_BOOL isBinded = RK_FALSE; // 【修复】增加绑定标志位，防止异常解绑导致 Abort

    s32Ret = test_vi_init(ctx);
    if (s32Ret != RK_SUCCESS) {
        RK_LOGE("vi init failed:%x", s32Ret);
        goto __FAILED;
    }

    for (i = 0; i < u32DstCount; i++) {
        init_venc_cfg(ctx, i, RK_VIDEO_ID_AVC);
        s32Ret = create_venc(ctx, ctx->stVencCfg[i].s32ChnId);
        if (s32Ret != RK_SUCCESS) {
            RK_LOGE("create %d ch venc failed", ctx->stVencCfg[i].s32ChnId);
            goto __FAILED;
        }

        stSrcChn.enModId    = RK_ID_VI;
        stSrcChn.s32DevId   =  ctx->devId;
        stSrcChn.s32ChnId   = ctx->channelId;

        stDestChn.enModId   = RK_ID_VENC;
        stDestChn.s32DevId  = i;
        stDestChn.s32ChnId  = ctx->stVencCfg[i].s32ChnId;

        s32Ret = RK_MPI_SYS_Bind(&stSrcChn, &stDestChn);
        if (s32Ret != RK_SUCCESS) {
            RK_LOGE("VI bind VENC fail:%x", s32Ret);
            goto __FAILED;
        }
        isBinded = RK_TRUE; // 【修复】标记绑定成功
         ctx->stFrame[i].pstPack = (VENC_PACK_S *)malloc(sizeof(VENC_PACK_S));
    }

    while (loopCount < ctx->loopCountSet) {
        for (i = 0; i < u32DstCount; i++) {
            RK_MPI_VI_SetChnFreeze(ctx->pipeId, ctx->channelId, ctx->bFreeze);

            // 获取编码的码流。
            s32Ret = RK_MPI_VENC_GetStream(ctx->stVencCfg[i].s32ChnId, &ctx->stFrame[i], -1); //超时参数设为-1 时，为阻塞接口；0时为非阻塞接口；大于 0 时为超时等待时间
            if (s32Ret == RK_SUCCESS) {
                if (ctx->stVencCfg[i].bOutDebugCfg && ctx->stVencCfg[i].fp) {
                    // 注意：一般一帧可能包含多个 pack，标准写法应该遍历 u32PackCount
                    pData = RK_MPI_MB_Handle2VirAddr(ctx->stFrame[i].pstPack->pMbBlk);
                    fwrite(pData, 1, ctx->stFrame[i].pstPack->u32Len, ctx->stVencCfg[i].fp);
                    fflush(ctx->stVencCfg[i].fp);
                }

                RK_LOGD("chn:%d, loopCount:%d enc->seq:%d len:%d\n", i, loopCount,
                         ctx->stFrame[i].u32Seq, ctx->stFrame[i].pstPack->u32Len);

                s32Ret = RK_MPI_VENC_ReleaseStream(ctx->stVencCfg[i].s32ChnId, &ctx->stFrame[i]);
                if (s32Ret != RK_SUCCESS) {
                    RK_LOGE("RK_MPI_VENC_ReleaseStream fail %x", s32Ret);
                }
                loopCount++;
            } else {
                RK_LOGE("RK_MPI_VENC_GetStream fail %x", s32Ret);
            }
        }
        usleep(10 * 1000);
    }

__FAILED:
    // 【修复】只有在真正绑定过的情况下才去解绑
    if (isBinded) {
        s32Ret = RK_MPI_SYS_UnBind(&stSrcChn, &stDestChn);
        if (s32Ret != RK_SUCCESS) {
            RK_LOGE("RK_MPI_SYS_UnBind fail %x", s32Ret);
        }
    }

    s32Ret = RK_MPI_VI_DisableChn(ctx->pipeId, ctx->channelId);
    if(s32Ret != RK_SUCCESS) RK_LOGE("RK_MPI_VI_DisableChn %x", s32Ret);

    for (i = 0; i < u32DstCount; i++) {
        RK_MPI_VENC_StopRecvFrame(ctx->stVencCfg[i].s32ChnId);
        RK_MPI_VENC_DestroyChn(ctx->stVencCfg[i].s32ChnId);
    }

    s32Ret = RK_MPI_VI_DisableDev(ctx->devId);
    if(s32Ret != RK_SUCCESS) RK_LOGE("RK_MPI_VI_DisableDev %x", s32Ret);

    for (i = 0; i < u32DstCount; i++) {
        // 【必须恢复这一行】释放 malloc 的内存
        if (ctx->stFrame[i].pstPack) {
            free(ctx->stFrame[i].pstPack);
            ctx->stFrame[i].pstPack = RK_NULL;
        }
        if (ctx->stVencCfg[i].fp) {
            fclose(ctx->stVencCfg[i].fp);
            ctx->stVencCfg[i].fp = RK_NULL;
        }
    }

    return s32Ret;
}

// ================= Main Entry =================

int main(int argc, char *argv[]) {
    RK_S32 s32Ret;
    TEST_VI_CTX_S ctx;
    
    // 1. Default Context Config
    memset(&ctx, 0, sizeof(ctx));
    ctx.width = 1920;
    ctx.height = 1080;
    ctx.devId = 1;
    ctx.pipeId = ctx.devId;
    ctx.channelId = 2;
    ctx.loopCountSet = 100;
    // ctx.enMode = TEST_VI_MODE_BIND_VENC;
    // ctx.bFreeze = RK_FALSE;
    // ctx.bNoUseLibv4l2 = RK_TRUE;
    // ctx.enCompressMode = COMPRESS_MODE_NONE;
    
    ctx.stChnAttr.stIspOpt.u32BufCount = 3;
    ctx.stChnAttr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF; // For sensor usually DMA, HDMI usually MMAP
    ctx.stChnAttr.stIspOpt.enCaptureType = VI_V4L2_CAPTURE_TYPE_VIDEO_CAPTURE;
    ctx.stChnAttr.u32Depth = 0; // 0 for bind mode
    ctx.stChnAttr.enPixelFormat = RK_FMT_YUV420SP;
    ctx.stChnAttr.stFrameRate.s32SrcFrameRate = -1;
    ctx.stChnAttr.stFrameRate.s32DstFrameRate = -1;
    
    // IMPORTANT: Modify this to match your hardware sensor/hdmi node!
    // e.g., "rkispp_scale0" or "/dev/video0"
    ctx.aEntityName = "rkvpss_scale1"; 


    //  // ==========================================
    // // 【新增】：配置 VI 通道 YUV 调试文件保存
    // // ==========================================
    // ctx.stDebugFile.bCfg = RK_TRUE; // 必须设为 TRUE，开启保存功能
    // // 设置保存路径，确保开发板上有 /data 目录且有写权限
    // memcpy(ctx.stDebugFile.aFilePath, "/data", strlen("/data")); 
    // // 设置保存的文件名，建议带 dev/pipe/chn 标识以便区分
    // // 注意：MAX_VI_FILE_PATH_LEN 宏定义在 rk_comm_vi.h 中
    // snprintf(ctx.stDebugFile.aFileName, MAX_VI_FILE_PATH_LEN, 
    //          "vi_debug_%d_%d_%d.yuv", ctx.devId, ctx.pipeId, ctx.channelId);
    // // ==========================================


    // Debug output setup (Save H264 stream to file)
    ctx.stVencCfg[0].bOutDebugCfg = RK_TRUE;
    snprintf(ctx.stVencCfg[0].dstFilePath, sizeof(ctx.stVencCfg[0].dstFilePath), "/tmp");
    snprintf(ctx.stVencCfg[0].dstFileName, sizeof(ctx.stVencCfg[0].dstFileName), "venc_0.h264");

    if (ctx.stVencCfg[0].bOutDebugCfg) {
        char name[256] = {0};
        snprintf(name, sizeof(name), "%s/%s", ctx.stVencCfg[0].dstFilePath, ctx.stVencCfg[0].dstFileName);
        ctx.stVencCfg[0].fp = fopen(name, "wb");
        if (ctx.stVencCfg[0].fp == RK_NULL) {
            RK_LOGE("Can't open file %s!\n", name);
            return -1;
        }
    }

    // 2. System Init
    if (RK_MPI_SYS_Init() != RK_SUCCESS) {
        RK_LOGE("rk mpi sys init fail!");
        if (ctx.stVencCfg[0].fp) fclose(ctx.stVencCfg[0].fp);
        return -1;
    }

    // 3. Run Test
    RK_LOGE("Starting VI bind VENC test...");
    s32Ret = test_vi_bind_venc_loop(&ctx);
    RK_LOGE("Test finished, ret = %x", s32Ret);

    // 4. System Exit
    RK_MPI_SYS_Exit();
    
    return 0;
}

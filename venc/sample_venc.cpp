#include <stdio.h>
#include <iostream>
#include <fstream>
#include <string.h>  /* needed by sscanf */
#include <string>

#include "getopt.hpp"
#include "sample_comm.h"

using namespace std;

#define FPCLOSE(fp) { if (fp) { fclose(fp); fp = NULL;} }
#define FREE(p) { if (p) { free(p); p = NULL; } }

typedef enum {
    CODEC_AVC = 0,
    CODEC_HEVC = 1
} EncMode;

typedef struct {
    VENC_CHN VencChn;
    PIC_SIZE_E enSize;
    EncMode enc_mode;
    SAMPLE_RC_E rc_mode;
    HI_U32 frame_cnt;
    HI_U32 frames;
    HI_U32 width;
    HI_U32 height;
    HI_U32 gop; /* gop length */
    HI_U32 bitrate;
    HI_U32 fixed_qp;
    string in_file;
    string out_file;
    HI_U8  *src_buf;
    FILE   *fp_raw;
    FILE   *fp_strm;
    VIDEO_FRAME_INFO_S * src_frame; /* input frame info */
    VB_BLK handleY;
    VB_POOL poolID;
    HI_U32 phyYaddr; /* physical address of input frame */
    HI_U8 *pVirYaddr; /* virtual address of input frame */
    VENC_CHN_ATTR_S chn_attr;
} EncCtx;

static void show_info(EncCtx *ctx)
{
    printf("input %s\n"
           "output %s\n"
           "width %d \nheight %d \nframes %d\n"
           "gop %d\n"
           "bitrate %d kbps\n"
           "fixed_qp %d\n"
           "rc_mode %d"
           "codec %s\n",
           ctx->in_file.c_str(), ctx->out_file.c_str(),
           ctx->width, ctx->height, ctx->frames,
           ctx->gop, ctx->bitrate, ctx->fixed_qp, ctx->rc_mode,
           (ctx->enc_mode == CODEC_AVC) ? "h264" : "h265");
}

static void yuv420p_to_yvu420sp(HI_U8 *src, HI_U8 *dst, HI_U32 width, HI_U32 height)
{
    HI_U32 i, j;
    HI_U32 size = width * height;

    HI_U8 *y = src;
    HI_U8 *u = src + size;
    HI_U8 *v = src + size * 5 / 4;

    HI_U8 *y_tmp  = dst;
    HI_U8 *uv_tmp = dst + size;

    memcpy(y_tmp, y, size);

    for(j = 0, i = 0; j < size / 2; j += 2, i++) {
        uv_tmp[j]   = v[i];
        uv_tmp[j + 1] = u[i];
    }
}

HI_S32 sample_init(EncCtx *ctx)
{
    HI_S32 ret = HI_SUCCESS;
    ctx->VencChn = 0;
    ctx->frame_cnt = 0;

    ctx->fp_raw = fopen(ctx->in_file.c_str(), "rb");
    if (ctx->fp_raw == NULL) {
        SAMPLE_PRT("fopen %s failed\n", ctx->in_file.c_str());
        ret = HI_FAILURE;
        goto exit;
    }

    ctx->fp_strm = fopen(ctx->out_file.c_str(), "wb");
    if (ctx->fp_strm == NULL) {
        SAMPLE_PRT("fopen %s failed\n", ctx->out_file.c_str());
        ret = HI_FAILURE;
        goto exit;
    }

    ctx->src_buf = (HI_U8 *)malloc(ctx->width * ctx->height * 2);
    if (ctx->src_buf == NULL) {
        SAMPLE_PRT("malloc src buf failed\n");
        ret = HI_FAILURE;
        goto exit;
    }

exit:
    return ret;
}

HI_S32 sample_deinit(EncCtx *ctx)
{
    HI_S32 ret = HI_SUCCESS;

    FPCLOSE(ctx->fp_raw);
    FPCLOSE(ctx->fp_strm);
    FREE(ctx->src_buf);

    return ret;
}

static void set_pic_size(EncCtx *ctx)
{
    PIC_SIZE_E enSize = PIC_720P;

    switch(ctx->width) {
      case 1920 :
          if(ctx->height == 1080)
              enSize = PIC_1080P;
          else {
              SAMPLE_PRT("unknow fmt width:%d height:%d\n", ctx->width, ctx->height);
          }
          break;
      case 1280 :
          if(ctx->height == 720)
              enSize = PIC_720P;
          else {
              SAMPLE_PRT("unknow fmt width:%d height:%d\n", ctx->width, ctx->height);
          }
          break;
      }

      SAMPLE_PRT("fmt width:%d height:%d\n", ctx->width, ctx->height);
      ctx->enSize = enSize;
}

HI_S32 venc_init(EncCtx *ctx)
{
    HI_S32 s32Ret = HI_SUCCESS;
    SAMPLE_PRT("enter\n");

    /******************************************
    step  1: init sys variable
    ******************************************/
    VB_CONFIG_S stVbConf;
    VENC_GOP_MODE_E enGopMode;
    VENC_GOP_ATTR_S stGopAttr;
    SAMPLE_RC_E     enRcMode = SAMPLE_RC_CBR;
    PAYLOAD_TYPE_E  enPayLoad  = PT_H265;
    HI_U32          u32Profile = 0;
    HI_BOOL         bRcnRefShareBuf = HI_TRUE;
    HI_U32 width = ctx->width;
    HI_U32 height = ctx->height;

    set_pic_size(ctx);

    memset(&stVbConf, 0, sizeof(VB_CONFIG_S));
    stVbConf.u32MaxPoolCnt = 128;
    //u32BlkSize = SAMPLE_COMM_SYS_CalcPicVbBlkSize(gs_enNorm,\
    //            enSize, SAMPLE_PIXEL_FORMAT, SAMPLE_SYS_ALIGN_WIDTH);

    stVbConf.astCommPool[0].u64BlkSize = width * height * 2;
    stVbConf.astCommPool[0].u32BlkCnt = 20;
    SAMPLE_PRT("BlkSize %lld BlkCnt %d\n", stVbConf.astCommPool[0].u64BlkSize, stVbConf.astCommPool[0].u32BlkCnt);

    /******************************************
    step 2: mpp system init.
    ******************************************/
    s32Ret = SAMPLE_COMM_SYS_Init(&stVbConf);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("system init failed with %#x!\n", s32Ret);
        goto exit;
    }

    enGopMode = VENC_GOPMODE_NORMALP;
    s32Ret = SAMPLE_COMM_VENC_GetGopAttr(enGopMode,&stGopAttr);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Venc Get GopAttr for %#x!\n", s32Ret);
        goto exit;
    }

    enPayLoad = (ctx->enc_mode == CODEC_HEVC) ? PT_H265 : PT_H264;
    s32Ret = SAMPLE_COMM_VENC_Start(ctx->VencChn, enPayLoad, ctx->enSize, enRcMode,u32Profile,bRcnRefShareBuf,&stGopAttr);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Venc Start failed for %#x!\n", s32Ret);
        goto exit;
    }

    ctx->src_frame = (VIDEO_FRAME_INFO_S *)malloc(sizeof(VIDEO_FRAME_INFO_S));
    if (ctx->src_frame == NULL) {
        SAMPLE_PRT("malloc frame info failed\n");
        s32Ret = HI_FAILURE;
        goto exit;
    }

    ctx->handleY = HI_MPI_VB_GetBlock(VB_INVALID_POOLID, width * height * 3 / 2, NULL);
    if (ctx->handleY == VB_INVALID_HANDLE) {
        SAMPLE_PRT("getblock for input frame failed\n");
        s32Ret = HI_FAILURE;
        goto exit;
    }

    ctx->poolID = HI_MPI_VB_Handle2PoolId (ctx->handleY);
    ctx->phyYaddr = HI_MPI_VB_Handle2PhysAddr(ctx->handleY);
    if(ctx->phyYaddr == 0) {
        SAMPLE_PRT("HI_MPI_VB_Handle2PhysAddr for handleY failed\n");
        s32Ret = HI_FAILURE;
        goto exit;
    }

    ctx->pVirYaddr = (HI_U8*) HI_MPI_SYS_Mmap(ctx->phyYaddr, width * height * 3 / 2);
    if (ctx->pVirYaddr == 0) {
        SAMPLE_PRT("HI_MPI_SYS_Mmap for handleY failed\n");
        s32Ret = HI_FAILURE;
        goto exit;
    }

exit:
    return s32Ret;
}

HI_S32 venc_deinit(EncCtx *ctx)
{
    HI_S32 ret = HI_SUCCESS;
    SAMPLE_PRT("enter\n");

    HI_MPI_SYS_Munmap(ctx->pVirYaddr, ctx->width * ctx->height * 3 / 2);
    HI_MPI_VB_ReleaseBlock(ctx->handleY);
    ctx->handleY = VB_INVALID_HANDLE;

    SAMPLE_COMM_VENC_Stop(ctx->VencChn);
    SAMPLE_COMM_SYS_Exit();
    FREE(ctx->src_frame);

    return ret;
}

static HI_S32 set_encoder_attr(EncCtx *ctx)
{
    HI_S32 ret = HI_SUCCESS;
    PAYLOAD_TYPE_E enPayLoad = (ctx->enc_mode == CODEC_HEVC) ? PT_H265 : PT_H264;
    SAMPLE_RC_E enRcMode = ctx->rc_mode;
    HI_U32 u32FrameRate = 30;
    HI_U32 u32StatTime = 1;
    HI_U32 u32Gop = ctx->gop;

    ret = HI_MPI_VENC_GetChnAttr(ctx->VencChn, &ctx->chn_attr);
    if (ret != HI_SUCCESS) {
        SAMPLE_PRT("get chn attr failed %#x\n", ret);
        goto exit;
    }

    switch (enPayLoad) {
        case PT_H265: {
            if (SAMPLE_RC_CBR == enRcMode) {
                VENC_H265_CBR_S    stH265Cbr;

                ctx->chn_attr.stRcAttr.enRcMode = VENC_RC_MODE_H265CBR;
                stH265Cbr.u32Gop            = u32Gop;
                stH265Cbr.u32StatTime       = u32StatTime; /* stream rate statics time(s) */
                stH265Cbr.u32SrcFrameRate   = u32FrameRate; /* input (vi) frame rate */
                stH265Cbr.fr32DstFrameRate  = u32FrameRate; /* target frame rate */
                switch (ctx->enSize) {
                    case PIC_720P:
                        stH265Cbr.u32BitRate = ctx->bitrate; //1024 * 2 + 1024*u32FrameRate/30;
                        break;
                    case PIC_1080P:
                        stH265Cbr.u32BitRate = ctx->bitrate; //1024 * 2 + 2048*u32FrameRate/30;
                        break;
                    case PIC_2592x1944:
                        stH265Cbr.u32BitRate = 1024 * 3 + 3072*u32FrameRate/30;
                        break;
                    case PIC_3840x2160:
                        stH265Cbr.u32BitRate = 1024 * 5  + 5120*u32FrameRate/30;
                        break;
                    case PIC_4000x3000:
                        stH265Cbr.u32BitRate = 1024 * 10 + 5120*u32FrameRate/30;
                        break;
                    case PIC_7680x4320:
                        stH265Cbr.u32BitRate = 1024 * 20 + 5120*u32FrameRate/30;
                        break;
                    default :
                        stH265Cbr.u32BitRate = 1024 * 15 + 2048*u32FrameRate/30;
                        break;
                }
                memcpy(&ctx->chn_attr.stRcAttr.stH265Cbr, &stH265Cbr, sizeof(VENC_H265_CBR_S));
            } else if (SAMPLE_RC_FIXQP == enRcMode) {
                VENC_H265_FIXQP_S    stH265FixQp;

                ctx->chn_attr.stRcAttr.enRcMode = VENC_RC_MODE_H265FIXQP;
                stH265FixQp.u32Gop              = 30;
                stH265FixQp.u32SrcFrameRate     = u32FrameRate;
                stH265FixQp.fr32DstFrameRate    = u32FrameRate;
                stH265FixQp.u32IQp              = 25;
                stH265FixQp.u32PQp              = 30;
                stH265FixQp.u32BQp              = 32;
                memcpy(&ctx->chn_attr.stRcAttr.stH265FixQp, &stH265FixQp, sizeof(VENC_H265_FIXQP_S));
            } else if (SAMPLE_RC_VBR == enRcMode) {
                VENC_H265_VBR_S    stH265Vbr;

                ctx->chn_attr.stRcAttr.enRcMode = VENC_RC_MODE_H265VBR;
                stH265Vbr.u32Gop           = u32Gop;
                stH265Vbr.u32StatTime      = u32StatTime;
                stH265Vbr.u32SrcFrameRate  = u32FrameRate;
                stH265Vbr.fr32DstFrameRate = u32FrameRate;
                switch (ctx->enSize) {
                    case PIC_720P:
                        stH265Vbr.u32MaxBitRate = ctx->bitrate; //1024 * 2 + 1024*u32FrameRate/30;
                        break;
                    case PIC_1080P:
                        stH265Vbr.u32MaxBitRate = ctx->bitrate; //1024 * 2 + 2048*u32FrameRate/30;
                        break;
                    case PIC_2592x1944:
                        stH265Vbr.u32MaxBitRate = 1024 * 3 + 3072*u32FrameRate/30;
                        break;
                    case PIC_3840x2160:
                        stH265Vbr.u32MaxBitRate = 1024 * 5  + 5120*u32FrameRate/30;
                        break;
                    case PIC_4000x3000:
                        stH265Vbr.u32MaxBitRate = 1024 * 10 + 5120*u32FrameRate/30;
                        break;
                    case PIC_7680x4320:
                        stH265Vbr.u32MaxBitRate = 1024 * 20 + 5120*u32FrameRate/30;
                        break;
                    default :
                        stH265Vbr.u32MaxBitRate    = 1024 * 15 + 2048*u32FrameRate/30;
                        break;
                }
                memcpy(&ctx->chn_attr.stRcAttr.stH265Vbr, &stH265Vbr, sizeof(VENC_H265_VBR_S));
            } else if(SAMPLE_RC_AVBR == enRcMode) {
                VENC_H265_AVBR_S    stH265AVbr;

                ctx->chn_attr.stRcAttr.enRcMode = VENC_RC_MODE_H265AVBR;
                stH265AVbr.u32Gop         = u32Gop;
                stH265AVbr.u32StatTime    = u32StatTime;
                stH265AVbr.u32SrcFrameRate  = u32FrameRate;
                stH265AVbr.fr32DstFrameRate = u32FrameRate;
                switch (ctx->enSize) {
                    case PIC_720P:
                        stH265AVbr.u32MaxBitRate = ctx->bitrate; //1024 * 2 + 1024*u32FrameRate/30;
                        break;
                    case PIC_1080P:
                        stH265AVbr.u32MaxBitRate = ctx->bitrate; //1024 * 2 + 2048*u32FrameRate/30;
                        break;
                    case PIC_2592x1944:
                        stH265AVbr.u32MaxBitRate = 1024 * 3 + 3072*u32FrameRate/30;
                        break;
                    case PIC_3840x2160:
                        stH265AVbr.u32MaxBitRate = 1024 * 5  + 5120*u32FrameRate/30;
                        break;
                    case PIC_4000x3000:
                        stH265AVbr.u32MaxBitRate = 1024 * 10 + 5120*u32FrameRate/30;
                        break;
                    case PIC_7680x4320:
                        stH265AVbr.u32MaxBitRate = 1024 * 20 + 5120*u32FrameRate/30;
                        break;
                    default :
                        stH265AVbr.u32MaxBitRate    = 1024 * 15 + 2048*u32FrameRate/30;
                        break;
                }
                memcpy(&ctx->chn_attr.stRcAttr.stH265AVbr, &stH265AVbr, sizeof(VENC_H265_AVBR_S));
            } else if(SAMPLE_RC_QVBR == enRcMode) {
                VENC_H265_QVBR_S    stH265QVbr;

                ctx->chn_attr.stRcAttr.enRcMode = VENC_RC_MODE_H265QVBR;
                stH265QVbr.u32Gop         = u32Gop;
                stH265QVbr.u32StatTime    = u32StatTime;
                stH265QVbr.u32SrcFrameRate  = u32FrameRate;
                stH265QVbr.fr32DstFrameRate = u32FrameRate;
                switch (ctx->enSize) {
                    case PIC_720P:
                        stH265QVbr.u32TargetBitRate= ctx->bitrate; //1024 * 2 + 1024*u32FrameRate/30;
                        break;
                    case PIC_1080P:
                        stH265QVbr.u32TargetBitRate = ctx->bitrate; //1024 * 2 + 2048*u32FrameRate/30;
                        break;
                    case PIC_2592x1944:
                        stH265QVbr.u32TargetBitRate = 1024 * 3 + 3072*u32FrameRate/30;
                        break;
                    case PIC_3840x2160:
                        stH265QVbr.u32TargetBitRate = 1024 * 5  + 5120*u32FrameRate/30;
                        break;
                    case PIC_4000x3000:
                        stH265QVbr.u32TargetBitRate = 1024 * 10 + 5120*u32FrameRate/30;
                        break;
                    case PIC_7680x4320:
                        stH265QVbr.u32TargetBitRate = 1024 * 20 + 5120*u32FrameRate/30;
                        break;
                    default :
                        stH265QVbr.u32TargetBitRate    = 1024 * 15 + 2048*u32FrameRate/30;
                        break;
                }
                memcpy(&ctx->chn_attr.stRcAttr.stH265QVbr, &stH265QVbr, sizeof(VENC_H265_QVBR_S));
            } else if(SAMPLE_RC_CVBR == enRcMode) {
                VENC_H265_CVBR_S    stH265CVbr;

                ctx->chn_attr.stRcAttr.enRcMode = VENC_RC_MODE_H265CVBR;
                stH265CVbr.u32Gop         = u32Gop;
                stH265CVbr.u32StatTime    = u32StatTime;
                stH265CVbr.u32SrcFrameRate  = u32FrameRate;
                stH265CVbr.fr32DstFrameRate = u32FrameRate;
                stH265CVbr.u32LongTermStatTime  = 1;
                stH265CVbr.u32ShortTermStatTime = u32StatTime;
                switch (ctx->enSize) {
                    case PIC_720P:
                        stH265CVbr.u32MaxBitRate         = 1024 * 3 + 1024*u32FrameRate/30;
                        stH265CVbr.u32LongTermMaxBitrate = 1024 * 2 + 1024*u32FrameRate/30;
                        stH265CVbr.u32LongTermMinBitrate = 512;
                        break;
                    case PIC_1080P:
                        stH265CVbr.u32MaxBitRate         = 1024 * 2 + 2048*u32FrameRate/30;
                        stH265CVbr.u32LongTermMaxBitrate = 1024 * 2 + 2048*u32FrameRate/30;
                        stH265CVbr.u32LongTermMinBitrate = 1024;
                        break;
                    case PIC_2592x1944:
                        stH265CVbr.u32MaxBitRate         = 1024 * 4 + 3072*u32FrameRate/30;
                        stH265CVbr.u32LongTermMaxBitrate = 1024 * 3 + 3072*u32FrameRate/30;
                        stH265CVbr.u32LongTermMinBitrate = 1024*2;
                        break;
                    case PIC_3840x2160:
                        stH265CVbr.u32MaxBitRate         = 1024 * 8  + 5120*u32FrameRate/30;
                        stH265CVbr.u32LongTermMaxBitrate = 1024 * 5  + 5120*u32FrameRate/30;
                        stH265CVbr.u32LongTermMinBitrate = 1024*3;
                        break;
                    case PIC_4000x3000:
                        stH265CVbr.u32MaxBitRate         = 1024 * 12  + 5120*u32FrameRate/30;
                        stH265CVbr.u32LongTermMaxBitrate = 1024 * 10 + 5120*u32FrameRate/30;
                        stH265CVbr.u32LongTermMinBitrate = 1024*4;
                        break;
                    case PIC_7680x4320:
                        stH265CVbr.u32MaxBitRate         = 1024 * 24  + 5120*u32FrameRate/30;
                        stH265CVbr.u32LongTermMaxBitrate = 1024 * 20 + 5120*u32FrameRate/30;
                        stH265CVbr.u32LongTermMinBitrate = 1024*6;
                        break;
                    default :
                        stH265CVbr.u32MaxBitRate         = 1024 * 24  + 5120*u32FrameRate/30;
                        stH265CVbr.u32LongTermMaxBitrate = 1024 * 15 + 2048*u32FrameRate/30;
                        stH265CVbr.u32LongTermMinBitrate = 1024*5;
                        break;
                }
                memcpy(&ctx->chn_attr.stRcAttr.stH265CVbr, &stH265CVbr, sizeof(VENC_H265_CVBR_S));
            } else if(SAMPLE_RC_QPMAP == enRcMode) {
                VENC_H265_QPMAP_S    stH265QpMap;

                ctx->chn_attr.stRcAttr.enRcMode = VENC_RC_MODE_H265QPMAP;
                stH265QpMap.u32Gop           = u32Gop;
                stH265QpMap.u32StatTime      = u32StatTime;
                stH265QpMap.u32SrcFrameRate  = u32FrameRate;
                stH265QpMap.fr32DstFrameRate = u32FrameRate;
                stH265QpMap.enQpMapMode      = VENC_RC_QPMAP_MODE_MEANQP;
                memcpy(&ctx->chn_attr.stRcAttr.stH265QpMap, &stH265QpMap, sizeof(VENC_H265_QPMAP_S));
            } else {
                SAMPLE_PRT("%s,%d,enRcMode(%d) not support\n",__FUNCTION__,__LINE__,enRcMode);
                return HI_FAILURE;
            }
            //ctx->chn_attr.stVencAttr.stAttrH265e.bRcnRefShareBuf = bRcnRefShareBuf;
        }
        break;
        case PT_H264:
        {
            if (SAMPLE_RC_CBR == enRcMode)
            {
                VENC_H264_CBR_S    stH264Cbr;

                ctx->chn_attr.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
                stH264Cbr.u32Gop                = u32Gop; /*the interval of IFrame*/
                stH264Cbr.u32StatTime           = u32StatTime; /* stream rate statics time(s) */
                stH264Cbr.u32SrcFrameRate       = u32FrameRate; /* input (vi) frame rate */
                stH264Cbr.fr32DstFrameRate      = u32FrameRate; /* target frame rate */
                switch (ctx->enSize)
                {
                    case PIC_720P:
                        stH264Cbr.u32BitRate         = ctx->bitrate; //1024 * 3 + 1024*u32FrameRate/30;
                        break;
                    case PIC_1080P:
                        stH264Cbr.u32BitRate         = ctx->bitrate; //1024 * 2 + 2048*u32FrameRate/30;
                        break;
                    case PIC_2592x1944:
                        stH264Cbr.u32BitRate         = 1024 * 4 + 3072*u32FrameRate/30;
                        break;
                    case PIC_3840x2160:
                        stH264Cbr.u32BitRate         = 1024 * 8  + 5120*u32FrameRate/30;
                        break;
                    case PIC_4000x3000:
                        stH264Cbr.u32BitRate         = 1024 * 12  + 5120*u32FrameRate/30;
                        break;
                    case PIC_7680x4320:
                        stH264Cbr.u32BitRate         = 1024 * 24  + 5120*u32FrameRate/30;
                        break;
                    default :
                        stH264Cbr.u32BitRate         = 1024 * 24  + 5120*u32FrameRate/30;
                        break;
                }

                memcpy(&ctx->chn_attr.stRcAttr.stH264Cbr, &stH264Cbr, sizeof(VENC_H264_CBR_S));
            }
            else if (SAMPLE_RC_FIXQP == enRcMode)
            {
                VENC_H264_FIXQP_S    stH264FixQp;

                ctx->chn_attr.stRcAttr.enRcMode = VENC_RC_MODE_H264FIXQP;
                stH264FixQp.u32Gop           = 30;
                stH264FixQp.u32SrcFrameRate  = u32FrameRate;
                stH264FixQp.fr32DstFrameRate = u32FrameRate;
                stH264FixQp.u32IQp           = 25;
                stH264FixQp.u32PQp           = 30;
                stH264FixQp.u32BQp           = 32;
                memcpy(&ctx->chn_attr.stRcAttr.stH264FixQp, &stH264FixQp, sizeof(VENC_H264_FIXQP_S));
            }
            else if (SAMPLE_RC_VBR == enRcMode)
            {
                VENC_H264_VBR_S    stH264Vbr;

                ctx->chn_attr.stRcAttr.enRcMode = VENC_RC_MODE_H264VBR;
                stH264Vbr.u32Gop           = u32Gop;
                stH264Vbr.u32StatTime      = u32StatTime;
                stH264Vbr.u32SrcFrameRate  = u32FrameRate;
                stH264Vbr.fr32DstFrameRate = u32FrameRate;
                switch (ctx->enSize)
                {
                    case PIC_360P:
                        stH264Vbr.u32MaxBitRate = 1024 * 2   + 1024*u32FrameRate/30;
                        break;
                    case PIC_720P:
                        stH264Vbr.u32MaxBitRate = ctx->bitrate; //1024 * 2   + 1024*u32FrameRate/30;
                        break;
                    case PIC_1080P:
                        stH264Vbr.u32MaxBitRate = ctx->bitrate; //1024 * 2   + 2048*u32FrameRate/30;
                        break;
                    case PIC_2592x1944:
                        stH264Vbr.u32MaxBitRate = 1024 * 3   + 3072*u32FrameRate/30;
                        break;
                    case PIC_3840x2160:
                        stH264Vbr.u32MaxBitRate = 1024 * 5   + 5120*u32FrameRate/30;
                        break;
                    case PIC_4000x3000:
                        stH264Vbr.u32MaxBitRate = 1024 * 10  + 5120*u32FrameRate/30;
                        break;
                    case PIC_7680x4320:
                        stH264Vbr.u32MaxBitRate = 1024 * 20  + 5120*u32FrameRate/30;
                        break;
                    default :
                        stH264Vbr.u32MaxBitRate = 1024 * 15  + 2048*u32FrameRate/30;
                        break;
                }
                memcpy(&ctx->chn_attr.stRcAttr.stH264Vbr, &stH264Vbr, sizeof(VENC_H264_VBR_S));
            }
            else if (SAMPLE_RC_AVBR == enRcMode)
            {
                VENC_H264_VBR_S    stH264AVbr;

                ctx->chn_attr.stRcAttr.enRcMode = VENC_RC_MODE_H264AVBR;
                stH264AVbr.u32Gop           = u32Gop;
                stH264AVbr.u32StatTime      = u32StatTime;
                stH264AVbr.u32SrcFrameRate  = u32FrameRate;
                stH264AVbr.fr32DstFrameRate = u32FrameRate;
                switch (ctx->enSize)
                {
                    case PIC_360P:
                        stH264AVbr.u32MaxBitRate = 1024 * 2   + 1024*u32FrameRate/30;
                        break;
                    case PIC_720P:
                        stH264AVbr.u32MaxBitRate = ctx->bitrate; //1024 * 2   + 1024*u32FrameRate/30;
                        break;
                    case PIC_1080P:
                        stH264AVbr.u32MaxBitRate = ctx->bitrate; //1024 * 2   + 2048*u32FrameRate/30;
                        break;
                    case PIC_2592x1944:
                        stH264AVbr.u32MaxBitRate = 1024 * 3   + 3072*u32FrameRate/30;
                        break;
                    case PIC_3840x2160:
                        stH264AVbr.u32MaxBitRate = 1024 * 5   + 5120*u32FrameRate/30;
                        break;
                    case PIC_4000x3000:
                        stH264AVbr.u32MaxBitRate = 1024 * 10  + 5120*u32FrameRate/30;
                        break;
                    case PIC_7680x4320:
                        stH264AVbr.u32MaxBitRate = 1024 * 20  + 5120*u32FrameRate/30;
                        break;
                    default :
                        stH264AVbr.u32MaxBitRate = 1024 * 15  + 2048*u32FrameRate/30;
                        break;
                }
                memcpy(&ctx->chn_attr.stRcAttr.stH264AVbr, &stH264AVbr, sizeof(VENC_H264_AVBR_S));
            }
            else if (SAMPLE_RC_QVBR == enRcMode)
            {
                VENC_H264_QVBR_S    stH264QVbr;

                ctx->chn_attr.stRcAttr.enRcMode = VENC_RC_MODE_H264QVBR;
                stH264QVbr.u32Gop           = u32Gop;
                stH264QVbr.u32StatTime      = u32StatTime;
                stH264QVbr.u32SrcFrameRate  = u32FrameRate;
                stH264QVbr.fr32DstFrameRate = u32FrameRate;
                switch (ctx->enSize)
                {
                    case PIC_360P:
                        stH264QVbr.u32TargetBitRate = 1024 * 2   + 1024*u32FrameRate/30;
                        break;
                    case PIC_720P:
                        stH264QVbr.u32TargetBitRate = ctx->bitrate; //1024 * 2   + 1024*u32FrameRate/30;
                        break;
                    case PIC_1080P:
                        stH264QVbr.u32TargetBitRate = ctx->bitrate; //1024 * 2   + 2048*u32FrameRate/30;
                        break;
                    case PIC_2592x1944:
                        stH264QVbr.u32TargetBitRate = 1024 * 3   + 3072*u32FrameRate/30;
                        break;
                    case PIC_3840x2160:
                        stH264QVbr.u32TargetBitRate = 1024 * 5   + 5120*u32FrameRate/30;
                        break;
                    case PIC_4000x3000:
                        stH264QVbr.u32TargetBitRate = 1024 * 10  + 5120*u32FrameRate/30;
                        break;
                    case PIC_7680x4320:
                        stH264QVbr.u32TargetBitRate = 1024 * 20  + 5120*u32FrameRate/30;
                        break;
                    default :
                        stH264QVbr.u32TargetBitRate = 1024 * 15  + 2048*u32FrameRate/30;
                        break;
                }
                memcpy(&ctx->chn_attr.stRcAttr.stH264QVbr, &stH264QVbr, sizeof(VENC_H264_QVBR_S));
            }
            else if(SAMPLE_RC_CVBR == enRcMode)
            {
                VENC_H264_CVBR_S    stH264CVbr;

                ctx->chn_attr.stRcAttr.enRcMode = VENC_RC_MODE_H264CVBR;
                stH264CVbr.u32Gop         = u32Gop;
                stH264CVbr.u32StatTime    = u32StatTime;
                stH264CVbr.u32SrcFrameRate  = u32FrameRate;
                stH264CVbr.fr32DstFrameRate = u32FrameRate;
                stH264CVbr.u32LongTermStatTime  = 1;
                stH264CVbr.u32ShortTermStatTime = u32StatTime;
                switch (ctx->enSize)
                {
                    case PIC_720P:
                        stH264CVbr.u32MaxBitRate         = 1024 * 3 + 1024*u32FrameRate/30;
                        stH264CVbr.u32LongTermMaxBitrate = 1024 * 2 + 1024*u32FrameRate/30;
                        stH264CVbr.u32LongTermMinBitrate = 512;
                        break;
                    case PIC_1080P:
                        stH264CVbr.u32MaxBitRate         = 1024 * 2 + 2048*u32FrameRate/30;
                        stH264CVbr.u32LongTermMaxBitrate = 1024 * 2 + 2048*u32FrameRate/30;
                        stH264CVbr.u32LongTermMinBitrate = 1024;
                        break;
                    case PIC_2592x1944:
                        stH264CVbr.u32MaxBitRate         = 1024 * 4 + 3072*u32FrameRate/30;
                        stH264CVbr.u32LongTermMaxBitrate = 1024 * 3 + 3072*u32FrameRate/30;
                        stH264CVbr.u32LongTermMinBitrate = 1024*2;
                        break;
                    case PIC_3840x2160:
                        stH264CVbr.u32MaxBitRate         = 1024 * 8  + 5120*u32FrameRate/30;
                        stH264CVbr.u32LongTermMaxBitrate = 1024 * 5  + 5120*u32FrameRate/30;
                        stH264CVbr.u32LongTermMinBitrate = 1024*3;
                        break;
                    case PIC_4000x3000:
                        stH264CVbr.u32MaxBitRate         = 1024 * 12  + 5120*u32FrameRate/30;
                        stH264CVbr.u32LongTermMaxBitrate = 1024 * 10 + 5120*u32FrameRate/30;
                        stH264CVbr.u32LongTermMinBitrate = 1024*4;
                        break;
                    case PIC_7680x4320:
                        stH264CVbr.u32MaxBitRate         = 1024 * 24  + 5120*u32FrameRate/30;
                        stH264CVbr.u32LongTermMaxBitrate = 1024 * 20 + 5120*u32FrameRate/30;
                        stH264CVbr.u32LongTermMinBitrate = 1024*6;
                        break;
                    default :
                        stH264CVbr.u32MaxBitRate         = 1024 * 24  + 5120*u32FrameRate/30;
                        stH264CVbr.u32LongTermMaxBitrate = 1024 * 15 + 2048*u32FrameRate/30;
                        stH264CVbr.u32LongTermMinBitrate = 1024*5;
                        break;
                }
                memcpy(&ctx->chn_attr.stRcAttr.stH264CVbr, &stH264CVbr, sizeof(VENC_H264_CVBR_S));
            }
            else if(SAMPLE_RC_QPMAP == enRcMode)
            {
                VENC_H264_QPMAP_S    stH264QpMap;

                ctx->chn_attr.stRcAttr.enRcMode = VENC_RC_MODE_H264QPMAP;
                stH264QpMap.u32Gop           = u32Gop;
                stH264QpMap.u32StatTime      = u32StatTime;
                stH264QpMap.u32SrcFrameRate  = u32FrameRate;
                stH264QpMap.fr32DstFrameRate = u32FrameRate;
                memcpy(&ctx->chn_attr.stRcAttr.stH264QpMap, &stH264QpMap, sizeof(VENC_H264_QPMAP_S));
            }
            else
            {
                SAMPLE_PRT("%s,%d,enRcMode(%d) not support\n",__FUNCTION__,__LINE__,enRcMode);
                return HI_FAILURE;
            }
            //ctx->chn_attr.stVencAttr.stAttrH264e.bRcnRefShareBuf = bRcnRefShareBuf;
        }
        break;
        case PT_MJPEG:
        {
            if (SAMPLE_RC_FIXQP == enRcMode)
            {
                VENC_MJPEG_FIXQP_S stMjpegeFixQp;

                ctx->chn_attr.stRcAttr.enRcMode = VENC_RC_MODE_MJPEGFIXQP;
                stMjpegeFixQp.u32Qfactor        = 95;
                stMjpegeFixQp.u32SrcFrameRate    = u32FrameRate;
                stMjpegeFixQp.fr32DstFrameRate   = u32FrameRate;

                memcpy(&ctx->chn_attr.stRcAttr.stMjpegFixQp, &stMjpegeFixQp,sizeof(VENC_MJPEG_FIXQP_S));
            }
            else if (SAMPLE_RC_CBR == enRcMode)
            {
                VENC_MJPEG_CBR_S stMjpegeCbr;

                ctx->chn_attr.stRcAttr.enRcMode = VENC_RC_MODE_MJPEGCBR;
                stMjpegeCbr.u32StatTime         = u32StatTime;
                stMjpegeCbr.u32SrcFrameRate     = u32FrameRate;
                stMjpegeCbr.fr32DstFrameRate    = u32FrameRate;
                switch (ctx->enSize)
                {
                    case PIC_360P:
                        stMjpegeCbr.u32BitRate = 1024 * 3  + 1024*u32FrameRate/30;
                        break;
                    case PIC_720P:
                        stMjpegeCbr.u32BitRate = 1024 * 5  + 1024*u32FrameRate/30;
                        break;
                    case PIC_1080P:
                        stMjpegeCbr.u32BitRate = 1024 * 8  + 2048*u32FrameRate/30;
                        break;
                    case PIC_2592x1944:
                        stMjpegeCbr.u32BitRate = 1024 * 20 + 3072*u32FrameRate/30;
                        break;
                    case PIC_3840x2160:
                        stMjpegeCbr.u32BitRate = 1024 * 25 + 5120*u32FrameRate/30;
                        break;
                    case PIC_4000x3000:
                        stMjpegeCbr.u32BitRate = 1024 * 30 + 5120*u32FrameRate/30;
                        break;
                    case PIC_7680x4320:
                        stMjpegeCbr.u32BitRate = 1024 * 40 + 5120*u32FrameRate/30;
                        break;
                    default :
                        stMjpegeCbr.u32BitRate = 1024 * 20 + 2048*u32FrameRate/30;
                        break;
                }

                memcpy(&ctx->chn_attr.stRcAttr.stMjpegCbr, &stMjpegeCbr,sizeof(VENC_MJPEG_CBR_S));
            }
            else if ((SAMPLE_RC_VBR == enRcMode) ||(SAMPLE_RC_AVBR == enRcMode)||
                     (SAMPLE_RC_QVBR == enRcMode)||(SAMPLE_RC_CVBR == enRcMode))
            {
                VENC_MJPEG_VBR_S   stMjpegVbr;

                if(SAMPLE_RC_AVBR == enRcMode)
                {
                    SAMPLE_PRT("Mjpege not support AVBR, so change rcmode to VBR!\n");
                }

                ctx->chn_attr.stRcAttr.enRcMode = VENC_RC_MODE_MJPEGVBR;
                stMjpegVbr.u32StatTime      = u32StatTime;
                stMjpegVbr.u32SrcFrameRate  = u32FrameRate;
                stMjpegVbr.fr32DstFrameRate = 5;

                switch (ctx->enSize)
                {
                    case PIC_360P:
                        stMjpegVbr.u32MaxBitRate = 1024 * 3 + 1024*u32FrameRate/30;
                        break;
                    case PIC_720P:
                        stMjpegVbr.u32MaxBitRate = 1024 * 5 + 1024*u32FrameRate/30;
                        break;
                    case PIC_1080P:
                        stMjpegVbr.u32MaxBitRate = 1024 * 8 + 2048*u32FrameRate/30;
                        break;
                    case PIC_2592x1944:
                        stMjpegVbr.u32MaxBitRate = 1024 * 20 + 3072*u32FrameRate/30;
                        break;
                    case PIC_3840x2160:
                        stMjpegVbr.u32MaxBitRate = 1024 * 25 + 5120*u32FrameRate/30;
                        break;
                    case PIC_4000x3000:
                        stMjpegVbr.u32MaxBitRate    = 1024 * 30 + 5120*u32FrameRate/30;
                        break;
                    case PIC_7680x4320:
                        stMjpegVbr.u32MaxBitRate = 1024 * 40 + 5120*u32FrameRate/30;
                        break;
                    default :
                        stMjpegVbr.u32MaxBitRate = 1024 * 20 + 2048*u32FrameRate/30;
                        break;
                }

                memcpy(&ctx->chn_attr.stRcAttr.stMjpegVbr, &stMjpegVbr,sizeof(VENC_MJPEG_VBR_S));
            }
            else
            {
                SAMPLE_PRT("cann't support other mode(%d) in this version!\n",enRcMode);
                return HI_FAILURE;
            }
        }
        break;

        case PT_JPEG:
            VENC_ATTR_JPEG_S  stJpegAttr;
            stJpegAttr.bSupportDCF     = HI_FALSE;
            stJpegAttr.stMPFCfg.u8LargeThumbNailNum = 0;
            stJpegAttr.enReceiveMode                = VENC_PIC_RECEIVE_SINGLE;
            memcpy(&ctx->chn_attr.stVencAttr.stAttrJpege, &stJpegAttr, sizeof(VENC_ATTR_JPEG_S));
            break;
        default:
            SAMPLE_PRT("cann't support this enType (%d) in this version!\n", enPayLoad);
            ret = HI_ERR_VENC_NOT_SUPPORT;
            goto exit;
    }

    ret = HI_MPI_VENC_SetChnAttr(ctx->VencChn, &ctx->chn_attr);
    if (ret != HI_SUCCESS) {
        SAMPLE_PRT("set chn attr failed %#x\n", ret);
        goto exit;
    }

exit:
    return ret;
}

static void set_frame_info(EncCtx *ctx)
{
    VIDEO_FRAME_INFO_S *frame = ctx->src_frame;
    HI_U32 width = ctx->width;
    HI_U32 height = ctx->height;

    memset(&(frame->stVFrame),0x00,sizeof(VIDEO_FRAME_S));
    frame->stVFrame.u32Width = width;
    frame->stVFrame.u32Height = height;
    frame->stVFrame.enPixelFormat = PIXEL_FORMAT_YVU_SEMIPLANAR_420;
    frame->u32PoolId = ctx->poolID;
    frame->stVFrame.u64PhyAddr[0] = ctx->phyYaddr;
    frame->stVFrame.u64PhyAddr[1] = ctx->phyYaddr + width * height;

    frame->stVFrame.u64VirAddr[0] = (HI_U64)ctx->pVirYaddr;
    frame->stVFrame.u64VirAddr[1] = (HI_U64)ctx->pVirYaddr + width * height;

    frame->stVFrame.u32Stride[0] = width;
    frame->stVFrame.u32Stride[1] = width;
    frame->stVFrame.enField     = VIDEO_FIELD_FRAME;

    frame->stVFrame.enCompressMode = COMPRESS_MODE_NONE;
    frame->stVFrame.enVideoFormat  = VIDEO_FORMAT_LINEAR;
    frame->stVFrame.u64PTS     = ctx->frame_cnt * 40;
    frame->stVFrame.u32TimeRef = ctx->frame_cnt * 2;
}

HI_S32 venc_encode(EncCtx *ctx)
{
    SAMPLE_PRT("enter\n");
    HI_S32 ret = HI_SUCCESS;
    struct timeval TimeoutVal;
    fd_set read_fds;
    HI_S32 VencFd;
    VENC_CHN_STATUS_S stStat;
    VENC_STREAM_S stStream;
    HI_S32 s32Ret , eos = 0;
    VENC_CHN VencChn = ctx->VencChn;
    HI_U32 width = ctx->width;
    HI_U32 height = ctx->height;
    HI_U32 read_len;

    VencFd = HI_MPI_VENC_GetFd(VencChn);
    if (VencFd < 0) {
        SAMPLE_PRT("HI_MPI_VENC_GetFd failed with %#x!\n",VencFd);
        return HI_FAILURE;
    }

    /******************************************
     step 2:  send frame and get stream
    ******************************************/
    while (!eos) {
        set_frame_info(ctx);

        read_len = fread(ctx->src_buf, width * height * 3 / 2, 1, ctx->fp_raw);
        if(read_len < 0) {
            SAMPLE_PRT("fread yuv file failed\n");
            ret = HI_FAILURE;
            goto exit;
        } else if(read_len == 0) {
            SAMPLE_PRT("fread NO bytes\n");
            eos = 1;
            continue;
        }

        yuv420p_to_yvu420sp(ctx->src_buf, ctx->pVirYaddr, width, height);

        ret = HI_MPI_VENC_SendFrame(VencChn, ctx->src_frame, 50);
        if(ret != HI_SUCCESS) {
            SAMPLE_PRT("HI_MPI_VENC_SendFrame failed 0x%08x\n",ret);
            goto exit;
        }

        FD_ZERO(&read_fds);
        FD_SET(VencFd, &read_fds);

        TimeoutVal.tv_sec  = 2;
        TimeoutVal.tv_usec = 0;
        s32Ret = select(VencFd + 1, &read_fds, NULL, NULL, &TimeoutVal);
        if (s32Ret < 0) {
            SAMPLE_PRT("select failed!\n");
            break;
        } else if (s32Ret == 0) {
            SAMPLE_PRT("get venc stream time out, exit thread\n");
            continue;
        } else {
            if (FD_ISSET(VencFd, &read_fds)) {
                /*******************************************************
                 step 2.1 : query how many packs in one-frame stream.
                *******************************************************/
                memset(&stStream, 0, sizeof(stStream));
                s32Ret = HI_MPI_VENC_QueryStatus(VencChn, &stStat);
                if (HI_SUCCESS != s32Ret) {
                    SAMPLE_PRT("HI_MPI_VENC_Query chn[%d] failed with %#x!\n", VencChn, s32Ret);
                    break;
                }

                /*******************************************************
                step 2.2 :suggest to check both u32CurPacks and u32LeftStreamFrames at the same time,for example:
                 if(0 == stStat.u32CurPacks || 0 == stStat.u32LeftStreamFrames)
                 {
                    SAMPLE_PRT("NOTE: Current  frame is NULL!\n");
                    continue;
                 }
                *******************************************************/
                if(0 == stStat.u32CurPacks) {
                    SAMPLE_PRT("NOTE: Current  frame is NULL!\n");
                    continue;
                }

                /*******************************************************
                 step 2.3 : malloc corresponding number of pack nodes.
                *******************************************************/
                stStream.pstPack = (VENC_PACK_S*)malloc(sizeof(VENC_PACK_S) * stStat.u32CurPacks);
                if (NULL == stStream.pstPack) {
                    SAMPLE_PRT("malloc stream pack failed!\n");
                    break;
                }

                /*******************************************************
                 step 2.4 : call mpi to get one-frame stream
                *******************************************************/
                stStream.u32PackCount = stStat.u32CurPacks;
                s32Ret = HI_MPI_VENC_GetStream(VencChn, &stStream, HI_TRUE);
                if (HI_SUCCESS != s32Ret) {
                    FREE(stStream.pstPack);
                    SAMPLE_PRT("HI_MPI_VENC_GetStream failed with %#x!\n", s32Ret);
                    break;
                }

                /*******************************************************
                 step 2.5 : save frame to file
                *******************************************************/
                s32Ret = SAMPLE_COMM_VENC_SaveStream(ctx->fp_strm, &stStream);
                if (HI_SUCCESS != s32Ret)
                {
                    FREE(stStream.pstPack);
                    SAMPLE_PRT("save stream failed!\n");
                    break;
                }

                /*******************************************************
                 step 2.6 : release stream
                *******************************************************/
                s32Ret = HI_MPI_VENC_ReleaseStream(VencChn, &stStream);
                if (HI_SUCCESS != s32Ret)
                {
                    FREE(stStream.pstPack);
                    break;
                }

                /*******************************************************
                 step 2.7 : free pack nodes
                *******************************************************/
                FREE(stStream.pstPack);
            }
        }

        SAMPLE_PRT("encode frame :%d\n", ctx->frame_cnt++);
        if(ctx->frame_cnt >= ctx->frames) {
            eos = 1;
            SAMPLE_PRT("encode get eos.\n");
        }
    }

exit:
    SAMPLE_PRT("finish encoding %d frames\n", ctx->frame_cnt);
    return ret;
}

int main(int argc, char **argv)
{
    HI_S32 ret = HI_SUCCESS;
    EncCtx enc_ctx;
    EncCtx *ctx = &enc_ctx;
    memset(ctx, 0, sizeof(EncCtx));
    ctx->in_file = getarg("F:\\rkvenc_verify\\input_yuv\\3903_720x576.yuv", "-i", "--input");
    ctx->out_file = getarg("F:\\rkvenc_verify\\input_yuv\\3903_720x576_hi_rk.yuv", "-o", "--output");
    ctx->width = getarg(1280, "-w", "--width");
    ctx->height = getarg(720, "-h", "--height");
    ctx->frames = getarg(3, "-f", "--frames");
    ctx->enc_mode = (EncMode)getarg(1, "-m", "--mode");
    ctx->rc_mode = (SAMPLE_RC_E)getarg(0, "-r", "--rc_mode");
    ctx->gop = getarg(30, "-g", "--gop");
    ctx->fixed_qp = getarg(26, "-q", "--qp");
    ctx->bitrate = getarg(2000, "-b", "--bitrate");

    if (argc < 2) {
        SAMPLE_PRT("Usage: ./sample_venc -i input.yuv -w=1280 -h=720 --frames=3 -m 1 -o out.bin\n");
        return 0;
    }
    show_info(ctx);

    ret = sample_init(ctx);
    if (ret != HI_SUCCESS) {
        SAMPLE_PRT("sample init failed\n");
        goto exit;
    }

    ret = venc_init(ctx);
    if (ret != HI_SUCCESS) {
        SAMPLE_PRT("venc init failed\n");
        goto exit;
    }

    ret = set_encoder_attr(ctx);
    if (ret != HI_SUCCESS) {
        SAMPLE_PRT("set encoder attr failed\n");
        goto exit;
    }

    ret = venc_encode(ctx);
    if (ret != HI_SUCCESS) {
        SAMPLE_PRT("venc encode failed\n");
        goto exit;
    }

exit:
    ret = venc_deinit(ctx);
    if (ret != HI_SUCCESS) {
        SAMPLE_PRT("venc deinit failed\n");
    }

    ret = sample_deinit(ctx);
    if (ret != HI_SUCCESS) {
        SAMPLE_PRT("sample deinit failed\n");
    }

    return 0;
}

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
    EncMode enc_mode;
    HI_U32 frame_cnt;
    HI_U32 frames;
    HI_U32 width;
    HI_U32 height;
    HI_U32 mb_x;
    HI_U32 mb_y;
    string in_file;
    string out_file;
    HI_U8  *src_buf;
    FILE   *fp_raw;
    FILE   *fp_strm;
    VIDEO_FRAME_INFO_S * src_frame; /* input frame info */
    VB_POOL poolID;
    HI_U32 phyYaddr; /* physical address of input frame */
    HI_U8 *pVirYaddr; /* virtual address of input frame */
} EncCtx;

static void show_info(EncCtx *ctx)
{
    printf("input %s\n"
           "output %s\n"
           "width %d height %d frames %d\n"
           "codec %s\n",
           ctx->in_file.c_str(), ctx->out_file.c_str(),
           ctx->width, ctx->height, ctx->frames,
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

HI_S32 venc_init(EncCtx *ctx)
{
    HI_S32 s32Ret = HI_SUCCESS;
    SAMPLE_PRT("enter\n");

    /******************************************
    step  1: init sys variable
    ******************************************/
    VB_CONFIG_S stVbConf;
    HI_U32 u32BlkSize;
    PIC_SIZE_E enSize = PIC_720P;
    VENC_GOP_MODE_E enGopMode;
    VENC_GOP_ATTR_S stGopAttr;
    SAMPLE_RC_E     enRcMode;
    PAYLOAD_TYPE_E  enPayLoad  = PT_H265;
    HI_U32          u32Profile = 0;
    HI_BOOL         bRcnRefShareBuf = HI_TRUE;
    memset(&stVbConf, 0, sizeof(VB_CONFIG_S));

    stVbConf.u32MaxPoolCnt = 128;
    //u32BlkSize = SAMPLE_COMM_SYS_CalcPicVbBlkSize(gs_enNorm,\
    //            enSize, SAMPLE_PIXEL_FORMAT, SAMPLE_SYS_ALIGN_WIDTH);

    stVbConf.astCommPool[0].u64BlkSize = u32BlkSize = 1280 * 720 * 2;
    stVbConf.astCommPool[0].u32BlkCnt = 20;
    printf("--------blksize = %d----------------\n",u32BlkSize);

    /******************************************
    step 2: mpp system init.
    ******************************************/
    s32Ret = SAMPLE_COMM_SYS_Init(&stVbConf);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("system init failed with %d!\n", s32Ret);
        goto exit;
    }

    enRcMode = SAMPLE_RC_CBR;
    enGopMode = VENC_GOPMODE_NORMALP;
    s32Ret = SAMPLE_COMM_VENC_GetGopAttr(enGopMode,&stGopAttr);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Venc Get GopAttr for %#x!\n", s32Ret);
        goto exit;
    }

    s32Ret = SAMPLE_COMM_VENC_Start(ctx->VencChn, enPayLoad,enSize, enRcMode,u32Profile,bRcnRefShareBuf,&stGopAttr);
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

exit:
    return s32Ret;
}

HI_S32 venc_deinit(EncCtx *ctx)
{
    HI_S32 ret = HI_SUCCESS;
    SAMPLE_PRT("enter\n");
    SAMPLE_COMM_VENC_Stop(ctx->VencChn);
    SAMPLE_COMM_SYS_Exit();
    FREE(ctx->src_frame);

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
    HI_S32 i;
    VENC_CHN_ATTR_S stVencChnAttr;
    SAMPLE_VENC_GETSTREAM_PARA_S* pstPara;
    HI_S32 maxfd = 0;
    struct timeval TimeoutVal;
    fd_set read_fds;
    HI_S32 VencFd;
    VENC_CHN_STATUS_S stStat;
    VENC_STREAM_S stStream;
    HI_S32 s32Ret ,frame = 0, eos = 0;
    VENC_CHN VencChn = ctx->VencChn;
    PAYLOAD_TYPE_E enPayLoadType;
    HI_U32 width = ctx->width;
    HI_U32 height = ctx->height;
    HI_U32 read_len;

    if(ctx->enc_mode == CODEC_AVC)
        enPayLoadType = PT_H264;
    else if(ctx->enc_mode == CODEC_HEVC)
        enPayLoadType = PT_H265;
    else
    {
        SAMPLE_PRT("unknown payload type\n");
        return HI_FAILURE;
    }

    VencFd = HI_MPI_VENC_GetFd(VencChn);
    if (VencFd < 0)
    {
        SAMPLE_PRT("HI_MPI_VENC_GetFd failed with %#x!\n",VencFd);
        return HI_FAILURE;
    }

    VB_BLK handleY = VB_INVALID_HANDLE;
    HI_U32 phyYaddr;
    HI_U8 *pVirYaddr;

    /******************************************
     step 2:  Start to get streams of each channel.
    ******************************************/
    while (!eos)
    {
        do {
            handleY = HI_MPI_VB_GetBlock(VB_INVALID_POOLID, width * height * 3 / 2, NULL);
        } while (VB_INVALID_HANDLE == handleY);

        if(handleY == VB_INVALID_HANDLE) {
            SAMPLE_PRT("getblock for y failed\n");
            ret = HI_FAILURE;
            goto exit;
        }

        ctx->poolID = HI_MPI_VB_Handle2PoolId (handleY);
        phyYaddr = HI_MPI_VB_Handle2PhysAddr(handleY);
        if( phyYaddr == 0) {
            SAMPLE_PRT("HI_MPI_VB_Handle2PhysAddr for handleY failed\n");
            ret = HI_FAILURE;
            goto exit;
        }

        pVirYaddr = (HI_U8*) HI_MPI_SYS_Mmap(phyYaddr, width * height * 3 / 2);
        ctx->phyYaddr = phyYaddr;
        ctx->pVirYaddr = pVirYaddr;
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

        yuv420p_to_yvu420sp(ctx->src_buf, pVirYaddr, width, height);

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

                HI_MPI_SYS_Munmap(pVirYaddr, width * height * 3 / 2);
                HI_MPI_VB_ReleaseBlock(handleY);
                handleY = VB_INVALID_HANDLE;
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
    bool help = getarg(false, "-H", "--help", "-?");
    ctx->in_file = getarg("F:\\rkvenc_verify\\input_yuv\\3903_720x576.yuv", "-i", "--input");
    ctx->out_file = getarg("F:\\rkvenc_verify\\input_yuv\\3903_720x576_hi_rk.yuv", "-o", "--output");
    ctx->width = getarg(1280, "-w", "--width");
    ctx->height = getarg(720, "-h", "--height");
    ctx->frames = getarg(3, "-f", "--frames");
    ctx->enc_mode = (EncMode)getarg(1, "-m", "--mode");

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

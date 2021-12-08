// HiMPP V3.0 媒体处理软件开发参考.pdf
// 1. 10-1   Hi3536 每秒可解码的像素数 1440Mpixel 相当于 6 路 3840*2160@30fps。
// 2. 4-1    VHD0 最多支持 64 画面;VHD1 最多支持 32 画面。
// 3. 4-3    最大输出时序: HD0 3840x2160@60; HD1 1920x1080@60。
// 4. 4-3    VHD0 固定绑定 DHD0；VHD1 固定绑定 DHD1。
// 5. 4-148  视频输出模块有 10 个视频层, 0：VHD0 1：VHD1。
// 6. 4-156  VHD0 视频层的最大分辨率为 3840 x 2160;VHD1 视频层的最大分辨率为 1920 x 1080。

// 视频解码输出过程涉及：VDEC、VPSS、VO 等对象。
// VDEC： 视频解码器，本应用使用 8 个视频解码通道，编号 0~7，分别对应 VPSS GROUP 0~7。
// VPSS： 视频处理子系统，本应用使用 8 个组（Group），每组包含一个通道，编号 0~7，分别对应视频输出通道 0~7。
// VO：   视频输出，本应用使用 2 个视频层，编号 0、1，分别对应 VHD0、VHD1；
//        每个视频层包含 4 个视频输出通道；
//        视频层 0 包含视频输出通道 0~3，视频层 1 包含视频输出通道 4~7。
#include <tea/tea.h>
#include "hi_common.h"
#include "hi_comm_sys.h"
#include "hi_comm_vb.h"
#include "hi_comm_vo.h"
#include "hi_comm_vpss.h"
#include "hi_comm_vdec.h"
#include "hi_comm_region.h"
#include "hi_comm_hdmi.h"
#include "hi_defines.h"
#include "mpi_sys.h"
#include "mpi_vb.h"
#include "mpi_vo.h"
#include "mpi_vpss.h"
#include "mpi_vdec.h"
#include "mpi_region.h"
#include "mpi_hdmi.h"
#include "sample_comm.h"

// environment

// settings
// 2 路视频输出（VO）：0-HDMI、1-BT1120。
int vo_mode[2];
// 单窗口模式时使能的通道。
// 会要求不使能任何通道吗？
int vo_chn[2];

// TODO: 支持 4K，一路？二路？
static SIZE_S stSize = {1920, 1080};

// internal state
#define DEC_CHN_NUM 4
static int32_t VdCnt = DEC_CHN_NUM;
static int32_t GrpCnt = DEC_CHN_NUM;
static VO_DEV VoDev[2] = {0,1};
static VO_LAYER VoLayer[2] = {0,1};
static PAYLOAD_TYPE_E enType[DEC_CHN_NUM];
static int current_vo_mode[2];
static int current_vo_chn[2];

// input

// monitor: statistics

// 约定：每个 VO 对应 4 通道。
HI_BOOL is_chn_enabled(int chn)
{
    int vo = chn / 4;
    if(VO_MODE_4MUX == vo_mode[vo])
    {
        return TRUE;
    }
    ASSERT(VO_MODE_1MUX == vo_mode[vo]);
    if(chn == vo_chn[vo])
    {
        return TRUE;
    }
    return FALSE;
}

// 未解除 VDEC 与 VPSS 的绑定，虽然能够工作，但是不确定有无隐藏问题。
static void stop_vdec_chn(VDEC_CHN VdChn, HI_BOOL bNoDestroy)
{
    HI_S32 __attribute__((unused)) s32Ret;

    Debug("STOP VDEC %d", VdChn);

    s32Ret = HI_MPI_VDEC_StopRecvStream(VdChn);
    ASSERT(0 == s32Ret);

    if(!bNoDestroy)
    {
        s32Ret = HI_MPI_VDEC_DestroyChn(VdChn);
        ASSERT(0 == s32Ret);
    }
}

static void start_vdec_chn(VDEC_CHN VdChn, SIZE_S* stSize, HI_BOOL bNoCreate)
{
    HI_S32 __attribute__((unused)) s32Ret;

    if(!bNoCreate)
    {
        VDEC_CHN_ATTR_S stVdecChnAttr;

        Debug("Create VDEC %d, enType: %d", VdChn, enType[VdChn]);

        stVdecChnAttr.enType       = enType[VdChn];
        stVdecChnAttr.u32BufSize   = stSize->u32Width * stSize->u32Height;
        stVdecChnAttr.u32Priority  = 5;
        stVdecChnAttr.u32PicWidth  = stSize->u32Width;
        stVdecChnAttr.u32PicHeight = stSize->u32Height;
        stVdecChnAttr.stVdecVideoAttr.enMode = VIDEO_MODE_FRAME;
        stVdecChnAttr.stVdecVideoAttr.u32RefFrameNum = 3;
        stVdecChnAttr.stVdecVideoAttr.bTemporalMvpEnable = 1;

        s32Ret = HI_MPI_VDEC_CreateChn(VdChn, &stVdecChnAttr);
        ASSERT(0 == s32Ret);

        s32Ret = HI_MPI_VDEC_StartRecvStream(VdChn);
        ASSERT(0 == s32Ret);

        s32Ret = HI_MPI_VDEC_StopRecvStream(VdChn);
        ASSERT(0 == s32Ret);

        VDEC_PRTCL_PARAM_S stPrtclParam;

        HI_MPI_VDEC_GetProtocolParam(VdChn, &stPrtclParam);
        stPrtclParam.enType = enType[VdChn];
        stPrtclParam.stH265PrtclParam.s32MaxSpsNum   = 2;
        stPrtclParam.stH265PrtclParam.s32MaxPpsNum   = 55;
        stPrtclParam.stH265PrtclParam.s32MaxSliceSegmentNum = 100;
        stPrtclParam.stH265PrtclParam.s32MaxVpsNum = 10;

        s32Ret = HI_MPI_VDEC_SetProtocolParam(VdChn, &stPrtclParam);
        ASSERT(0 == s32Ret);
    }

    if(is_chn_enabled(VdChn))
    {
        if(bNoCreate)
        {
            Debug("RESET VDEC %d", VdChn);
            s32Ret = HI_MPI_VDEC_ResetChn(VdChn);
            ASSERT(0 == s32Ret);
        }
        Debug("START VDEC %d", VdChn);
        s32Ret = HI_MPI_VDEC_StartRecvStream(VdChn);
        ASSERT(0 == s32Ret);
    }
}

static void restart_vdec_chn(VDEC_CHN VdChn, SIZE_S* stSize)
{
    Debug("%d", VdChn);
    stop_vdec_chn(VdChn, FALSE);
    start_vdec_chn(VdChn, stSize, FALSE);
}

static void VDEC_Start(VO_LAYER VoLayer, HI_BOOL bNoCreate)
{
    int i;
    for(i=0; i<VdCnt; i++)
    {
        VDEC_CHN VdChn = i + VoLayer*VdChn;
        start_vdec_chn(VdChn, &stSize, bNoCreate);
    }
}

static void VDEC_Stop(VO_LAYER VoLayer, HI_BOOL bNoDestroy)
{
    int i;
    for(i=0; i<VdCnt; i++)
    {
        VDEC_CHN VdChn = i + VoLayer*VdChn;
        stop_vdec_chn(VdChn, bNoDestroy);
    }
}

static tea_result_t tsk_init(worker_t* worker)
{
    HI_S32 __attribute__((unused)) s32Ret;
    HI_S32 i;

    for(i=0; i<2; i++)
    {
        current_vo_mode[i] = vo_mode[i];
        current_vo_chn[i] = vo_chn[i];
    }

    VB_CONF_S stVbConf;

    // 不可能解 8 路 4K 吧？ -- 先预留 2 路吧。
    // 先调好 1080P 解码，再调 4K 解码。
    memset(&stVbConf, 0, sizeof(VB_CONF_S));
    stVbConf.u32MaxPoolCnt = 4;
    stVbConf.astCommPool[0].u32BlkSize = (3840 * 2160 * 3) >> 1;
    stVbConf.astCommPool[0].u32BlkCnt	 = 2*6;
    stVbConf.astCommPool[1].u32BlkSize = 3840*2160;
    stVbConf.astCommPool[1].u32BlkCnt	 = 2*6;
    stVbConf.astCommPool[2].u32BlkSize = (1920 * 1080 * 3) >> 1;
    stVbConf.astCommPool[2].u32BlkCnt	 = 8*6;
    stVbConf.astCommPool[3].u32BlkSize = 1920*1080;
    stVbConf.astCommPool[3].u32BlkCnt	 = 8*6;

    SAMPLE_COMM_SYS_Init(&stVbConf);

    VB_CONF_S stModVbConf;

    SAMPLE_COMM_VDEC_ModCommPoolConf(&stModVbConf, PT_H265, &stSize, VdCnt);
    s32Ret = SAMPLE_COMM_VDEC_InitModCommVb(&stModVbConf);

    // start VDEC
    VDEC_Start(VoLayer[0], FALSE);

    // start VPSS
    SAMPLE_COMM_VPSS_Start(VoLayer[0], GrpCnt, &stSize, 1, NULL, vo_mode[0], FALSE);

    // start VO
    VO_PUB_ATTR_S stVoPubAttr;

    stVoPubAttr.enIntfSync = VO_OUTPUT_3840x2160_30;
    stVoPubAttr.enIntfType = VO_INTF_HDMI;

    s32Ret = HI_MPI_VO_SetPubAttr(VoDev[0], &stVoPubAttr);
    ASSERT(0 == s32Ret);

    s32Ret = HI_MPI_VO_Enable(VoDev[0]);
    ASSERT(0 == s32Ret);

    s32Ret = SAMPLE_COMM_VO_HdmiStart(stVoPubAttr.enIntfSync);
    ASSERT(0 == s32Ret);

    VO_VIDEO_LAYER_ATTR_S stVoLayerAttr;
    s32Ret = SAMPLE_COMM_VO_GetWH(stVoPubAttr.enIntfSync, \
        &stVoLayerAttr.stDispRect.u32Width, &stVoLayerAttr.stDispRect.u32Height, &stVoLayerAttr.u32DispFrmRt);
    ASSERT(0 == s32Ret);

    stVoLayerAttr.stImageSize.u32Width = stVoLayerAttr.stDispRect.u32Width;
    stVoLayerAttr.stImageSize.u32Height = stVoLayerAttr.stDispRect.u32Height;
    stVoLayerAttr.bClusterMode = HI_FALSE;
    stVoLayerAttr.bDoubleFrame = HI_FALSE;
    stVoLayerAttr.enPixFormat = SAMPLE_PIXEL_FORMAT;

    s32Ret = HI_MPI_VO_SetVideoLayerAttr(VoLayer[0], &stVoLayerAttr);
    ASSERT(0 == s32Ret);

    s32Ret = HI_MPI_VO_EnableVideoLayer(VoLayer[0]);
    ASSERT(0 == s32Ret);

    s32Ret = SAMPLE_COMM_VO_StartChn(VoLayer[0], vo_mode[0], vo_chn[0]);
    ASSERT(0 == s32Ret);

    // VDEC bind VPSS
    for(i=0; i<VdCnt; i++)
    {
        s32Ret = SAMPLE_COMM_VDEC_BindVpss(i, i);
        ASSERT(0 == s32Ret);
    }

    // VPSS bind VO
    for(i=0; i<GrpCnt; i++)
    {
        s32Ret = SAMPLE_COMM_VO_BindVpss(VoLayer[0], i, i, VPSS_CHN0);
        ASSERT(0 == s32Ret);
    }

    task_stream_setopt(worker, 0, stream_opt_rtp, (void*) FALSE);

    return TEA_RSLT_SUCCESS;
}

static void apply_vo_mode(int vo)
{
    int32_t ret;

    ret = SAMPLE_COMM_VO_StopChn(VoLayer[vo], current_vo_mode[vo], current_vo_chn[vo]);
    ASSERT(0 == ret);

    ret = SAMPLE_COMM_VPSS_Stop(VoLayer[vo], GrpCnt, 1, TRUE);
    ASSERT(0 == ret);

    VDEC_Stop(VoLayer[vo], TRUE);
    VDEC_Start(VoLayer[vo], TRUE);

    ret = SAMPLE_COMM_VPSS_Start(VoLayer[vo], GrpCnt, &stSize, 1, NULL, vo_mode[vo], TRUE);
    ASSERT(0 == ret);

    ret = SAMPLE_COMM_VO_StartChn(VoLayer[vo], vo_mode[vo], vo_chn[vo]);
    ASSERT(0 == ret);
}

static tea_result_t tsk_repeat(worker_t* worker)
{
    struct generic_rtp_header* generic_rtp_header;
    enum frame_flag flags;
    stream_frame_t stream_frame;
    timeout_t wait_time = {1, 0};
    int32_t ret;
    int32_t ret_val = TEA_RSLT_SUCCESS;
    VDEC_CHN VdChn;
    int i;

    for(i=0; i<2; i++)
    {
        if(current_vo_mode[i] != vo_mode[i] || current_vo_chn[i] != vo_chn[i])
        {
            ASSERT(0 == i);
            apply_vo_mode(i);
            current_vo_mode[i] = vo_mode[i];
            current_vo_chn[i] = vo_chn[i];
        }
    }

    ret = task_stream_get_frame_3_retry(worker, 0, &stream_frame, &wait_time, &flags, (rtp_hdr_t**) &generic_rtp_header);
    if(RESULT_FAIL(ret))
    {
        return 0;
    }

    if(0 == (flags & frame_flag_rtp)
            || 0 == generic_rtp_header->rtp_hdr.x
            || TEA_GENERIC_PROFILE != ntohs(generic_rtp_header->profile)
            || 0 == (generic_rtp_header->frame.flags & FRAME_FLAG_EXTENSION)
            || generic_rtp_header->extension.stream_index >= VdCnt )
    {
        goto EXIT;
    }

    if(stream_type_h264 != generic_rtp_header->frame.type
            && stream_type_h265 != generic_rtp_header->frame.type)
    {
        goto EXIT;
    }

    VdChn = generic_rtp_header->extension.stream_index;

    if(!is_chn_enabled(VdChn))
    {
        goto EXIT;
    }

    // TODO: 应该等待下一 I 帧到来再开始解码。
    if(PT_H264 == enType[VdChn] && stream_type_h265 == generic_rtp_header->frame.type)
    {
        enType[VdChn] = PT_H265;
        restart_vdec_chn(VdChn, &stSize);
    }
    else if(PT_H265 == enType[VdChn] && stream_type_h264 == generic_rtp_header->frame.type)
    {
        enType[VdChn] = PT_H264;
        restart_vdec_chn(VdChn, &stSize);
    }

    VDEC_STREAM_S stStream;

    stStream.u64PTS       = generic_rtp_header->frame.timestamp;
#if 0
    // debug pts
    static int64_t last_pts=0;
    Debug("%lld-%lld=%lld", stStream.u64PTS, last_pts, stStream.u64PTS-last_pts);
    last_pts = stStream.u64PTS;
#endif
    stStream.pu8Addr      = stream_frame.buf;
    stStream.u32Len       = stream_frame.len;
    stStream.bEndOfFrame  = HI_TRUE;
    stStream.bEndOfStream = HI_FALSE;

    ret = HI_MPI_VDEC_SendStream(VdChn, &stStream, 1000);
    if(0 != ret)
    {
        Debug("%x", ret);
    }

//    VDEC_CHN_STAT_S status;
//    ret = HI_MPI_VDEC_Query(VdChn, &status);
//    if(0 == ret)
//    {
//    }

EXIT:
    ret = task_stream_release_frame(worker, 0);

    return ret_val;
}

static tea_result_t tsk_cleanup(worker_t* worker)
{
    HI_S32  i;
    HI_S32 s32Ret;

    for(i=0; i<GrpCnt; i++)
    {
        MPP_CHN_S stSrcChn;
        MPP_CHN_S stDestChn;

        stSrcChn.enModId = HI_ID_VPSS;
        stSrcChn.s32DevId = i;
        stSrcChn.s32ChnId = VPSS_CHN0;

        stDestChn.enModId = HI_ID_VOU;
        stDestChn.s32DevId = VoLayer[0];
        stDestChn.s32ChnId = i;

        s32Ret = HI_MPI_SYS_UnBind(&stSrcChn, &stDestChn);
        ASSERT(0 == s32Ret);
    }

    for(i=0; i<GrpCnt; i++)
    {
        MPP_CHN_S stSrcChn;
        MPP_CHN_S stDestChn;

        stSrcChn.enModId = HI_ID_VDEC;
        stSrcChn.s32DevId = 0;
        stSrcChn.s32ChnId = i;

        stDestChn.enModId = HI_ID_VPSS;
        stDestChn.s32DevId = i;
        stDestChn.s32ChnId = 0;

        s32Ret = HI_MPI_SYS_UnBind(&stSrcChn, &stDestChn);
        ASSERT(0 == s32Ret);
    }

    SAMPLE_COMM_VO_StopChn(VoLayer[0], current_vo_mode[0], current_vo_chn[0]);
    SAMPLE_COMM_VO_StopLayer(VoLayer[0]);
    SAMPLE_COMM_VO_HdmiStop();
    SAMPLE_COMM_VO_StopDev(VoDev[0]);
    SAMPLE_COMM_VPSS_Stop(VoLayer[0], GrpCnt, 1, FALSE);
    SAMPLE_COMM_VDEC_Stop(VdCnt);
    SAMPLE_COMM_SYS_Exit();

    return TEA_RSLT_SUCCESS;
}

static tea_result_t create(struct N_node* nn)
{
    int i;
    int r;

    for(i=0; i<DEC_CHN_NUM; i++)
    {
        enType[i] = PT_H265;
    }

    struct N_node* nn_vo;
    for(i=0; i<2; i++)
    {
        r = xN_goto_3(nn, &nn_vo, "vo[%d]", i+1);
        ASSERT(0 == r);

        r = xN_bind_int_variable_2(nn_vo, "mode", &vo_mode[i]);
        ASSERT(0 == r);

        r = xN_bind_int_variable_2(nn_vo, "channel", &vo_chn[i]);
        ASSERT(0 == r);
    }

    return TEA_RSLT_SUCCESS;
}

static tea_result_t destroy(struct N_node *nn)
{
    return TEA_RSLT_SUCCESS;
}

static tea_result_t apply(struct N_node* nn)
{
    return TEA_RSLT_SUCCESS;
}

static task_func_t repeat_table[] = {tsk_repeat, NULL};

static struct task_logic logic =
{
init:
    tsk_init,
repeat:
    repeat_table,
cleanup:
    tsk_cleanup
};

tea_app_t hi3536_vdec =
{
version:
    TEA_VERSION(0,5),
create:
    create,
destroy:
    destroy,
apply:
    apply,
task:
    &logic
};

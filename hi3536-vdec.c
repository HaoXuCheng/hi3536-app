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

// internal state
static int32_t VdCnt = 4;
static int32_t GrpCnt = 4;
static VO_DEV VoDev = 0;
static VO_LAYER VoLayer = 0;
// input

// monitor: statistics

static tea_result_t tsk_init(worker_t* worker)
{
    HI_S32 __attribute__((unused)) s32Ret = HI_FAILURE;
    HI_S32 i;

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

    SIZE_S stSize = {1920, 1080};
    VB_CONF_S stModVbConf;

    SAMPLE_COMM_VDEC_ModCommPoolConf(&stModVbConf, PT_H265, &stSize, VdCnt);
    s32Ret = SAMPLE_COMM_VDEC_InitModCommVb(&stModVbConf);

    PAYLOAD_TYPE_E enType = PT_H265;
    VDEC_CHN_ATTR_S stVdecChnAttr;

    for(i=0; i<VdCnt; i++)
    {
        stVdecChnAttr.enType       = enType;
        stVdecChnAttr.u32BufSize   = stSize.u32Width * stSize.u32Height;
        stVdecChnAttr.u32Priority  = 5;
        stVdecChnAttr.u32PicWidth  = stSize.u32Width;
        stVdecChnAttr.u32PicHeight = stSize.u32Height;
        stVdecChnAttr.stVdecVideoAttr.enMode = VIDEO_MODE_FRAME;
        stVdecChnAttr.stVdecVideoAttr.u32RefFrameNum = 3;
        stVdecChnAttr.stVdecVideoAttr.bTemporalMvpEnable = 1;

        s32Ret = HI_MPI_VDEC_CreateChn(i, &stVdecChnAttr);
        ASSERT(0 == s32Ret);

        s32Ret = HI_MPI_VDEC_StartRecvStream(i);
        ASSERT(0 == s32Ret);

        s32Ret = HI_MPI_VDEC_StopRecvStream(i);
        ASSERT(0 == s32Ret);

        VDEC_PRTCL_PARAM_S stPrtclParam;

        HI_MPI_VDEC_GetProtocolParam(i, &stPrtclParam);
        stPrtclParam.enType = enType;
        stPrtclParam.stH265PrtclParam.s32MaxSpsNum   = 2;
        stPrtclParam.stH265PrtclParam.s32MaxPpsNum   = 55;
        stPrtclParam.stH265PrtclParam.s32MaxSliceSegmentNum = 100;
        stPrtclParam.stH265PrtclParam.s32MaxVpsNum = 10;

        s32Ret = HI_MPI_VDEC_SetProtocolParam(i, &stPrtclParam);
        ASSERT(0 == s32Ret);

        s32Ret = HI_MPI_VDEC_StartRecvStream(i);
        ASSERT(0 == s32Ret);
    }

    // start VPSS

    VPSS_GRP_ATTR_S stVpssGrpAttr = {0};

    stVpssGrpAttr.enDieMode = VPSS_DIE_MODE_NODIE;
    stVpssGrpAttr.bIeEn     = HI_FALSE;
    stVpssGrpAttr.bDciEn    = HI_TRUE;
    stVpssGrpAttr.bNrEn     = HI_TRUE;
    stVpssGrpAttr.bHistEn   = HI_FALSE;
    stVpssGrpAttr.enPixFmt  = PIXEL_FORMAT_YUV_SEMIPLANAR_420;
    stVpssGrpAttr.u32MaxW   = ALIGN_UP(stSize.u32Width,  16);
    stVpssGrpAttr.u32MaxH   = ALIGN_UP(stSize.u32Height, 16);

    SAMPLE_COMM_VPSS_Start(GrpCnt, &stSize, 1, &stVpssGrpAttr);

    // start VO
    VO_PUB_ATTR_S stVoPubAttr;

    stVoPubAttr.enIntfSync = VO_OUTPUT_3840x2160_30;
    stVoPubAttr.enIntfType = VO_INTF_HDMI;

    s32Ret = HI_MPI_VO_SetPubAttr(VoDev, &stVoPubAttr);
    ASSERT(0 == s32Ret);

    s32Ret = HI_MPI_VO_Enable(VoDev);
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

    s32Ret = HI_MPI_VO_SetVideoLayerAttr(VoLayer, &stVoLayerAttr);
    ASSERT(0 == s32Ret);

    s32Ret = HI_MPI_VO_EnableVideoLayer(VoLayer);
    ASSERT(0 == s32Ret);

    s32Ret = SAMPLE_COMM_VO_StartChn(VoLayer, VO_MODE_4MUX);
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
        s32Ret = SAMPLE_COMM_VO_BindVpss(VoLayer, i, i, VPSS_CHN0);
        ASSERT(0 == s32Ret);
    }

    task_stream_setopt(worker, 0, stream_opt_rtp, (void*) FALSE);

    return TEA_RSLT_SUCCESS;
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

    VDEC_CHN_STAT_S status;
    ret = HI_MPI_VDEC_Query(VdChn, &status);
    if(0 == ret)
    {
    }

EXIT:
    ret = task_stream_release_frame(worker, 0);

    return ret_val;
}

static tea_result_t tsk_cleanup(worker_t* worker)
{
    HI_S32  i;
    HI_S32 s32Ret;

    for(i=0; i<VdCnt; i++)
    {
        HI_MPI_VDEC_StopRecvStream(i);
    }

    for(i=0; i<GrpCnt; i++)
    {
        MPP_CHN_S stSrcChn;
        MPP_CHN_S stDestChn;

        stSrcChn.enModId = HI_ID_VPSS;
        stSrcChn.s32DevId = i;
        stSrcChn.s32ChnId = VPSS_CHN0;

        stDestChn.enModId = HI_ID_VOU;
        stDestChn.s32DevId = VoLayer;
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

    SAMPLE_COMM_VO_StopChn(VoLayer, VO_MODE_4MUX);
    SAMPLE_COMM_VO_StopLayer(VoLayer);
    SAMPLE_COMM_VO_HdmiStop();
    SAMPLE_COMM_VO_StopDev(VoDev);
    SAMPLE_COMM_VPSS_Stop(GrpCnt, VPSS_CHN0);
    SAMPLE_COMM_VDEC_Stop(VdCnt);
    SAMPLE_COMM_SYS_Exit();

    return TEA_RSLT_SUCCESS;
}

static tea_result_t init(void)
{
    return TEA_RSLT_SUCCESS;
}

static tea_result_t fini(void)
{
    return TEA_RSLT_SUCCESS;
}

static tea_result_t create(struct N_node* nn)
{
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
init:
    init,
fini:
    fini,
create:
    create,
destroy:
    destroy,
apply:
    apply,
task:
    &logic
};

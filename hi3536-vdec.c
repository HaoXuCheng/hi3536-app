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
//        每个视频层包含 4 个视频输出通道，每个视频层的视频输出通道编号都是 0~3。参考[1]。
// 参考
// 1 Hi3536 V100R001C02SPC060/01.software/board/Hi3536_SDK_V2.0.6.0/mpp_single/sample/common/sample_comm_vo.c
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
#include <sys/stat.h>

// environment

// settings
// 2 路视频输出（VO）：0-HDMI、1-BT1120。
static int vo_mode[2];
// 单窗口模式时使能的通道。
// 会要求不使能任何通道吗？
static int vo_chn[2]; // 取值 0~3
static int IntfType[2];

// TODO: 支持 4K，一路？二路？
static SIZE_S stSize = {1920, 1080};

// internal state
#define CHN_NUM 4       // 每块屏幕最多显示的窗口数量。
#define CHN_NUM_TOTAL   (2*CHN_NUM)
static PAYLOAD_TYPE_E enType[CHN_NUM_TOTAL];
static MUTEX_T lock;
static int check_control;   // 控制 tsk_dec 线程检查时间间隔，避免检查过于频繁。
static struct timespec prev[CHN_NUM_TOTAL]; // 解码通道上次成功接收输入码流的时刻。
static int user_pic_control[CHN_NUM_TOTAL]; // 控制字：控制通道是否输出用户图片。
static int user_pic_state[CHN_NUM_TOTAL];   // 状态字：表示通道当前是否正在输出用户图片。
static int current_vo_mode[2];
static int current_vo_chn[2];

// input

// monitor: statistics

#define USER_PIC_WIDTH  960     // 根据参考代码必须为 16 整数倍。
#define USER_PIC_HEIGHT 544     // 根据参考代码必须为 16 整数倍。
#define USER_PIC_SIZE          (USER_PIC_WIDTH*USER_PIC_HEIGHT)
#define USER_PIC_BUFFER_SIZE   (USER_PIC_WIDTH*USER_PIC_HEIGHT*3/2)

static VIDEO_FRAME_INFO_S stUsrPicInfo;
static VB_BLK u32BlkHandle;
static VB_POOL u32PoolId;
static int no_stream_mode = 0; // 0-不处理，相当于输出静帧或者黑屏；1-输出用户图片。

static char* get_image_file(void)
{
    int r;
    int i;
    char* fpath = NULL;

    for(i=1; ;i++)
    {
        r = xT_read_nolock_3(app_nn_root, &fpath, "hi3536_vdec/no_stream/image[%d]", i);
        if(0 == r)
        {
            struct stat buf;
            r = stat(fpath, &buf);
            if(0 == r)
            {
                Debug("%s", fpath);
                return fpath;
            }
        }
        else
        {
            break;
        }
    }
    ASSERT(0);
    return NULL;
}

static HI_VOID VDEC_PREPARE_USERPIC(VIDEO_FRAME_INFO_S *pstUsrPicInfo)
{
    HI_U32   u32PhyAddr;
    HI_VOID  *pVirAddr;
    HI_U32 s32Ret = HI_SUCCESS;

    u32BlkHandle = HI_MPI_VB_GetBlock(4, USER_PIC_BUFFER_SIZE, "anonymous");
    if (VB_INVALID_HANDLE == u32BlkHandle || HI_ERR_VB_ILLEGAL_PARAM == u32BlkHandle)
    {
        ASSERT(0);
        return;
    }
    u32PhyAddr = HI_MPI_VB_Handle2PhysAddr(u32BlkHandle);
    u32PoolId = HI_MPI_VB_Handle2PoolId(u32BlkHandle);
    ASSERT(u32PoolId=4);

    HI_MPI_VB_MmapPool(u32PoolId);
    s32Ret = HI_MPI_VB_GetBlkVirAddr(u32PoolId, u32PhyAddr, &pVirAddr);
    if(s32Ret != HI_SUCCESS)
    {
        ASSERT(0);
        HI_MPI_VB_ReleaseBlock(u32BlkHandle);
        HI_MPI_VB_MunmapPool(u32PoolId);
        return;
    }

    // 图片必须为 960X544 YUV420SP 格式。
    // ffmpeg -i nostream.png -pix_fmt nv21 nostream.yuv
    char* fname = get_image_file();
    if(NULL != fname)
    {
        fread_p((void*) pVirAddr, 1, USER_PIC_BUFFER_SIZE, fname);
    }

    pstUsrPicInfo->u32PoolId = u32PoolId;
    pstUsrPicInfo->stVFrame.enCompressMode = COMPRESS_MODE_NONE;
    pstUsrPicInfo->stVFrame.enVideoFormat = VIDEO_FORMAT_LINEAR;
    pstUsrPicInfo->stVFrame.enPixelFormat = PIXEL_FORMAT_YUV_SEMIPLANAR_420;
    pstUsrPicInfo->stVFrame.u32Width = USER_PIC_WIDTH;
    pstUsrPicInfo->stVFrame.u32Height = USER_PIC_HEIGHT;
    pstUsrPicInfo->stVFrame.u32Field = VIDEO_FIELD_FRAME;
    pstUsrPicInfo->stVFrame.u32PhyAddr[0] = u32PhyAddr;
    pstUsrPicInfo->stVFrame.u32PhyAddr[1] = u32PhyAddr + USER_PIC_SIZE;
    pstUsrPicInfo->stVFrame.u32Stride[0] = USER_PIC_WIDTH;
    pstUsrPicInfo->stVFrame.u32Stride[1] = USER_PIC_WIDTH;
    pstUsrPicInfo->stVFrame.u64pts = 0;
}

static HI_VOID VDEC_RELEASE_USERPIC()
{
    HI_S32 s32Ret = HI_SUCCESS;

    s32Ret = HI_MPI_VB_ReleaseBlock(u32BlkHandle);
    ASSERT(0 == s32Ret);

    s32Ret = HI_MPI_VB_MunmapPool(u32PoolId);
    ASSERT(0 == s32Ret);

    (void) s32Ret;
}

// 约定：每个 VO 对应 4 通道。
HI_BOOL is_chn_enabled(int vo, int chn)
{
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

    s32Ret = HI_MPI_VDEC_DisableUserPic(VdChn);
    ASSERT(0 == s32Ret);

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

        // 设置显示模式为 PREVIEW 实现实时预览。P10-3。
        // 默认为 PLAYBACK 模式，查看 /proc/umap/vdec 时发现 FrmInVdec 高达 309 !
        s32Ret = HI_MPI_VDEC_SetDisplayMode(VdChn, VIDEO_DISPLAY_MODE_PREVIEW);
        ASSERT(0 == s32Ret);

        s32Ret = HI_MPI_VDEC_SetUserPic(VdChn, &stUsrPicInfo);
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

// 即使解码通道不显示，仍继续解码，以尽量避免在切换显示不同解码通道时输出黑屏。
//    if(is_chn_enabled(VdChn/CHN_NUM, VdChn%CHN_NUM))
    {
//        if(bNoCreate)
//        {
//            Debug("RESET VDEC %d", VdChn);
//            s32Ret = HI_MPI_VDEC_ResetChn(VdChn);
//            ASSERT(0 == s32Ret);
//        }

        Debug("START VDEC %d", VdChn);

        s32Ret = HI_MPI_VDEC_StartRecvStream(VdChn);
        ASSERT(0 == s32Ret);

        user_pic_state[VdChn] = FALSE;
        check_control = TRUE;
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
    for(i=0; i<CHN_NUM; i++)
    {
        VDEC_CHN VdChn = i + VoLayer*CHN_NUM;
        start_vdec_chn(VdChn, &stSize, bNoCreate);
    }
}

static void VDEC_Stop(VO_LAYER VoLayer, HI_BOOL bNoDestroy)
{
    int i;
    for(i=0; i<CHN_NUM; i++)
    {
        VDEC_CHN VdChn = i + VoLayer*CHN_NUM;
        stop_vdec_chn(VdChn, bNoDestroy);
    }
}

static tea_result_t tsk_init(worker_t* worker)
{
    HI_S32 __attribute__((unused)) s32Ret;
    HI_S32 i;
    HI_S32 j;

    VB_CONF_S stVbConf;

    // 不可能解 8 路 4K 吧？ -- 先预留 2 路吧。
    // 先调好 1080P 解码，再调 4K 解码。
    memset(&stVbConf, 0, sizeof(VB_CONF_S));
    stVbConf.u32MaxPoolCnt = 5;
    stVbConf.astCommPool[0].u32BlkSize = (3840 * 2160 * 3) >> 1;
    stVbConf.astCommPool[0].u32BlkCnt  = 2*6;
    stVbConf.astCommPool[1].u32BlkSize = 3840*2160;
    stVbConf.astCommPool[1].u32BlkCnt  = 2*6;
    stVbConf.astCommPool[2].u32BlkSize = (1920 * 1080 * 3) >> 1;
    stVbConf.astCommPool[2].u32BlkCnt  = 6*CHN_NUM_TOTAL;
    stVbConf.astCommPool[3].u32BlkSize = 1920*1080;
    stVbConf.astCommPool[3].u32BlkCnt  = 6*CHN_NUM_TOTAL;
    stVbConf.astCommPool[4].u32BlkSize = USER_PIC_BUFFER_SIZE;
    stVbConf.astCommPool[4].u32BlkCnt  = 1;
    SAMPLE_COMM_SYS_Init(&stVbConf);

    VB_CONF_S stModVbConf;

    SAMPLE_COMM_VDEC_ModCommPoolConf(&stModVbConf, PT_H265, &stSize, CHN_NUM_TOTAL);
    s32Ret = SAMPLE_COMM_VDEC_InitModCommVb(&stModVbConf);

    VDEC_PREPARE_USERPIC(&stUsrPicInfo);

    int BgColor;
    s32Ret = xT_read_int_3(worker->nn_inst, &BgColor, "BgColor");
    ASSERT(0 == s32Ret);

    for(i=0; i<2; i++)
    {
        current_vo_mode[i] = vo_mode[i];
        current_vo_chn[i] = vo_chn[i];

        // start VDEC
        VDEC_Start(i, FALSE);

        // start VPSS
        SAMPLE_COMM_VPSS_Start(i, CHN_NUM, &stSize, 1, NULL, vo_mode[i], FALSE);

        // start VO
        VO_PUB_ATTR_S stVoPubAttr;
        int IntfSync;

        s32Ret = xT_read_int_3(worker->nn_inst, &IntfSync, "vo[%d]/IntfSync", i+1);
        ASSERT(0 == s32Ret);

        s32Ret = xT_read_int_3(worker->nn_inst, IntfType+i, "vo[%d]/IntfType", i+1);
        ASSERT(0 == s32Ret);

        stVoPubAttr.enIntfSync = IntfSync;
        stVoPubAttr.enIntfType = IntfType[i];
        stVoPubAttr.u32BgColor = BgColor;

        s32Ret = HI_MPI_VO_SetPubAttr(i, &stVoPubAttr);
        ASSERT(0 == s32Ret);

        s32Ret = HI_MPI_VO_Enable(i);
        ASSERT(0 == s32Ret);

        if(IntfType[i] == VO_INTF_HDMI)
        {
            s32Ret = SAMPLE_COMM_VO_HdmiStart(stVoPubAttr.enIntfSync);
            ASSERT(0 == s32Ret);
        }

        VO_VIDEO_LAYER_ATTR_S stVoLayerAttr;
        s32Ret = SAMPLE_COMM_VO_GetWH(stVoPubAttr.enIntfSync, \
                                      &stVoLayerAttr.stDispRect.u32Width, &stVoLayerAttr.stDispRect.u32Height, &stVoLayerAttr.u32DispFrmRt);
        ASSERT(0 == s32Ret);

        stVoLayerAttr.stImageSize.u32Width = stVoLayerAttr.stDispRect.u32Width;
        stVoLayerAttr.stImageSize.u32Height = stVoLayerAttr.stDispRect.u32Height;
        stVoLayerAttr.bClusterMode = HI_FALSE;
        stVoLayerAttr.bDoubleFrame = HI_FALSE;
        stVoLayerAttr.enPixFormat = SAMPLE_PIXEL_FORMAT;

        s32Ret = HI_MPI_VO_SetVideoLayerAttr(i, &stVoLayerAttr);
        ASSERT(0 == s32Ret);

        s32Ret = HI_MPI_VO_EnableVideoLayer(i);
        ASSERT(0 == s32Ret);

        s32Ret = SAMPLE_COMM_VO_StartChn(i, vo_mode[i], vo_chn[i]);
        ASSERT(0 == s32Ret);
    }

    // VDEC bind VPSS
    for(i=0; i<CHN_NUM_TOTAL; i++)
    {
        s32Ret = SAMPLE_COMM_VDEC_BindVpss(i, i);
        ASSERT(0 == s32Ret);
    }

    // VPSS bind VO
    for(i=0; i<2; i++)
    {
        for(j=0; j<CHN_NUM; j++)
        {
            s32Ret = SAMPLE_COMM_VO_BindVpss(i, j, j+i*CHN_NUM, VPSS_CHN0);
            ASSERT(0 == s32Ret);
        }
    }

    task_stream_setopt(worker, 0, stream_opt_rtp, (void*) FALSE);

    for(i=0; i<CHN_NUM_TOTAL; i++)
    {
        prev[i].tv_sec = 0;
        prev[i].tv_nsec = 0;
        user_pic_control[i] = FALSE;
        user_pic_state[i] = FALSE;
    }
    check_control = FALSE;

    return TEA_RSLT_SUCCESS;
}

static void apply_vo_mode(int vo)
{
    int32_t __attribute__((unused)) ret;

    ret = SAMPLE_COMM_VO_StopChn(vo, current_vo_mode[vo], current_vo_chn[vo]);
    ASSERT(0 == ret);

    ret = SAMPLE_COMM_VPSS_Stop(vo, CHN_NUM, 1, TRUE);
    ASSERT(0 == ret);

    VDEC_Stop(vo, TRUE);
    VDEC_Start(vo, TRUE);

    ret = SAMPLE_COMM_VPSS_Start(vo, CHN_NUM, &stSize, 1, NULL, vo_mode[vo], TRUE);
    ASSERT(0 == ret);

    ret = SAMPLE_COMM_VO_StartChn(vo, vo_mode[vo], vo_chn[vo]);
    ASSERT(0 == ret);
}

static tea_result_t tsk_dec(worker_t* worker)
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
        goto EXIT_2;
    }

    if(0 == (flags & frame_flag_rtp)
            || 0 == generic_rtp_header->rtp_hdr.x
            || TEA_GENERIC_PROFILE != ntohs(generic_rtp_header->profile)
            || 0 == (generic_rtp_header->frame.flags & FRAME_FLAG_EXTENSION)
            || generic_rtp_header->extension.stream_index >= CHN_NUM_TOTAL )
    {
        goto EXIT;
    }

    if(stream_type_h264 != generic_rtp_header->frame.type
            && stream_type_h265 != generic_rtp_header->frame.type)
    {
        goto EXIT;
    }

    VdChn = generic_rtp_header->extension.stream_index;

//    if(!is_chn_enabled(VdChn/CHN_NUM, VdChn%CHN_NUM))
//    {
//        goto EXIT;
//    }

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

    LOCK(&lock);
    clock_gettime(CLOCK_MONOTONIC, &prev[VdChn]);
    user_pic_control[VdChn] = FALSE;
    // 获取码流输入成功之后，立即结束输出用户图片。
    if(user_pic_state[VdChn])
    {
        ret = HI_MPI_VDEC_DisableUserPic(VdChn);
        ASSERT(0 == ret);

        ret = HI_MPI_VDEC_StartRecvStream(VdChn);
        ASSERT(0 == ret);

        user_pic_state[VdChn] = FALSE;

        // 停止输出用户图片
        Debug("Chn %d: Stop output user pic", VdChn);
    }
    UNLOCK(&lock);

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
        Debug("Chn %d: %x", VdChn, ret);
    }

//    VDEC_CHN_STAT_S status;
//    ret = HI_MPI_VDEC_Query(VdChn, &status);
//    if(0 == ret)
//    {
//    }

EXIT:
    ret = task_stream_release_frame(worker, 0);

EXIT_2:
    LOCK(&lock);
    if(check_control)
    {
        for(i=0; i<CHN_NUM_TOTAL; i++)
        {
            if(user_pic_control[i] && !user_pic_state[i])
            {
                ret = HI_MPI_VDEC_StopRecvStream(i);
                ASSERT(0 == ret);

                // 延迟插入。
                ret = HI_MPI_VDEC_EnableUserPic(i, 0);
                ASSERT(0 == ret);

                user_pic_state[i] = TRUE;

                // 开始输出用户图片
                Debug("Chn %d: Start output user pic", i);
            }
        }
        check_control = FALSE;
    }
    UNLOCK(&lock);

    return ret_val;
}

static tea_result_t tsk_chk(worker_t* worker)
{
    struct timespec now,elapse;
    int i;

    SLEEP(1,0);

    clock_gettime(CLOCK_MONOTONIC, &now);
    LOCK(&lock);
    for(i=0; i<CHN_NUM_TOTAL; i++)
    {
        timeoutsub(&now, &prev[i], &elapse);
        if(elapse.tv_sec > 3    // 应该是多少合适？
             && !user_pic_control[i]
             && 1 == no_stream_mode)
        {
            user_pic_control[i] = TRUE;
            Debug("Ch %d: Enable User Picture", i);
        }
    }
    check_control = TRUE;
    UNLOCK(&lock);

    return TEA_RSLT_SUCCESS;
}

static tea_result_t tsk_cleanup(worker_t* worker)
{
    HI_S32  i;
    HI_S32  j;
    HI_S32 __attribute__((unused)) s32Ret;

    for(i=0; i<2; i++)
    {
        for(j=0; j<CHN_NUM; j++)
        {
            MPP_CHN_S stSrcChn;
            MPP_CHN_S stDestChn;

            stSrcChn.enModId = HI_ID_VPSS;
            stSrcChn.s32DevId = j+i*CHN_NUM;
            stSrcChn.s32ChnId = VPSS_CHN0;

            stDestChn.enModId = HI_ID_VOU;
            stDestChn.s32DevId = i;
            stDestChn.s32ChnId = j;

            s32Ret = HI_MPI_SYS_UnBind(&stSrcChn, &stDestChn);
            ASSERT(0 == s32Ret);
        }

        for(j=0; j<CHN_NUM; j++)
        {
            MPP_CHN_S stSrcChn;
            MPP_CHN_S stDestChn;

            stSrcChn.enModId = HI_ID_VDEC;
            stSrcChn.s32DevId = 0;
            stSrcChn.s32ChnId = j+i*CHN_NUM;

            stDestChn.enModId = HI_ID_VPSS;
            stDestChn.s32DevId = j+i*CHN_NUM;
            stDestChn.s32ChnId = 0;

            s32Ret = HI_MPI_SYS_UnBind(&stSrcChn, &stDestChn);
            ASSERT(0 == s32Ret);
        }

        SAMPLE_COMM_VO_StopChn(i, current_vo_mode[i], current_vo_chn[i]);
        SAMPLE_COMM_VO_StopLayer(i);
        if(IntfType[i] == VO_INTF_HDMI)
        {
            SAMPLE_COMM_VO_HdmiStop();
        }
        SAMPLE_COMM_VO_StopDev(i);
        SAMPLE_COMM_VPSS_Stop(i, CHN_NUM, 1, FALSE);
    }

    SAMPLE_COMM_VDEC_Stop(CHN_NUM_TOTAL);
    VDEC_RELEASE_USERPIC();
    SAMPLE_COMM_SYS_Exit();

    return TEA_RSLT_SUCCESS;
}

static tea_result_t create(struct N_node* nn)
{
    int i;
    int __attribute__((unused)) r;

    for(i=0; i<CHN_NUM_TOTAL; i++)
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

    INIT_LOCK(&lock);

    r = xN_bind_int_variable_2(nn, "no_stream/mode", &no_stream_mode);
    ASSERT(0 == r);

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

static task_func_t repeat_table[] = {tsk_dec, tsk_chk, NULL};

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

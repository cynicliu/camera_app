#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "sample_comm.h"
#include "rk_mpi_ai.h"
#include "rk_mpi_aenc.h"
#include "rk_mpi_amix.h"
#include "rk_mpi_ao.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_vo.h"
#include "rk_mpi_venc.h"
#include "rkmuxer.h"
#include "lvgl/lvgl.h"
#include "camera_ui.h"
#include "media_callbacks.h"

#define SCREEN_W 720
#define SCREEN_H 720
#define VIDEO_W 720
#define VIDEO_H 406
#define VIDEO_Y 107
#define BAR_Y 620
#define BAR_H 100
#define UI_CHN 1
#define AUDIO_RATE 16000
#define AUDIO_SAMPLES 256
#define VIDEO_FPS 25
#define VIDEO_BITRATE_KBPS 2048
#define AUDIO_BITRATE 32000
#define MUXER_ID 0

typedef struct {
    SAMPLE_VI_CTX_S vi;
    SAMPLE_VPSS_CTX_S vpss;
    SAMPLE_VO_CTX_S vo;
    SAMPLE_AI_CTX_S ai;
    SAMPLE_AO_CTX_S ao;
    SAMPLE_VENC_CTX_S venc;
    SAMPLE_AENC_CTX_S aenc;
    MPP_CHN_S vi_src;
    MPP_CHN_S vpss_dst;
    MPP_CHN_S vpss_src;
    MPP_CHN_S vo_dst;
    MPP_CHN_S venc_dst;
    pthread_t audio_thread;
    media_callbacks_t stream_callbacks;
    pthread_mutex_t lock;
    pthread_mutex_t muxer_lock;
    bool running;
    bool audio_enabled;
    bool quit;
    bool isp_ready;
    bool mpi_ready;
    bool vi_ready;
    bool vpss_ready;
    bool vo_ready;
    bool ui_ready;
    bool ai_ready;
    bool ao_ready;
    bool audio_thread_ready;
    bool venc_ready;
    bool aenc_ready;
    bool venc_bound;
    bool muxer_ready;
    bool stream_callbacks_ready;
    bool video_bound;
    MB_BLK ui_mb;
    VIDEO_FRAME_INFO_S ui_frame;
    camera_ui_t ui;
} APP_CTX;

static volatile sig_atomic_t g_signal_stop;
static APP_CTX *g_app;
static int g_touch_fd = -1;
static int g_touch_x;
static int g_touch_y;
static int g_touch_max_x = SCREEN_W - 1;
static int g_touch_max_y = SCREEN_H - 1;
static bool g_touch_down;
static lv_disp_draw_buf_t g_lv_draw_buf;
static lv_color_t g_lv_pixels[SCREEN_W * 20];

static void signal_handler(int sig) {
    (void)sig;
    g_signal_stop = 1;
}

static void lvgl_flush(lv_disp_drv_t *drv, const lv_area_t *area,
                       lv_color_t *colors) {
    uint8_t *rgb = RK_MPI_MB_Handle2VirAddr(g_app->ui_mb);
    int x, y;
    (void)drv;
    for (y = area->y1; y <= area->y2; ++y) {
        for (x = area->x1; x <= area->x2; ++x) {
            lv_color_t c = *colors++;
            uint8_t *p = rgb + ((size_t)y * SCREEN_W + x) * 3;
            p[0] = (uint8_t)(c.ch.red * 255 / 31);
            p[1] = (uint8_t)(c.ch.green * 255 / 63);
            p[2] = (uint8_t)(c.ch.blue * 255 / 31);
        }
    }
    if (lv_disp_flush_is_last(drv)) {
        RK_MPI_SYS_MmzFlushCache(g_app->ui_mb, RK_FALSE);
        RK_MPI_VO_SendFrame(g_app->vo.s32LayerId, UI_CHN,
                            &g_app->ui_frame, 1000);
    }
    lv_disp_flush_ready(drv);
}

static void lvgl_touch_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    (void)drv;
    data->point.x = g_touch_x * (SCREEN_W - 1) / g_touch_max_x;
    data->point.y = g_touch_y * (SCREEN_H - 1) / g_touch_max_y - BAR_Y;
    if (data->point.y < 0) data->point.y = 0;
    if (data->point.y >= BAR_H) data->point.y = BAR_H - 1;
    data->state = g_touch_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static RK_S32 set_video_visible(APP_CTX *app, bool visible) {
    RK_S32 ret = RK_SUCCESS;
    if (visible && !app->video_bound) {
        ret = SAMPLE_COMM_Bind(&app->vpss_src, &app->vo_dst);
        if (ret == RK_SUCCESS) app->video_bound = true;
    } else if (!visible && app->video_bound) {
        ret = SAMPLE_COMM_UnBind(&app->vpss_src, &app->vo_dst);
        if (ret == RK_SUCCESS) app->video_bound = false;
    }
    return ret;
}

static void *audio_loop(void *opaque) {
    APP_CTX *app = opaque;
    while (!app->quit && !g_signal_stop) {
        AUDIO_FRAME_S frame;
        bool play, stream;
        RK_S32 ret = RK_MPI_AI_GetFrame(0, 0, &frame, NULL, 200);
        if (ret != RK_SUCCESS) continue;
        pthread_mutex_lock(&app->lock);
        play = app->running && app->audio_enabled;
        stream = app->running && app->muxer_ready;
        pthread_mutex_unlock(&app->lock);
        if (play) {
            frame.bBypassMbBlk = RK_FALSE;
            ret = RK_MPI_AO_SendFrame(0, 0, &frame, 200);
            if (ret != RK_SUCCESS)
                fprintf(stderr, "audio output dropped: %#x\n", ret);
        }
        if (stream) {
            frame.bBypassMbBlk = RK_FALSE;
            ret = RK_MPI_AENC_SendFrame(app->aenc.s32ChnId, &frame, NULL, 200);
            if (ret != RK_SUCCESS)
                fprintf(stderr, "AAC input dropped: %#x\n", ret);
        }
        RK_MPI_AI_ReleaseFrame(0, 0, &frame, NULL);
    }
    return NULL;
}

static bool is_h265_keyframe(const VENC_PACK_S *pack) {
    return pack->DataType.enH265EType == H265E_NALU_ISLICE ||
           pack->DataType.enH265EType == H265E_NALU_IDRSLICE;
}

static void rtmp_video_callback(const VENC_STREAM_S *stream, void *userdata) {
    APP_CTX *app = userdata;
    const VENC_PACK_S *pack = stream->pstPack;
    RK_S32 ret;
    pthread_mutex_lock(&app->lock);
    bool write_frame = app->running && app->muxer_ready;
    pthread_mutex_unlock(&app->lock);
    if (!write_frame) return;
    unsigned char *data = RK_MPI_MB_Handle2VirAddr(pack->pMbBlk);
    pthread_mutex_lock(&app->muxer_lock);
    ret = rkmuxer_write_video_frame(MUXER_ID, data, pack->u32Len,
                                     pack->u64PTS, is_h265_keyframe(pack));
    pthread_mutex_unlock(&app->muxer_lock);
    if (ret != 0) fprintf(stderr, "RTMP video write failed: %d\n", ret);
}

static void rtmp_audio_callback(const AUDIO_STREAM_S *stream, void *userdata) {
    APP_CTX *app = userdata;
    const unsigned int adts_header_size = 7;
    RK_S32 ret;
    pthread_mutex_lock(&app->lock);
    bool write_frame = app->running && app->muxer_ready;
    pthread_mutex_unlock(&app->lock);
    if (!write_frame || stream->u32Len <= adts_header_size) return;
    unsigned char *data = RK_MPI_MB_Handle2VirAddr(stream->pMbBlk);
    pthread_mutex_lock(&app->muxer_lock);
    ret = rkmuxer_write_audio_frame(MUXER_ID, data + adts_header_size,
                                     stream->u32Len - adts_header_size,
                                     stream->u64TimeStamp);
    pthread_mutex_unlock(&app->muxer_lock);
    if (ret != 0) fprintf(stderr, "RTMP audio write failed: %d\n", ret);
}

static RK_S32 init_audio(APP_CTX *app) {
    AIO_ATTR_S *attr;
    memset(&app->ai, 0, sizeof(app->ai));
    memset(&app->ao, 0, sizeof(app->ao));

    app->ai.s32DevId = 0; app->ai.s32ChnId = 0;
    app->ai.s32ChnSampleRate = AUDIO_RATE;
    attr = &app->ai.stAiAttr;
    snprintf((char *)attr->u8CardName, sizeof(attr->u8CardName), "hw:0,0");
    attr->soundCard.channels = 2; attr->soundCard.sampleRate = AUDIO_RATE;
    attr->soundCard.bitWidth = AUDIO_BIT_WIDTH_16;
    attr->enSamplerate = AUDIO_RATE; attr->enBitwidth = AUDIO_BIT_WIDTH_16;
    attr->enSoundmode = AUDIO_SOUND_MODE_MONO;
    attr->u32PtNumPerFrm = AUDIO_SAMPLES; attr->u32FrmNum = 4; attr->u32ChnCnt = 2;
    if (SAMPLE_COMM_AI_CreateChn(&app->ai) != RK_SUCCESS) return RK_FAILURE;
    app->ai_ready = true;

    app->ao.s32DevId = 0; app->ao.s32ChnId = 0;
    app->ao.s32ChnSampleRate = AUDIO_RATE;
    attr = &app->ao.stAoAttr;
    snprintf((char *)attr->u8CardName, sizeof(attr->u8CardName), "hw:0,0");
    attr->soundCard.channels = 2; attr->soundCard.sampleRate = AUDIO_RATE;
    attr->soundCard.bitWidth = AUDIO_BIT_WIDTH_16;
    attr->enSamplerate = AUDIO_RATE; attr->enBitwidth = AUDIO_BIT_WIDTH_16;
    attr->enSoundmode = AUDIO_SOUND_MODE_MONO;
    attr->u32PtNumPerFrm = AUDIO_SAMPLES; attr->u32FrmNum = 4; attr->u32ChnCnt = 2;
    if (SAMPLE_COMM_AO_CreateChn(&app->ao) != RK_SUCCESS) return RK_FAILURE;
    app->ao_ready = true;

    RK_MPI_AI_SetVolume(0, 70);
    RK_MPI_AO_SetVolume(0, 70);
    RK_MPI_AO_SetTrackMode(0, AUDIO_TRACK_OUT_STEREO);
    RK_MPI_AMIX_SetControl(0, "ADC ALC Left Volume", "22");
    RK_MPI_AMIX_SetControl(0, "DAC LINEOUT Volume", "20");
    return RK_SUCCESS;
}

static RK_S32 init_streaming(APP_CTX *app, const char *rtmp_url) {
    VideoParam video;
    AudioParam audio;
    if (!rtmp_url || !rtmp_url[0]) {
        printf("RTMP disabled: no server URL was supplied\n");
        return RK_SUCCESS;
    }
    memset(&app->venc, 0, sizeof(app->venc));
    app->venc.s32ChnId = 0;
    app->venc.u32Width = 1920; app->venc.u32Height = 1080;
    app->venc.u32Fps = VIDEO_FPS; app->venc.u32Gop = VIDEO_FPS * 2;
    app->venc.u32BitRate = VIDEO_BITRATE_KBPS;
    app->venc.u32StreamBufCnt = 3; app->venc.u32BuffSize = 1920 * 1080 / 2;
    app->venc.enPixelFormat = RK_FMT_YUV420SP;
    app->venc.enCodecType = RK_CODEC_TYPE_H265;
    app->venc.enRcMode = VENC_RC_MODE_H265CBR;
    if (SAMPLE_COMM_VENC_CreateChn(&app->venc) != RK_SUCCESS) return RK_FAILURE;
    app->venc_ready = true;

    memset(&app->aenc, 0, sizeof(app->aenc));
    app->aenc.s32ChnId = 0;
    app->aenc.stChnAttr.enType = RK_AUDIO_ID_ACC;
    app->aenc.stChnAttr.u32BufCount = 4;
    app->aenc.stChnAttr.stCodecAttr.enType = RK_AUDIO_ID_ACC;
    app->aenc.stChnAttr.stCodecAttr.u32Channels = 1;
    app->aenc.stChnAttr.stCodecAttr.u32SampleRate = AUDIO_RATE;
    app->aenc.stChnAttr.stCodecAttr.enBitwidth = AUDIO_BIT_WIDTH_16;
    app->aenc.stChnAttr.stCodecAttr.u32Resv[0] = 2;
    app->aenc.stChnAttr.stCodecAttr.u32Resv[1] = AUDIO_BITRATE;
    if (SAMPLE_COMM_AENC_CreateChn(&app->aenc) != RK_SUCCESS) return RK_FAILURE;
    app->aenc_ready = true;

    app->venc_dst = (MPP_CHN_S){RK_ID_VENC, 0, app->venc.s32ChnId};
    if (SAMPLE_COMM_Bind(&app->vi_src, &app->venc_dst) != RK_SUCCESS)
        return RK_FAILURE;
    app->venc_bound = true;

    memset(&video, 0, sizeof(video));
    snprintf(video.format, sizeof(video.format), "NV12");
    snprintf(video.codec, sizeof(video.codec), "H.265");
    video.width = 1920; video.height = 1080;
    video.vir_width = 1920; video.vir_height = 1080;
    video.bit_rate = VIDEO_BITRATE_KBPS * 1000;
    video.frame_rate_num = VIDEO_FPS; video.frame_rate_den = 1;
    memset(&audio, 0, sizeof(audio));
    snprintf(audio.format, sizeof(audio.format), "S16");
    snprintf(audio.codec, sizeof(audio.codec), "ACC");
    audio.channels = 1; audio.sample_rate = AUDIO_RATE;
    audio.frame_size = AUDIO_SAMPLES;
    if (rkmuxer_init(MUXER_ID, "flv", rtmp_url, &video, &audio) != 0)
        return RK_FAILURE;
    app->muxer_ready = true;
    if (media_callbacks_start(&app->stream_callbacks, app->venc.s32ChnId,
                              app->aenc.s32ChnId, rtmp_video_callback,
                              rtmp_audio_callback, app) != 0)
        return RK_FAILURE;
    app->stream_callbacks_ready = true;
    printf("RTMP publishing: %s (H.265 1920x1080@%d, AAC %d Hz mono)\n",
           rtmp_url, VIDEO_FPS, AUDIO_RATE);
    return RK_SUCCESS;
}

static RK_S32 init_video(APP_CTX *app) {
    VO_CHN_ATTR_S ui_attr;

    memset(&app->vi, 0, sizeof(app->vi));
    app->vi.u32Width = 1920; app->vi.u32Height = 1080;
    app->vi.s32DevId = 0; app->vi.u32PipeId = 0; app->vi.s32ChnId = 1;
    app->vi.stChnAttr.stIspOpt.u32BufCount = 3;
    app->vi.stChnAttr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
    app->vi.stChnAttr.u32Depth = 0;
    app->vi.stChnAttr.enPixelFormat = RK_FMT_YUV420SP;
    app->vi.stChnAttr.enCompressMode = COMPRESS_MODE_NONE;
    app->vi.stChnAttr.stFrameRate.s32SrcFrameRate = -1;
    app->vi.stChnAttr.stFrameRate.s32DstFrameRate = -1;
    if (SAMPLE_COMM_VI_CreateChn(&app->vi) != RK_SUCCESS) return RK_FAILURE;
    app->vi_ready = true;

    memset(&app->vpss, 0, sizeof(app->vpss));
    app->vpss.s32GrpId = 0; app->vpss.s32ChnId = 0;
    app->vpss.enVProcDevType = VIDEO_PROC_DEV_RGA;
    app->vpss.stGrpVpssAttr.enPixelFormat = RK_FMT_YUV420SP;
    app->vpss.stGrpVpssAttr.enCompressMode = COMPRESS_MODE_NONE;
    app->vpss.stVpssChnAttr[0].enChnMode = VPSS_CHN_MODE_USER;
    app->vpss.stVpssChnAttr[0].enCompressMode = COMPRESS_MODE_NONE;
    app->vpss.stVpssChnAttr[0].enDynamicRange = DYNAMIC_RANGE_SDR8;
    app->vpss.stVpssChnAttr[0].enPixelFormat = RK_FMT_YUV420SP;
    app->vpss.stVpssChnAttr[0].stFrameRate.s32SrcFrameRate = -1;
    app->vpss.stVpssChnAttr[0].stFrameRate.s32DstFrameRate = -1;
    app->vpss.stVpssChnAttr[0].u32Width = VIDEO_W;
    app->vpss.stVpssChnAttr[0].u32Height = VIDEO_H;
    if (SAMPLE_COMM_VPSS_CreateChn(&app->vpss) != RK_SUCCESS) return RK_FAILURE;
    app->vpss_ready = true;

    memset(&app->vo, 0, sizeof(app->vo));
    app->vo.s32DevId = 0; app->vo.s32LayerId = 0; app->vo.s32ChnId = 0;
    app->vo.Volayer_mode = VO_LAYER_MODE_GRAPHIC; app->vo.u32DispBufLen = 3;
    app->vo.stVoPubAttr.enIntfType = VO_INTF_DEFAULT;
    app->vo.stVoPubAttr.enIntfSync = VO_OUTPUT_DEFAULT;
    app->vo.stLayerAttr.stDispRect.u32Width = SCREEN_W;
    app->vo.stLayerAttr.stDispRect.u32Height = SCREEN_H;
    app->vo.stLayerAttr.stImageSize.u32Width = SCREEN_W;
    app->vo.stLayerAttr.stImageSize.u32Height = SCREEN_H;
    app->vo.stLayerAttr.u32DispFrmRt = 30;
    app->vo.stLayerAttr.enPixFormat = RK_FMT_RGB888;
    app->vo.stChnAttr.stRect.s32Y = VIDEO_Y;
    app->vo.stChnAttr.stRect.u32Width = VIDEO_W;
    app->vo.stChnAttr.stRect.u32Height = VIDEO_H;
    app->vo.stChnAttr.u32Priority = 1;
    if (SAMPLE_COMM_VO_CreateChn(&app->vo) != RK_SUCCESS) return RK_FAILURE;
    app->vo_ready = true;

    memset(&ui_attr, 0, sizeof(ui_attr));
    ui_attr.stRect.s32Y = BAR_Y; ui_attr.stRect.u32Width = SCREEN_W;
    ui_attr.stRect.u32Height = BAR_H; ui_attr.u32FgAlpha = 255;
    ui_attr.u32Priority = 2;
    if (RK_MPI_VO_SetChnAttr(0, UI_CHN, &ui_attr) != RK_SUCCESS ||
        RK_MPI_VO_EnableChn(0, UI_CHN) != RK_SUCCESS) return RK_FAILURE;
    app->ui_ready = true;

    memset(&app->ui_frame, 0, sizeof(app->ui_frame));
    if (RK_MPI_SYS_MmzAlloc(&app->ui_mb, NULL, NULL, SCREEN_W * BAR_H * 3)
        != RK_SUCCESS) return RK_FAILURE;
    app->ui_frame.stVFrame.pMbBlk = app->ui_mb;
    app->ui_frame.stVFrame.u32Width = SCREEN_W;
    app->ui_frame.stVFrame.u32Height = BAR_H;
    app->ui_frame.stVFrame.u32VirWidth = SCREEN_W;
    app->ui_frame.stVFrame.u32VirHeight = BAR_H;
    app->ui_frame.stVFrame.enPixelFormat = RK_FMT_RGB888;
    app->ui_frame.stVFrame.enCompressMode = COMPRESS_MODE_NONE;

    app->vi_src = (MPP_CHN_S){RK_ID_VI, 0, 1};
    app->vpss_dst = (MPP_CHN_S){RK_ID_VPSS, 0, 0};
    app->vpss_src = (MPP_CHN_S){RK_ID_VPSS, 0, 0};
    app->vo_dst = (MPP_CHN_S){RK_ID_VO, 0, 0};
    if (SAMPLE_COMM_Bind(&app->vi_src, &app->vpss_dst) != RK_SUCCESS)
        return RK_FAILURE;
    if (set_video_visible(app, true) != RK_SUCCESS) return RK_FAILURE;
    return RK_SUCCESS;
}

static int open_touchscreen(void) {
    char path[64], name[256];
    int i;
    for (i = 0; i < 32; ++i) {
        int fd;
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        memset(name, 0, sizeof(name));
        ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);
        if (strstr(name, "Goodix") || strstr(name, "goodix") ||
            strstr(name, "GT911") || strstr(name, "Touch")) {
            struct input_absinfo abs;
            if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &abs) == 0)
                g_touch_max_x = abs.maximum;
            else if (ioctl(fd, EVIOCGABS(ABS_X), &abs) == 0)
                g_touch_max_x = abs.maximum;
            if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &abs) == 0)
                g_touch_max_y = abs.maximum;
            else if (ioctl(fd, EVIOCGABS(ABS_Y), &abs) == 0)
                g_touch_max_y = abs.maximum;
            printf("touchscreen: %s (%s)\n", name, path);
            return fd;
        }
        close(fd);
    }
    return -1;
}

static void toggle_play(APP_CTX *app) {
    bool next;
    pthread_mutex_lock(&app->lock);
    app->running = !app->running;
    next = app->running;
    pthread_mutex_unlock(&app->lock);
    set_video_visible(app, next);
    if (next && app->venc_ready)
        RK_MPI_VENC_RequestIDR(app->venc.s32ChnId, RK_TRUE);
    camera_ui_set_running(&app->ui, next);
}

static void toggle_audio(APP_CTX *app) {
    bool enabled;
    pthread_mutex_lock(&app->lock);
    app->audio_enabled = !app->audio_enabled;
    enabled = app->audio_enabled;
    pthread_mutex_unlock(&app->lock);
    camera_ui_set_audio_enabled(&app->ui, enabled);
}

static void ui_set_running(void *userdata, bool running) {
    APP_CTX *app = userdata;
    pthread_mutex_lock(&app->lock);
    app->running = running;
    pthread_mutex_unlock(&app->lock);
    set_video_visible(app, running);
    if (running && app->venc_ready)
        RK_MPI_VENC_RequestIDR(app->venc.s32ChnId, RK_TRUE);
}

static void ui_set_audio_enabled(void *userdata, bool enabled) {
    APP_CTX *app = userdata;
    pthread_mutex_lock(&app->lock);
    app->audio_enabled = enabled;
    pthread_mutex_unlock(&app->lock);
}

static void ui_request_exit(void *userdata) {
    ((APP_CTX *)userdata)->quit = true;
}

static void init_lvgl(APP_CTX *app) {
    static lv_disp_drv_t display_driver;
    static lv_indev_drv_t input_driver;
    camera_ui_callbacks_t callbacks = {
        ui_set_running, ui_set_audio_enabled, ui_request_exit, app
    };

    g_app = app;
    lv_init();
    lv_disp_draw_buf_init(&g_lv_draw_buf, g_lv_pixels, NULL,
                          sizeof(g_lv_pixels) / sizeof(g_lv_pixels[0]));
    lv_disp_drv_init(&display_driver);
    display_driver.hor_res = SCREEN_W;
    display_driver.ver_res = BAR_H;
    display_driver.flush_cb = lvgl_flush;
    display_driver.draw_buf = &g_lv_draw_buf;
    lv_disp_drv_register(&display_driver);

    lv_indev_drv_init(&input_driver);
    input_driver.type = LV_INDEV_TYPE_POINTER;
    input_driver.read_cb = lvgl_touch_read;
    lv_indev_drv_register(&input_driver);

    camera_ui_create(&app->ui, &callbacks, app->running, app->audio_enabled);
}

static void read_touch_events(void) {
    struct input_event ev;
    if (g_touch_fd < 0) return;
    while (read(g_touch_fd, &ev, sizeof(ev)) == sizeof(ev)) {
        if (ev.type == EV_ABS &&
            (ev.code == ABS_X || ev.code == ABS_MT_POSITION_X)) g_touch_x = ev.value;
        if (ev.type == EV_ABS &&
            (ev.code == ABS_Y || ev.code == ABS_MT_POSITION_Y)) g_touch_y = ev.value;
        if (ev.type == EV_KEY && ev.code == BTN_TOUCH) g_touch_down = ev.value != 0;
        if (ev.type == EV_ABS && ev.code == ABS_MT_TRACKING_ID)
            g_touch_down = ev.value >= 0;
    }
}

static void event_loop(APP_CTX *app) {
    uint64_t previous_ms = 0;
    struct timespec now;
    g_touch_fd = open_touchscreen();
    init_lvgl(app);
    printf("Controls: touch bottom bar, or type p (play/pause), m (mute), q (quit).\n");
    fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK);
    while (!app->quit && !g_signal_stop) {
        char key;
        uint64_t current_ms;
        read_touch_events();
        if (read(STDIN_FILENO, &key, 1) == 1) {
            if (key == 'p') toggle_play(app);
            else if (key == 'm') toggle_audio(app);
            else if (key == 'q') app->quit = true;
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
        current_ms = (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
        if (previous_ms == 0) previous_ms = current_ms;
        lv_tick_inc((uint32_t)(current_ms - previous_ms));
        previous_ms = current_ms;
        lv_timer_handler();
        usleep(5000);
    }
    if (g_touch_fd >= 0) close(g_touch_fd);
    g_touch_fd = -1;
}

static void cleanup(APP_CTX *app) {
    app->quit = true;
    if (app->audio_thread_ready) pthread_join(app->audio_thread, NULL);
    if (app->stream_callbacks_ready) media_callbacks_stop(&app->stream_callbacks);
    if (app->muxer_ready) rkmuxer_deinit(MUXER_ID);
    if (app->venc_bound) SAMPLE_COMM_UnBind(&app->vi_src, &app->venc_dst);
    if (app->aenc_ready) SAMPLE_COMM_AENC_DestroyChn(&app->aenc);
    if (app->venc_ready) SAMPLE_COMM_VENC_DestroyChn(&app->venc);
    if (app->video_bound) SAMPLE_COMM_UnBind(&app->vpss_src, &app->vo_dst);
    if (app->vpss_ready) SAMPLE_COMM_UnBind(&app->vi_src, &app->vpss_dst);
    if (app->ui_mb) RK_MPI_MB_ReleaseMB(app->ui_mb);
    if (app->ui_ready) RK_MPI_VO_DisableChn(0, UI_CHN);
    if (app->vo_ready) SAMPLE_COMM_VO_DestroyChn(&app->vo);
    if (app->vpss_ready) SAMPLE_COMM_VPSS_DestroyChn(&app->vpss);
    if (app->vi_ready) SAMPLE_COMM_VI_DestroyChn(&app->vi);
    if (app->ao_ready) SAMPLE_COMM_AO_DestroyChn(&app->ao);
    if (app->ai_ready) SAMPLE_COMM_AI_DestroyChn(&app->ai);
    if (app->mpi_ready) RK_MPI_SYS_Exit();
    if (app->isp_ready) SAMPLE_COMM_ISP_Stop(0);
    pthread_mutex_destroy(&app->muxer_lock);
    pthread_mutex_destroy(&app->lock);
}

int main(int argc, char **argv) {
    APP_CTX app;
    const char *iq_dir = argc > 1 ? argv[1] : "/etc/iqfiles";
    const char *rtmp_url = argc > 2 ? argv[2] : NULL;
    memset(&app, 0, sizeof(app));
    pthread_mutex_init(&app.lock, NULL);
    pthread_mutex_init(&app.muxer_lock, NULL);
    app.running = true; app.audio_enabled = true;
    signal(SIGINT, signal_handler); signal(SIGTERM, signal_handler);

    if (SAMPLE_COMM_ISP_Init(0, RK_AIQ_WORKING_MODE_NORMAL, RK_FALSE, iq_dir)
        != RK_SUCCESS) {
        fprintf(stderr, "RKAIQ initialization failed\n"); cleanup(&app); return 1;
    }
    app.isp_ready = true;
    if (SAMPLE_COMM_ISP_Run(0) != RK_SUCCESS) {
        fprintf(stderr, "RKAIQ start failed\n"); cleanup(&app); return 1;
    }
    if (RK_MPI_SYS_Init() != RK_SUCCESS) {
        fprintf(stderr, "RK_MPI_SYS_Init failed\n"); cleanup(&app); return 1;
    }
    app.mpi_ready = true;
    if (init_video(&app) != RK_SUCCESS) {
        fprintf(stderr, "video initialization failed\n"); cleanup(&app); return 1;
    }
    if (init_audio(&app) != RK_SUCCESS) {
        fprintf(stderr, "audio initialization failed\n"); cleanup(&app); return 1;
    }
    if (init_streaming(&app, rtmp_url) != RK_SUCCESS) {
        fprintf(stderr, "RTMP initialization failed for: %s\n",
                rtmp_url ? rtmp_url : "(none)");
        cleanup(&app); return 1;
    }
    if (pthread_create(&app.audio_thread, NULL, audio_loop, &app) != 0) {
        fprintf(stderr, "audio thread creation failed\n"); cleanup(&app); return 1;
    }
    app.audio_thread_ready = true;
    event_loop(&app);
    cleanup(&app);
    return 0;
}

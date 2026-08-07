#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/fb.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "sample_comm.h"
#include "rk_mpi_ai.h"
#include "rk_mpi_amix.h"
#include "rk_mpi_ao.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_vo.h"
#include "rk_mpi_venc.h"
#include "rkmuxer.h"
#include "lvgl/lvgl.h"
#include "camera_config.h"
#include "camera_ui.h"
#include "audio_encoder.h"
#include "live555_rtsp_server.h"
#include "media_callbacks.h"

#define SCREEN_W 480
#define SCREEN_H 480
#define VIDEO_W 480
#define VIDEO_H 270
#define VIDEO_Y 71
#define BAR_Y 413
#define BAR_H 67
#define UI_CHN 1
#define AUDIO_RATE 16000
#define AUDIO_SAMPLES 256
#define VIDEO_FPS 25
#define VIDEO_BITRATE_KBPS 2048
#define AUDIO_BITRATE 32000
#define MUXER_ID 0
#define VPSS_PREVIEW_CHN 0
#define VPSS_STREAM_CHN 1

typedef enum {
    FRAME_SOURCE_VI,
    FRAME_SOURCE_VPSS,
} FRAME_SOURCE_E;

typedef enum {
    VIDEO_CODEC_H264,
    VIDEO_CODEC_H265,
} VIDEO_CODEC_E;

typedef enum {
    AUDIO_CODEC_AAC,
    AUDIO_CODEC_MP3,
} AUDIO_CODEC_E;

typedef struct {
    SAMPLE_VI_CTX_S vi;
    SAMPLE_VPSS_CTX_S vpss;
    SAMPLE_VO_CTX_S vo;
    SAMPLE_AI_CTX_S ai;
    SAMPLE_AO_CTX_S ao;
    SAMPLE_VENC_CTX_S venc;
    MPP_CHN_S vi_src;
    MPP_CHN_S vpss_dst;
    MPP_CHN_S vpss_src;
    MPP_CHN_S vpss_stream_src;
    MPP_CHN_S vo_dst;
    MPP_CHN_S venc_src;
    MPP_CHN_S venc_dst;
    pthread_t audio_thread;
    pthread_t preview_thread;
    media_callbacks_t stream_callbacks;
    pthread_mutex_t lock;
    pthread_mutex_t muxer_lock;
    live555_rtsp_server_t *rtsp_server;
    audio_encoder_t *audio_encoder;
    bool running;
    bool audio_enabled;
    bool lvgl_preview;
    FRAME_SOURCE_E frame_source;
    VIDEO_CODEC_E video_codec;
    AUDIO_CODEC_E audio_codec;
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
    bool preview_thread_ready;
    bool venc_ready;
    bool venc_bound;
    bool muxer_ready;
    bool rtsp_ready;
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
static int g_lv_fb_fd = -1;
static uint8_t *g_lv_fb;
static size_t g_lv_fb_len;
static struct fb_fix_screeninfo g_lv_fb_fix;

static void signal_handler(int sig) {
    (void)sig;
    g_signal_stop = 1;
}

static bool lvgl_framebuffer_open(void) {
    struct fb_var_screeninfo var;

    if (g_lv_fb) return true;
    g_lv_fb_fd = open("/dev/fb0", O_RDWR);
    if (g_lv_fb_fd < 0 ||
        ioctl(g_lv_fb_fd, FBIOGET_FSCREENINFO, &g_lv_fb_fix) ||
        ioctl(g_lv_fb_fd, FBIOGET_VSCREENINFO, &var) ||
        var.bits_per_pixel != 32) {
        if (g_lv_fb_fd >= 0) close(g_lv_fb_fd);
        g_lv_fb_fd = -1;
        return false;
    }
    g_lv_fb_len = g_lv_fb_fix.smem_len;
    g_lv_fb = mmap(NULL, g_lv_fb_len, PROT_READ | PROT_WRITE,
                   MAP_SHARED, g_lv_fb_fd, 0);
    if (g_lv_fb == MAP_FAILED) {
        g_lv_fb = NULL;
        close(g_lv_fb_fd);
        g_lv_fb_fd = -1;
        return false;
    }
    return true;
}

static void lvgl_framebuffer_flush(lv_disp_drv_t *drv, const lv_area_t *area,
                                   lv_color_t *colors) {
    int x, y;

    if (lvgl_framebuffer_open()) {
        for (y = area->y1; y <= area->y2; ++y) {
            uint8_t *dst = g_lv_fb + (size_t)(BAR_Y + y) * g_lv_fb_fix.line_length +
                           (size_t)area->x1 * 4;
            for (x = area->x1; x <= area->x2; ++x) {
                lv_color_t color = *colors++;
                dst[0] = (uint8_t)(color.ch.blue * 255 / 31);
                dst[1] = (uint8_t)(color.ch.green * 255 / 63);
                dst[2] = (uint8_t)(color.ch.red * 255 / 31);
                dst += 4;
            }
        }
    }
    lv_disp_flush_ready(drv);
}

static void lvgl_framebuffer_close(void) {
    if (g_lv_fb) munmap(g_lv_fb, g_lv_fb_len);
    if (g_lv_fb_fd >= 0) close(g_lv_fb_fd);
    g_lv_fb = NULL;
    g_lv_fb_fd = -1;
    g_lv_fb_len = 0;
}

static uint8_t clamp_u8(int value) {
    return (uint8_t)(value < 0 ? 0 : value > 255 ? 255 : value);
}

static void *framebuffer_preview_loop(void *opaque) {
    APP_CTX *app = opaque;
    struct fb_fix_screeninfo fix;
    struct fb_var_screeninfo var;
    size_t map_len;
    uint8_t *fb;
    unsigned int failures = 0;
    bool logged_frame = false;
    int fb_fd = open("/dev/fb0", O_RDWR);

    if (fb_fd < 0 || ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix) ||
        ioctl(fb_fd, FBIOGET_VSCREENINFO, &var)) {
        if (fb_fd >= 0) close(fb_fd);
        return NULL;
    }
    map_len = fix.smem_len;
    fb = mmap(NULL, map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fb == MAP_FAILED) {
        close(fb_fd);
        return NULL;
    }

    /* Remove the boot framebuffer before the first camera frame arrives. */
    memset(fb, 0, map_len);

    while (!app->quit && !g_signal_stop) {
        VIDEO_FRAME_INFO_S frame;
        uint8_t *src;
        uint32_t x, y, stride;
        bool display_frame;
        RK_S32 ret;

        if (app->frame_source == FRAME_SOURCE_VPSS)
            ret = RK_MPI_VPSS_GetChnFrame(app->vpss.s32GrpId,
                                          app->vpss.s32ChnId, &frame, 200);
        else
            ret = RK_MPI_VI_GetChnFrame(app->vi.s32DevId,
                                        app->vi.s32ChnId, &frame, 200);
        if (ret != RK_SUCCESS) {
            if (++failures % 10 == 0)
                fprintf(stderr, "framebuffer preview: %s get frame failed: %#x\n",
                        app->frame_source == FRAME_SOURCE_VPSS ? "VPSS" : "VI",
                        ret);
            continue;
        }
        if (!logged_frame) {
            fprintf(stderr, "framebuffer preview: source=%s frame=%ux%u stride=%u format=%d\n",
                    app->frame_source == FRAME_SOURCE_VPSS ? "VPSS" : "VI",
                    frame.stVFrame.u32Width, frame.stVFrame.u32Height,
                    frame.stVFrame.u32VirWidth, frame.stVFrame.enPixelFormat);
            logged_frame = true;
        }
        src = RK_MPI_MB_Handle2VirAddr(frame.stVFrame.pMbBlk);
        stride = frame.stVFrame.u32VirWidth;
        pthread_mutex_lock(&app->lock);
        display_frame = app->running;
        pthread_mutex_unlock(&app->lock);
        if (display_frame) {
            if (app->frame_source == FRAME_SOURCE_VPSS) {
                for (y = 0; y < VIDEO_H; ++y) {
                    const uint8_t *src_row = src +
                        (size_t)y * frame.stVFrame.u32VirWidth * 3;
                    uint32_t *dst = (uint32_t *)(fb +
                        (size_t)(VIDEO_Y + y) * fix.line_length);
                    for (x = 0; x < VIDEO_W; ++x) {
                        const uint8_t *rgb = src_row + (size_t)x * 3;
                        dst[x] = ((uint32_t)rgb[0] << 16) |
                                 ((uint32_t)rgb[1] << 8) | rgb[2];
                    }
                }
            } else {
            /*
            
                Y Y Y Y ... (Y平面，高度=frame_height)
                Y Y Y Y ...
                ...
                UV UV UV UV ... (UV平面，高度=frame_height/2)
                UV UV UV UV ...

                src + stride * frame.stVFrame.u32VirHeight：跳到UV平面起始

                (y / 2) * stride：每2个Y行共享1个UV行

                (x & ~1U)：每2个X像素共享1个UV值（对齐到偶数）

                ~1U = 0xFFFFFFFE，确保x是偶数

                // 系数放大256倍，避免浮点运算
                R = (298 * c + 409 * vv + 128) >> 8
                G = (298 * c - 100 * uu - 208 * vv + 128) >> 8
                B = (298 * c + 516 * uu + 128) >> 8
            
            */
            for (y = 0; y < VIDEO_H; ++y) {
                uint32_t *dst = (uint32_t *)(fb + (size_t)(VIDEO_Y + y) * fix.line_length);
                for (x = 0; x < VIDEO_W; ++x) {
                    int yy = src[y * stride + x];
                    uint8_t *uv = src + stride * frame.stVFrame.u32VirHeight +
                                  (y / 2) * stride + (x & ~1U);
                    int uu = uv[0] - 128, vv = uv[1] - 128;
                    int c = yy - 16;
                    dst[x] = (uint32_t)(clamp_u8((298 * c + 409 * vv + 128) >> 8) << 16) |
                             (uint32_t)(clamp_u8((298 * c - 100 * uu - 208 * vv + 128) >> 8) << 8) |
                             clamp_u8((298 * c + 516 * uu + 128) >> 8);
                }
            }
            }
        }
        if (app->frame_source == FRAME_SOURCE_VPSS)
            RK_MPI_VPSS_ReleaseChnFrame(app->vpss.s32GrpId,
                                        app->vpss.s32ChnId, &frame);
        else
            RK_MPI_VI_ReleaseChnFrame(app->vi.s32DevId,
                                      app->vi.s32ChnId, &frame);
    }
    munmap(fb, map_len);
    close(fb_fd);
    return NULL;
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
    return RK_SUCCESS;
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
        stream = app->running && (app->muxer_ready || app->rtsp_ready);
        pthread_mutex_unlock(&app->lock);
        if (play) {
            frame.bBypassMbBlk = RK_FALSE;
            ret = RK_MPI_AO_SendFrame(0, 0, &frame, 200);
            if (ret != RK_SUCCESS)
                fprintf(stderr, "audio output dropped: %#x\n", ret);
        }
        if (stream) {
            const int16_t *samples = RK_MPI_MB_Handle2VirAddr(frame.pMbBlk);
            if (audio_encoder_push_s16(app->audio_encoder, samples,
                                       frame.u32Len / sizeof(*samples),
                                       frame.u64TimeStamp) != 0)
                fprintf(stderr, "software audio encoder dropped PCM\n");
        }
        RK_MPI_AI_ReleaseFrame(0, 0, &frame, NULL);
    }
    return NULL;
}

static bool is_video_keyframe(const APP_CTX *app, const VENC_PACK_S *pack) {
    if (app->video_codec == VIDEO_CODEC_H264)
        return pack->DataType.enH264EType == H264E_NALU_ISLICE ||
               pack->DataType.enH264EType == H264E_NALU_IDRSLICE;
    return pack->DataType.enH265EType == H265E_NALU_ISLICE ||
           pack->DataType.enH265EType == H265E_NALU_IDRSLICE;
}

static void streaming_video_callback(const VENC_STREAM_S *stream, void *userdata) {
    APP_CTX *app = userdata;
    const VENC_PACK_S *pack = stream->pstPack;
    RK_S32 ret = 0;
    bool send_rtmp, send_rtsp;

    pthread_mutex_lock(&app->lock);
    send_rtmp = app->running && app->muxer_ready;
    send_rtsp = app->running && app->rtsp_ready;
    pthread_mutex_unlock(&app->lock);
    if (!send_rtmp && !send_rtsp) return;
    unsigned char *data = RK_MPI_MB_Handle2VirAddr(pack->pMbBlk);
    pthread_mutex_lock(&app->muxer_lock);
    if (send_rtmp)
        ret = rkmuxer_write_video_frame(MUXER_ID, data, pack->u32Len,
                                         pack->u64PTS,
                                         is_video_keyframe(app, pack));
    if (send_rtsp) {
        if (live555_rtsp_server_push_video(app->rtsp_server, data,
                                           pack->u32Len, pack->u64PTS) != 0)
            fprintf(stderr, "RTSP video queue dropped a frame\n");
    }
    pthread_mutex_unlock(&app->muxer_lock);
    if (send_rtmp && ret != 0)
        fprintf(stderr, "RTMP video write failed: %d\n", ret);
}

static void streaming_audio_packet(const uint8_t *data, size_t size,
                                   uint64_t pts_us, void *userdata) {
    APP_CTX *app = userdata;
    RK_S32 ret = 0;
    bool send_rtmp, send_rtsp;

    pthread_mutex_lock(&app->lock);
    send_rtmp = app->running && app->muxer_ready;
    send_rtsp = app->running && app->rtsp_ready;
    pthread_mutex_unlock(&app->lock);
    if (!send_rtmp && !send_rtsp) return;
    if (send_rtmp) {
        pthread_mutex_lock(&app->muxer_lock);
        ret = rkmuxer_write_audio_frame(MUXER_ID, (unsigned char *)data, size,
                                         pts_us);
        pthread_mutex_unlock(&app->muxer_lock);
        if (ret != 0) fprintf(stderr, "RTMP audio write failed: %d\n", ret);
    }
    if (send_rtsp &&
        live555_rtsp_server_push_audio(app->rtsp_server,
                                       data, size, pts_us) != 0)
        fprintf(stderr, "RTSP audio queue dropped a frame\n");
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

static RK_S32 init_streaming(APP_CTX *app, const camera_config_t *config) {
    VideoParam video;
    AudioParam audio;
    bool rtmp_enabled = config->rtmp_url[0] != '\0';

    if (!rtmp_enabled && !config->rtsp_enabled) {
        printf("RTMP and RTSP disabled\n");
        return RK_SUCCESS;
    }
    memset(&app->venc, 0, sizeof(app->venc));
    app->venc.s32ChnId = 0;
    app->venc.u32Width = 1920; app->venc.u32Height = 1080;
    app->venc.u32Fps = VIDEO_FPS; app->venc.u32Gop = VIDEO_FPS * 2;
    app->venc.u32BitRate = VIDEO_BITRATE_KBPS;
    app->venc.u32StreamBufCnt = 3; app->venc.u32BuffSize = 1920 * 1080 / 2;
    app->venc.enPixelFormat = RK_FMT_YUV420SP;
    app->venc.enCodecType = app->video_codec == VIDEO_CODEC_H264
        ? RK_CODEC_TYPE_H264 : RK_CODEC_TYPE_H265;
    app->venc.enRcMode = app->video_codec == VIDEO_CODEC_H264
        ? VENC_RC_MODE_H264CBR : VENC_RC_MODE_H265CBR;
    if (SAMPLE_COMM_VENC_CreateChn(&app->venc) != RK_SUCCESS) return RK_FAILURE;
    app->venc_ready = true;

    app->venc_src = app->frame_source == FRAME_SOURCE_VPSS
        ? app->vpss_stream_src : app->vi_src;
    app->venc_dst = (MPP_CHN_S){RK_ID_VENC, 0, app->venc.s32ChnId};
    if (SAMPLE_COMM_Bind(&app->venc_src, &app->venc_dst) != RK_SUCCESS)
        return RK_FAILURE;
    app->venc_bound = true;

    if (rtmp_enabled) {
        memset(&video, 0, sizeof(video));
        snprintf(video.format, sizeof(video.format), "NV12");
        snprintf(video.codec, sizeof(video.codec), "%s",
                 app->video_codec == VIDEO_CODEC_H264 ? "H.264" : "H.265");
        video.width = 1920; video.height = 1080;
        video.vir_width = 1920; video.vir_height = 1080;
        video.bit_rate = VIDEO_BITRATE_KBPS * 1000;
        video.frame_rate_num = VIDEO_FPS; video.frame_rate_den = 1;
        memset(&audio, 0, sizeof(audio));
        snprintf(audio.format, sizeof(audio.format), "S16");
        snprintf(audio.codec, sizeof(audio.codec), "%s",
                 app->audio_codec == AUDIO_CODEC_AAC ? "ACC" : "MP3");
        audio.channels = 1; audio.sample_rate = AUDIO_RATE;
        audio.frame_size = app->audio_codec == AUDIO_CODEC_AAC ? 1024 : 576;
        if (rkmuxer_init(MUXER_ID, "flv", config->rtmp_url, &video, &audio) != 0)
            return RK_FAILURE;
        app->muxer_ready = true;
    }
    if (config->rtsp_enabled) {
        if (live555_rtsp_server_start(&app->rtsp_server, config->rtsp_port,
                                      config->rtsp_path,
                                      app->video_codec == VIDEO_CODEC_H264
                                          ? LIVE555_VIDEO_H264
                                          : LIVE555_VIDEO_H265,
                                      app->audio_codec == AUDIO_CODEC_AAC
                                          ? LIVE555_AUDIO_AAC
                                          : LIVE555_AUDIO_MP3,
                                      AUDIO_RATE, 1,
                                      "1408") != 0)
            return RK_FAILURE;
        app->rtsp_ready = true;
    }
    if (audio_encoder_create(&app->audio_encoder,
                             app->audio_codec == AUDIO_CODEC_AAC
                                 ? AUDIO_ENCODER_AAC : AUDIO_ENCODER_MP3,
                             AUDIO_RATE, 1, AUDIO_BITRATE,
                             streaming_audio_packet, app) != 0)
        return RK_FAILURE;
    if (media_callbacks_start(&app->stream_callbacks, app->venc.s32ChnId,
                              streaming_video_callback, app) != 0)
        return RK_FAILURE;
    app->stream_callbacks_ready = true;
    if (rtmp_enabled)
        printf("RTMP publishing: %s (%s 1920x1080@%d, %s %d Hz mono)\n",
               config->rtmp_url,
               app->video_codec == VIDEO_CODEC_H264 ? "H.264" : "H.265",
               VIDEO_FPS,
               app->audio_codec == AUDIO_CODEC_AAC ? "AAC" : "MP3",
               AUDIO_RATE);
    if (config->rtsp_enabled)
        printf("RTSP serving: rtsp://0.0.0.0:%d%s "
               "(%s 1920x1080@%d, %s %d Hz mono)\n",
               config->rtsp_port, config->rtsp_path,
               app->video_codec == VIDEO_CODEC_H264 ? "H.264" : "H.265",
               VIDEO_FPS,
               app->audio_codec == AUDIO_CODEC_AAC ? "AAC" : "MP3",
               AUDIO_RATE);
    return RK_SUCCESS;
}

static RK_S32 init_video(APP_CTX *app) {
    memset(&app->vi, 0, sizeof(app->vi));
    app->vi.u32Width = VIDEO_W; app->vi.u32Height = VIDEO_H;
    app->vi.s32DevId = 0; app->vi.u32PipeId = 0; app->vi.s32ChnId = 1;
    app->vi.stChnAttr.stIspOpt.u32BufCount = 3;
    app->vi.stChnAttr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
    // 增加了 VPSS 通道的 u32Depth = 1，使应用可通过 RK_MPI_VPSS_GetChnFrame() 获取帧
    app->vi.stChnAttr.u32Depth = 1;
    app->vi.stChnAttr.enPixelFormat = RK_FMT_YUV420SP;
    app->vi.stChnAttr.enCompressMode = COMPRESS_MODE_NONE;
    app->vi.stChnAttr.stFrameRate.s32SrcFrameRate = -1;
    app->vi.stChnAttr.stFrameRate.s32DstFrameRate = -1;
    if (SAMPLE_COMM_VI_CreateChn(&app->vi) != RK_SUCCESS) return RK_FAILURE;
    app->vi_ready = true;
    app->vi_src = (MPP_CHN_S){RK_ID_VI, 0, app->vi.s32ChnId};
    if (app->frame_source == FRAME_SOURCE_VI) return RK_SUCCESS;

    memset(&app->vpss, 0, sizeof(app->vpss));
    app->vpss.s32GrpId = 0; app->vpss.s32ChnId = VPSS_PREVIEW_CHN;
    app->vpss.enVProcDevType = VIDEO_PROC_DEV_RGA;
    app->vpss.stGrpVpssAttr.enPixelFormat = RK_FMT_YUV420SP;
    app->vpss.stGrpVpssAttr.enCompressMode = COMPRESS_MODE_NONE;
    app->vpss.s32ChnRotation[VPSS_PREVIEW_CHN] = ROTATION_0;
    app->vpss.stGrpVpssAttr.u32MaxW = VIDEO_W;
    app->vpss.stGrpVpssAttr.u32MaxH = VIDEO_H;
    app->vpss.stVpssChnAttr[VPSS_PREVIEW_CHN].enChnMode = VPSS_CHN_MODE_USER;
    app->vpss.stVpssChnAttr[VPSS_PREVIEW_CHN].enCompressMode = COMPRESS_MODE_NONE;
    app->vpss.stVpssChnAttr[VPSS_PREVIEW_CHN].enDynamicRange = DYNAMIC_RANGE_SDR8;
    app->vpss.stVpssChnAttr[VPSS_PREVIEW_CHN].enPixelFormat = RK_FMT_RGB888;
    app->vpss.stVpssChnAttr[VPSS_PREVIEW_CHN].stFrameRate.s32SrcFrameRate = -1;
    app->vpss.stVpssChnAttr[VPSS_PREVIEW_CHN].stFrameRate.s32DstFrameRate = -1;
    app->vpss.stVpssChnAttr[VPSS_PREVIEW_CHN].u32Depth = 1;
    app->vpss.stVpssChnAttr[VPSS_PREVIEW_CHN].u32Width = VIDEO_W;
    app->vpss.stVpssChnAttr[VPSS_PREVIEW_CHN].u32Height = VIDEO_H;
    app->vpss.s32ChnRotation[VPSS_STREAM_CHN] = ROTATION_0;
    app->vpss.stVpssChnAttr[VPSS_STREAM_CHN].enChnMode = VPSS_CHN_MODE_USER;
    app->vpss.stVpssChnAttr[VPSS_STREAM_CHN].enCompressMode = COMPRESS_MODE_NONE;
    app->vpss.stVpssChnAttr[VPSS_STREAM_CHN].enDynamicRange = DYNAMIC_RANGE_SDR8;
    app->vpss.stVpssChnAttr[VPSS_STREAM_CHN].enPixelFormat = RK_FMT_YUV420SP;
    app->vpss.stVpssChnAttr[VPSS_STREAM_CHN].stFrameRate.s32SrcFrameRate = -1;
    app->vpss.stVpssChnAttr[VPSS_STREAM_CHN].stFrameRate.s32DstFrameRate = -1;
    app->vpss.stVpssChnAttr[VPSS_STREAM_CHN].u32Width = 1920;
    app->vpss.stVpssChnAttr[VPSS_STREAM_CHN].u32Height = 1080;
    if (SAMPLE_COMM_VPSS_CreateChn(&app->vpss) != RK_SUCCESS) return RK_FAILURE;
    app->vpss_ready = true;
    app->vpss_dst = (MPP_CHN_S){RK_ID_VPSS, 0, 0};
    app->vpss_src = (MPP_CHN_S){RK_ID_VPSS, 0, VPSS_PREVIEW_CHN};
    app->vpss_stream_src = (MPP_CHN_S){RK_ID_VPSS, 0, VPSS_STREAM_CHN};
    if (SAMPLE_COMM_Bind(&app->vi_src, &app->vpss_dst) != RK_SUCCESS)
        return RK_FAILURE;
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
    display_driver.flush_cb = lvgl_framebuffer_flush;
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
    if (app->lvgl_preview) init_lvgl(app);

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
        if (app->lvgl_preview)
            lv_tick_inc((uint32_t)(current_ms - previous_ms));
        previous_ms = current_ms;
        if (app->lvgl_preview) lv_timer_handler();
        usleep(5000);
    }
    if (g_touch_fd >= 0) close(g_touch_fd);
    g_touch_fd = -1;
}

static void cleanup(APP_CTX *app) {
    app->quit = true;
    lvgl_framebuffer_close();
    if (app->preview_thread_ready) pthread_join(app->preview_thread, NULL);
    if (app->audio_thread_ready) pthread_join(app->audio_thread, NULL);
    if (app->audio_encoder) {
        audio_encoder_flush(app->audio_encoder);
        audio_encoder_destroy(app->audio_encoder);
        app->audio_encoder = NULL;
    }
    if (app->stream_callbacks_ready) media_callbacks_stop(&app->stream_callbacks);
    if (app->muxer_ready) rkmuxer_deinit(MUXER_ID);
    if (app->rtsp_server) live555_rtsp_server_stop(app->rtsp_server);
    if (app->venc_bound) SAMPLE_COMM_UnBind(&app->venc_src, &app->venc_dst);
    if (app->venc_ready) SAMPLE_COMM_VENC_DestroyChn(&app->venc);
    if (app->video_bound) SAMPLE_COMM_UnBind(&app->vi_src, &app->vo_dst);
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

static void print_usage(const char *program) {
    fprintf(stderr,
            "Usage: %s [iq_dir] [rtmp_url] [--config=path] [--lvgl] "
            "[--frame-source=vi|vpss] [--video-codec=h264|h265] "
            "[--audio-codec=aac|mp3]\n", program);
}

/*
 * ./camera_live /etc/iqfiles --lvgl --frame-source=vi
 * ./camera_live /etc/iqfiles --lvgl --frame-source=vpss
*/
int main(int argc, char **argv) {
    APP_CTX app;
    camera_config_t config;
    const char *config_path = "/userdata/camera_live.conf";
    bool config_explicit = false;
    FRAME_SOURCE_E frame_source;
    VIDEO_CODEC_E video_codec;
    AUDIO_CODEC_E audio_codec;
    int positional = 0;
    int i;

    camera_config_defaults(&config);
    for (i = 1; i < argc; ++i) {
        if (strncmp(argv[i], "--config=", 9) == 0) {
            config_path = argv[i] + 9;
            config_explicit = true;
        }
    }
    if ((config_explicit || access(config_path, R_OK) == 0) &&
        camera_config_load(&config, config_path) != 0)
        return 2;
    frame_source = strcmp(config.frame_source, "vpss") == 0
        ? FRAME_SOURCE_VPSS : FRAME_SOURCE_VI;
    video_codec = strcmp(config.video_codec, "h264") == 0
        ? VIDEO_CODEC_H264 : VIDEO_CODEC_H265;
    audio_codec = strcmp(config.audio_codec, "mp3") == 0
        ? AUDIO_CODEC_MP3 : AUDIO_CODEC_AAC;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--lvgl") == 0) {
            config.lvgl = true;
        } else if (strncmp(argv[i], "--config=", 9) == 0) {
            continue;
        } else if (strncmp(argv[i], "--frame-source=", 15) == 0) {
            const char *value = argv[i] + 15;
            if (strcmp(value, "vi") == 0)
                frame_source = FRAME_SOURCE_VI;
            else if (strcmp(value, "vpss") == 0)
                frame_source = FRAME_SOURCE_VPSS;
            else {
                fprintf(stderr, "Invalid frame source: %s\n", value);
                print_usage(argv[0]);
                return 2;
            }
        } else if (strncmp(argv[i], "--video-codec=", 14) == 0) {
            const char *value = argv[i] + 14;
            if (strcmp(value, "h264") == 0)
                video_codec = VIDEO_CODEC_H264;
            else if (strcmp(value, "h265") == 0)
                video_codec = VIDEO_CODEC_H265;
            else {
                fprintf(stderr, "Invalid video codec: %s\n", value);
                print_usage(argv[0]);
                return 2;
            }
        } else if (strncmp(argv[i], "--audio-codec=", 14) == 0) {
            const char *value = argv[i] + 14;
            if (strcmp(value, "aac") == 0)
                audio_codec = AUDIO_CODEC_AAC;
            else if (strcmp(value, "mp3") == 0)
                audio_codec = AUDIO_CODEC_MP3;
            else {
                fprintf(stderr, "Invalid audio codec: %s\n", value);
                print_usage(argv[0]);
                return 2;
            }
        } else if (strcmp(argv[i], "--help") == 0 ||
                   strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 2;
        } else if (positional++ == 0) {
            snprintf(config.iq_dir, sizeof(config.iq_dir), "%s", argv[i]);
        } else if (positional == 2) {
            snprintf(config.rtmp_url, sizeof(config.rtmp_url), "%s", argv[i]);
        } else {
            fprintf(stderr, "Too many positional arguments\n");
            print_usage(argv[0]);
            return 2;
        }
    }
    memset(&app, 0, sizeof(app));
    pthread_mutex_init(&app.lock, NULL);
    pthread_mutex_init(&app.muxer_lock, NULL);
    app.running = true; app.audio_enabled = true;
    app.lvgl_preview = config.lvgl;
    app.frame_source = frame_source;
    app.video_codec = video_codec;
    app.audio_codec = audio_codec;
    printf("Frame source: %s\n",
           frame_source == FRAME_SOURCE_VPSS ? "VPSS" : "VI");
    printf("Video codec: %s\n",
           video_codec == VIDEO_CODEC_H264 ? "H.264" : "H.265");
    printf("Audio codec: %s\n",
           audio_codec == AUDIO_CODEC_AAC ? "AAC" : "MP3");
    signal(SIGINT, signal_handler); signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    if (SAMPLE_COMM_ISP_Init(0, RK_AIQ_WORKING_MODE_NORMAL, RK_FALSE,
                             config.iq_dir)
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
    if (pthread_create(&app.preview_thread, NULL, framebuffer_preview_loop, &app) != 0) {
        fprintf(stderr, "framebuffer preview thread creation failed\n"); cleanup(&app); return 1;
    }
    app.preview_thread_ready = true;
    if (init_audio(&app) != RK_SUCCESS) {
        fprintf(stderr, "audio initialization failed\n"); cleanup(&app); return 1;
    }
    if (init_streaming(&app, &config) != RK_SUCCESS) {
        fprintf(stderr, "streaming initialization failed\n");
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

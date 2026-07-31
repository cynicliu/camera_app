#include <SDL2/SDL.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "camera_ui.h"

#define SCREEN_W 720
#define SCREEN_H 720
#define VIDEO_W 720
#define VIDEO_H 406
#define VIDEO_Y 107
#define BAR_Y 620
#define BAR_H 100
#define CAM_W 640
#define CAM_H 480
#define CAM_BUFFERS 4

typedef struct {
    void *data;
    size_t length;
} camera_buffer_t;

typedef struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    SDL_AudioDeviceID capture_device;
    SDL_AudioDeviceID playback_device;
    uint32_t pixels[SCREEN_W * SCREEN_H];
    lv_color_t lv_pixels[SCREEN_W * 20];
    lv_disp_draw_buf_t draw_buffer;
    camera_ui_t ui;
    int camera_fd;
    camera_buffer_t camera_buffers[CAM_BUFFERS];
    unsigned camera_buffer_count;
    bool camera_streaming;
    bool running;
    bool audio_enabled;
    bool quit;
    int mouse_x;
    int mouse_y;
    bool mouse_down;
    unsigned test_frame;
} simulator_t;

static simulator_t *g_simulator;

static int xioctl(int fd, unsigned long request, void *arg) {
    int result;
    do result = ioctl(fd, request, arg); while (result < 0 && errno == EINTR);
    return result;
}

static void close_camera(simulator_t *sim) {
    unsigned i;
    if (sim->camera_streaming) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(sim->camera_fd, VIDIOC_STREAMOFF, &type);
    }
    for (i = 0; i < sim->camera_buffer_count; ++i)
        if (sim->camera_buffers[i].data)
            munmap(sim->camera_buffers[i].data, sim->camera_buffers[i].length);
    if (sim->camera_fd >= 0) close(sim->camera_fd);
    sim->camera_fd = -1;
    sim->camera_streaming = false;
    sim->camera_buffer_count = 0;
}

static bool open_camera(simulator_t *sim, const char *path) {
    struct v4l2_format format;
    struct v4l2_requestbuffers request;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    unsigned i;

    sim->camera_fd = open(path, O_RDWR | O_NONBLOCK);
    if (sim->camera_fd < 0) return false;
    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = CAM_W;
    format.fmt.pix.height = CAM_H;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    format.fmt.pix.field = V4L2_FIELD_ANY;
    if (xioctl(sim->camera_fd, VIDIOC_S_FMT, &format) < 0 ||
        format.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV) {
        close_camera(sim); return false;
    }
    memset(&request, 0, sizeof(request));
    request.count = CAM_BUFFERS;
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;
    if (xioctl(sim->camera_fd, VIDIOC_REQBUFS, &request) < 0) {
        close_camera(sim); return false;
    }
    sim->camera_buffer_count = request.count < CAM_BUFFERS ? request.count : CAM_BUFFERS;
    for (i = 0; i < sim->camera_buffer_count; ++i) {
        struct v4l2_buffer buffer;
        memset(&buffer, 0, sizeof(buffer));
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = i;
        if (xioctl(sim->camera_fd, VIDIOC_QUERYBUF, &buffer) < 0) {
            close_camera(sim); return false;
        }
        sim->camera_buffers[i].length = buffer.length;
        sim->camera_buffers[i].data = mmap(NULL, buffer.length,
            PROT_READ | PROT_WRITE, MAP_SHARED, sim->camera_fd, buffer.m.offset);
        if (sim->camera_buffers[i].data == MAP_FAILED) {
            sim->camera_buffers[i].data = NULL; close_camera(sim); return false;
        }
        if (xioctl(sim->camera_fd, VIDIOC_QBUF, &buffer) < 0) {
            close_camera(sim); return false;
        }
    }
    if (xioctl(sim->camera_fd, VIDIOC_STREAMON, &type) < 0) {
        close_camera(sim); return false;
    }
    sim->camera_streaming = true;
    printf("Camera source: %s (%dx%d YUYV)\n", path, CAM_W, CAM_H);
    return true;
}

static uint8_t clamp_color(int value) {
    return value < 0 ? 0 : value > 255 ? 255 : (uint8_t)value;
}

static uint32_t yuv_to_argb(int y, int u, int v) {
    int c = y - 16, d = u - 128, e = v - 128;
    uint8_t r = clamp_color((298 * c + 409 * e + 128) >> 8);
    uint8_t g = clamp_color((298 * c - 100 * d - 208 * e + 128) >> 8);
    uint8_t b = clamp_color((298 * c + 516 * d + 128) >> 8);
    return 0xff000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static void draw_camera_frame(simulator_t *sim, const uint8_t *yuyv) {
    int x, y;
    for (y = 0; y < VIDEO_H; ++y) {
        int source_y = y * CAM_H / VIDEO_H;
        for (x = 0; x < VIDEO_W; x += 2) {
            int source_x = x * CAM_W / VIDEO_W;
            source_x &= ~1;
            const uint8_t *p = yuyv + (source_y * CAM_W + source_x) * 2;
            sim->pixels[(VIDEO_Y + y) * SCREEN_W + x] =
                yuv_to_argb(p[0], p[1], p[3]);
            sim->pixels[(VIDEO_Y + y) * SCREEN_W + x + 1] =
                yuv_to_argb(p[2], p[1], p[3]);
        }
    }
}

static void draw_test_frame(simulator_t *sim) {
    int x, y;
    unsigned frame = sim->test_frame++;
    for (y = 0; y < VIDEO_H; ++y) {
        for (x = 0; x < VIDEO_W; ++x) {
            uint8_t r = (uint8_t)((x + frame) & 255);
            uint8_t g = (uint8_t)((y * 2 + frame) & 255);
            uint8_t b = (uint8_t)(((x / 80 + y / 58) & 1) ? 180 : 45);
            sim->pixels[(VIDEO_Y + y) * SCREEN_W + x] =
                0xff000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
    }
}

static void update_video(simulator_t *sim) {
    struct v4l2_buffer buffer;
    if (!sim->running) return;
    if (sim->camera_streaming) {
        memset(&buffer, 0, sizeof(buffer));
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        if (xioctl(sim->camera_fd, VIDIOC_DQBUF, &buffer) == 0) {
            draw_camera_frame(sim, sim->camera_buffers[buffer.index].data);
            xioctl(sim->camera_fd, VIDIOC_QBUF, &buffer);
        }
    } else {
        draw_test_frame(sim);
    }
}

static void audio_capture(void *userdata, Uint8 *stream, int length) {
    simulator_t *sim = userdata;
    if (sim->audio_enabled && sim->running && sim->playback_device)
        SDL_QueueAudio(sim->playback_device, stream, (Uint32)length);
}

static void init_audio(simulator_t *sim) {
    SDL_AudioSpec desired;
    memset(&desired, 0, sizeof(desired));
    desired.freq = 16000;
    desired.format = AUDIO_S16SYS;
    desired.channels = 1;
    desired.samples = 256;
    desired.callback = audio_capture;
    desired.userdata = sim;
    sim->capture_device = SDL_OpenAudioDevice(NULL, 1, &desired, NULL, 0);
    desired.callback = NULL;
    desired.userdata = NULL;
    sim->playback_device = SDL_OpenAudioDevice(NULL, 0, &desired, NULL, 0);
    if (sim->capture_device && sim->playback_device) {
        SDL_PauseAudioDevice(sim->playback_device, 0);
        SDL_PauseAudioDevice(sim->capture_device, 0);
        printf("Audio source: SDL default capture/playback (16 kHz mono)\n");
    } else {
        fprintf(stderr, "Audio monitoring unavailable: %s\n", SDL_GetError());
    }
}

static void lvgl_flush(lv_disp_drv_t *driver, const lv_area_t *area,
                       lv_color_t *colors) {
    int x, y;
    (void)driver;
    for (y = area->y1; y <= area->y2; ++y) {
        for (x = area->x1; x <= area->x2; ++x) {
            lv_color_t color = *colors++;
            uint8_t r = (uint8_t)(color.ch.red * 255 / 31);
            uint8_t g = (uint8_t)(color.ch.green * 255 / 63);
            uint8_t b = (uint8_t)(color.ch.blue * 255 / 31);
            g_simulator->pixels[(BAR_Y + y) * SCREEN_W + x] =
                0xff000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
    }
    lv_disp_flush_ready(driver);
}

static void mouse_read(lv_indev_drv_t *driver, lv_indev_data_t *data) {
    (void)driver;
    data->point.x = g_simulator->mouse_x;
    if (data->point.x < 0) data->point.x = 0;
    if (data->point.x >= SCREEN_W) data->point.x = SCREEN_W - 1;
    data->point.y = g_simulator->mouse_y - BAR_Y;
    if (data->point.y < 0) data->point.y = 0;
    if (data->point.y >= BAR_H) data->point.y = BAR_H - 1;
    data->state = g_simulator->mouse_down ? LV_INDEV_STATE_PRESSED
                                          : LV_INDEV_STATE_RELEASED;
}

static void set_running(void *userdata, bool running) {
    simulator_t *sim = userdata;
    sim->running = running;
    if (!running)
        memset(sim->pixels + VIDEO_Y * SCREEN_W, 0,
               VIDEO_W * VIDEO_H * sizeof(sim->pixels[0]));
}

static void set_audio_enabled(void *userdata, bool enabled) {
    simulator_t *sim = userdata;
    sim->audio_enabled = enabled;
    if (!enabled && sim->playback_device)
        SDL_ClearQueuedAudio(sim->playback_device);
}

static void request_exit(void *userdata) {
    ((simulator_t *)userdata)->quit = true;
}

static void init_lvgl(simulator_t *sim) {
    static lv_disp_drv_t display_driver;
    static lv_indev_drv_t input_driver;
    camera_ui_callbacks_t callbacks = {
        set_running, set_audio_enabled, request_exit, sim
    };
    lv_init();
    lv_disp_draw_buf_init(&sim->draw_buffer, sim->lv_pixels, NULL,
                          sizeof(sim->lv_pixels) / sizeof(sim->lv_pixels[0]));
    lv_disp_drv_init(&display_driver);
    display_driver.hor_res = SCREEN_W;
    display_driver.ver_res = BAR_H;
    display_driver.flush_cb = lvgl_flush;
    display_driver.draw_buf = &sim->draw_buffer;
    lv_disp_drv_register(&display_driver);
    lv_indev_drv_init(&input_driver);
    input_driver.type = LV_INDEV_TYPE_POINTER;
    input_driver.read_cb = mouse_read;
    lv_indev_drv_register(&input_driver);
    camera_ui_create(&sim->ui, &callbacks, true, true);
}

static void process_events(simulator_t *sim) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) sim->quit = true;
        else if (event.type == SDL_MOUSEMOTION) {
            float logical_x, logical_y;
            SDL_RenderWindowToLogical(sim->renderer, event.motion.x,
                                      event.motion.y, &logical_x, &logical_y);
            sim->mouse_x = (int)logical_x; sim->mouse_y = (int)logical_y;
        } else if (event.type == SDL_MOUSEBUTTONDOWN) {
            float logical_x, logical_y;
            SDL_RenderWindowToLogical(sim->renderer, event.button.x,
                                      event.button.y, &logical_x, &logical_y);
            sim->mouse_x = (int)logical_x; sim->mouse_y = (int)logical_y;
            sim->mouse_down = true;
        } else if (event.type == SDL_MOUSEBUTTONUP) {
            float logical_x, logical_y;
            SDL_RenderWindowToLogical(sim->renderer, event.button.x,
                                      event.button.y, &logical_x, &logical_y);
            sim->mouse_x = (int)logical_x; sim->mouse_y = (int)logical_y;
            sim->mouse_down = false;
        } else if (event.type == SDL_KEYDOWN) {
            if (event.key.keysym.sym == SDLK_SPACE) {
                camera_ui_set_running(&sim->ui, !sim->running);
                set_running(sim, !sim->running);
            } else if (event.key.keysym.sym == SDLK_m &&
                       (event.key.keysym.mod & KMOD_CTRL)) {
                SDL_MinimizeWindow(sim->window);
            } else if (event.key.keysym.sym == SDLK_m) {
                camera_ui_set_audio_enabled(&sim->ui, !sim->audio_enabled);
                set_audio_enabled(sim, !sim->audio_enabled);
            } else if (event.key.keysym.sym == SDLK_F11) {
                Uint32 flags = SDL_GetWindowFlags(sim->window);
                if (flags & SDL_WINDOW_MAXIMIZED)
                    SDL_RestoreWindow(sim->window);
                else
                    SDL_MaximizeWindow(sim->window);
            } else if (event.key.keysym.sym == SDLK_r) {
                SDL_RestoreWindow(sim->window);
            } else if (event.key.keysym.sym == SDLK_ESCAPE) sim->quit = true;
        }
    }
}

int main(int argc, char **argv) {
    simulator_t sim;
    const char *camera_path = argc > 1 ? argv[1] : "/dev/video0";
    uint64_t previous_tick;
    unsigned rendered_frames = 0;
    unsigned frame_limit = 0;
    const char *frame_limit_env = getenv("CAMERA_SIM_FRAMES");
    const char *screenshot_path = getenv("CAMERA_SIM_SCREENSHOT");
    if (frame_limit_env) frame_limit = (unsigned)strtoul(frame_limit_env, NULL, 10);
    memset(&sim, 0, sizeof(sim));
    sim.camera_fd = -1;
    sim.running = true;
    sim.audio_enabled = true;
    g_simulator = &sim;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL initialization failed: %s\n", SDL_GetError());
        return 1;
    }
    sim.window = SDL_CreateWindow("Luckfox Camera Monitor Simulator",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, SCREEN_W, SCREEN_H,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (sim.window) SDL_SetWindowMinimumSize(sim.window, 480, 480);
    sim.renderer = SDL_CreateRenderer(sim.window, -1,
                                      SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!sim.renderer)
        sim.renderer = SDL_CreateRenderer(sim.window, -1, SDL_RENDERER_SOFTWARE);
    if (sim.renderer)
        SDL_RenderSetLogicalSize(sim.renderer, SCREEN_W, SCREEN_H);
    sim.texture = SDL_CreateTexture(sim.renderer, SDL_PIXELFORMAT_ARGB8888,
                                    SDL_TEXTUREACCESS_STREAMING, SCREEN_W, SCREEN_H);
    if (!sim.window || !sim.renderer || !sim.texture) {
        fprintf(stderr, "SDL display creation failed: %s\n", SDL_GetError());
        SDL_Quit(); return 1;
    }
    memset(sim.pixels, 0x10, sizeof(sim.pixels));
    if (!open_camera(&sim, camera_path))
        printf("Camera source unavailable; using animated test pattern.\n");
    init_audio(&sim);
    init_lvgl(&sim);
    previous_tick = SDL_GetTicks64();
    while (!sim.quit) {
        uint64_t current_tick = SDL_GetTicks64();
        process_events(&sim);
        update_video(&sim);
        lv_tick_inc((uint32_t)(current_tick - previous_tick));
        previous_tick = current_tick;
        lv_timer_handler();
        SDL_UpdateTexture(sim.texture, NULL, sim.pixels, SCREEN_W * sizeof(uint32_t));
        SDL_RenderClear(sim.renderer);
        SDL_RenderCopy(sim.renderer, sim.texture, NULL, NULL);
        SDL_RenderPresent(sim.renderer);
        ++rendered_frames;
        if (screenshot_path && rendered_frames == 30) {
            SDL_Surface *surface = SDL_CreateRGBSurfaceFrom(
                sim.pixels, SCREEN_W, SCREEN_H, 32, SCREEN_W * sizeof(uint32_t),
                0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000);
            if (surface) {
                SDL_SaveBMP(surface, screenshot_path);
                SDL_FreeSurface(surface);
            }
        }
        if (frame_limit && rendered_frames >= frame_limit) sim.quit = true;
        SDL_Delay(5);
    }
    close_camera(&sim);
    if (sim.capture_device) SDL_CloseAudioDevice(sim.capture_device);
    if (sim.playback_device) SDL_CloseAudioDevice(sim.playback_device);
    SDL_DestroyTexture(sim.texture);
    SDL_DestroyRenderer(sim.renderer);
    SDL_DestroyWindow(sim.window);
    SDL_Quit();
    return 0;
}

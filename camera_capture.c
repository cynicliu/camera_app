#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sample_comm.h"

static volatile sig_atomic_t g_stop;

static void on_signal(int signal_number) {
    (void)signal_number;
    g_stop = 1;
}

static void usage(const char *program) {
    printf("Usage: %s [options]\n", program);
    printf("  -w, --width N       Frame width (default: 1920)\n");
    printf("  -h, --height N      Frame height (default: 1080)\n");
    printf("  -n, --frames N      Frames to capture (default: 30)\n");
    printf("  -o, --output FILE   NV12 output (default: /tmp/capture.nv12)\n");
    printf("  -a, --aiq DIR       IQ file directory (default: /etc/iqfiles)\n");
    printf("  -c, --channel N     VI channel (default: 1)\n");
    printf("  -I, --camera N      Camera/VI device id (default: 0)\n");
    printf("      --help          Show this help\n");
}

static int positive_number(const char *text, const char *name) {
    char *end = NULL;
    long value;

    errno = 0;
    value = strtol(text, &end, 10);
    if (errno || !end || *end != '\0' || value <= 0 || value > INT32_MAX) {
        fprintf(stderr, "Invalid %s: %s\n", name, text);
        return -1;
    }
    return (int)value;
}

static int nonnegative_number(const char *text, const char *name) {
    char *end = NULL;
    long value;

    errno = 0;
    value = strtol(text, &end, 10);
    if (errno || !end || *end != '\0' || value < 0 || value > INT32_MAX) {
        fprintf(stderr, "Invalid %s: %s\n", name, text);
        return -1;
    }
    return (int)value;
}

int main(int argc, char **argv) {
    RK_U32 width = 1920;
    RK_U32 height = 1080;
    RK_S32 frame_limit = 30;
    RK_S32 camera_id = 0;
    RK_S32 channel_id = 1;
    const char *output_path = "/tmp/capture.nv12";
    const char *iq_dir = "/etc/iqfiles";
    SAMPLE_VI_CTX_S vi;
    FILE *output = NULL;
    void *frame_data = NULL;
    RK_S32 ret;
    RK_S32 frame_count = 0;
    bool isp_initialized = false;
    bool mpi_started = false;
    bool vi_started = false;
    int exit_code = EXIT_FAILURE;

    static const struct option options[] = {
        {"width", required_argument, NULL, 'w'},
        {"height", required_argument, NULL, 'h'},
        {"frames", required_argument, NULL, 'n'},
        {"output", required_argument, NULL, 'o'},
        {"aiq", required_argument, NULL, 'a'},
        {"channel", required_argument, NULL, 'c'},
        {"camera", required_argument, NULL, 'I'},
        {"help", no_argument, NULL, 1000},
        {NULL, 0, NULL, 0},
    };

    for (;;) {
        int option = getopt_long(argc, argv, "w:h:n:o:a:c:I:", options, NULL);
        int value;
        if (option == -1)
            break;

        switch (option) {
        case 'w':
            value = positive_number(optarg, "width");
            if (value < 0) return EXIT_FAILURE;
            width = (RK_U32)value;
            break;
        case 'h':
            value = positive_number(optarg, "height");
            if (value < 0) return EXIT_FAILURE;
            height = (RK_U32)value;
            break;
        case 'n':
            value = positive_number(optarg, "frame count");
            if (value < 0) return EXIT_FAILURE;
            frame_limit = (RK_S32)value;
            break;
        case 'o': output_path = optarg; break;
        case 'a': iq_dir = optarg; break;
        case 'c':
            value = nonnegative_number(optarg, "channel");
            if (value < 0) return EXIT_FAILURE;
            channel_id = (RK_S32)value;
            break;
        case 'I':
            value = nonnegative_number(optarg, "camera id");
            if (value < 0) return EXIT_FAILURE;
            camera_id = (RK_S32)value;
            break;
        case 1000: usage(argv[0]); return EXIT_SUCCESS;
        default: usage(argv[0]); return EXIT_FAILURE;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    memset(&vi, 0, sizeof(vi));

    printf("camera=%d channel=%d size=%ux%u frames=%d\n",
           camera_id, channel_id, width, height, frame_limit);
    printf("iq=%s output=%s\n", iq_dir, output_path);

    ret = SAMPLE_COMM_ISP_Init(camera_id, RK_AIQ_WORKING_MODE_NORMAL,
                               RK_FALSE, iq_dir);
    if (ret != RK_SUCCESS) {
        fprintf(stderr, "SAMPLE_COMM_ISP_Init failed: %#x\n", ret);
        goto cleanup;
    }
    isp_initialized = true;
    ret = SAMPLE_COMM_ISP_Run(camera_id);
    if (ret != RK_SUCCESS) {
        fprintf(stderr, "SAMPLE_COMM_ISP_Run failed: %#x\n", ret);
        goto cleanup;
    }

    ret = RK_MPI_SYS_Init();
    if (ret != RK_SUCCESS) {
        fprintf(stderr, "RK_MPI_SYS_Init failed: %#x\n", ret);
        goto cleanup;
    }
    mpi_started = true;

    vi.u32Width = width;
    vi.u32Height = height;
    vi.s32DevId = camera_id;
    vi.u32PipeId = (RK_U32)camera_id;
    vi.s32ChnId = channel_id;
    vi.stChnAttr.stIspOpt.u32BufCount = 3;
    vi.stChnAttr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
    vi.stChnAttr.u32Depth = 1;
    vi.stChnAttr.enPixelFormat = RK_FMT_YUV420SP;
    vi.stChnAttr.enCompressMode = COMPRESS_MODE_NONE;
    vi.stChnAttr.stFrameRate.s32SrcFrameRate = -1;
    vi.stChnAttr.stFrameRate.s32DstFrameRate = -1;

    ret = SAMPLE_COMM_VI_CreateChn(&vi);
    if (ret != RK_SUCCESS) {
        fprintf(stderr, "SAMPLE_COMM_VI_CreateChn failed: %#x\n", ret);
        goto cleanup;
    }
    vi_started = true;

    output = fopen(output_path, "wb");
    if (!output) {
        fprintf(stderr, "Cannot open %s: %s\n", output_path, strerror(errno));
        goto cleanup;
    }

    while (!g_stop && frame_count < frame_limit) {
        size_t frame_size;

        ret = SAMPLE_COMM_VI_GetChnFrame(&vi, &frame_data);
        if (ret != RK_SUCCESS) {
            fprintf(stderr, "Get frame %d failed: %#x\n", frame_count, ret);
            usleep(10000);
            continue;
        }

        frame_size = (size_t)vi.stViFrame.stVFrame.u64PrivateData;
        if (frame_size == 0)
            frame_size = (size_t)width * height * 3 / 2;

        if (fwrite(frame_data, 1, frame_size, output) != frame_size) {
            fprintf(stderr, "Write failed: %s\n", strerror(errno));
            SAMPLE_COMM_VI_ReleaseChnFrame(&vi);
            goto cleanup;
        }

        ++frame_count;
        printf("frame=%d seq=%u bytes=%zu pts=%lldus\n", frame_count,
               vi.stViFrame.stVFrame.u32TimeRef, frame_size,
               (long long)vi.stViFrame.stVFrame.u64PTS);
        SAMPLE_COMM_VI_ReleaseChnFrame(&vi);
        frame_data = NULL;
    }

    if (fflush(output) != 0) {
        fprintf(stderr, "Flush failed: %s\n", strerror(errno));
        goto cleanup;
    }
    printf("Captured %d frame(s) to %s\n", frame_count, output_path);
    exit_code = EXIT_SUCCESS;

cleanup:
    if (output)
        fclose(output);
    if (vi_started)
        SAMPLE_COMM_VI_DestroyChn(&vi);
    if (mpi_started)
        RK_MPI_SYS_Exit();
    if (isp_initialized)
        SAMPLE_COMM_ISP_Stop(camera_id);
    return exit_code;
}

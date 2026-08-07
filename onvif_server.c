#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "onvif_server.h"

#define ONVIF_SRVD_DEFAULT "/usr/bin/onvif_srvd"
#define WSDD_DEFAULT "/usr/bin/wsdd"

struct onvif_server {
    pid_t service_pid;
    pid_t discovery_pid;
};

static const char *program_path(const char *environment, const char *fallback) {
    const char *value = getenv(environment);
    return value && value[0] ? value : fallback;
}

static int child_running(pid_t pid) {
    int status;
    pid_t result = waitpid(pid, &status, WNOHANG);
    return result == 0;
}

static pid_t start_service(const onvif_server_config_t *cfg) {
    char port[16], width[16], height[16], audio_rate[16], audio_bitrate[16], url[256];
    const char *codec = strcmp(cfg->video_codec, "h264") == 0 ? "H264" : "H265";
    const char *binary = program_path("ONVIF_SRVD_PATH", ONVIF_SRVD_DEFAULT);
    pid_t pid;
    snprintf(port, sizeof(port), "%d", cfg->http_port);
    snprintf(width, sizeof(width), "%d", cfg->width);
    snprintf(height, sizeof(height), "%d", cfg->height);
    snprintf(audio_rate, sizeof(audio_rate), "%d", cfg->audio_sample_rate / 1000);
    snprintf(audio_bitrate, sizeof(audio_bitrate), "%d", cfg->audio_bitrate / 1000);
    snprintf(url, sizeof(url), "rtsp://%%s:%d%s", cfg->rtsp_port,
             cfg->rtsp_path);
    pid = fork();
    if (pid != 0) return pid;
    execl(binary, binary, "--no_fork", "--no_chdir", "--no_close",
          "--port", port, "--ifs", cfg->interface_name,
          "--user", cfg->username, "--password", cfg->password,
          "--manufacturer", "Luckfox", "--model", "RV1106 Camera",
          "--hardware_id", "RV1106", "--firmware_ver", "1.0",
          "--scope", "onvif://www.onvif.org/type/video_encoder",
          "--scope", "onvif://www.onvif.org/Profile/Streaming",
          "--scope", cfg->device_scope,
          "--name", cfg->device_name, "--width", width,
          "--height", height, "--url", url,
          "--audio_type", strcmp(cfg->audio_codec, "mp3") == 0 ? "MP3" : "AAC",
          "--audio_rate", audio_rate, "--audio_bitrate", audio_bitrate,
          "--type", codec,
          (char *)NULL);
    fprintf(stderr, "cannot start %s: %s\n", binary, strerror(errno));
    _exit(127);
}

static pid_t start_discovery(const onvif_server_config_t *cfg) {
    char xaddr[256], scope[512];
    const char *binary = program_path("WSDD_PATH", WSDD_DEFAULT);
    pid_t pid;
    snprintf(xaddr, sizeof(xaddr), "http://%%s:%d/onvif/device_service",
             cfg->http_port);
    snprintf(scope, sizeof(scope),
             "onvif://www.onvif.org/type/video_encoder "
             "onvif://www.onvif.org/Profile/Streaming %s", cfg->device_scope);
    pid = fork();
    if (pid != 0) return pid;
    execl(binary, binary, "--no_fork", "--no_chdir", "--no_close",
          "--if_name", cfg->interface_name,
          "--type", "tdn:NetworkVideoTransmitter",
          "--scope", scope, "--xaddr", xaddr, (char *)NULL);
    fprintf(stderr, "cannot start %s: %s\n", binary, strerror(errno));
    _exit(127);
}

static void stop_child(pid_t pid) {
    int i;
    if (pid <= 0) return;
    kill(pid, SIGTERM);
    for (i = 0; i < 20; ++i) {
        if (waitpid(pid, NULL, WNOHANG) == pid) return;
        usleep(50000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
}

int onvif_server_start(onvif_server_t **result,
                       const onvif_server_config_t *cfg) {
    onvif_server_t *server;
    if (!result || !cfg || !cfg->interface_name || !cfg->username ||
        !cfg->password || !cfg->device_name || !cfg->device_scope) return -1;
    server = calloc(1, sizeof(*server));
    if (!server) return -1;
    server->service_pid = start_service(cfg);
    if (server->service_pid <= 0) goto fail;
    server->discovery_pid = start_discovery(cfg);
    if (server->discovery_pid <= 0) goto fail;
    usleep(150000);
    if (!child_running(server->service_pid) ||
        !child_running(server->discovery_pid)) goto fail;
    printf("ONVIF gSOAP service started on port %d (%s, user=%s)\n",
           cfg->http_port, cfg->interface_name, cfg->username);
    *result = server;
    return 0;
fail:
    stop_child(server->discovery_pid);
    stop_child(server->service_pid);
    free(server);
    return -1;
}

void onvif_server_stop(onvif_server_t *server) {
    if (!server) return;
    stop_child(server->discovery_pid);
    stop_child(server->service_pid);
    free(server);
}

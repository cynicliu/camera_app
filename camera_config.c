#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "camera_config.h"

static char *trim(char *text) {
    char *end;

    while (isspace((unsigned char)*text)) ++text;
    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) --end;
    *end = '\0';
    return text;
}

static int copy_value(char *dst, size_t size, const char *value,
                      const char *key, unsigned int line_number) {
    if (strlen(value) >= size) {
        fprintf(stderr, "config line %u: %s is too long\n", line_number, key);
        return -1;
    }
    snprintf(dst, size, "%s", value);
    return 0;
}

static int parse_bool(const char *value, bool *result) {
    if (strcmp(value, "true") == 0 || strcmp(value, "yes") == 0 ||
        strcmp(value, "1") == 0 || strcmp(value, "on") == 0) {
        *result = true;
        return 0;
    }
    if (strcmp(value, "false") == 0 || strcmp(value, "no") == 0 ||
        strcmp(value, "0") == 0 || strcmp(value, "off") == 0) {
        *result = false;
        return 0;
    }
    return -1;
}

void camera_config_defaults(camera_config_t *config) {
    memset(config, 0, sizeof(*config));
    snprintf(config->iq_dir, sizeof(config->iq_dir), "/etc/iqfiles");
    config->rtsp_port = 554;
    snprintf(config->rtsp_path, sizeof(config->rtsp_path), "/live/0");
    config->onvif_enabled = true;
    config->onvif_port = 8080;
    snprintf(config->onvif_device_name, sizeof(config->onvif_device_name),
             "Luckfox Camera");
    snprintf(config->onvif_interface, sizeof(config->onvif_interface), "eth0");
    snprintf(config->onvif_username, sizeof(config->onvif_username), "admin");
    snprintf(config->onvif_password, sizeof(config->onvif_password), "admin");
    snprintf(config->frame_source, sizeof(config->frame_source), "vi");
    snprintf(config->video_codec, sizeof(config->video_codec), "h265");
    snprintf(config->audio_codec, sizeof(config->audio_codec), "aac");
}

int camera_config_load(camera_config_t *config, const char *path) {
    char line[1024];
    char *key = NULL;
    unsigned int line_number = 0;
    FILE *file = fopen(path, "r");

    if (!file) {
        fprintf(stderr, "cannot open config %s: %s\n", path, strerror(errno));
        return -1;
    }
    while (fgets(line, sizeof(line), file)) {
        char *value, *separator;
        long port;
        char *end;

        ++line_number;
        key = trim(line);
        if (!key[0] || key[0] == '#' || key[0] == ';') continue;
        separator = strchr(key, '=');
        if (!separator) {
            fprintf(stderr, "config line %u: expected key=value\n", line_number);
            fclose(file);
            return -1;
        }
        *separator = '\0';
        value = trim(separator + 1);
        key = trim(key);

        if (strcmp(key, "iq_dir") == 0) {
            if (copy_value(config->iq_dir, sizeof(config->iq_dir), value,
                           key, line_number)) goto invalid;
        } else if (strcmp(key, "rtmp_url") == 0) {
            if (copy_value(config->rtmp_url, sizeof(config->rtmp_url), value,
                           key, line_number)) goto invalid;
        } else if (strcmp(key, "rtsp_enabled") == 0) {
            if (parse_bool(value, &config->rtsp_enabled)) goto invalid_value;
        } else if (strcmp(key, "rtsp_port") == 0) {
            port = strtol(value, &end, 10);
            if (*end || port < 1 || port > 65535) goto invalid_value;
            config->rtsp_port = (int)port;
        } else if (strcmp(key, "rtsp_path") == 0) {
            if (value[0] != '/') goto invalid_value;
            if (copy_value(config->rtsp_path, sizeof(config->rtsp_path), value,
                           key, line_number)) goto invalid;
        } else if (strcmp(key, "onvif_enabled") == 0) {
            if (parse_bool(value, &config->onvif_enabled)) goto invalid_value;
        } else if (strcmp(key, "onvif_port") == 0) {
            port = strtol(value, &end, 10);
            if (*end || port < 1 || port > 65535) goto invalid_value;
            config->onvif_port = (int)port;
        } else if (strcmp(key, "onvif_device_name") == 0) {
            if (copy_value(config->onvif_device_name,
                           sizeof(config->onvif_device_name), value,
                           key, line_number)) goto invalid;
        } else if (strcmp(key, "onvif_interface") == 0) {
            if (copy_value(config->onvif_interface,
                           sizeof(config->onvif_interface), value,
                           key, line_number)) goto invalid;
        } else if (strcmp(key, "onvif_username") == 0) {
            if (copy_value(config->onvif_username,
                           sizeof(config->onvif_username), value,
                           key, line_number)) goto invalid;
        } else if (strcmp(key, "onvif_password") == 0) {
            if (copy_value(config->onvif_password,
                           sizeof(config->onvif_password), value,
                           key, line_number)) goto invalid;
        } else if (strcmp(key, "frame_source") == 0) {
            if (strcmp(value, "vi") != 0 && strcmp(value, "vpss") != 0)
                goto invalid_value;
            if (copy_value(config->frame_source, sizeof(config->frame_source), value,
                           key, line_number)) goto invalid;
        } else if (strcmp(key, "video_codec") == 0) {
            if (strcmp(value, "h264") != 0 && strcmp(value, "h265") != 0)
                goto invalid_value;
            if (copy_value(config->video_codec, sizeof(config->video_codec), value,
                           key, line_number)) goto invalid;
        } else if (strcmp(key, "audio_codec") == 0) {
            if (strcmp(value, "aac") != 0 && strcmp(value, "mp3") != 0)
                goto invalid_value;
            if (copy_value(config->audio_codec, sizeof(config->audio_codec), value,
                           key, line_number)) goto invalid;
        } else if (strcmp(key, "lvgl") == 0) {
            if (parse_bool(value, &config->lvgl)) goto invalid_value;
        } else {
            fprintf(stderr, "config line %u: unknown key %s\n", line_number, key);
            fclose(file);
            return -1;
        }
    }
    if (ferror(file)) {
        fprintf(stderr, "cannot read config %s\n", path);
        fclose(file);
        return -1;
    }
    fclose(file);
    return 0;

invalid_value:
    fprintf(stderr, "config line %u: invalid value for %s\n", line_number, key);
invalid:
    fclose(file);
    return -1;
}

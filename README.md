# Luckfox Pico Ultra W camera capture

This application initializes RKAIQ/ISP and a VI channel, then saves uncompressed
NV12 frames to a file. It is built for the RV1106 uClibc ARM target.

`camera_live` adds a 720x720 low-latency monitor for the Ultra W RGB panel.
Its control bar is implemented with the SDK-provided LVGL 8.2:

- camera preview through VI -> VPSS -> VO;
- microphone monitoring through AI -> AO;
- GT911 touch toolbar with play/pause, speaker mute and exit controls;
- `p`, `m`, and `q` keyboard fallbacks over a serial/SSH terminal.

## Build on the Ubuntu SDK host

```sh
cd /home/cynic/source/luckfox-pico
export RK_CHIP=rv1106
export RK_TOOLCHAIN_CROSS=arm-rockchip830-linux-uclibcgnueabihf
export RK_APP_TYPE=RKIPC_RV1106
export RK_ENABLE_LVGL=y
export PATH="$PWD/tools/linux/toolchain/$RK_TOOLCHAIN_CROSS/bin:$PATH"
make -C project/app/component/lvgl

# Build Live555 with the SDK's RV1106/uClibc toolchain.
make -C sysdrv/source/buildroot/buildroot-2023.02.6 O=output live555

cd /home/cynic/source/luckfox-pico/camera_app
make
file camera_capture
file camera_live
```

## Copy to the board

From the SDK host, replace the board address as needed:

```sh
scp camera_capture root@<board-ip>:/userdata/
scp camera_live root@<board-ip>:/userdata/
scp camera_live.conf root@<board-ip>:/userdata/
```

Live555 is linked dynamically. Install these Buildroot staging libraries in
the board's `/usr/lib`, preserving their SONAMEs:

```text
libliveMedia.so.94
libgroupsock.so.30
libBasicUsageEnvironment.so.1
libUsageEnvironment.so.3
libavcodec.so.58
libavutil.so.56
libswresample.so.3
libiconv.so.2
libz.so.1
```

The board also needs `libssl.so.1.1` and `libcrypto.so.1.1`, which are normally
already present in the matching firmware.

The firmware must also contain the matching sensor IQ JSON under
`/etc/iqfiles`. The Ultra W board configuration currently packages SC4336,
SC3336 and MIS5001 IQ files.

## Run on the board

Stop any service that already owns RKAIQ/VI before starting this application.

```sh
chmod +x /userdata/camera_capture
/userdata/camera_capture -w 1920 -h 1080 -n 30 \
  -a /etc/iqfiles -o /userdata/capture_1920x1080.nv12
```

View the resulting file on a PC with ffplay:

```sh
ffplay -f rawvideo -pixel_format nv12 -video_size 1920x1080 \
  capture_1920x1080.nv12
```

Run `./camera_capture --help` for all options. Channel 1 is the default ISP
self-path used by the SDK sample. Use a resolution supported by the active
sensor and ISP configuration.

For live monitoring, first stop any process that owns the camera, display, or
sound card, then run:

```sh
chmod +x /userdata/camera_live
/userdata/camera_live /etc/iqfiles
```

`camera_live` reads `/userdata/camera_live.conf` when it exists. Use another
file with `--config=/path/to/camera_live.conf`. Command-line IQ, RTMP,
`--lvgl`, `--frame-source`, `--video-codec`, and `--audio-codec` values
override the loaded configuration.

```ini
iq_dir=/etc/iqfiles
frame_source=vi
video_codec=h265
audio_codec=aac
lvgl=false
rtmp_url=rtmp://192.168.1.10/live/camera01
rtsp_enabled=true
rtsp_port=554
rtsp_path=/live/0
```

The framebuffer preview reads frames from VI by default. Select VPSS when the
preview should use its RGB888 output instead:

```sh
/userdata/camera_live /etc/iqfiles --lvgl --frame-source=vi
/userdata/camera_live /etc/iqfiles --lvgl --frame-source=vpss
```

Select H.264 or H.265 independently of the frame source:

```sh
/userdata/camera_live --video-codec=h264
/userdata/camera_live --video-codec=h265
```

Select AAC or MP3 audio independently:

```sh
/userdata/camera_live --audio-codec=aac
/userdata/camera_live --audio-codec=mp3
```

VPSS mode binds VI channel 1 to VPSS group 0/channel 0 and does not create or
bind a VO channel.

The RTMP encoder follows `--frame-source` as well. VI mode binds VI directly
to VENC. VPSS mode uses a separate 1920x1080 NV12 VPSS output channel for VENC,
while channel 0 remains the RGB888 framebuffer preview source.

To keep the local preview and publish video plus AAC or MP3 audio at the same
time, pass the RTMP destination as the second argument:

```sh
/userdata/camera_live /etc/iqfiles \
  'rtmp://192.168.1.10/live/camera01'
```

The stream is 1920x1080 at 25 fps, H.264 or H.265 CBR at 2 Mbit/s, with 16 kHz
mono AAC or MP3 at 32 kbit/s. Play/pause controls both local preview and RTMP output;
mute only affects the local speaker. The RTMP server and player must support
the selected video codec in FLV/RTMP.

When RTSP is enabled, the same encoder stream is available at:

```sh
ffplay rtsp://<board-ip>:554/live/0
```

The RTSP server uses Live555 and publishes the selected H.264 or H.265 video
with 16 kHz mono AAC or MP3. AAC uses MPEG4-GENERIC/AAC-hbr RTP and is encoded
by the SDK's FFmpeg `libavcodec`; its AudioSpecificConfig is `1408`. MP3 uses
Live555's MPEG audio RTP sink and the Rockchip software MP3 encoder exported by
`librkaudio`. Neither path depends on RK AENC. RTMP carries the same selected
video and audio codecs.

Encoded VENC frames are dispatched to every enabled streaming protocol.
`media_callbacks.c` owns video frame retrieval and release; `aac_encoder.c`
and `audio_encoder.c` own PCM accumulation and software audio encoding.

The microphone is played through the nearby speaker, so acoustic feedback is
possible. Start with low volume and keep the microphone away from the speaker.

## Run the x86_64 simulator

The simulator and RV1106 application share `ui/camera_ui.c`. Only the camera,
display, touch, and audio backends differ.

Install SDL2 development files once:

```sh
sudo apt install libsdl2-dev
```

Build and run with the default USB camera:

```sh
cd /home/cynic/source/luckfox-pico/camera_app
make simulator
./camera_simulator /dev/video0
```

The camera must support 640x480 YUYV. If it cannot be opened, the simulator
automatically displays an animated test pattern. Mouse clicks operate the LVGL
buttons. The native title bar supports dragging, minimizing, maximizing, and
restoring. Space toggles preview, `m` toggles audio, F11 toggles
maximize/restore, Ctrl+M minimizes, `r` restores, and Escape exits. Window
resizing preserves the internal 720x720 coordinate system and touch mapping.

Automated headless verification:

```sh
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
CAMERA_SIM_FRAMES=40 CAMERA_SIM_SCREENSHOT=/tmp/camera_simulator.bmp \
./camera_simulator /dev/nonexistent
```

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
```

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

The framebuffer preview reads frames from VI by default. Select VPSS when the
preview should use its RGB888 output instead:

```sh
/userdata/camera_live /etc/iqfiles --lvgl --frame-source=vi
/userdata/camera_live /etc/iqfiles --lvgl --frame-source=vpss
```

VPSS mode binds VI channel 1 to VPSS group 0/channel 0 and does not create or
bind a VO channel.

To keep the local preview and publish H.265 video plus AAC audio at the same
time, pass the RTMP destination as the second argument:

```sh
/userdata/camera_live /etc/iqfiles \
  'rtmp://192.168.1.10/live/camera01'
```

The stream is 1920x1080 at 25 fps, H.265 CBR at 2 Mbit/s, with 16 kHz mono
AAC at 32 kbit/s. Play/pause controls both local preview and RTMP output;
mute only affects the local speaker. The RTMP server and player must support
H.265 in FLV/RTMP.

Encoded VENC and AENC frames are delivered to the RTMP writer through
registered callbacks. `media_callbacks.c` owns frame retrieval and release,
following the callback lifecycle used by RKADK's `rkadk_rtmp.c`.

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

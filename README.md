# vaapi-codec-example

基于 FFmpeg `AVHWDeviceContext` / `AVHWFramesContext` 调用 VAAPI 的硬件编解码示例工程，
覆盖硬件编码、硬件解码、零拷贝转码、播放和运行环境自检。AMD（radeonsi）、Intel（iHD）通用，
不依赖任何私有 SDK。

## 功能

| 命令 | 功能 |
|------|------|
| `info` | 查看驱动、VA-API 版本、显卡支持的 profile/entrypoint、FFmpeg 硬件编解码器 |
| `encode` | 硬件编码：YUV（I420/NV12）→ H.264 / H.265，支持 CQP 与 VBR 两种码控 |
| `decode` | 硬件解码：MP4 / H.264 / H.265 → YUV（NV12 或 I420），可存首帧截图 |
| VPP 缩放 | 编码前 / 解码后 / 转码中调用 VAAPI VideoProc 在显存内缩放（`-S 1280x720`） |
| 零拷贝显示 | 解码帧导出 dmabuf → EGLImage → GL 纹理，画面不经过 CPU（`play --zero-copy`） |
| `play` | 解码后实时播放（EGL 窗口，支持 Wayland / X11 两种后端，`--zero-copy` 走 dmabuf） |
| `transcode` | 解码 → 编码，默认零拷贝：帧数据全程留在显存 |
| `selftest` | 一键自检：编码 → 解码 → PSNR 比对，判定整条硬件链路是否正常 |

VPP 走 FFmpeg 的 `scale_vaapi` 过滤器（VAAPI VideoProc 通道），输入输出都是硬件帧，
缩放过程不把画面拷回 CPU。

本机实测（AMD Radeon RX 7900 XTX，1920×1080）：编码约 205 fps，解码约 100–140 fps，
零拷贝转码约 390–460 fps（同场景 CPU 中转仅 158 fps），编解码往返 PSNR 37.9 dB。

## 环境要求

- 支持 VAAPI 的 AMD / Intel 显卡，`vainfo` 能正常列出 profile；
- FFmpeg 开发库（编译时需 `--enable-vaapi`）、libva、CMake ≥ 3.16、GCC；
- EGL 开发库，以及窗口后端之一：Wayland（wayland-client/wayland-egl）或 X11（libX11），
  仅 `play` 窗口与零拷贝显示需要。不使用 GLFW（GLFW 3.3 不支持 Wayland）。
  纯 X 环境或 XWayland 下用 `--backend x11`；桌面是 Wayland 时默认自动选 Wayland。

```shell
# Ubuntu / Kylin
sudo apt install build-essential cmake pkg-config vainfo \
                 libva-dev libva-drm2 libdrm-dev \
                 libavcodec-dev libavformat-dev libavutil-dev libavfilter-dev libswscale-dev
sudo apt install libwayland-dev libegl-dev  # 可选：Wayland 窗口
sudo apt install libx11-dev                # 可选：X11 窗口（纯 X / XWayland）

vainfo                               # 先确认驱动可用
```

## 构建与运行

```shell
make            # 编译，产物 build/bin/vaapi_demo
make install    # 安装到 ./install（自包含目录，可整体拷贝分发）
./run           # 交互菜单
```

也可以直接使用 CMake：`cmake -S . -B build && cmake --build build -j`。
其他目标：`make run`（编译并进菜单）、`make selftest`、`make clean`。

交互菜单：

```
1) 环境自检            6) 零拷贝转码 MP4 -> H.264
2) 编码 YUV -> H.264   7) 零拷贝转码 MP4 -> H.265
3) 编码 YUV -> H.265   8) 一键全流程自检
4) 解码 MP4 -> YUV     9) 完整帮助
5) 解码并播放          q) 退出
```

`run` 也可直接透传命令：`./run selftest`、`./run encode -c hevc`。

## 命令速查

所有命令都带默认值，不带参数时使用 `samples/` 下的示例文件。

```shell
vaapi_demo info
vaapi_demo encode                                   # 示例 YUV -> output/encoded.h264
vaapi_demo encode -c hevc -b 4000                   # H.265，4Mbps 码率控制
vaapi_demo encode -i in.nv12 -p nv12 -s 1280x720    # 指定输入格式与分辨率
vaapi_demo encode -S 1280x720                       # 编码前用 VPP 缩放到 720p
vaapi_demo decode --snapshot first.ppm              # 解码并保存首帧，便于无界面环境查看
vaapi_demo decode -p nv12 --max-frames 60           # 只解前 60 帧
vaapi_demo decode -S 640x360 --snapshot small.ppm   # 解码后 VPP 缩放到 360p
vaapi_demo play -i samples/video1080.mp4
vaapi_demo play --backend x11                      # 指定 X11 窗口（纯 X 环境 / XWayland）
vaapi_demo play --zero-copy                         # 解码帧零拷贝进 GL（dmabuf -> EGLImage）
vaapi_demo transcode -c hevc -o output/out.h265     # 零拷贝转码
vaapi_demo transcode -S 1280x720                    # 转码同时缩放到 720p
vaapi_demo transcode --no-zero-copy                 # CPU 中转，用于性能对比
vaapi_demo selftest
vaapi_demo dmabuf                                   # 导出 DRM PRIME 并打印描述符（无需显示环境）
vaapi_demo dmabuf --gl                              # 无头环境下导入并读回比对（验证零拷贝链路）
```

常用选项：

| 选项 | 说明 |
|------|------|
| `-i / -o` | 输入 / 输出文件 |
| `-s 宽x高`、`-p i420\|nv12` | 编码输入规格（原始 YUV 必须与文件一致） |
| `-c h264\|hevc` | 编解码格式，默认 h264 |
| `-b <kbps>` / `--qp <n>` | 码率控制（VBR）/ 恒定质量，默认 CQP 26 |
| `-g <帧数>`、`--bframes <n>` | GOP、连续 B 帧（默认等于帧率、3） |
| `-S, --scale <宽x高>` | VPP 缩放目标尺寸，如 `1280x720`（默认不缩放，宽高需为偶数） |
| `--zero-copy` | `play` 专用：走 dmabuf 零拷贝显示，不支持时自动回退 CPU 上传 |
| `--backend <wayland\|x11\|auto>` | `play` 窗口后端，默认按环境自动选择 |
| `--max-frames <n>` | 只处理前 n 帧 |
| `-d <节点>`、`--connection-type drm\|x11` | 多显卡指定设备；默认 drm，适合无界面服务器 |

各命令完整选项：`vaapi_demo <命令> --help`。

## 示例数据

| 文件 | 说明 |
|------|------|
| `samples/video1080.mp4` | 1920×1080 H.264，150 帧，用于解码 / 转码 / 播放 |
| `samples/1920x1080_24_i420.yuv` | 编码输入，63MB，**未纳入仓库** |

原始 YUV 体积较大，仓库中不保存，需要时执行 `./samples/make_sample.sh` 生成
（依赖 ffmpeg，从 `video1080.mp4` 抽取 21 帧并转为 I420）。

## 实现要点

VAAPI 调用链（编码方向，解码方向对称）：

```
av_hwdevice_ctx_create(AV_HWDEVICE_TYPE_VAAPI)      打开 /dev/dri/renderD128
av_hwframe_ctx_alloc/init                           建立 GPU 表面池（NV12）
avcodec_open2("h264_vaapi" / "hevc_vaapi")          打开硬件编码器
av_hwframe_get_buffer + av_hwframe_transfer_data    内存图像上传到 GPU
avcodec_send_frame / avcodec_receive_packet         取回码流
```

VPP 缩放调用 VAAPI 的 VideoProc 通道（`scale_vaapi`），编码前 / 解码后都只是在显存里
接一个缩放环节，不引入额外的内存往返：

```c
VaapiVpp *vpp = vaapi_vpp_create(device_ctx, NULL, 1920, 1080, 1280, 720, 25);
AVFrame *scaled = NULL;
vaapi_vpp_process(vpp, hw_frame_1080p, &scaled);   /* 输出 1280x720 硬件帧 */
```

窗口与显示完全走 EGL，后端可选 Wayland（`wayland-client` + `wayland-egl`）或 X11（libX11），
没有 GLFW 依赖。X11 后端已用 XWayland 验证（`--backend x11`）：窗口与 CPU 上传路径正常；
该环境的 XWayland GL 是 llvmpipe 软件渲染，因此 dmabuf 零拷贝会自动回退到 CPU 上传。
在 GPU 直通的 X 桌面（DRI3 正常）上，X11 后端同样可以使用零拷贝。

零拷贝显示是"导出 + 导入"两段接口：

```c
/* 1. 导出：VAAPI 表面 -> dmabuf 描述符（fd/offset/pitch/modifier） */
vaExportSurfaceHandle(display, surface,
                      VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                      VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS,
                      &desc);

/* 2. 导入：dmabuf -> EGLImage -> GL 纹理，着色器直接采样 */
EGLImageKHR image = eglCreateImageKHR(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
```

注意两点：AMD/Intel 导出的表面常带 tiled modifier，EGL 导入时必须把
`EGL_DMA_BUF_PLANE0_MODIFIER_LO/HI_EXT` 一并传入；表面高度会按 16 像素对齐
（1080 -> 1088），渲染时按可见尺寸裁剪。

零拷贝转码直接复用解码器的设备与帧池，编码器不再自己建池：

```c
config.hw_device_ctx = vaapi_decoder_device_ctx(decoder);
config.hw_frames_ctx = frame->hw_frames_ctx;   /* 解码输出帧自带帧池 */
```

源码索引：

| 文件 | 内容 |
|------|------|
| `src/vaapi_encoder.c` | 硬件编码封装，可独立复用 |
| `src/vaapi_decoder.c` | 硬件解码封装，可独立复用 |
| `src/vaapi_vpp.c` | VPP 缩放封装（scale_vaapi / VideoProc） |
| `src/vaapi_dmabuf.c` | VAAPI 表面导出为 DRM PRIME（vaExportSurfaceHandle） |
| `src/egl_interop.c` | dmabuf -> EGLImage -> GL 纹理（需要 EGL，编译期可选） |
| `src/vaapi_common.c` | 设备创建（DRM）、图像上传、错误码转换 |
| `src/vaapi_info.c` | 驱动与显卡能力自检 |
| `src/main.c` | 命令行入口与各子命令，含零拷贝转码实现 |
| `src/renderer.c` | OpenGL NV12 显示（可选） |

`vaapi_encoder.*` / `vaapi_decoder.*` 不依赖 CLI 代码，可直接拷贝到自有工程使用。

## 常见问题

**创建 VAAPI 设备失败**：确认 `vainfo` 正常、用户在 `video`/`render` 组、多显卡用 `-d` 指定
`/dev/dri/renderD128`；服务器保持 `--connection-type drm`（默认值）。

**提示未提供 h264_vaapi**：系统 FFmpeg 编译时未启用 VAAPI，用
`ffmpeg -hide_banner -encoders | grep vaapi` 确认。

**没有播放功能**：按需安装 `libwayland-dev libegl-dev`（Wayland）或 `libx11-dev libegl-dev`（X11）
后 `make clean && make`；无图形界面时用 `decode --snapshot` 保存图片查看画面。

**播放时日志出现 `invalid method 8, object xdg_surface`**：这是 Mutter 的已知行为，
创建窗口后已先调用 `xdg_toplevel.set_parent`，窗口仍会正常呈现，不影响播放；如遇到
画面不显示，可用 `dmabuf --gl` 无头验证导入链路。

**`play --zero-copy` 提示回退 CPU 上传**：说明当前 GL 上下文不是 EGL 创建、驱动缺少
`EGL_EXT_image_dma_buf_import`，或 GL 是 llvmpipe/软件渲染（XWayland 常见）。
安装 `libegl-dev` 重新编译，X 环境下确认 DRI3 可用；也可用 `dmabuf --gl` 无头验证导入链路。

**H.265 输出为 1920×1088**：按 16 像素对齐的正常现象，与 FFmpeg 官方 `hevc_vaapi` 一致。

**驱动不支持 B 帧 / CQP**：程序会自动退化为无 B 帧、VBR 模式，日志中有提示。

## 目录结构

```
├── src/                编解码封装、CLI、播放、工具
├── third_party/glad/   OpenGL 函数加载器（仅播放使用）
├── samples/            示例数据与生成脚本
├── Makefile / run      一键构建与交互菜单
└── CMakeLists.txt
```

## 版本

vaapi_demo 1.0.0，已验证 FFmpeg 7.1（含 libavfilter/scale_vaapi）/ libva 1.20 /
Mesa 25.2（AMD RX 7900 XTX）/ Ubuntu 24.04（GNOME Wayland 会话）。

零拷贝显示链路（vaExportSurfaceHandle -> EGLImage -> GL 纹理）已在 AMD RX 7900 XTX 上
用 EGL surfaceless + FBO 读回验证，Y/UV 平面与 CPU 解码结果逐像素一致。

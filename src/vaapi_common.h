/*
 * vaapi_common.h - VAAPI 编解码示例公共定义
 *
 * 本文件只做两件事：
 *   1. 统一日志输出；
 *   2. 封装 VAAPI 硬件设备的创建（DRM / X11 两种连接方式）。
 *
 * 所有示例代码均基于 FFmpeg 的 AVHWDeviceContext + AVHWFramesContext
 * 这套标准接口访问 VAAPI，这也是客户在自己的产品代码里应当沿用的方式。
 */
#ifndef VAAPI_COMMON_H
#define VAAPI_COMMON_H

#include <stdint.h>
#include <stdio.h>

#include <libavutil/avutil.h>
#include <libavutil/buffer.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>

#define VAAPI_TAG "[vaapi] "

#define LOG_INFO(fmt, ...)  fprintf(stdout, VAAPI_TAG fmt "\n", ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  fprintf(stderr, VAAPI_TAG "警告: " fmt "\n", ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) fprintf(stderr, VAAPI_TAG "错误: " fmt "\n", ##__VA_ARGS__)

/*
 * 把 FFmpeg 的负错误码转换成可读字符串。
 * 返回的指针指向线程局部缓冲区，可以连续格式化多个错误。
 */
const char *vaapi_error_string(int errnum);

/*
 * 创建 VAAPI 硬件设备（AVHWDeviceContext）。
 *
 * @param device        DRM 设备节点，例如 "/dev/dri/renderD128"；
 *                      传 NULL 表示让 libva 自动选择。
 * @param connection_type  "drm" / "x11" / NULL(自动)。
 *                      服务器无图形界面时必须用 "drm"，这也是本示例的默认值。
 * @return 成功返回设备引用，失败返回 NULL（调用方用 av_buffer_unref 释放）。
 */
AVBufferRef *vaapi_device_create(const char *device, const char *connection_type);

/*
 * 解析 "<宽>x<高>" 形式的字符串，成功返回 0。
 */
int vaapi_parse_size(const char *text, int *width, int *height);

#endif /* VAAPI_COMMON_H */

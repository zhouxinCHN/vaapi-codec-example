/*
 * util.h - 与 VAAPI 无关的通用小工具：
 *          计时、进度显示、YUV 原始帧读写、PPM 截图、PSNR 计算、示例文件定位。
 */
#ifndef UTIL_H
#define UTIL_H

#include <stdint.h>
#include <stdio.h>

#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>

/* ---------- 时间 ---------- */
int64_t util_now_us(void);

/* ---------- 文件 ---------- */
int util_file_exists(const char *path);
int64_t util_file_size(const char *path);
const char *util_basename(const char *path);

/*
 * 查找随本 Demo 发布的示例文件（samples 目录）。
 * 查找顺序: $VAAPI_DEMO_SAMPLES -> ./samples -> <exe>/../samples -> <exe>/samples
 *          -> <安装目录>/share/vaapi_demo/samples
 * 找到返回 1 并写入 path；找不到返回 0。
 */
int util_find_sample(const char *name, char *path, size_t size);

/* ---------- YUV 原始帧读写（支持 I420 / YUV420P / NV12） ---------- */
int util_read_yuv_frame(FILE *file, AVFrame *frame);
int util_write_yuv_frame(FILE *file, const AVFrame *frame);

/* 把一帧图像转成 RGB24 并写成 PPM(P6)，用于无显示环境下的画面校验。 */
int util_write_ppm(const char *path, const AVFrame *frame);

/* ---------- PSNR ---------- */
typedef struct {
    double sum;      /* 累计平方误差 */
    int64_t count;   /* 累计采样点个数 */
} PsnrAccum;

void psnr_accumulate(PsnrAccum *acc, const uint8_t *ref, int ref_linesize,
                     const uint8_t *dst, int dst_linesize, int width, int height);
double psnr_value(const PsnrAccum *acc);

/* ---------- 进度显示 ---------- */
typedef struct {
    int64_t start_us;
    int64_t last_us;
    int64_t frames;
    int64_t bytes;
    int tty;
    int quiet;
} Progress;

void progress_init(Progress *p, int quiet);
void progress_tick(Progress *p, int64_t bytes);
void progress_finish(Progress *p);

/* 把字节数/码率格式化成人类可读字符串。 */
const char *util_format_size(int64_t bytes);
const char *util_format_bitrate(double bits_per_second);

#endif /* UTIL_H */

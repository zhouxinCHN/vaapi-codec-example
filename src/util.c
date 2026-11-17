#include "util.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#ifndef VAAPI_DEMO_SHARE_DIR
#define VAAPI_DEMO_SHARE_DIR "/usr/local/share/vaapi_demo"
#endif

int64_t util_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

int util_file_exists(const char *path)
{
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

int64_t util_file_size(const char *path)
{
    struct stat st;
    if (!path || stat(path, &st) != 0)
        return -1;
    return (int64_t)st.st_size;
}

const char *util_basename(const char *path)
{
    const char *slash;

    if (!path)
        return "";
    slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* 取可执行文件所在目录，失败返回空串。 */
static void executable_dir(char *buf, size_t size)
{
    ssize_t len = readlink("/proc/self/exe", buf, size - 1);

    if (len <= 0) {
        buf[0] = '\0';
        return;
    }
    buf[len] = '\0';
    char *slash = strrchr(buf, '/');
    if (slash)
        *slash = '\0';
    else
        buf[0] = '\0';
}

int util_find_sample(const char *name, char *path, size_t size)
{
    static const char *patterns[] = {
        "samples/%s",
        "%s/../samples/%s",
        "%s/samples/%s",
        "%s/../../samples/%s",
    };
    char exe_dir[4000];
    char candidate[4096];
    const char *env;

    if (!name || !*name || !path || size == 0)
        return 0;

    env = getenv("VAAPI_DEMO_SAMPLES");
    if (env && *env) {
        snprintf(candidate, sizeof(candidate), "%s/%s", env, name);
        if (util_file_exists(candidate)) {
            snprintf(path, size, "%s", candidate);
            return 1;
        }
    }

    snprintf(candidate, sizeof(candidate), patterns[0], name);
    if (util_file_exists(candidate)) {
        snprintf(path, size, "%s", candidate);
        return 1;
    }

    executable_dir(exe_dir, sizeof(exe_dir));
    if (exe_dir[0]) {
        for (size_t i = 1; i < sizeof(patterns) / sizeof(patterns[0]); i++) {
            snprintf(candidate, sizeof(candidate), patterns[i], exe_dir, name);
            if (util_file_exists(candidate)) {
                snprintf(path, size, "%s", candidate);
                return 1;
            }
        }
    }

    snprintf(candidate, sizeof(candidate), "%s/samples/%s", VAAPI_DEMO_SHARE_DIR, name);
    if (util_file_exists(candidate)) {
        snprintf(path, size, "%s", candidate);
        return 1;
    }

    return 0;
}

/* ---------- YUV 原始帧 ---------- */

static int plane_layout(enum AVPixelFormat format, int width, int height, int plane,
                        int *plane_width, int *plane_height, int *pixel_bytes)
{
    switch (format) {
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P:
        if (plane == 0) {
            *plane_width  = width;
            *plane_height = height;
        } else if (plane < 3) {
            *plane_width  = (width + 1) / 2;
            *plane_height = (height + 1) / 2;
        } else {
            return -1;
        }
        *pixel_bytes = 1;
        return 0;

    case AV_PIX_FMT_NV12:
        if (plane == 0) {
            *plane_width  = width;
            *plane_height = height;
            *pixel_bytes  = 1;
        } else if (plane == 1) {
            *plane_width  = (width + 1) / 2;
            *plane_height = (height + 1) / 2;
            *pixel_bytes  = 2;
        } else {
            return -1;
        }
        return 0;

    default:
        return -1;
    }
}

int util_read_yuv_frame(FILE *file, AVFrame *frame)
{
    int planes = frame->format == AV_PIX_FMT_NV12 ? 2 : 3;

    for (int p = 0; p < planes; p++) {
        int pw, ph, bytes, row_bytes;

        if (plane_layout(frame->format, frame->width, frame->height, p, &pw, &ph, &bytes) < 0)
            return 0;
        row_bytes = pw * bytes;
        for (int y = 0; y < ph; y++) {
            uint8_t *dst = frame->data[p] + (ptrdiff_t)y * frame->linesize[p];
            if (fread(dst, 1, (size_t)row_bytes, file) != (size_t)row_bytes)
                return 0; /* 文件结束或数据不完整 */
        }
    }
    return 1;
}

int util_write_yuv_frame(FILE *file, const AVFrame *frame)
{
    int planes = frame->format == AV_PIX_FMT_NV12 ? 2 : 3;

    for (int p = 0; p < planes; p++) {
        int pw, ph, bytes, row_bytes;

        if (plane_layout(frame->format, frame->width, frame->height, p, &pw, &ph, &bytes) < 0)
            return -1;
        row_bytes = pw * bytes;
        for (int y = 0; y < ph; y++) {
            const uint8_t *src = frame->data[p] + (ptrdiff_t)y * frame->linesize[p];
            if (fwrite(src, 1, (size_t)row_bytes, file) != (size_t)row_bytes)
                return -1;
        }
    }
    return 0;
}

int util_write_ppm(const char *path, const AVFrame *frame)
{
    struct SwsContext *sws;
    AVFrame *rgb;
    FILE *file;
    int ret = -1;

    if (!path || !frame || !frame->data[0])
        return -1;

    rgb = av_frame_alloc();
    if (!rgb)
        return -1;
    rgb->format = AV_PIX_FMT_RGB24;
    rgb->width  = frame->width;
    rgb->height = frame->height;
    if (av_frame_get_buffer(rgb, 32) < 0)
        goto end;

    sws = sws_getContext(frame->width, frame->height, (enum AVPixelFormat)frame->format,
                         frame->width, frame->height, AV_PIX_FMT_RGB24,
                         SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws)
        goto end;
    sws_scale(sws, (const uint8_t * const *)frame->data, frame->linesize, 0,
              frame->height, rgb->data, rgb->linesize);
    sws_freeContext(sws);

    file = fopen(path, "wb");
    if (!file)
        goto end;
    fprintf(file, "P6\n%d %d\n255\n", rgb->width, rgb->height);
    for (int y = 0; y < rgb->height; y++)
        fwrite(rgb->data[0] + (ptrdiff_t)y * rgb->linesize[0], 1, (size_t)rgb->width * 3, file);
    fclose(file);
    ret = 0;

end:
    av_frame_free(&rgb);
    return ret;
}

/* ---------- PSNR ---------- */

void psnr_accumulate(PsnrAccum *acc, const uint8_t *ref, int ref_linesize,
                     const uint8_t *dst, int dst_linesize, int width, int height)
{
    for (int y = 0; y < height; y++) {
        const uint8_t *r = ref + (ptrdiff_t)y * ref_linesize;
        const uint8_t *d = dst + (ptrdiff_t)y * dst_linesize;

        for (int x = 0; x < width; x++) {
            int diff = (int)r[x] - (int)d[x];
            acc->sum += (double)diff * diff;
        }
        acc->count += width;
    }
}

double psnr_value(const PsnrAccum *acc)
{
    double mse;

    if (!acc || acc->count == 0)
        return 0.0;
    mse = acc->sum / (double)acc->count;
    if (mse <= 0.0)
        return 99.0;
    return 10.0 * log10((255.0 * 255.0) / mse);
}

/* ---------- 进度 ---------- */

void progress_init(Progress *p, int quiet)
{
    memset(p, 0, sizeof(*p));
    p->start_us = util_now_us();
    p->last_us  = p->start_us;
    p->tty      = isatty(STDOUT_FILENO);
    p->quiet    = quiet;
}

void progress_tick(Progress *p, int64_t bytes)
{
    int64_t now;

    p->frames++;
    p->bytes += bytes;

    now = util_now_us();
    if (p->quiet || now - p->last_us < 500000)
        return;
    p->last_us = now;

    double elapsed = (now - p->start_us) / 1000000.0;
    double fps     = elapsed > 0 ? p->frames / elapsed : 0.0;
    double bitrate = elapsed > 0 ? p->bytes * 8.0 / elapsed : 0.0;

    if (p->tty) {
        fprintf(stdout, "\r[进度] %d 帧 | %.1f fps | %s | %s   ",
                (int)p->frames, fps, util_format_bitrate(bitrate), util_format_size(p->bytes));
        fflush(stdout);
    } else {
        fprintf(stdout, "[进度] %d 帧 | %.1f fps | %s | %s\n",
                (int)p->frames, fps, util_format_bitrate(bitrate), util_format_size(p->bytes));
    }
}

void progress_finish(Progress *p)
{
    if (p->tty && !p->quiet) {
        fputc('\r', stdout);
        fflush(stdout);
    }
}

static _Thread_local char size_buffers[4][64];
static _Thread_local int size_buffer_index;

const char *util_format_size(int64_t bytes)
{
    char *buf = size_buffers[size_buffer_index++ & 3];

    if (bytes < 1024)
        snprintf(buf, 64, "%lld B", (long long)bytes);
    else if (bytes < 1024 * 1024)
        snprintf(buf, 64, "%.1f KB", bytes / 1024.0);
    else if (bytes < 1024LL * 1024 * 1024)
        snprintf(buf, 64, "%.1f MB", bytes / (1024.0 * 1024.0));
    else
        snprintf(buf, 64, "%.2f GB", bytes / (1024.0 * 1024.0 * 1024.0));
    return buf;
}

const char *util_format_bitrate(double bits_per_second)
{
    static _Thread_local char buffers[4][64];
    static _Thread_local int index;
    char *buf = buffers[index++ & 3];

    if (bits_per_second < 1000)
        snprintf(buf, 64, "%.0f bps", bits_per_second);
    else if (bits_per_second < 1000 * 1000)
        snprintf(buf, 64, "%.1f Kbps", bits_per_second / 1000.0);
    else
        snprintf(buf, 64, "%.2f Mbps", bits_per_second / (1000.0 * 1000.0));
    return buf;
}

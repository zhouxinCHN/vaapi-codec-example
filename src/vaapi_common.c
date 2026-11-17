#include "vaapi_common.h"

#include <stdlib.h>
#include <string.h>

#include <libavutil/opt.h>

static _Thread_local char error_buffer[AV_ERROR_MAX_STRING_SIZE];

const char *vaapi_error_string(int errnum)
{
    if (errnum >= 0) {
        snprintf(error_buffer, sizeof(error_buffer), "成功");
        return error_buffer;
    }
    if (av_strerror(errnum, error_buffer, sizeof(error_buffer)) < 0)
        snprintf(error_buffer, sizeof(error_buffer), "未知错误码 %d", errnum);
    return error_buffer;
}

AVBufferRef *vaapi_device_create(const char *device, const char *connection_type)
{
    AVBufferRef *device_ctx = NULL;
    AVDictionary *options = NULL;
    const char *type = (connection_type && *connection_type) ? connection_type : NULL;
    int err;

    if (type)
        av_dict_set(&options, "connection_type", type, 0);

    err = av_hwdevice_ctx_create(&device_ctx, AV_HWDEVICE_TYPE_VAAPI, device, options, 0);
    av_dict_free(&options);

    if (err < 0) {
        /* 指定了连接方式但失败时，退回 libva 的自动探测再试一次。 */
        if (type) {
            LOG_WARN("使用 connection_type=%s 创建设备失败(%s)，改为自动探测重试",
                     type, vaapi_error_string(err));
            err = av_hwdevice_ctx_create(&device_ctx, AV_HWDEVICE_TYPE_VAAPI, device, NULL, 0);
        }
        if (err < 0) {
            LOG_ERROR("创建 VAAPI 硬件设备失败: %s", vaapi_error_string(err));
            LOG_ERROR("请确认: 1) 已安装显卡驱动 (vainfo 可正常输出); "
                      "2) 当前用户对 %s 有读写权限; 3) 设备节点是否正确 (-d 参数)",
                      device ? device : "/dev/dri/renderD*");
            return NULL;
        }
    }

    return device_ctx;
}

int vaapi_parse_size(const char *text, int *width, int *height)
{
    char *end = NULL;
    long w, h;

    if (!text)
        return -1;

    w = strtol(text, &end, 10);
    if (end == text || (*end != 'x' && *end != 'X' && *end != '*'))
        return -1;
    h = strtol(end + 1, &end, 10);
    if (end == text || w <= 0 || h <= 0)
        return -1;

    *width  = (int)w;
    *height = (int)h;
    return 0;
}

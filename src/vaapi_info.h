/*
 * vaapi_info.h - 运行环境自检：打印驱动、VA-API 能力、FFmpeg 硬件编解码器支持情况。
 */
#ifndef VAAPI_INFO_H
#define VAAPI_INFO_H

#include "vaapi_common.h"

/*
 * 打印当前机器的 VAAPI 环境信息。
 * @param device  设备节点，NULL 表示自动选择
 * @return 0 表示 VAAPI 可用
 */
int vaapi_print_environment(const char *device);

#endif /* VAAPI_INFO_H */

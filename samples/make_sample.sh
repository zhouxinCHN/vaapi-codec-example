#!/usr/bin/env bash
#
# 生成编码测试用的 1920x1080 I420 原始素材（默认 21 帧，约 63MB）。
# 优先从同目录的 video1080.mp4 抽取画面；若文件不存在，则用 ffmpeg 内置测试图案生成。
#
# 用法: ./samples/make_sample.sh
#
set -euo pipefail

cd "$(dirname "$0")"

OUT="1920x1080_24_i420.yuv"
FRAMES=21

if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "错误: 未找到 ffmpeg，请先安装 (sudo apt install ffmpeg)" >&2
    exit 1
fi

if [ -f video1080.mp4 ]; then
    echo "从 video1080.mp4 生成 ${OUT} ..."
    ffmpeg -hide_banner -v warning -i video1080.mp4 \
        -vf "scale=1920:1080,fps=25" -frames:v "${FRAMES}" \
        -pix_fmt yuv420p -f rawvideo -y "${OUT}"
else
    echo "未找到 video1080.mp4，使用测试图案生成 ${OUT} ..."
    ffmpeg -hide_banner -v warning \
        -f lavfi -i "testsrc2=size=1920x1080:rate=25" -frames:v "${FRAMES}" \
        -pix_fmt yuv420p -f rawvideo -y "${OUT}"
fi

echo "已生成: $(ls -lh "${OUT}" | awk '{print $5, $9}')"
echo "编码示例: ./build/bin/vaapi_demo encode -i samples/${OUT}"

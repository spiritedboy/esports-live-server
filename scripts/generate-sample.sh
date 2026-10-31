#!/usr/bin/env bash
# FFmpeg 命令行仅生成测试素材，核心 Remux 由 esports_remux 执行。
set -euo pipefail
output="${1:-build/samples/input.mp4}"
mkdir -p -- "$(dirname -- "$output")"
# -n拒绝覆盖；-bf 2刻意生成含B帧的输入，便于验证PTS/DTS和解码排空。
ffmpeg -hide_banner -loglevel error -n \
  -f lavfi -i testsrc2=size=320x180:rate=25 \
  -f lavfi -i sine=frequency=1000:sample_rate=48000 \
  -t 3 -c:v libx264 -preset fast -bf 2 -g 25 -pix_fmt yuv420p \
  -c:a aac "$output"
printf '样片已生成：%s\n' "$output"

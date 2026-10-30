#!/usr/bin/env bash
# 只生成合成测试素材，实际合成由 esports_composite 完成。
set -euo pipefail
directory="${1:-build/samples/mix}"
mkdir -p -- "$directory"
colors=(red lime blue yellow)
sizes=(160x120 256x144 320x180 128x96)
rates=(15 30 24 25)
for i in 0 1 2 3; do
  ffmpeg -hide_banner -loglevel error -n \
    -f lavfi -i "color=c=${colors[$i]}:s=${sizes[$i]}:r=${rates[$i]}" \
    -t 3 -c:v libx264 -bf 2 "$directory/${colors[$i]}.mp4"
done
printf '四路不同尺寸/帧率素材已生成：%s\n' "$directory"

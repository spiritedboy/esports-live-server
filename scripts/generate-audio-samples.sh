#!/usr/bin/env bash
# AAC 游戏声和 Opus 解说声的可复现测试素材。
set -euo pipefail
# 两种频率便于用频谱区分各路贡献，44.1kHz与48kHz输入用于验证重采样。
directory="${1:-build/samples/audio}"
mkdir -p -- "$directory"
ffmpeg -hide_banner -loglevel error -n \
  -f lavfi -i sine=frequency=440:sample_rate=44100:duration=3 \
  -c:a aac "$directory/game.m4a"
ffmpeg -hide_banner -loglevel error -n \
  -f lavfi -i sine=frequency=880:sample_rate=48000:duration=3 \
  -c:a libopus "$directory/voice.opus"
printf '两路音频样片已生成：%s\n' "$directory"

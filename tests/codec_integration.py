#!/usr/bin/env python3
# 使用真实解码与尾帧计数检查排空；重编码后字节通常不同，不能照搬Remux压缩包哈希判据。
"""完整直播：真实编解码、格式转换和排空验证。"""
import argparse
import csv
import json
from pathlib import Path
import signal
import subprocess
import tempfile
import time

p = argparse.ArgumentParser()
for name in ["program", "ffmpeg", "ffprobe", "work"]:
    p.add_argument("--" + name, required=True)
a = p.parse_args()
Path(a.work).mkdir(parents=True, exist_ok=True)


def run(command, expected=0):
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True, timeout=25)
    if result.returncode != expected:
        raise RuntimeError(f"{command}\n退出 {result.returncode}\n{result.stderr}")
    return result


def check(value, message):
    if not value:
        raise RuntimeError(message)


def ff(*options):
    return run([a.ffmpeg, "-v", "error", "-y", *options])


def probe(path):
    return json.loads(run([a.ffprobe, "-v", "error", "-count_frames",
                          "-show_streams", "-show_packets", "-of", "json", path]).stdout)


def convert(source, target, *options, expected=0):
    metrics = ["--metrics-file", str(target)+".csv"] if expected == 0 else []
    return run([a.program, "--input", source, "--input", source, "--duration-ms", "2120", "--output", target,
                "--no-realtime", *metrics, *options], expected)


def validate(path, count, audio=True):
    data = probe(path)
    # 节目调度可能复用最后一帧；还要检查每个解码器实际产帧数，才能发现B帧尾部未排空。
    with Path(str(path)+".csv").open() as metrics:
        last = list(csv.DictReader(metrics))[-1]
    check(int(last["v0_frames"]) == count and int(last["v1_frames"]) == count,
          "输入解码尾帧缺失，不能用节目重复帧掩盖")
    video = next(s for s in data["streams"] if s["codec_type"] == "video")
    check(video["codec_name"] == "h264" and video["pix_fmt"] == "yuv420p", "视频输出格式错误")
    check((video["width"], video["height"]) == (640, 360), "尺寸错误")
    check(int(video["nb_read_frames"]) == count, "排空丢失视频帧")
    if audio:
        stream = next(s for s in data["streams"] if s["codec_type"] == "audio")
        check(stream["codec_name"] == "aac" and stream["sample_rate"] == "48000"
              and stream["channels"] == 2, "音频重采样/编码格式错误")
    for s in data["streams"]:
        packets = [x for x in data["packets"] if x["stream_index"] == s["index"]]
        dts = [float(x["dts_time"]) for x in packets]
        check(all(x < y for x, y in zip(dts, dts[1:])), "DTS 不递增")
    ff("-xerror", "-i", path, "-f", "null", "-")
    return data


with tempfile.TemporaryDirectory(dir=a.work, prefix="codec-") as directory:
    root = Path(directory)
    source = root / "h264.mp4"
    ff("-f", "lavfi", "-i", "testsrc2=size=320x180:rate=25",
       "-f", "lavfi", "-i", "sine=frequency=700:sample_rate=44100",
       "-t", "2.12", "-c:v", "libx264", "-bf", "3", "-c:a", "aac", source)
    out = root / "h264.flv"
    convert(source, out)
    data = validate(out, 53)
    packets = data["packets"]
    v = [x for x in packets if x["codec_type"] == "video"]
    audio = [x for x in packets if x["codec_type"] == "audio"]
    # AAC 编码预卷和末帧填充允许最多两帧音频偏差。
    check(abs(float(v[0]["pts_time"]) - float(audio[0]["pts_time"])) < .05, "起点音画偏差")
    check(abs(float(v[-1]["pts_time"]) - float(audio[-1]["pts_time"])) < .08, "尾部音画偏差")
    pcm = root / "audio.f32"
    ff("-i", out, "-vn", "-f", "f32le", "-acodec", "pcm_f32le", pcm)
    samples = pcm.stat().st_size // 8
    check(abs(samples / 48000 - 2.12) < .07, "重采样尾部采样缺失")
    print(f"通过：H.264 B 帧排空 53/53；44100Hz 单声道→48000Hz 双声道，{samples} 采样")

    hevc = root / "hevc.mp4"
    ff("-f", "lavfi", "-i", "testsrc2=size=256x144:rate=25",
       "-t", "1.08", "-c:v", "libx265", "-pix_fmt", "yuv420p10le",
       "-x265-params", "pools=1:frame-threads=1:log-level=error", hevc)
    hevc_out = root / "hevc.flv"
    convert(hevc, hevc_out, "--duration-ms", "1080")
    validate(hevc_out, 27)
    print("通过：H.265 10-bit 解码、8-bit 像素转换、H.264 编码，27/27 帧")

    # 独立源内容偏移由live_integration验证；完整节目即使无输入音轨也输出静音AAC。
    digest = out.read_bytes()
    convert(source, out, expected=1)
    check(out.read_bytes() == digest, "覆盖旧输出")
    convert(source, root / "bad-size.flv", "--width", "321", expected=1)
    convert(root / "missing", root / "missing.flv", expected=1)
    print("通过：输出保护、无效尺寸及缺失输入")

    partial = root / "partial.flv"
    proc = subprocess.Popen([a.program, "--input", str(source), "--input", str(source), "--output", str(partial)],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        time.sleep(.6)
        proc.send_signal(signal.SIGINT)
        _, errors = proc.communicate(timeout=6)
        check(proc.returncode == 130, str(errors))
        ff("-xerror", "-i", partial, "-f", "null", "-")
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.communicate()
    print("通过：转码中用户停止及部分输出可解码")

#!/usr/bin/env python3
# 使用真实解码与尾帧计数检查排空；重编码后字节通常不同，不能照搬Remux压缩包哈希判据。
"""Phase 2：真实编解码、格式转换和排空验证。"""
import argparse
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
    return run([a.program, "--input", source, "--output", target,
                "--no-realtime", *options], expected)


def validate(path, count, audio=True):
    data = probe(path)
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
    convert(hevc, hevc_out)
    validate(hevc_out, 27, audio=False)
    print("通过：H.265 10-bit 解码、8-bit 像素转换、H.264 编码，27/27 帧")

    offset = root / "offset.mp4"
    ff("-i", source, "-itsoffset", "0.3", "-i", source,
       "-map", "0:v", "-map", "1:a", "-c", "copy", offset)
    offset_out = root / "offset.flv"
    convert(offset, offset_out)
    offset_data = validate(offset_out, 53)
    starts = {s["codec_type"]: float(s["start_time"]) for s in offset_data["streams"]}
    check(.24 < starts["audio"] - starts["video"] < .34, "音频偏移被重置")
    print("通过：300ms 音频起点偏移保留")

    audio_only = root / "audio.m4a"
    ff("-i", source, "-vn", "-c:a", "copy", audio_only)
    only_out = root / "audio.flv"
    convert(audio_only, only_out)
    check(len(probe(only_out)["streams"]) == 1, "单音轨转码失败")
    ff("-xerror", "-i", only_out, "-f", "null", "-")

    digest = out.read_bytes()
    convert(source, out, expected=1)
    check(out.read_bytes() == digest, "覆盖旧输出")
    convert(source, root / "bad-size.flv", "--width", "321", expected=1)
    convert(root / "missing", root / "missing.flv", expected=1)
    print("通过：单音轨、输出保护、无效尺寸及缺失输入")

    partial = root / "partial.flv"
    proc = subprocess.Popen([a.program, "--input", str(source), "--output", str(partial)],
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

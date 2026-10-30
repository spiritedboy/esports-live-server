#!/usr/bin/env python3
"""Phase 4：解码后波形/频谱、内容音画对齐、同步报告、故障退出验证。"""
import argparse
import array
import csv
import hashlib
import json
import math
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


def run(cmd, expected=0, binary=False):
    r = subprocess.run(list(map(str, cmd)), capture_output=True, text=not binary, timeout=40)
    if r.returncode != expected:
        raise RuntimeError(f"退出 {r.returncode}，期望 {expected}\n{cmd}\n{r.stderr}")
    return r


def ff(*args):
    return run([a.ffmpeg, "-v", "error", "-y", *args])


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


def pcm(path):
    raw = run([a.ffmpeg, "-v", "error", "-i", path, "-vn", "-ac", "1",
               "-ar", "48000", "-f", "f32le", "-"], binary=True).stdout
    result = array.array("f")
    result.frombytes(raw)
    require(all(math.isfinite(x) for x in result), "输出非有限采样")
    return result


def amplitude(data, frequency, start=.4, duration=.5):
    values = data[int(start * 48000):int((start + duration) * 48000)]
    require(len(values) > 1000, "音频不足")
    real = sum(x * math.cos(2 * math.pi * frequency * i / 48000) for i, x in enumerate(values))
    imag = sum(x * math.sin(2 * math.pi * frequency * i / 48000) for i, x in enumerate(values))
    return 2 * math.hypot(real, imag) / len(values)


def probe(path):
    return json.loads(run([a.ffprobe, "-v", "error", "-show_streams",
                          "-show_packets", "-of", "json", path]).stdout)


def validate(path, seconds):
    d = probe(path)
    require([s["codec_name"] for s in d["streams"]] == ["h264", "aac"], "输出不是 H.264/AAC")
    require(d["streams"][1]["sample_rate"] == "48000" and d["streams"][1]["channels"] == 2,
            "音频格式未统一")
    for stream in d["streams"]:
        pts = [float(x["dts_time"]) for x in d["packets"] if x["stream_index"] == stream["index"]]
        require(all(x < y for x, y in zip(pts, pts[1:])), "输出 DTS 不递增")
        require(abs(pts[-1] - seconds) < .1, "输出末尾时间轴异常")
    ff("-xerror", "-i", path, "-f", "null", "-")


def onset(data):
    for i in range(0, len(data) - 480, 480):
        if math.sqrt(sum(x * x for x in data[i:i + 480]) / 480) > .02:
            return i / 48000
    raise RuntimeError("未检测到音频脉冲")


with tempfile.TemporaryDirectory(dir=a.work, prefix="live-") as directory:
    root = Path(directory)
    v1, v2 = root / "red.mp4", root / "blue.mp4"
    for path, color in [(v1, "red"), (v2, "blue")]:
        ff("-f", "lavfi", "-i", f"color=c={color}:s=160x120:r=25:d=3",
           "-c:v", "libx264", path)
    game, voice = root / "game.m4a", root / "voice.opus"
    ff("-f", "lavfi", "-i", "sine=frequency=440:sample_rate=44100:duration=3", "-c:a", "aac", game)
    ff("-f", "lavfi", "-i", "sine=frequency=880:sample_rate=48000:duration=3", "-c:a", "libopus", voice)

    def command(output, audios=(game, voice), *options):
        args = [a.program, "--input", str(v1), "--input", str(v2),
                "--output", str(output), "--width", "320", "--height", "240",
                "--duration-ms", "3000", "--no-realtime"]
        for path in audios:
            args.extend(["--audio-input", str(path)])
        return args + list(options)

    base = root / "base.flv"
    report = root / "sync.csv"
    run(command(base, (game, voice), "--stats-file", str(report)))
    validate(base, 3)
    data = pcm(base)
    f440, f880 = amplitude(data, 440), amplitude(data, 880)
    require(f440 > .04 and f880 > .04, f"未同时混入两路频率：{f440},{f880}")
    rows = list(csv.DictReader(report.open()))
    require(len(rows) >= 4 and max(abs(int(r["av_delta_us"])) for r in rows) <= 40001,
            "同步误差报告越界")
    require(max(abs(x) for x in data) < 1, "正常音量产生过载")
    require(amplitude(data, 1320) < .006 and amplitude(data, 1760) < .006,
            "正常混音出现明显额外谐波")
    print(f"通过：AAC/44.1kHz 与 Opus/48kHz 混音，440Hz={f440:.4f}，880Hz={f880:.4f}")

    half = root / "half.flv"
    run(command(half, (game, voice), "--gain", "0:0.5"))
    hdata = pcm(half)
    require(.44 < amplitude(hdata, 440) / f440 < .56, "增益比例错误")
    require(.9 < amplitude(hdata, 880) / f880 < 1.1, "调整一路影响另一条音轨")
    mute = root / "mute.flv"
    run(command(mute, (game, voice), "--mute", "1"))
    mdata = pcm(mute)
    require(amplitude(mdata, 880) < .003 and amplitude(mdata, 440) > .04, "逐路静音失败")
    dynamic = root / "dynamic.flv"
    run(command(dynamic, (game, voice), "--change-at-ms", "1000", "--change-gain", "1:0"))
    dynamic_data = pcm(dynamic)
    require(amplitude(dynamic_data, 880, .3) > .04 and amplitude(dynamic_data, 880, 1.4) < .003,
            "运行中音量变化未生效")
    print("通过：逐路半音量、静音、运行中渐变到零及另一声源保持")

    # 视频从1秒变白；同源音频脉冲因为容器偏移晚250ms。
    pulse = root / "pulse.mp4"
    ff("-f", "lavfi", "-i", "color=black:s=160x120:r=25:d=3",
       "-f", "lavfi", "-i", "aevalsrc=0.2*sin(2*PI*440*t)*gte(t\\,1):s=48000:d=3",
       "-filter_complex", "[0:v]drawbox=c=white:t=fill:enable='gte(t,1)'[v];[1:a]asetpts=PTS+0.25/TB[a]",
       "-map", "[v]", "-map", "[a]", "-c:v", "libx264", "-c:a", "aac", pulse)
    for label, offset in [("preserved", 0), ("aligned", -250)]:
        out = root / (label + ".flv")
        cmd = [a.program, "--input", str(pulse), "--input", str(v2), "--audio-input", str(pulse),
               "--output", str(out), "--width", "320", "--height", "240", "--no-realtime",
               "--duration-ms", "3000", "--audio-offset-ms", f"0:{offset}"]
        run(cmd)
        pixels = run([a.ffmpeg, "-v", "error", "-i", out, "-an",
                      "-vf", "crop=8:8:30:30,format=gray", "-fps_mode", "passthrough", "-f", "rawvideo", "-"], binary=True).stdout
        frames = [pixels[i:i + 64] for i in range(0, len(pixels), 64)]
        first = next(i for i, f in enumerate(frames) if sum(f) / 64 > 200)
        d = probe(out)
        video_pts = [float(packet["pts_time"]) for packet in d["packets"]
                     if packet["stream_index"] == 0]
        require(len(frames) == len(video_pts), "测量工具改变了视频帧数量")
        delta = onset(pcm(out)) - video_pts[first]
        require(abs(delta - (0.25 if offset == 0 else 0)) < .02, f"内容音画偏差错误：{delta}")
        print(f"通过：{label} 内容音画偏差 {delta * 1000:.1f}ms")

    missing = root / "missing.flv"
    result = run(command(missing, (game, root / "absent.opus")))
    require("降级静音" in result.stderr, "缺失声源未报告")
    require(amplitude(pcm(missing), 440) > .04, "缺失声源阻塞正常声源")
    run([a.program, "--input", root / "no-video1", "--input", root / "no-video2",
         "--output", root / "all-missing.flv", "--no-realtime"], expected=1)
    require(not (root / "all-missing.flv").exists(), "所有源无媒体时不应创建节目")
    no_audio = root / "silence.flv"
    run(command(no_audio, (v1, v2)))
    require(max(abs(x) for x in pcm(no_audio)) < .001, "无音轨没有输出静音")
    print("通过：音频源失败或无音轨时补静音，正常源继续")

    # 30秒媒体时间的真实编码/混音，检查报告首尾无累积偏移；离线加速，不冒称实际长时间运行。
    long_tone = root / "long.opus"
    ff("-f", "lavfi", "-i", "sine=frequency=880:sample_rate=48000:duration=30",
       "-c:a", "libopus", long_tone)
    long_out, long_stats = root / "long.flv", root / "long.csv"
    run(command(long_out, (long_tone,), "--duration-ms", "30000", "--stats-file", str(long_stats)))
    validate(long_out, 30)
    rows = list(csv.DictReader(long_stats.open()))
    errors = [abs(int(row["av_delta_us"])) for row in rows]
    require(len(rows) >= 30 and max(errors) <= 40001 and errors[-1] <= 22000,
            "30秒媒体时间同步偏差累积")
    print(f"通过：30秒真实输出，{len(rows)} 条同步记录，采样点误差峰值 {max(errors)}us")

    digest = hashlib.sha256(base.read_bytes()).digest()
    run(command(base), expected=1)
    require(hashlib.sha256(base.read_bytes()).digest() == digest, "覆盖已有输出")
    run(command(root / "bad.flv", (game,), "--gain", "3:1"), expected=1)
    run(command(root / "bad2.flv", (game,), "--gain", "0:nan"), expected=1)
    run(command(root / "bad3.flv", (game,), "--stats-file", str(report)), expected=1)

    partial = root / "partial.flv"
    cmd = command(partial)
    cmd.remove("--no-realtime")
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        time.sleep(.7)
        proc.send_signal(signal.SIGINT)
        _, stderr = proc.communicate(timeout=6)
        require(proc.returncode == 130, str(stderr))
        ff("-xerror", "-i", partial, "-f", "null", "-")
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.communicate()
    print("通过：文件保护、参数校验、SIGINT 退出及部分文件解码")

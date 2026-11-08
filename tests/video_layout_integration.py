#!/usr/bin/env python3
# 独立解码输出后检查区域像素和帧时间戳；退出码为0本身不能证明布局与CFR正确。
"""完整直播：像素区域、CFR 时间戳、源故障和线程退出的真实集成测试。"""
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


def run(command, expected=0, binary=False):
    result = subprocess.run(list(map(str, command)), capture_output=True,
                            text=not binary, timeout=20)
    if result.returncode != expected:
        raise RuntimeError(f"{command}\n退出 {result.returncode}\n{result.stderr}")
    return result


def require(value, message):
    if not value:
        raise RuntimeError(message)


def ff(*options):
    return run([a.ffmpeg, "-v", "error", "-y", *options])


# 完整节目按固定2秒验收布局/CFR；源尾帧时长的微秒舍入不应决定此测试的节目长度。
def command(inputs, output, *options):
    cmd = [a.program]
    for source in inputs:
        cmd.extend(["--input", str(source)])
    return cmd + ["--output", str(output), "--width", "320", "--height", "240",
                  "--fps", "20", "--duration-ms", "2000", *options]


def composite(inputs, output, *options, expected=0):
    result = run(command(inputs, output, "--no-realtime", *options), expected)
    return result


def validate(path, count):
    data = json.loads(run([a.ffprobe, "-v", "error", "-count_frames", "-show_streams",
                           "-show_packets", "-of", "json", path]).stdout)
    require([s["codec_name"] for s in data["streams"]] == ["h264", "aac"], "完整节目应输出H.264/AAC")
    stream = data["streams"][0]
    require((stream["width"], stream["height"], stream["codec_name"]) == (320, 240, "h264"),
            "输出格式错误")
    require(int(stream["nb_read_frames"]) == count, f"输出帧数应为 {count}，实为 {stream['nb_read_frames']}")
    pts = [float(x["pts_time"]) for x in data["packets"] if x["stream_index"] == stream["index"]]
    require(all(abs((y - x) - .05) < .002 for x, y in zip(pts, pts[1:])), "非稳定 20fps 时间轴")
    ff("-xerror", "-i", path, "-f", "null", "-")


def color(path, seconds, x, y):
    raw = run([a.ffmpeg, "-v", "error", "-ss", str(seconds), "-i", path,
               "-frames:v", "1", "-vf", f"crop=16:16:{x}:{y},format=rgb24",
               "-f", "rawvideo", "-"], binary=True).stdout
    require(len(raw) == 16 * 16 * 3, "像素区域未取到")
    return tuple(sum(raw[c::3]) / 256 for c in range(3))


def expect(path, seconds, x, y, target):
    actual = color(path, seconds, x, y)
    require(all(abs(v - t) < 35 for v, t in zip(actual, target)),
            f"像素验证失败 {path} t={seconds} ({x},{y}) {actual} != {target}")


with tempfile.TemporaryDirectory(dir=a.work, prefix="mix-") as directory:
    root = Path(directory)
    inputs = []
    for name, size, fps in [("red", "160x120", 15), ("lime", "256x144", 30),
                            ("blue", "320x180", 24), ("yellow", "128x96", 25)]:
        path = root / (name + ".mp4")
        ff("-f", "lavfi", "-i", f"color=c={name}:s={size}:r={fps}",
           "-t", "2", "-c:v", "libx264", "-bf", "2", path)
        inputs.append(path)
    grid = root / "grid.flv"
    composite(inputs, grid, "--layout", "grid")
    validate(grid, 40)
    for x, y, target in [(40, 30, (255, 0, 0)), (220, 30, (0, 255, 0)),
                          (40, 170, (0, 0, 255)), (220, 170, (255, 255, 0))]:
        expect(grid, .5, x, y, target)
    print("通过：4 路不同分辨率/帧率 → 20fps 四宫格，40 帧及四色像素校验")

    split = root / "split.flv"
    composite(inputs[:2], split)
    validate(split, 40)
    expect(split, .5, 40, 80, (255, 0, 0))
    expect(split, .5, 220, 80, (0, 255, 0))
    pip = root / "pip.flv"
    composite(inputs[:2], pip, "--layout", "pip")
    validate(pip, 40)
    expect(pip, .5, 240, 30, (255, 0, 0))
    expect(pip, .5, 260, 190, (0, 255, 0))
    switched = root / "switched.flv"
    composite(inputs[:2], switched, "--switch-at-ms", "1000", "--switch-layout", "pip")
    validate(switched, 40)
    expect(switched, .5, 240, 30, (0, 255, 0))
    expect(switched, 1.5, 240, 30, (255, 0, 0))
    print("通过：双画面、右下画中画、1 秒处布局切换及连续时间轴")

    changing = root / "changing.mp4"
    ff("-f", "lavfi", "-i", "color=c=red:s=160x120:r=15:d=1",
       "-f", "lavfi", "-i", "color=c=white:s=160x120:r=15:d=1",
       "-filter_complex", "[0:v][1:v]concat=n=2:v=1:a=0",
       "-c:v", "libx264", "-output_ts_offset", "5", changing)
    scheduled = root / "scheduled.flv"
    composite([changing, inputs[1]], scheduled)
    validate(scheduled, 40)
    expect(scheduled, .4, 40, 80, (255, 0, 0))
    expect(scheduled, 1.4, 40, 80, (255, 255, 255))
    print("通过：5 秒输入时间原点归零、15fps→20fps 动态画面调度")

    short = root / "short.mp4"
    ff("-i", inputs[1], "-t", "0.4", "-c", "copy", short)
    interrupted = root / "short-source.flv"
    composite([inputs[0], short], interrupted, "--stale-ms", "200")
    validate(interrupted, 40)
    expect(interrupted, .1, 220, 80, (0, 255, 0))
    expect(interrupted, 1.2, 220, 80, (0, 0, 0))
    missing = root / "missing.flv"
    result = composite([inputs[0], root / "nonexistent.mp4"], missing)
    require("一路视频源失败" in result.stderr, "缺失源没有报告")
    validate(missing, 40)
    expect(missing, .5, 40, 80, (255, 0, 0))
    expect(missing, .5, 220, 80, (0, 0, 0))
    composite([root / "bad1", root / "bad2"], root / "all-fail.flv", expected=1)
    print("通过：短源停帧后切黑、单源失败保持另一源、全源失败退出")

    realtime = root / "realtime.flv"
    start = time.monotonic()
    run(command(inputs[:2], realtime, "--duration-ms", "1000"))
    elapsed = time.monotonic() - start
    validate(realtime, 20)
    require(.85 < elapsed < 4, f"实时调度耗时异常 {elapsed}")
    print(f"通过：1 秒实时调度，实耗 {elapsed:.3f} 秒")

    digest = grid.read_bytes()
    composite(inputs, grid, "--layout", "grid", expected=1)
    require(grid.read_bytes() == digest, "已有输出被覆盖")
    composite(inputs[:2], root / "bad-grid.flv", "--layout", "grid", expected=1)
    composite(inputs[:2], root / "bad-size.flv", "--width", "322", expected=1)
    print("通过：合成输出保护和非法布局/尺寸参数")

    partial = root / "stop.flv"
    process = subprocess.Popen(command(inputs, partial, "--layout", "grid", "--duration-ms", "10000"),
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        time.sleep(.6)
        process.send_signal(signal.SIGINT)
        _, errors = process.communicate(timeout=5)
        require(process.returncode == 130, str(errors))
        ff("-xerror", "-i", partial, "-f", "null", "-")
    finally:
        if process.poll() is None:
            process.kill()
            process.communicate()
    print("通过：四路队列背压期间 SIGINT，所有线程退出，部分视频可解码")

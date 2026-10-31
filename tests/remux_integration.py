#!/usr/bin/env python3
# 核对压缩数据与时间轴，确保转封装没有偷偷重新编码或丢失音视频相对偏移。
"""Phase 1 实际媒体集成测试；FFmpeg CLI 仅生成素材和独立验证，不执行被测 Remux。"""
import argparse
import hashlib
import json
from pathlib import Path
import resource
import signal
import socket
import subprocess
import tempfile
import threading
import time

parser = argparse.ArgumentParser()
parser.add_argument("--program", required=True)
parser.add_argument("--ffmpeg", required=True)
parser.add_argument("--ffprobe", required=True)
parser.add_argument("--work", required=True)
args = parser.parse_args()
Path(args.work).mkdir(parents=True, exist_ok=True)


def run(command, expected=0, timeout=15, **kwargs):
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, timeout=timeout, **kwargs)
    if result.returncode != expected:
        raise RuntimeError(f"退出码 {result.returncode}，期望 {expected}: {command}\n"
                           f"{result.stdout}\n{result.stderr}")
    return result


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def ffmpeg(*arguments):
    return run([args.ffmpeg, "-hide_banner", "-loglevel", "error", "-y", *map(str, arguments)])


def remux(source, output, *options, expected=0, **kwargs):
    return run([args.program, "--input", str(source), "--output", str(output),
                *options], expected=expected, **kwargs)


def probe(path):
    return json.loads(run([args.ffprobe, "-v", "error", "-show_streams", "-show_packets",
                           "-show_data_hash", "sha256", "-of", "json", str(path)]).stdout)


def decode(path):
    ffmpeg("-xerror", "-i", path, "-f", "null", "-")


def compare(source, output):
    before, after = probe(source), probe(output)
    require([s["codec_name"] for s in before["streams"]] ==
            [s["codec_name"] for s in after["streams"]], "输出编码发生变化")
    global_shift = None
    for index in range(len(before["streams"])):
        original = [p for p in before["packets"] if p["stream_index"] == index]
        copied = [p for p in after["packets"] if p["stream_index"] == index]
        require(len(original) == len(copied) and len(copied) > 0, "包数量不一致")
        last_dts = None
        for a, b in zip(original, copied):
            require(a["data_hash"] == b["data_hash"], "压缩包内容被改变")
            dts = float(b["dts_time"])
            require(last_dts is None or dts > last_dts, "DTS 不递增")
            last_dts = dts
            shift = dts - float(a["dts_time"])
            if global_shift is None:
                global_shift = shift
            require(abs(shift - global_shift) < .002, "音视频共同时间原点被破坏")
            composition_a = float(a["pts_time"]) - float(a["dts_time"])
            composition_b = float(b["pts_time"]) - dts
            require(abs(composition_a - composition_b) < .002, "PTS-DTS 差值改变")
    decode(output)


with tempfile.TemporaryDirectory(prefix="phase1-", dir=args.work) as folder:
    root = Path(folder)
    source = root / "input.mp4"
    ffmpeg("-f", "lavfi", "-i", "testsrc2=size=320x180:rate=25",
           "-f", "lavfi", "-i", "sine=frequency=1000:sample_rate=48000",
           "-t", "3", "-c:v", "libx264", "-preset", "fast", "-bf", "2",
           "-g", "25", "-pix_fmt", "yuv420p", "-c:a", "aac", source)
    video_packets = [p for p in probe(source)["packets"] if p["stream_index"] == 0]
    require(any(p["pts"] != p["dts"] for p in video_packets), "样片没有重排序时间戳")
    output = root / "output.flv"
    remux(source, output, "--no-realtime")
    compare(source, output)
    print("通过：B 帧样片、217 个包、负初始 DTS、压缩数据哈希、时间基及音视频偏移")

    # 修改 FLV 视频数据标签的第二个 DTS 为第一个 DTS，注入明确的回退错误。
    broken_bytes = bytearray(output.read_bytes())
    position = int.from_bytes(broken_bytes[5:9], "big") + 4
    first_timestamp = None
    changed = False
    while position + 15 <= len(broken_bytes):
        size = int.from_bytes(broken_bytes[position + 1:position + 4], "big")
        if (broken_bytes[position] == 9 and size >= 2 and
                broken_bytes[position + 12] == 1):
            if first_timestamp is None:
                first_timestamp = bytes(broken_bytes[position + 4:position + 8])
            else:
                broken_bytes[position + 4:position + 8] = first_timestamp
                changed = True
                break
        position += 11 + size + 4
    require(changed, "未找到可修改的视频标签")
    backwards = root / "backwards.flv"
    backwards.write_bytes(broken_bytes)
    result = remux(backwards, root / "rejected.flv", "--no-realtime", expected=1)
    require("DTS 不严格递增" in result.stderr, result.stderr)
    print("通过：人为注入的重复 DTS 被明确拒绝")

    start = time.monotonic()
    paced = root / "paced.flv"
    remux(source, paced)
    elapsed = time.monotonic() - start
    require(2.8 <= elapsed <= 7, f"实时发送节奏异常：{elapsed:.3f}s")
    compare(source, paced)
    print(f"通过：3 秒样片实时发送，实耗 {elapsed:.3f} 秒")

    # 反向流顺序及音频晚到，确保流映射不依赖固定 0=视频/1=音频。
    offset = root / "offset.mp4"
    ffmpeg("-i", source, "-itsoffset", "0.3", "-i", source,
           "-map", "1:a:0", "-map", "0:v:0", "-c", "copy", offset)
    offset_out = root / "offset.flv"
    remux(offset, offset_out, "--no-realtime")
    compare(offset, offset_out)
    print("通过：音频在前的流映射和 300ms 音频偏移")

    for kind, selector in [("video", "0:v:0"), ("audio", "0:a:0")]:
        single = root / (kind + ".mp4")
        ffmpeg("-i", source, "-map", selector, "-c", "copy", single)
        single_out = root / (kind + ".flv")
        remux(single, single_out, "--no-realtime")
        compare(single, single_out)
    print("通过：仅视频和仅音频输入")

    digest = hashlib.sha256(output.read_bytes()).digest()
    result = remux(source, output, "--no-realtime", expected=1)
    require("禁止覆盖" in result.stderr and hashlib.sha256(output.read_bytes()).digest() == digest,
            "已有输出被覆盖")
    original_digest = hashlib.sha256(source.read_bytes()).digest()
    remux(source, source, "--no-realtime", expected=1)
    require(hashlib.sha256(source.read_bytes()).digest() == original_digest, "输入被覆盖")
    alias = root / "alias.flv"
    alias.symlink_to(source)
    remux(source, alias, "--no-realtime", expected=1)
    require(hashlib.sha256(source.read_bytes()).digest() == original_digest, "符号链接目标被覆盖")
    print("通过：已有输出、输入同路径、符号链接保护")

    remux(root / "missing.mp4", root / "missing.flv", expected=1)
    bad = root / "bad.mp4"
    bad.write_text("这不是媒体文件")
    remux(bad, root / "bad.flv", expected=1)
    incompatible = root / "mpeg4.mp4"
    ffmpeg("-f", "lavfi", "-i", "testsrc2=size=160x90:rate=25",
           "-t", "0.2", "-c:v", "mpeg4", incompatible)
    result = remux(incompatible, root / "incompatible.flv", expected=1)
    require("不兼容编码" in result.stderr and not (root / "incompatible.flv").exists(),
            "不兼容编码没有在创建输出前拒绝")
    raw = root / "raw.h264"
    ffmpeg("-i", source, "-map", "0:v", "-c", "copy", "-bsf:v", "h264_mp4toannexb", raw)
    result = remux(raw, root / "raw.flv", "--no-realtime", expected=1)
    require("缺少 PTS/DTS" in result.stderr, "缺失时间戳未明确拒绝")
    remux(source, root / "nonexistent" / "output.flv", expected=1)
    for value in ["0", "-1", "123x", "999999999999999999999999"]:
        remux(source, root / "invalid.flv", "--timeout-ms", value, expected=1)
    print("通过：缺失/损坏文件、不兼容编码、缺失时间戳、错误路径及无效参数")

    # bind 但不 listen：目标端口归测试持有，确定不存在可接受连接的服务。
    with socket.socket() as unavailable:
        unavailable.bind(("127.0.0.1", 0))
        port = unavailable.getsockname()[1]
        for protocol in ["rtmp", "rtsp"]:
            remux(f"{protocol}://127.0.0.1:{port}/live/test",
                  root / (protocol + ".flv"), "--timeout-ms", "300", expected=1)
        remux(source, f"rtmp://127.0.0.1:{port}/live/test",
              "--timeout-ms", "300", expected=1)
    print("通过：RTMP/RTSP 输入和 RTMP 输出连接拒绝")

    # 仅 TCP 故障注入：接受连接但不响应协议，不实现 RTSP/RTMP 服务端。
    def stalled_connection(protocol, interrupt=False):
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen(1)
            listener.settimeout(5)
            finished = threading.Event()
            errors = []

            def accept_only():
                try:
                    connection, _ = listener.accept()
                    with connection:
                        finished.wait(5)
                except Exception as error:
                    errors.append(error)

            worker = threading.Thread(target=accept_only)
            worker.start()
            start_time = time.monotonic()
            process = None
            try:
                command = [args.program, "--input",
                           f"{protocol}://127.0.0.1:{listener.getsockname()[1]}/live/test",
                           "--output", str(root / f"stall-{protocol}-{interrupt}.flv"),
                           "--timeout-ms", "5000" if interrupt else "300"]
                process = subprocess.Popen(command, stdout=subprocess.PIPE,
                                           stderr=subprocess.PIPE, text=True)
                if interrupt:
                    time.sleep(.25)
                    process.send_signal(signal.SIGINT)
                _, stderr = process.communicate(timeout=4)
                require(process.returncode == (130 if interrupt else 1), stderr)
                require(("用户停止" if interrupt else "超时") in stderr, stderr)
                require(time.monotonic() - start_time < 3, "阻塞 I/O 未及时中断")
            finally:
                if process is not None and process.poll() is None:
                    process.kill()
                    process.communicate()
                finished.set()
                worker.join()
            require(not errors, f"故障注入线程异常：{errors}")

    stalled_connection("rtsp")
    stalled_connection("rtmp")
    stalled_connection("rtsp", interrupt=True)
    print("通过：RTSP/RTMP 握手无响应超时、阻塞输入中的 SIGINT")

    partial = root / "partial.flv"
    process = subprocess.Popen([args.program, "--input", str(source), "--output", str(partial)],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        time.sleep(.8)
        process.send_signal(signal.SIGINT)
        _, stderr = process.communicate(timeout=5)
        require(process.returncode == 130, stderr)
        require(len(probe(partial)["packets"]) > 0, "停止后没有可读媒体包")
        decode(partial)
    finally:
        if process.poll() is None:
            process.kill()
            process.communicate()
    print("通过：实时发送中 SIGINT、部分 FLV 收尾及可解码性")

    def limit_output_size():
        signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
        resource.setrlimit(resource.RLIMIT_FSIZE, (4096, 4096))

    result = remux(source, root / "limited.flv", "--no-realtime",
                   expected=1, preexec_fn=limit_output_size)
    require("File too large" in result.stderr, "未报告实际文件写入失败")
    print("通过：输出文件大小限制引发的真实写入错误")

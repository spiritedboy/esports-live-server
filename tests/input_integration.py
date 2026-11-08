#!/usr/bin/env python3
"""完整直播的RTMP/RTSP输入拒绝、超时与停止；TCP仅注入故障，不提供媒体协议服务。"""
import argparse
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import threading
import time

p = argparse.ArgumentParser()
p.add_argument('--program', required=True)
p.add_argument('--work', required=True)
a = p.parse_args()
Path(a.work).mkdir(parents=True, exist_ok=True)


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


with tempfile.TemporaryDirectory(dir=a.work, prefix='input-') as directory:
    root = Path(directory)

    def command(url, name, timeout=300):
        # 两路视频，音频继承URL：同一端点会收到多个独立输入连接。
        return [a.program, '--input', url, '--input', url, '--output', str(root/name),
                '--timeout-ms', str(timeout)]

    with socket.socket() as unavailable:
        unavailable.bind(('127.0.0.1', 0))
        for protocol in ('rtmp', 'rtsp'):
            url = f'{protocol}://127.0.0.1:{unavailable.getsockname()[1]}/live/test'
            result = subprocess.run(command(url, protocol+'.flv'), capture_output=True, text=True, timeout=5)
            require(result.returncode == 1 and '所有音视频源均失败' in result.stderr, result.stderr)

    for protocol, interrupt in [('rtmp', False), ('rtsp', False), ('rtmp', True), ('rtsp', True)]:
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            listener.listen(8)
            listener.settimeout(.05)
            stop, connected = threading.Event(), threading.Event()
            connections, errors = [], []

            def accept_only():
                try:
                    while not stop.is_set():
                        try:
                            connection, _ = listener.accept()
                            connections.append(connection)
                            connected.set()
                        except socket.timeout:
                            pass
                except Exception as error:
                    errors.append(error)

            worker = threading.Thread(target=accept_only)
            worker.start()
            process = None
            try:
                url = f'{protocol}://127.0.0.1:{listener.getsockname()[1]}/live/test'
                process = subprocess.Popen(command(url, f'{protocol}-{interrupt}.flv', 5000 if interrupt else 300),
                                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                require(connected.wait(3), '未建立真实TCP输入连接')
                began = time.monotonic()
                if interrupt:
                    process.send_signal(signal.SIGINT)
                _, error = process.communicate(timeout=4)
                require(process.returncode == (130 if interrupt else 1), error)
                require(time.monotonic()-began < 3, '输入I/O未及时中断')
                if not interrupt:
                    require('超时' in error, error)
            finally:
                if process is not None and process.poll() is None:
                    process.kill()
                    process.communicate()
                stop.set()
                worker.join()
                for connection in connections:
                    connection.close()
            require(not errors, str(errors))
print('通过：完整直播RTMP/RTSP输入拒绝、无响应超时和SIGINT回收')

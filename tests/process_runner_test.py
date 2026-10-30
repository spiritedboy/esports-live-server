#!/usr/bin/env python3
"""确保测试超时不遗留包装器启动的子进程。Linux /proc检查。"""
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
sys.path.insert(0, sys.argv[1])
from process_runner import run_checked


def require(value, message):
    if not value:
        raise RuntimeError(message)


def alive(pid):
    try:
        # 被杀死后尚待系统reap的僵尸不再执行代码。
        return Path(f'/proc/{pid}/stat').read_text().split(') ')[1].split()[0] != 'Z'
    except FileNotFoundError:
        return False


with tempfile.TemporaryDirectory(prefix='esports-process-') as tmp:
    root = Path(tmp)
    child = root/'child.py'
    child.write_text('''import os, signal, sys, time
from pathlib import Path
signal.signal(signal.SIGINT, signal.SIG_IGN)
Path(sys.argv[1]).write_text(str(os.getpid()))
while True: time.sleep(.05)
''')
    wrapper = root/'wrapper.py'
    wrapper.write_text('''import os, signal, subprocess, sys, time
from pathlib import Path
signal.signal(signal.SIGINT, lambda *_: sys.exit(0))
subprocess.Popen([sys.executable, sys.argv[1], sys.argv[2]])
while True: time.sleep(.05)
''')
    pidfile = root/'child.pid'
    start = time.monotonic()
    try:
        run_checked([sys.executable, wrapper, child, pidfile], timeout=1, grace_seconds=.3)
        raise RuntimeError('超时被错误报告为成功')
    except subprocess.TimeoutExpired:
        pass
    require(time.monotonic()-start < 3, '强制退出没有界限')
    pid = int(pidfile.read_text())
    require(not alive(pid), '包装器退出后仍遗留媒体子进程')
    print('通过：超时、包装器先退出、忽略SIGINT的后代被强制终止')

    graceful = root/'graceful.py'
    marker = root/'closed.txt'
    graceful.write_text('''import signal, sys, time
from pathlib import Path
def close(*_):
    Path(sys.argv[1]).write_text('closed')
    sys.exit(0)
signal.signal(signal.SIGINT, close)
while True: time.sleep(.05)
''')
    try:
        run_checked([sys.executable, graceful, marker], timeout=.5, grace_seconds=1)
        raise RuntimeError('预期超时')
    except subprocess.TimeoutExpired:
        pass
    require(marker.read_text() == 'closed', '未给予子进程正常收尾机会')
    result = run_checked([sys.executable, '-c', 'pass'], timeout=2)
    require(result.returncode == 0, '正常退出错误')
    try:
        run_checked([sys.executable, '-c', 'raise SystemExit(7)'], timeout=2)
        raise RuntimeError('非零退出被忽略')
    except subprocess.CalledProcessError as e:
        require(e.returncode == 7, '未保留退出码')
    print('通过：SIGINT温和收尾、正常退出及错误退出码传播')

    driver = root/'driver.py'
    interrupt_pid = root/'interrupt.pid'
    driver.write_text('''import sys
sys.path.insert(0, sys.argv[1])
from process_runner import run_checked
run_checked([sys.executable, sys.argv[2], sys.argv[3], sys.argv[4]], timeout=20, grace_seconds=.3)
''')
    with (root/'interrupt.log').open('w') as log:
        process = subprocess.Popen([sys.executable, driver, sys.argv[1], wrapper, child, interrupt_pid],
                                   stdout=log, stderr=log, start_new_session=True)
        deadline = time.monotonic()+3
        while not interrupt_pid.exists() and time.monotonic() < deadline:
            time.sleep(.01)
        require(interrupt_pid.exists(), '中断测试未启动子进程')
        process.send_signal(signal.SIGINT)
        code = process.wait(timeout=5)
    require(code != 0, '用户中断被错误报告为成功')
    require(not alive(int(interrupt_pid.read_text())), 'Ctrl+C留下后代进程')
    print('通过：实际向测试工具发送Ctrl+C，包装器及后代全部停止')

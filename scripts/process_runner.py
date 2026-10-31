"""测试子进程生命周期：独立进程组、超时/中断时温和退出后强制回收。"""
import os
import signal
import subprocess
import time
from pathlib import Path


def _signal_group(process, sig):
    try:
        os.killpg(process.pid, sig)
    except ProcessLookupError:
        pass


# 忽略Z/X状态：它们已不再执行；此检查针对仍运行的同进程组后代，不是僵尸回收器。
def _group_running(group):
    for path in Path('/proc').glob('[0-9]*/stat'):
        try:
            fields = path.read_text().rsplit(') ', 1)[1].split()
            if int(fields[2]) == group and fields[0] not in ('Z', 'X'):
                return True
        except (FileNotFoundError, ProcessLookupError, PermissionError):
            continue
    return False


def _stop_group(process, grace_seconds):
    _signal_group(process, signal.SIGINT)
    deadline = time.monotonic() + grace_seconds
    # 包装器可能先退出，而媒体子进程仍运行；不能只等包装器。
    while time.monotonic() < deadline:
        process.poll()
        try:
            os.killpg(process.pid, 0)
        except ProcessLookupError:
            break
        time.sleep(min(.05, max(0, deadline - time.monotonic())))
    _signal_group(process, signal.SIGKILL)
    process.wait(timeout=2)
    # kill只是投递信号；包装器已退出时wait不会等待后代实际停止。
    deadline = time.monotonic() + 2
    while _group_running(process.pid):
        if time.monotonic() >= deadline:
            raise RuntimeError('进程组收到SIGKILL后仍未停止，可能处于不可中断内核I/O')
        time.sleep(.01)


def run_checked(command, *, timeout, stdout=None, stderr=None, grace_seconds=5):
    """文件重定向版本；不使用PIPE，避免wait期间填满管道。

    超时或Ctrl+C向本次创建的进程组发送SIGINT，允许FFmpeg写尾，
    超过宽限再SIGKILL；保留原异常，绝不报告成功。
    """
    if timeout <= 0 or grace_seconds < 0:
        raise ValueError('超时必须大于0，退出宽限不能为负')
    if stdout == subprocess.PIPE or stderr == subprocess.PIPE:
        raise ValueError('请使用文件重定向，禁止未消费的PIPE')
    args = list(map(str, command))
    process = subprocess.Popen(args, stdout=stdout, stderr=stderr, start_new_session=True)
    try:
        code = process.wait(timeout=timeout)
        if code:
            # 包装器失败时也清理可能仍然运行的子进程。
            raise subprocess.CalledProcessError(code, args)
        return subprocess.CompletedProcess(args, code)
    except BaseException:
        _stop_group(process, grace_seconds)
        raise

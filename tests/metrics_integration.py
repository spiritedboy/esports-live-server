#!/usr/bin/env python3
# 除CSV字段外也验证真实输出及超时收尾；短跑指标不用于宣称长期无内存泄漏。
"""短负载检查指标语义、正常排空与指标文件保护，不替代长跑。"""
import argparse
from pathlib import Path
import subprocess
import tempfile
p = argparse.ArgumentParser()
for name in ('program','script','ffmpeg','ffprobe','work'):
    p.add_argument('--'+name, required=True)
a = p.parse_args()
Path(a.work).mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(dir=a.work, prefix='metrics-') as tmp:
    root = Path(tmp)/'run'
    subprocess.run(['python3', a.script, '--program', a.program, '--directory', str(root),
        '--seconds', '3', '--width', '160', '--height', '120', '--fps', '25',
        '--ffmpeg', a.ffmpeg, '--ffprobe', a.ffprobe], check=True, timeout=40)
    before = (root/'metrics.csv').read_bytes()
    result = subprocess.run([a.program, '--input', str(root/'source.mp4'), '--input', str(root/'source.mp4'),
        '--output', str(root/'protected.flv'), '--metrics-file', str(root/'metrics.csv')],
        capture_output=True, timeout=5)
    if result.returncode == 0 or (root/'metrics.csv').read_bytes() != before or (root/'protected.flv').exists():
        raise RuntimeError('指标文件保护失败')
    print('通过：四路真实输入计数、CPU/RSS、计时、队列上限、输出排空、录制解码和指标文件保护')

    # 实际媒体链路超时：验证进程组退出确实给予录制写尾机会。
    import json
    import sys
    sys.path.insert(0, str(Path(a.script).resolve().parent))
    from process_runner import run_checked
    command = json.loads((root/'command.json').read_text())
    for option, value in {
        '--output': root/'interrupted.flv',
        '--record-prefix': root/'interrupted-record',
        '--metrics-file': root/'interrupted-metrics.csv',
        '--stats-file': root/'interrupted-sync.csv',
    }.items():
        command[command.index(option)+1] = str(value)
    with (root/'interrupted.log').open('w') as log:
        try:
            run_checked(command, stdout=log, stderr=log, timeout=1.2)
            raise RuntimeError('媒体超时被错误报告为成功')
        except subprocess.TimeoutExpired:
            pass
    index = root/'interrupted-record.index.tsv'
    if not index.exists() or len(index.read_text().splitlines()) < 2:
        raise RuntimeError('超时收尾没有登记有效录制段')
    for path in (root/'interrupted.flv', root/'interrupted-record-000000.mp4'):
        run_checked([a.ffmpeg, '-v', 'error', '-xerror', '-i', path, '-f', 'null', '-'], timeout=10)
    print('通过：真实四路媒体超时，FLV与部分MP4均正常写尾并解码')

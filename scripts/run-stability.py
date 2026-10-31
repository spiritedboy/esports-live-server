#!/usr/bin/env python3
"""生成足够长的真实媒体，执行有界本地压力测试并保留日志与CSV；不宣称网络/24小时验收。"""
import argparse
import csv
import json
from pathlib import Path
import statistics
import subprocess
import time
from process_runner import run_checked

p = argparse.ArgumentParser()
p.add_argument('--program', required=True)
p.add_argument('--directory', required=True)
p.add_argument('--seconds', type=int, default=60)
p.add_argument('--width', type=int, default=640)
p.add_argument('--height', type=int, default=360)
p.add_argument('--fps', type=int, default=30)
p.add_argument('--ffmpeg', default='ffmpeg')
p.add_argument('--ffprobe', default='ffprobe')
p.add_argument('--perf', action='store_true')
a = p.parse_args()
if not (1 <= a.seconds <= 86400 and 16 <= a.width <= 3840 and 16 <= a.height <= 2160
        and a.width % 4 == 0 and a.height % 4 == 0 and 1 <= a.fps <= 60):
    p.error('时长/尺寸/帧率超出范围')
root = Path(a.directory).resolve()
root.mkdir(parents=True, exist_ok=False)
program = str(Path(a.program).resolve())
def run(command, **kwargs):
    return run_checked(command, **kwargs)
# 素材比测量时长多1秒，避免尾部EOF后用黑帧/静音凑出表面正常的压力测试。
source = root / 'source.mp4'
with (root/'generation.log').open('w') as log:
    run([a.ffmpeg, '-v', 'warning', '-f', 'lavfi', '-i',
         f'testsrc2=size={a.width}x{a.height}:rate={a.fps}', '-f', 'lavfi', '-i',
         'sine=frequency=440:sample_rate=48000', '-t', a.seconds+1,
         '-c:v', 'libx264', '-preset', 'ultrafast', '-threads', '2', '-crf', '28',
         '-c:a', 'aac', source], stdout=log, stderr=log, timeout=max(60, a.seconds*10))
# 同一素材打开4个独立解码会话以产生负载；并不模拟4台设备的网络与独立时钟。
command = [program] + ['--input', str(source)]*4 + [
    '--layout', 'grid', '--width', str(a.width), '--height', str(a.height), '--fps', str(a.fps),
    '--duration-ms', str(a.seconds*1000), '--output', str(root/'live.flv'),
    '--record-prefix', str(root/'record'), '--segment-ms', '10000',
    '--metrics-file', str(root/'metrics.csv'), '--stats-file', str(root/'sync.csv')]
(root/'command.json').write_text(json.dumps(command, ensure_ascii=False, indent=2)+'\n')
if a.perf:
    command = ['perf', 'record', '-e', 'cpu-clock', '-F', '99', '--call-graph', 'dwarf',
               '-o', str(root/'perf.data'), '--'] + command
# 测量区间只覆盖节目进程，排除素材生成和事后解码校验。
start = time.monotonic()
with (root/'run.log').open('w') as log:
    run(command, stdout=log, stderr=log, timeout=a.seconds*3+30)
elapsed = time.monotonic()-start
with (root/'metrics.csv').open() as f:
    rows = [{k:float(v) for k,v in r.items()} for r in csv.DictReader(f)]
with (root/'sync.csv').open() as f:
    sync = list(csv.DictReader(f))
if len(rows) < a.seconds+1 or elapsed < a.seconds-.2:
    raise RuntimeError('运行时长或采样行数不足')
for r in rows:
    if r['rss_kib'] <= 0 or r['threads'] < 1 or r['video_queued'] > 16 or r['audio_queued'] > 64:
        raise RuntimeError('资源指标或队列上限异常')
    if r['cpu_percent'] < 0 or r['output_dropped'] or r['skipped_video'] or r['skipped_audio']:
        raise RuntimeError('出现非预期丢包/跳过或CPU统计异常')
for name in ('v0_frames','v1_frames','v2_frames','v3_frames','decode_us','encode_us','processing_us'):
    if any(y[name] < x[name] for x,y in zip(rows,rows[1:])) or rows[-1][name] <= 0:
        raise RuntimeError('累计指标不单调或未生效：'+name)
for i in range(4):
    if rows[-1][f'v{i}_frames'] < a.seconds*a.fps or rows[-1][f'a{i}_frames'] < a.seconds*48000//1024:
        raise RuntimeError('某路媒体提前结束，不能以黑帧静音冒充稳定性验证')
if int(sync[-1]['output_video']) != a.seconds*a.fps or int(sync[-1]['queue_packets']) != 0:
    raise RuntimeError('输出丢尾帧或未排空')
with (root/'probe.json').open('w') as probe:
    run([a.ffprobe, '-v', 'error', '-count_frames', '-show_entries',
         'stream=codec_name,nb_read_frames', '-of', 'json', root/'live.flv'],
        stdout=probe, timeout=a.seconds*3+30)
info = json.loads((root/'probe.json').read_text())
if int(info['streams'][0]['nb_read_frames']) != a.seconds*a.fps:
    raise RuntimeError('实际可解码视频帧数不符')
run([a.ffmpeg, '-v', 'error', '-xerror', '-i', root/'live.flv', '-f', 'null', '-'], timeout=a.seconds*3+30)
with (root/'record.index.tsv').open() as f:
    segments = list(csv.DictReader(f, delimiter='\t'))
if not segments or '录制完成' not in (root/'run.log').read_text():
    raise RuntimeError('并行录制未完成')
for segment in segments:
    run([a.ffmpeg, '-v', 'error', '-xerror', '-i', segment['path'], '-f', 'null', '-'], timeout=90)
# 排除第一条启动和最后收尾窗口；RSS首尾取窗口中位数，仍不代表长期无泄漏。
steady = rows[1:-1] or rows
window = max(1, min(5, len(steady)//3))
summary = dict(seconds_requested=a.seconds, wall_seconds=elapsed, width=a.width, height=a.height,
    fps=a.fps, sources=4, output_frames=int(sync[-1]['output_video']),
    cpu_percent_mean=statistics.mean(r['cpu_percent'] for r in steady),
    cpu_percent_max=max(r['cpu_percent'] for r in steady),
    rss_kib_max=max(r['rss_kib'] for r in rows),
    rss_window_delta_kib=statistics.median(r['rss_kib'] for r in steady[-window:])-
        statistics.median(r['rss_kib'] for r in steady[:window]),
    threads_max=max(r['threads'] for r in rows),
    decode_us=rows[-1]['decode_us'], encode_us=rows[-1]['encode_us'], processing_us=rows[-1]['processing_us'],
    queue_packets_peak=max(int(r['queue_packets']) for r in sync),
    wall_lag_us_max=max(int(r['wall_lag_us']) for r in sync),
    recording_segments=len(segments), perf_requested=a.perf,
    note='本地短时负载，不是实际RTMP、24小时稳定性或端到端延迟验收')
(root/'summary.json').write_text(json.dumps(summary, ensure_ascii=False, indent=2)+'\n')
if a.perf:
    with (root/'perf-report.txt').open('w') as report:
        run(['perf', 'report', '--stdio', '--no-children', '-i', root/'perf.data', '--percent-limit', '1'],
            stdout=report, stderr=subprocess.STDOUT, timeout=120)
print(json.dumps(summary, ensure_ascii=False, indent=2))

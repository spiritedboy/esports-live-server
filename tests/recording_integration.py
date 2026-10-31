# 检查每段独立解码、索引定位及回放，同时检验录制失败对主输出的隔离。
"""Phase 6真实录制、关键帧提取、实时回放与录制故障隔离。"""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time
p = argparse.ArgumentParser()
for name in ('program', 'replay', 'ffmpeg', 'ffprobe', 'work'):
    p.add_argument('--' + name, required=True)
a = p.parse_args()

def run(cmd, ok=True):
    r = subprocess.run([str(x) for x in cmd], capture_output=True, text=True, timeout=30)
    if ok and r.returncode != 0:
        raise RuntimeError(str(cmd) + '\n' + r.stderr)
    if not ok and r.returncode == 0:
        raise RuntimeError('预期失败却成功: ' + str(cmd))
    return r

def probe(path):
    return json.loads(run([a.ffprobe, '-v', 'error', '-show_streams', '-show_packets', '-show_data_hash', 'sha256', '-of', 'json', path]).stdout)

def validate(path, zero=True):
    data = probe(path)
    v = next((s for s in data['streams'] if s['codec_type'] == 'video'))
    au = next((s for s in data['streams'] if s['codec_type'] == 'audio'))
    if not (v['codec_name'] == 'h264' and au['codec_name'] == 'aac'):
        raise RuntimeError("校验失败：v['codec_name'] == 'h264' and au['codec_name'] == 'aac'")
    packets = data['packets']
    videos = [x for x in packets if x['stream_index'] == v['index']]
    if not (videos and 'K' in videos[0]['flags']):
        raise RuntimeError("校验失败：videos and 'K' in videos[0]['flags']")
    if zero:
        if not abs(float(videos[0]['dts_time'])) < 0.002:
            raise RuntimeError("校验失败：abs(float(videos[0]['dts_time'])) < 0.002")
    elif not abs(float(videos[0]['dts_time']) - 0.021) < 0.002:
        raise RuntimeError("校验失败：abs(float(videos[0]['dts_time']) - 0.021) < 0.002")
    for s in data['streams']:
        ts = [float(x['dts_time']) for x in packets if x['stream_index'] == s['index']]
        if not all((y > x for x, y in zip(ts, ts[1:]))):
            raise RuntimeError('校验失败：all((y > x for x, y in zip(ts, ts[1:])))')
    run([a.ffmpeg, '-v', 'error', '-xerror', '-i', path, '-f', 'null', '-'])
    return videos
Path(a.work).mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix='recording-', dir=a.work) as tmp:
    root = Path(tmp)
    src = root / 'input.mp4'
    run([a.ffmpeg, '-v', 'error', '-f', 'lavfi', '-i', 'testsrc2=size=160x120:rate=25', '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000', '-t', '3.2', '-c:v', 'libx264', '-threads', '1', '-c:a', 'aac', src])
    common = [a.program, '--input', src, '--input', src, '--width', '320', '--height', '240', '--duration-ms', '3200', '--gop-ms', '1000', '--no-realtime']

    def live(name, extra=()):
        return run(common + ['--output', root / (name + '.flv')] + list(extra))
    for fmt in ('mp4', 'mkv'):
        prefix = root / fmt
        r = live(fmt, ['--record-prefix', prefix, '--record-format', fmt, '--segment-ms', '1000'])
        if not '录制完成' in r.stderr:
            raise RuntimeError(r.stderr)
        with Path(str(prefix) + '.index.tsv').open() as f:
            rows = list(csv.DictReader(f, delimiter='\t'))
        if not len(rows) == 4:
            raise RuntimeError(rows)
        recorded = []
        for i, row in enumerate(rows):
            if not int(row['segment']) == i:
                raise RuntimeError("校验失败：int(row['segment']) == i")
            if not abs(int(row['start_us']) - i * 1000000) < 2000:
                raise RuntimeError("校验失败：abs(int(row['start_us']) - i * 1000000) < 2000")
            if not int(row['end_us']) > int(row['start_us']):
                raise RuntimeError("校验失败：int(row['end_us']) > int(row['start_us'])")
            recorded += validate(row['path'])
        original = probe(root / (fmt + '.flv'))
        original_v = [x for x in original['packets'] if x['stream_index'] == 0]
        if not len(recorded) == len(original_v) == 80:
            raise RuntimeError('校验失败：len(recorded) == len(original_v) == 80')
        if not [x['data_hash'] for x in recorded] == [x['data_hash'] for x in original_v]:
            raise RuntimeError("校验失败：[x['data_hash'] for x in recorded] == [x['data_hash'] for x in original_v]")
        print('通过：' + fmt + ' 直播与4段录制并行，80帧包哈希一致，各段独立解码、索引和DTS正确')
    r = live('long', ['--record-prefix', root / 'long', '--segment-ms', '10000'])
    if not '录制完成' in r.stderr:
        raise RuntimeError("校验失败：'录制完成' in r.stderr")
    recording = root / 'long-000000.mp4'
    keys = run([a.replay, '--input', recording, '--list-keyframes']).stdout.splitlines()
    if not len(keys) == 5:
        raise RuntimeError(keys)
    clip = root / 'clip.mkv'
    r = run([a.replay, '--input', recording, '--output', clip, '--start-ms', '1400', '--duration-ms', '700'])
    if not '实际片段起点us=1000000' in r.stderr:
        raise RuntimeError(r.stderr)
    if not len(validate(clip)) == 28:
        raise RuntimeError('校验失败：len(validate(clip)) == 28')
    print('通过：关键帧扫描、1.4秒请求向前定位至1.0秒、提取至2.1秒，可解码')
    start = time.monotonic()
    run([a.replay, '--input', recording, '--output', root / 'play.flv', '--mode', 'replay', '--duration-ms', '1000'])
    elapsed = time.monotonic() - start
    if not 0.9 < elapsed < 3:
        raise RuntimeError(elapsed)
    validate(root / 'play.flv')
    print(f'通过：真实回放1秒按DTS节奏发送，墙钟{elapsed:.3f}秒')
    for name, extra in [('missing', ['--record-prefix', root / 'absent' / 'record']), ('tiny', ['--record-prefix', root / 'tiny', '--record-queue-bytes', '1024']), ('exists', ['--record-prefix', root / 'mp4'])]:
        r = live(name, extra)
        if not '录制已停止，直播继续' in r.stderr:
            raise RuntimeError(r.stderr)
        if not len(validate(root / (name + '.flv'), zero=False)) == 80:
            raise RuntimeError("校验失败：len(validate(root / (name + '.flv'), zero=False)) == 80")
    print('通过：目录不存在、录制容量不足、已有索引均隔离失败，直播80帧完整')
    marker = root / 'conflict-000001.mp4'
    marker.write_bytes(b'keep-existing')
    r = live('conflict', ['--record-prefix', root / 'conflict', '--segment-ms', '1000'])
    if not '录制已停止，直播继续' in r.stderr:
        raise RuntimeError("校验失败：'录制已停止，直播继续' in r.stderr")
    if not marker.read_bytes() == b'keep-existing':
        raise RuntimeError("校验失败：marker.read_bytes() == b'keep-existing'")
    validate(root / 'conflict-000000.mp4')
    if not len(validate(root / 'conflict.flv', zero=False)) == 80:
        raise RuntimeError("校验失败：len(validate(root / 'conflict.flv', zero=False)) == 80")
    print('通过：第二段路径冲突停止录制，首段可用且不覆盖旧文件，直播继续')
    before = hashlib.sha256(clip.read_bytes()).hexdigest()
    run([a.replay, '--input', recording, '--output', clip], ok=False)
    if not hashlib.sha256(clip.read_bytes()).hexdigest() == before:
        raise RuntimeError('校验失败：hashlib.sha256(clip.read_bytes()).hexdigest() == before')
    run([a.replay, '--input', recording, '--output', root / 'outside.mp4', '--start-ms', '9000'], ok=False)
    broken = root / 'broken.mp4'
    broken.write_bytes(b'bad media')
    run([a.replay, '--input', broken, '--output', root / 'bad.mp4'], ok=False)
    for opts in (['--record-format', 'avi'], ['--segment-ms', '0']):
        run(common + ['--output', root / 'invalid.flv'] + opts, ok=False)
    print('通过：已有片段不覆盖、越界定位、损坏录像、非法录制参数拒绝')
    log = root / 'signal.log'
    with log.open('w') as f:
        proc = subprocess.Popen([str(x) for x in common if x != '--no-realtime'] + ['--output', str(root / 'signal.flv'), '--record-prefix', str(root / 'signal')], stderr=f, stdout=f)
        time.sleep(0.6)
        proc.send_signal(signal.SIGINT)
        code = proc.wait(timeout=8)
    if not code == 130:
        raise RuntimeError(log.read_text())
    validate(root / 'signal-000000.mp4')
    print('通过：SIGINT录制线程排空写尾，部分MP4可解码')

    indexed = root / 'indexed.mp4'
    r = run([a.replay, '--index', root/'mp4.index.tsv', '--start-ms', '1400',
             '--duration-ms', '400', '--output', indexed])
    if 'mp4-000001.mp4' not in r.stderr or len(validate(indexed)) != 20:
        raise RuntimeError('索引定位第二段及关键帧提取失败：'+r.stderr)
    run([a.replay, '--index', root/'mp4.index.tsv', '--start-ms', '1400',
         '--duration-ms', '900', '--output', root/'cross.mp4'], ok=False)
    bad_index = root/'bad.tsv'
    bad_index.write_text('segment\tstart_us\tend_us\tpath\n0\tbad\t10\tx\n')
    run([a.replay, '--index', bad_index, '--output', root/'bad-index.mp4'], ok=False)
    print('通过：节目索引定位第二段、拒绝跨段提取及损坏索引')

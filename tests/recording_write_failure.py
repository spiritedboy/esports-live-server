"""内核EFBIG写失败，不填满真实磁盘，也不冒充ENOSPC/断电验收。"""
import argparse
import csv
import json
from pathlib import Path
import subprocess
import tempfile
p = argparse.ArgumentParser()
for key in ('fixture','program','ffmpeg','ffprobe','work'):
    p.add_argument('--'+key, required=True)
a = p.parse_args()
Path(a.work).mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(dir=a.work, prefix='write-failure-') as tmp:
    root = Path(tmp)
    for name, limit in [('baseline',0), ('limited',4096)]:
        # 必须用PIPE而不是将stdout直接重定向普通文件，否则主支路也会受文件上限影响。
        result = subprocess.run([a.fixture, str(root/name), str(limit)], capture_output=True, timeout=15)
        log = result.stderr.decode(errors='replace')
        if result.returncode != 0 or '主支路完整：视频=100' not in log:
            raise RuntimeError(log)
        live = root/(name+'.flv'); live.write_bytes(result.stdout)
        info = json.loads(subprocess.check_output([a.ffprobe, '-v', 'error', '-show_packets',
            '-show_data_hash', 'sha256', '-of', 'json', str(live)], timeout=10))
        frames = [pkt['data_hash'] for pkt in info['packets'] if pkt['codec_type']=='video']
        if len(frames) != 100:
            raise RuntimeError('写失败影响主支路视频完整性')
        references = [line[len('SHA256REF='):] for line in log.splitlines() if line.startswith('SHA256REF=')]
        if frames != references:
            raise RuntimeError('主FLV视频包与本次分流前编码包不一致')
        subprocess.run([a.ffmpeg, '-v', 'error', '-xerror', '-i', str(live), '-f', 'null', '-'],
                       check=True, timeout=10)
        with (root/(name+'.index.tsv')).open() as f:
            rows = list(csv.DictReader(f, delimiter='\t'))
        if limit:
            if '录制已停止，直播继续' not in log or 'File too large' not in log:
                raise RuntimeError('没有观察到真实EFBIG写入错误：'+log)
            if rows or (root/(name+'-000000.mp4')).stat().st_size > limit or '录制完成' in log:
                raise RuntimeError('失败段错误登记成功或突破内核文件上限')
        else:
            if len(rows)!=1 or '录制完成' not in log:
                raise RuntimeError('对照组录制未成功：'+log)
            subprocess.run([a.ffmpeg, '-v', 'error', '-xerror', '-i', rows[0]['path'], '-f', 'null', '-'],
                           check=True, timeout=10)
    print('通过：真实内核EFBIG、失败MP4不登记、主FLV100帧完整且与各自分流前包哈希一致')
    print('范围：文件大小上限错误；没有模拟真实磁盘耗尽、断电或成功RTMP接收')

    # 真实esports_live在文件上限下必须走错误退出，而不是被SIGXFSZ杀死。
    import resource
    def limit_files():
        resource.setrlimit(resource.RLIMIT_FSIZE, (4096, 4096))
    result = subprocess.run([a.program, '--input', str(root/'baseline-000000.mp4'),
        '--input', str(root/'baseline-000000.mp4'), '--output', str(root/'live-limited.flv'),
        '--no-realtime'], capture_output=True, timeout=15, preexec_fn=limit_files)
    if result.returncode != 1 or b'File too large' not in result.stderr:
        raise RuntimeError('主程序未正常报告文件上限错误，退出码='+str(result.returncode))
    print('通过：真实esports_live文件上限失败被报告，退出1而非被SIGXFSZ终止')

"""配置合并、严格类型、只读校验及真实媒体执行。"""
import argparse
import json
import subprocess
import tempfile
from pathlib import Path
p = argparse.ArgumentParser()
for key in ('program','sample','ffmpeg','ffprobe','work'):
    p.add_argument('--'+key, required=True)
a = p.parse_args()
Path(a.work).mkdir(parents=True, exist_ok=True)

def run(args, ok=True):
    result = subprocess.run([a.program]+list(map(str,args)), capture_output=True, text=True, timeout=15)
    if (result.returncode == 0) != ok:
        raise RuntimeError(str(args)+'\n'+result.stderr)
    return result

def require(value, message):
    if not value:
        raise RuntimeError(message)

with tempfile.TemporaryDirectory(dir=a.work, prefix='config-') as tmp:
    root = Path(tmp)
    config = root/'配置.json'
    basic = {'inputs':['a.mp4','b.mp4'], 'output':str(root/'untouched.flv')}
    def save(value):
        config.write_text(json.dumps(value, ensure_ascii=False), encoding='utf-8')
    def dump(extra=()):
        return json.loads(run(['--config',config,'--dump-config']+list(extra)).stdout)
    save(basic)
    defaults = dump()
    require(defaults['width']==640 and defaults['height']==360 and defaults['fps']==25, '缺省值错误')
    require(defaults['audio_inputs']==basic['inputs'] and defaults['gains']==[1,1], '音频默认继承错误')
    require(not (root/'untouched.flv').exists(), 'dump意外创建输出')
    save(dict(basic, width=1280,height=720,fps=30,realtime=False,gains=[.25,.5],muted=[True,True],
              audio_offsets_ms=[100,-200],video_offsets_ms=[-50,0],change_at_ms=500,change_gain=[1,.8],
              switch_at_ms=1000,switch_layout='pip'))
    effective = dump(['--width','320','--height','240','--fps','20','--realtime','--gain','0:1',
                      '--unmute','1','--audio-offset-ms','1:50'])
    require(effective['width']==320 and effective['fps']==20 and effective['realtime'], '标量覆盖错误')
    require(effective['gains']==[1,.5] and effective['muted']==[True,False], '逐路覆盖错误')
    require(effective['audio_offsets_ms']==[100,50] and effective['change_gain']==[1,.8], '偏移或动态配置丢失')
    before = json.loads(run(['--width','320','--config',config,'--dump-config']).stdout)
    require(before['width']==320,'--config位置改变优先级')
    run(['--config',config,'--input','new1','--input','new2','--audio-input','sound','--check-config'], ok=False)
    save(basic)
    changed = dump(['--input','new1','--input','new2','--audio-input','sound'])
    require(changed['inputs']==['new1','new2'] and changed['audio_inputs']==['sound'], 'CLI列表没有替换配置列表')
    save(dict(basic, fps=0))
    require(dump(['--fps','25'])['fps']==25, '未在覆盖后校验有效值')
    save(defaults)
    require(dump()==defaults, '有效配置导出后不能重新加载')
    run(['--config',a.sample,'--check-config'])
    run(['--config',config,'--config',config,'--check-config'], ok=False)
    run(['--config',root/'missing.json','--check-config'], ok=False)
    print('通过：默认值、JSON、CLI优先级，列表替换、逐路覆盖、双向布尔覆盖和导出往返')

    invalid = [dict(basic, unknown=1), dict(basic, width='640'), dict(basic, width=True),
        dict(basic, fps=25.5), dict(basic, width=18),dict(basic, gains=[5]),
        dict(basic, muted=[1]),dict(basic, realtime=1),dict(basic, inputs=[]),
        dict(basic, inputs=['x']*5),dict(basic, audio_offsets_ms=[0,0,0]),
        dict(basic, change_gain=[0]),dict(basic, switch_at_ms=100),
        dict(basic, output='rtmp://test/live',realtime=False),
        dict(basic, record_format='avi'), dict(basic, packet_queue_bytes=1),
        dict(basic, width=None), dict(basic, layout={'nested':'split'}),
        dict(basic, output='nul\x00path'), dict(basic, fps=1e100)]
    for value in invalid:
        save(value); run(['--config',config,'--check-config'], ok=False)
    malformed = ['{"width":1,"width":2}', '{"width":01}', '{"width":1.}',
        '{"fps":1e999}', '{"width":2,}', '[]', '{"inputs":[[[[[1]]]]]}',
        '{"output":"\\uD800"}', '{} garbage', '{} {}', '{', '', '{"fps":'+('9'*10000)+'}']
    for value in malformed:
        config.write_text(value); run(['--config',config,'--check-config'], ok=False)
    config.write_bytes(b'{"output":"\xff"}')
    run(['--config',config,'--check-config'], ok=False)
    import os
    fifo = root/'config.fifo'; os.mkfifo(fifo)
    run(['--config',fifo,'--check-config'],ok=False)
    config.write_bytes(b' '*(1024*1024+1))
    run(['--config',config,'--check-config'], ok=False)
    require(not (root/'untouched.flv').exists(),'错误配置创建了输出')
    print('通过：未知字段、重复键、错类型、越界、非法JSON/UTF-8/NUL、超大文件均在启动前拒绝')

    source = root/'源.mp4'
    subprocess.run([a.ffmpeg,'-v','error','-f','lavfi','-i','testsrc2=size=160x120:rate=25',
        '-f','lavfi','-i','sine=frequency=440:sample_rate=48000','-t','1.2',
        '-c:v','libx264','-threads','1','-c:a','aac',str(source)],check=True,timeout=15)
    live = root/'配置输出.flv'
    save(dict(inputs=[str(source),str(source)],output=str(live),width=320,height=240,fps=20,
        duration_ms=1000,realtime=False,record_prefix=str(root/'record'),segment_ms=500,
        stats_file=str(root/'sync.csv'),metrics_file=str(root/'metrics.csv')))
    run(['--config',config,'--fps','25'])
    info=json.loads(subprocess.check_output([a.ffprobe,'-v','error','-count_frames',
        '-show_entries','stream=width,height,nb_read_frames','-of','json',str(live)],timeout=15))
    require(info['streams'][0]['nb_read_frames']=='25' and info['streams'][0]['width']==320, 'JSON没有真正驱动媒体链路')
    for path in [live,root/'record-000000.mp4']:
        subprocess.run([a.ffmpeg,'-v','error','-xerror','-i',str(path),'-f','null','-'],check=True,timeout=15)
    require((root/'sync.csv').exists() and (root/'metrics.csv').exists(),'JSON统计路径未生效')
    before=live.read_bytes();run(['--config',config],ok=False)
    require(live.read_bytes()==before,'JSON输出绕过文件保护')
    print('通过：JSON启动真实混合音视频/录制，CLI覆盖输出25帧，中文路径和已有文件保护通过')

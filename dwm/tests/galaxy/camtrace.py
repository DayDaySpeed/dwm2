#!/usr/bin/env python3
"""分析 GALAXY_TRACE=1 时日志里的镜头轨迹 (每帧一行 `cam t sa sb mix dist pitch yaw roll tx ty tz`).

用法: camtrace.py [日志, 默认 ~/.cache/dwm-galaxy:7.log]
输出:
  - 各量的最大变化速度: 角度 (度/秒)、距离 (焦距/秒)、目标点 (焦距/秒, 按 2130px 焦距换算)
  - 「跳变」: 某帧的速度比前后 0.5 秒的中位数大 4 倍以上且超过阈值, 通常意味着镜头参数不连续
平滑的标准: 没有跳变; yaw 最大角速度 < 40 度/秒, 目标点速度 < .35 焦距/秒。
"""
import os, statistics, sys

path = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser('~/.cache/dwm-galaxy:7.log')
rows = []
for line in open(path):
    if line.startswith('cam '):
        f = line.split()
        rows.append([float(f[1]), int(f[2]), int(f[3])] + [float(x) for x in f[4:]])
if len(rows) < 10:
    sys.exit('日志里没有镜头轨迹 (启动测试 dwm 时要带 GALAXY_TRACE=1)')

FOCAL = 2130.0
names = ['dist', 'pitch', 'yaw', 'roll', 'target']
rate = {n: [] for n in names}
times = []
for a, b in zip(rows, rows[1:]):
    dt = b[0] - a[0]
    if dt <= 0 or dt > .2:
        continue
    times.append(b[0])
    rate['dist'].append(abs(b[4] - a[4]) / dt)
    for i, n in ((5, 'pitch'), (6, 'yaw'), (7, 'roll')):
        d = (b[i] - a[i] + 180) % 360 - 180
        rate[n].append(abs(d) / dt)
    rate['target'].append(sum((b[i] - a[i]) ** 2 for i in (8, 9, 10)) ** .5 / FOCAL / dt)

unit = {'dist': '焦距/秒', 'pitch': '度/秒', 'yaw': '度/秒', 'roll': '度/秒', 'target': '焦距/秒'}
floor = {'dist': .05, 'pitch': 3, 'yaw': 3, 'roll': 3, 'target': .05}
print(f'{len(times)} 帧, {times[-1] - times[0]:.1f} 秒')
jumps = 0
for n in names:
    v = rate[n]
    print(f'  {n:7s} 最大 {max(v):7.2f} {unit[n]}   p99 {sorted(v)[int(len(v) * .99)]:7.2f}')
    for i, x in enumerate(v):
        med = statistics.median(v[max(0, i - 15):i + 16])
        if x > floor[n] and x > 4 * (med + floor[n] / 4):
            jumps += 1
            if jumps <= 15:
                r = rows[i + 1]
                print(f'    跳变 t={times[i] - times[0]:7.2f}s {n} {x:.2f} (中位 {med:.2f})  机位 {r[1]}->{r[2]} mix {r[3]:.2f}')
print('跳变总数', jumps)

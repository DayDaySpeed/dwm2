#!/usr/bin/env python3
"""找录屏里的跳帧: 逐帧计算与上一帧的平均灰度差, 与前后 1 秒的中位数比较, 列出最突兀的几帧.
用法: jumps.py 录屏.mp4 [从第几秒开始 默认 9.5 (跳过开场)] [列出几帧 默认 10]
录屏用 record.sh (默认 20fps). 某帧比值明显高于 2.5 且前后都低时, 多半是一帧的跳变; 连续几帧都高是快速但连续的变化.
注意 Xvfb 软件渲染偶尔慢一帧也会表现为单帧高值, 需要结合画面判断."""
import os, statistics, subprocess, sys, tempfile

video = sys.argv[1]
start = float(sys.argv[2]) if len(sys.argv) > 2 else 9.5
top = int(sys.argv[3]) if len(sys.argv) > 3 else 10
W, H, FPS = 160, 90, 20
raw = subprocess.run(['ffmpeg', '-loglevel', 'error', '-i', video, '-vf', f'fps={FPS},scale={W}:{H},format=gray',
                      '-f', 'rawvideo', '-'], capture_output=True, check=True).stdout
n = len(raw) // (W * H)
fr = [raw[i * W * H:(i + 1) * W * H] for i in range(n)]
diff = [sum(abs(a - b) for a, b in zip(fr[i], fr[i + 1])) / (W * H) for i in range(n - 1)]
rows = []
for i, d in enumerate(diff):
    t = (i + 1) / FPS
    if t < start:
        continue
    med = statistics.median(diff[max(0, i - FPS // 2):i + FPS // 2 + 1])
    rows.append((d / (med + .05), t, d, med))
rows.sort(reverse=True)
for r, t, d, med in sorted(rows[:top], key=lambda x: x[1]):
    print(f't={t:7.2f}s  diff={d:5.2f}  median={med:5.2f}  ratio={r:4.1f}')

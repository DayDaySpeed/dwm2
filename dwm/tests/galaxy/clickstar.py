#!/usr/bin/env python3
"""驻留态截一帧, 找出指定颜色的窗口卡片 (匹配像素最密的格子附近的质心), 移过去稍等再点击.
用法: clickstar.py R G B [容差]   例: clickstar.py 74 26 92 40  (purple-a)"""
import os, subprocess, sys, time
from PIL import Image

disp = os.environ.get('GALAXY_TEST_DISPLAY', ':7')
rt = os.path.join(os.environ.get('XDG_RUNTIME_DIR', '/tmp'), 'galaxy-test')
shot = os.path.join(rt, 'click.png')
R, G, B = map(int, sys.argv[1:4])
tol = int(sys.argv[4]) if len(sys.argv) > 4 else 30
env = dict(os.environ, DISPLAY=disp)
size = subprocess.run(['xdpyinfo'], capture_output=True, text=True, env=env).stdout.split('dimensions:')[1].split()[0]
W, H = map(int, size.split('x'))


def locate():
    subprocess.run(['ffmpeg', '-loglevel', 'error', '-y', '-f', 'x11grab', '-video_size', size, '-i', disp,
                    '-frames:v', '1', '-vf', 'scale=640:-2', shot], check=True)
    im = Image.open(shot).convert('RGB')
    px, (w, h) = im.load(), im.size
    pts = [(x, y) for y in range(h) for x in range(w) if sum(abs(a - b) for a, b in zip(px[x, y], (R, G, B))) < tol]
    if len(pts) < 30:
        print('not found')
        sys.exit(1)
    cells = {}
    for x, y in pts:
        cells[(x // 20, y // 20)] = cells.get((x // 20, y // 20), 0) + 1
    cx, cy = max(cells, key=cells.get)
    near = [(x, y) for x, y in pts if abs(x - (cx * 20 + 10)) < 40 and abs(y - (cy * 20 + 10)) < 40]
    k = W / w
    return int(sum(x for x, _ in near) / len(near) * k), int(sum(y for _, y in near) / len(near) * k), len(near)


X, Y, n = locate()
# 驻留态默认只展示: 首次左键唤醒鼠标, 不选中窗口。
subprocess.run(['xdotool', 'click', '1'], env=env, check=True)
subprocess.run(['xdotool', 'mousemove', str(X), str(Y)], env=env)
time.sleep(1.2)                      # 等镜头因交互停住
X, Y, n = locate()
subprocess.run(['xdotool', 'mousemove', str(X), str(Y), 'click', '1'], env=env)
print('click', X, Y, 'pixels', n)

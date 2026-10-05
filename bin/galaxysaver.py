#!/usr/bin/env python3
"""galaxysaver.py — 星系屏保守护 (autostart 启动): 无操作 10 分钟进入 Super+Z 星系, 20 分钟由 xss-lock 锁屏 (见 bin/dpms.sh)

做法: 每 5 秒用 libXss (XScreenSaverQueryInfo) 读空闲时间, 到点时在 root 上设置 _DWM_GALAXY=saver,
dwm 收到后以屏保模式启动星系 (任何按键 / 点击 / 移动鼠标都会飞回桌面)。
只读空闲时间, 不模拟任何输入, 所以不会重置 xss-lock 的锁屏计时。

不触发的情况:
  - 自动锁屏已暂停 (Super+P 菜单 pause auto lock, 即存在 $XDG_CACHE_HOME/nolock)
  - 当前窗口是全屏 (看视频 / 演示时)
  - 这一轮空闲已经触发过 (要等有输入、空闲时间回落之后才会再次触发)

  galaxysaver.py           守护进程
  galaxysaver.py idle      打印当前空闲毫秒数 (调试用)
环境变量 GALAXY_SAVER_SEC 可改触发时间 (秒, 默认 600)
"""
import ctypes
import ctypes.util
import os
import subprocess
import sys
import time

SAVER_SEC = float(os.environ.get('GALAXY_SAVER_SEC', 600))
PAUSED = os.path.join(os.environ.get('XDG_CACHE_HOME') or os.path.expanduser('~/.cache'), 'nolock')


class XScreenSaverInfo(ctypes.Structure):
    _fields_ = [('window', ctypes.c_ulong), ('state', ctypes.c_int), ('kind', ctypes.c_int),
                ('til_or_since', ctypes.c_ulong), ('idle', ctypes.c_ulong), ('eventMask', ctypes.c_ulong)]


xlib = ctypes.cdll.LoadLibrary(ctypes.util.find_library('X11') or 'libX11.so.6')
xss = ctypes.cdll.LoadLibrary(ctypes.util.find_library('Xss') or 'libXss.so.1')
xlib.XOpenDisplay.restype = ctypes.c_void_p
xlib.XOpenDisplay.argtypes = [ctypes.c_char_p]
xlib.XDefaultRootWindow.restype = ctypes.c_ulong
xlib.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
xss.XScreenSaverAllocInfo.restype = ctypes.POINTER(XScreenSaverInfo)
xss.XScreenSaverQueryInfo.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(XScreenSaverInfo)]


def idle_ms(dpy, root, info):
    if not xss.XScreenSaverQueryInfo(dpy, root, info):
        return 0
    return info.contents.idle


def fullscreen():
    """当前活动窗口是否全屏 (_NET_WM_STATE_FULLSCREEN)"""
    try:
        win = subprocess.run(['xprop', '-root', '_NET_ACTIVE_WINDOW'], capture_output=True, text=True, timeout=3).stdout.split()[-1]
        if not win.startswith('0x') or int(win, 16) == 0:
            return False
        state = subprocess.run(['xprop', '-id', win, '_NET_WM_STATE'], capture_output=True, text=True, timeout=3).stdout
        return '_NET_WM_STATE_FULLSCREEN' in state
    except (OSError, IndexError, ValueError, subprocess.SubprocessError):
        return False


def main():
    dpy = xlib.XOpenDisplay(None)
    if not dpy:
        sys.exit('galaxysaver: cannot open display')
    root = xlib.XDefaultRootWindow(dpy)
    info = xss.XScreenSaverAllocInfo()
    if len(sys.argv) > 1 and sys.argv[1] == 'idle':
        print(idle_ms(dpy, root, info))
        return
    fired = False
    while True:
        time.sleep(5)
        idle = idle_ms(dpy, root, info) / 1000
        if idle < SAVER_SEC:
            fired = False
            continue
        if fired or os.path.exists(PAUSED) or fullscreen():
            continue
        fired = True
        subprocess.run(['xprop', '-root', '-f', '_DWM_GALAXY', '8s', '-set', '_DWM_GALAXY', 'saver'], timeout=5)


if __name__ == '__main__':
    main()

#!/bin/bash
# 起一个独立的测试会话: Xvfb + xrender 后端的 picom + 测试副本 dwm-galaxytest, 并建好测试窗口
#   start.sh            单屏 2560x1440
#   start.sh --multi    Xinerama 双屏: 2560x1440 (左) + 1920x1080 (右), 用嵌在隐藏 Xvfb 里的 Xephyr 实现
# 测试副本的进程名不是 dwm, 不会被 bin/reload.sh (pgrep -x dwm) 误中
. "$(dirname "$0")/lib.sh"
if xdpyinfo >/dev/null 2>&1; then echo "$DISPLAY 已经在运行"; exit 1; fi
if [ "$1" = --multi ]; then
    # Xvfb 的多屏 Xinerama 不能和 Composite 同时用, RandR 虚拟显示器也不支持;
    # 改为在一个看不见的 Xvfb ($GALAXY_TEST_HOST, 默认 :17) 里嵌一个 Xephyr, 两块屏左右排开
    HOST=${GALAXY_TEST_HOST:-:17}
    setsid -f Xvfb $HOST -screen 0 4600x1500x24 -nolisten tcp > "$RT/xvfb-host.log" 2>&1
    for i in $(seq 1 50); do DISPLAY=$HOST xdpyinfo >/dev/null 2>&1 && break; pause .1; done
    DISPLAY=$HOST setsid -f Xephyr $DISPLAY -screen 2560x1440 -screen 1920x1080 +xinerama \
        +extension Composite +extension RENDER -ac -nolisten tcp > "$RT/xvfb.log" 2>&1
else
    setsid -f Xvfb $DISPLAY -screen 0 2560x1440x24 +extension Composite +extension RENDER -nolisten tcp > "$RT/xvfb.log" 2>&1
fi
for i in $(seq 1 50); do xdpyinfo >/dev/null 2>&1 && break; pause .1; done
pause .5
printf 'backend = "xrender";\nvsync = false;\nshadow = false;\nfading = false;\n' > "$RT/picom.conf"
setsid -f picom --config "$RT/picom.conf" > "$RT/picom.log" 2>&1
cc -O2 -o "$RT/xres" "$T/xres.c" -lX11 || exit 1
make -C "$REPO/dwm" >/dev/null || exit 1
cp "$REPO/dwm/dwm" "$RT/dwm-galaxytest"
pause .5
DWM_RESTARTED=1 DWM=$REPO setsid -f "$RT/dwm-galaxytest" > "$RT/dwm.log" 2>&1 < /dev/null
pause 1.5
# 壁纸放在 dwm 启动之后设, 并确认 _XROOTPMAP_ID 已写入 (太早设偶尔不生效, 星系背景会是黑的)
for i in 1 2 3 4 5; do
    feh --bg-fill "$REPO/wallpaper/static/wallhaven-7jxlg3_1920x1080.png"
    xprop -root _XROOTPMAP_ID | grep -q 'pixmap id' && break
    pause .5
done
"$T/setup.sh"
clients=$(xprop -root _NET_CLIENT_LIST)
count=$(printf '%s\n' "$clients" | grep -oE '0x[[:xdigit:]]+' | wc -l)
if [ "$count" -lt 8 ]; then
    echo "测试会话 $DISPLAY 启动失败: 预期至少 8 个测试窗口, 实际 $count 个 ($clients)" >&2
    exit 1
fi
echo "测试会话 $DISPLAY 就绪: $count 个窗口"

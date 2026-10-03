#! /bin/bash
# 壁纸: 动态 (xwinwrap 在桌面最底层放一个窗口, mpv 在里面循环播放视频) 或 静态 (feh 随机图片)
#   livewall.sh auto     开机 (autostart 调用): 动态模式且有视频 -> 播放随机视频, 否则随机静态壁纸
#   livewall.sh next     换一张: 动态模式换一个视频, 静态模式换一张图片 (状态栏菜单图标左键)
#   livewall.sh toggle   动态 / 静态 切换, 切过去后随机挑一个 (Super+P 菜单 switch wallpaper); 选择会记住, 下次开机沿用
#   livewall.sh cron     定时换壁纸 (autostart 每 300 秒调用): 只换静态壁纸, 视频本身在循环, 不打断
# 视频放 $DWM/wallpaper/live (不提交到 git), 图片放 $DWM/wallpaper/static
# 动态模式下根窗口 (feh) 设为视频的一帧: 锁屏背景 (bin/blurlock.sh 读 ~/.fehbg) 也就是这一帧
# 切换时视频窗口淡入淡出, 与根窗口上的图片 / 视频帧交叉过渡

DWM=${DWM:-$(cd "$(dirname "$0")/.."; pwd)}
PATH=$HOME/.local/bin:$PATH                # 手动编译安装的 xwinwrap 可能在这里
LIVE=$DWM/wallpaper/live
STATIC=$DWM/wallpaper/static
CACHE=${XDG_CACHE_HOME:-$HOME/.cache}/livewall
MODE=$CACHE/mode                           # live / static, 默认 live
SOCK=$CACHE/mpv.sock                       # mpv 的 IPC, 换视频时不重开播放器
START=1                                    # 视频从第几秒开始播放, 也从这里截帧 (很多视频第 0 秒是黑场)

# 切换动画: 视频窗口的透明度 (_NET_WM_WINDOW_OPACITY, picom 优先于 opacity-rule) 分 STEPS 步渐变,
# 每步间隔 STEP_SEC, 先慢后快再慢 (smoothstep); picom 的 fading 再把相邻两步之间补平滑
STEPS=16
STEP_SEC=0.02                              # 加上 xprop 本身的耗时, 一次淡入 / 淡出约 0.7 秒

mode() { cat "$MODE" 2>/dev/null || echo live; }
videos() { find "$LIVE" -maxdepth 1 -type f \( -iname '*.mp4' -o -iname '*.mkv' -o -iname '*.webm' -o -iname '*.mov' \) 2>/dev/null; }
running() { pgrep -x xwinwrap >/dev/null; }
can_live() { command -v xwinwrap >/dev/null && command -v mpv >/dev/null && [ -n "$(videos)" ]; }

# 给 mpv 发一条命令 (JSON IPC), 打印回复中的 data; mpv 不在时什么也不打印
ipc() {
    python3 - "$SOCK" "$@" 2>/dev/null <<'EOF'
import json, socket, sys
s = socket.socket(socket.AF_UNIX)
s.settimeout(1)
s.connect(sys.argv[1])
s.sendall((json.dumps({"command": sys.argv[2:]}) + "\n").encode())
for line in s.makefile():
    r = json.loads(line)
    if "error" in r:                       # 跳过事件, 只要命令的回复
        print(r.get("data", ""))
        break
EOF
}

# 等 mpv 真正播放 $1 并画出几帧 (之前窗口是透明的), 最多 5 秒
wait_frames() {
    local i
    for i in $(seq 50); do
        [ "$(ipc get_property path)" = "$1" ] && [ "$(ipc get_property estimated-frame-number)" -ge 3 ] 2>/dev/null && return
        sleep 0.1
    done
}

# 视频窗口: mpv 窗口的父窗口, 即 xwinwrap 建的顶层窗口 (picom 按它算透明度)
tops() {
    local w
    for w in $(xdotool search --classname '^livewall$' 2>/dev/null); do
        xwininfo -id "$w" -tree 2>/dev/null | awk '/Parent window id/{print $4}'
    done | sort -u
}

# fade in|out: 视频窗口淡入 / 淡出
fade() {
    local i t v id wins=$(tops)
    for i in $(seq 1 $STEPS); do
        t=$i; [ "$1" = out ] && t=$((STEPS - i))
        v=$((4294967295 * (3 * t * t * STEPS - 2 * t * t * t) / (STEPS * STEPS * STEPS)))
        for id in $wins; do xprop -id "$id" -f _NET_WM_WINDOW_OPACITY 32c -set _NET_WM_WINDOW_OPACITY $v 2>/dev/null; done
        sleep $STEP_SEC
    done
}

stop_video() {
    local i
    pkill -x xwinwrap                      # xwinwrap 收到 SIGTERM 会转给 mpv
    pkill -f -- '--x11-name=livewall'      # 以防万一 mpv 没跟着退出
    # 等它们退出干净: 旧窗口还在时新开的 xwinwrap 会把它当成桌面, 建在它里面
    for i in $(seq 20); do
        pgrep -x xwinwrap >/dev/null || pgrep -f -- '--x11-name=livewall' >/dev/null || return 0
        sleep 0.1
    done
    pkill -9 -x xwinwrap; pkill -9 -f -- '--x11-name=livewall'
}

# 生成锁屏背景很吃 CPU (ImageMagick 处理整屏图片), 等切换动画结束后再做, 避免动画卡顿
lock_bg() { (sleep 2; $DWM/bin/blurlock.sh update) >/dev/null 2>&1 8>&- & }

# 静态: 先把新图片设到视频下面 (根窗口), 再让视频淡出, 露出新图片
static() {
    feh --randomize --bg-fill "$STATIC"
    running && fade out
    stop_video
    lock_bg
}

# 动态: 随机挑一个视频 (尽量不和当前的重复)
#   原来是静态: 新开播放器, 窗口全透明, 等画面出来后淡入, 盖住图片
#   原来是视频: 把新视频的第一帧设到根窗口, 旧视频淡出; 同一个播放器换成新视频 (从截帧处开始),
#              再淡入 (淡入前后画面相同, 看不出接缝). 不能同时开两个 xwinwrap: 新的会把旧窗口
#              当成桌面建在它里面, 旧的一关新的也被销毁
play() {
    local v frame
    v=$(videos | grep -vxF "$(cat "$CACHE/current" 2>/dev/null)" | shuf -n1)
    [ -n "$v" ] || v=$(videos | head -1)
    mkdir -p "$CACHE/frames"
    frame=$CACHE/frames/$(printf %s "$v" | md5sum | cut -c1-12).jpg
    [ -f "$frame" ] || nice ffmpeg -v error -y -ss $START -i "$v" -frames:v 1 -q:v 2 "$frame"
    if running && [ -n "$(ipc get_property pid)" ]; then
        feh --bg-fill "$frame"
        fade out
        ipc loadfile "$v" >/dev/null
    else
        stop_video                         # 没有 IPC 的残留进程
        # -b -ov -fdt: 最底层、不受 dwm 管理、桌面类型窗口 (picom 中按 window_type = 'desktop' 排除阴影圆角等)
        # -o 0: 窗口以全透明出现; --panscan=1.0 裁剪铺满屏幕 (同 feh --bg-fill)
        # opengl + 硬解: 用核显 (vaapi) 解码, 启动约 0.1 秒; 默认的 vulkan 会唤醒独显, 启动要 2~3 秒
        setsid -f xwinwrap -g "$(xwininfo -root | awk '/-geometry/{print $2}')" -ni -s -st -sp -nf -b -un -ov -fdt -o 0 -- \
            mpv --wid=%WID --x11-name=livewall --no-config --input-ipc-server="$SOCK" \
                --start=$START --loop-file=inf --no-audio \
                --no-osc --no-osd-bar --no-input-default-bindings --input-vo-keyboard=no \
                --panscan=1.0 --vo=gpu-next --gpu-api=opengl --hwdec=auto-safe --really-quiet "$v" >/dev/null 2>&1 8>&-
    fi
    wait_frames "$v"
    fade in
    feh --bg-fill "$frame"                 # 根窗口 (被视频盖住) 设为视频的一帧: 视频退出后不黑屏, 锁屏背景也取这一帧
    echo "$v" > "$CACHE/current"
    lock_bg
}

mkdir -p "$CACHE"
exec 8>"$CACHE/.lock"
flock 8                                    # 连续快速切换时排队执行, 不让两次动画交叠

case $1 in
    auto)
        if [ "$(mode)" = live ] && can_live; then
            running || play                # dwm 原地重启 (Super+Shift+R) 会再次执行 autostart, 不打断正在播放的
        else
            static
        fi ;;
    next)
        if [ "$(mode)" = live ] && can_live; then play; else static; fi ;;
    toggle)
        if running; then
            echo static > "$MODE"; static
        elif can_live; then
            echo live > "$MODE"; play
        else
            notify-send " 动态壁纸" "没有可用的视频 ($LIVE) 或未安装 xwinwrap / mpv"
        fi ;;
    cron)
        running || static ;;
esac

#! /bin/bash
# 壁纸: 动态 (xwinwrap 在桌面最底层放一个窗口, mpv 在里面循环播放视频) 或 静态 (feh 随机图片)
#   livewall.sh auto     开机 (autostart 调用): 动态模式且有视频 -> 播放随机视频, 否则随机静态壁纸
#   livewall.sh next     换一张: 动态模式换一个视频, 静态模式换一张图片 (状态栏菜单图标左键)
#   livewall.sh toggle   动态 / 静态 切换, 切过去后随机挑一个 (Super+P 菜单 switch wallpaper mode to live / static); 选择会记住, 下次开机沿用
#   livewall.sh cron     定时换壁纸 (autostart 每 300 秒调用): 只换静态壁纸, 视频本身在循环, 不打断
#   livewall.sh sync     把 Steam 上新订阅的 Wallpaper Engine 壁纸复制进 wallpaper/live (auto 时也会自动做)
# 动态壁纸都在 $DWM/wallpaper/live, 图片放 $DWM/wallpaper/static。wallpaper/ 是私有 Git LFS 子模块, 播放不依赖 Steam
#   live 下的视频文件: mpv 播放
#   live 下的 Wallpaper Engine 场景目录 (含 project.json): linux-wallpaperengine 渲染 (未安装时跳过)
#     开一个盖住屏幕的窗口, 改成不受 dwm 管理的最底层窗口 (见 start_scene)
#   Steam 只用来下载: 订阅后执行 sync (或重新登录) 复制过来, 之后可以取消订阅; 网页类壁纸不支持; 不想要的直接从 live 删掉
# 多显示器: 几块屏显示同一张壁纸, 各自完整铺满 (不会一张画面横跨几块屏); 视频 / 场景每屏开一个实例 (见 monitors, pick)
# 动态模式下根窗口 (feh) 设为各屏当前壁纸的真实截图 (全分辨率): 锁屏背景 (bin/blurlock.sh 读 ~/.fehbg) 也就是这几帧
# 切换: 截下当前画面铺到根窗口定格 (A), 关掉 / 隐藏旧壁纸, 新壁纸全透明启动, 画好后截图 (B),
#   bin/transition.py 用 GPU 在各屏同时播放 A->B 的转场特效 (config/transitions/ 下的 gl-transitions 着色器, 每次随机一个,
#   见 TRANSITION / TRANSITION_SAVE), 播完露出正在运行的新壁纸; 切到静态图片 (含每 5 分钟换图) 也用同一个转场;
#   起始画面截不到时用缓存帧 / 纯黑 (见 start_frame); 转场程序出错时直接显示, 记到 transition.log
#   全程只有真实的全分辨率画面 (以前铺的场景预览图多是小方图, 放大后会闪一下模糊)
# 显卡 (核显渲染 4K 视频会大量丢帧, 4K60 实测丢约 28%):
#   混合模式 (X 跑在核显上): 视频用 PRIME render offload 在独显上渲染 (高画质缩放 + 去色带,
#     低于屏幕分辨率的用 Anime4K 放大), 整机多耗约 10~20W; 场景用核显
#   独显直连 (BIOS 切换, 见 docs/manual/dwm.md): X 本身在独显上, 视频和场景都用独显, 场景 60 帧
#   省电模式 (bin/powersave.sh) 会切到静态壁纸; 期间手动切回动态时, 混合模式下只用核显, 不唤醒独显

DWM=${DWM:-$(cd "$(dirname "$0")/.."; pwd)}
PATH=$HOME/.local/bin:$PATH                # 手动编译安装的 xwinwrap 可能在这里
LIVE=$DWM/wallpaper/live
STATIC=$DWM/wallpaper/static
STEAM=$HOME/.local/share/Steam/steamapps
WS=$STEAM/workshop/content/431960          # Steam 下载的 Wallpaper Engine 壁纸 (431960 是 Wallpaper Engine 的 appid)
WE_ASSETS=$STEAM/common/wallpaper_engine/assets
ASSETS=$LIVE/.assets                       # 场景公共资源的副本 (linux-wallpaperengine --assets-dir)
IMPORTED=$LIVE/.imported                   # 已从 Steam 导入的壁纸 id
GPU=${GPU:-nvidia}                         # nvidia: 独显渲染 (不可用时退回核显); intel: 只用核显, 省电
SCENE_FPS_INTEL=30                         # 场景渲染帧率: 核显 (混合模式 / 省电模式)
SCENE_FPS_NV=60                            #               独显 (独显直连时, 见 play_scene)
NV_ENV=(__NV_PRIME_RENDER_OFFLOAD=1 __GLX_VENDOR_LIBRARY_NAME=nvidia __VK_LAYER_NV_optimus=NVIDIA_only)
ANIME4K=1                                  # 1: 低于屏幕分辨率的视频用 Anime4K 放大 (只在独显上); 0: 关闭
A4K=$DWM/config/anime4k
TRANSITION=${TRANSITION:-random}           # 壁纸切换的转场 (动态 / 静态都用): random (每次随机) / fade (GPU 淡入) / config/transitions/ 下的着色器名
TRANSITION_SAVE=${TRANSITION_SAVE:-random} # 省电模式下的转场, 取值同上; 想素雅一点设成 fade (GPU 逐帧缓动淡入)
TRANS_SEC=0.8                              # 转场时长 (秒)
POWERSAVE=${XDG_CACHE_HOME:-$HOME/.cache}/powersave/on   # 省电模式开着 (bin/powersave.sh)
CACHE=${XDG_CACHE_HOME:-$HOME/.cache}/livewall
MODE=$CACHE/mode                           # live / static, 默认 live
START=1                                    # 视频从第几秒开始播放, 也从这里截帧 (很多视频第 0 秒是黑场)


mode() { cat "$MODE" 2>/dev/null || echo live; }
# 独显直连 (BIOS 里切到独显模式) 时 NVIDIA 是主卡 (boot_vga): X 本身就跑在独显上, 不需要 PRIME offload, 核显不可用
nv_primary() {
    local d
    for d in /sys/bus/pci/devices/*; do
        [ "$(cat "$d/vendor" 2>/dev/null)" = 0x10de ] && [ "$(cat "$d/boot_vga" 2>/dev/null)" = 1 ] && return 0
    done
    return 1
}

# 用哪张显卡渲染视频: 直连时只有独显; 混合模式下省电模式 (bin/powersave.sh) 开着时只用核显, 不唤醒独显
gpu() {
    if nv_primary; then echo nvidia
    elif [ "$GPU" = nvidia ] && [ ! -f "$POWERSAVE" ] && nvidia-smi -L >/dev/null 2>&1; then echo nvidia
    else echo intel
    fi
}

# live 下所有可播放的动态壁纸, 每行 "类型<TAB>路径": video<TAB>视频文件 / scene<TAB>Wallpaper Engine 壁纸目录
items() {
    local mpv= scene=
    command -v xwinwrap >/dev/null && command -v mpv >/dev/null && mpv=1
    command -v linux-wallpaperengine >/dev/null && scene=1
    python3 - "$LIVE" "$mpv" "$scene" <<'EOF'
import json, os, sys
live, mpv, scene = sys.argv[1], sys.argv[2] == "1", sys.argv[3] == "1"
VIDEO = (".mp4", ".mkv", ".webm", ".mov")
for name in sorted(os.listdir(live)) if os.path.isdir(live) else []:
    p = os.path.join(live, name)
    if os.path.isfile(p):
        if mpv and name.lower().endswith(VIDEO):
            print("video\t" + p)
        continue
    try:
        j = json.load(open(os.path.join(p, "project.json"), encoding="utf-8-sig"))
    except Exception:
        continue                           # 不是 Wallpaper Engine 壁纸, 或还没下载完
    t = str(j.get("type", "")).lower()     # 有的作者写 Video / Scene
    if t == "video" and mpv and os.path.isfile(os.path.join(p, j.get("file", ""))):
        print("video\t" + os.path.join(p, j["file"]))
    elif t == "scene" and scene:
        print("scene\t" + p)
EOF
}

# 把 Steam 下载的壁纸复制进 live (之后不再依赖 Steam, 复制完可以在 Steam 里取消订阅):
#   视频类只复制视频文件, 以标题命名; 场景类复制整个目录; 场景要用的 Wallpaper Engine 公共资源复制到 $ASSETS
#   导入过的记在 $IMPORTED, 不会重复导入: 不想要的壁纸直接从 live 删掉即可
sync_ws() {
    mkdir -p "$LIVE"
    python3 - "$WS" "$LIVE" "$IMPORTED" "$WE_ASSETS" "$ASSETS" <<'EOF'
import filecmp, json, os, re, shutil, sys
ws, live, imported, we_assets, assets = sys.argv[1:]
if os.path.isdir(we_assets) and not os.path.isdir(assets):
    shutil.copytree(we_assets, assets)
    print("+ Wallpaper Engine 公共资源 -> " + assets)
done = set(open(imported).read().split()) if os.path.exists(imported) else set()
for d in sorted(os.listdir(ws)) if os.path.isdir(ws) else []:
    src = os.path.join(ws, d)
    if d in done:
        continue
    try:
        j = json.load(open(os.path.join(src, "project.json"), encoding="utf-8-sig"))
    except Exception:
        continue                           # 还没下载完
    t = str(j.get("type", "")).lower()     # 有的作者写 Video / Scene
    title = re.sub(r'[/\\:*?"<>|\x00-\x1f]', "_", str(j.get("title", "")).strip())[:40].strip() or d
    if t == "video":
        f = os.path.join(src, j.get("file", ""))
        if not os.path.isfile(f):
            continue
        ext = os.path.splitext(f)[1].lower()
        dst = os.path.join(live, title + ext)
        if os.path.isfile(dst) and filecmp.cmp(f, dst, shallow=False):
            pass                           # 同一个视频以前手动放过
        else:
            if os.path.lexists(dst):
                dst = os.path.join(live, f"{title}-{d}{ext}")
            shutil.copy2(f, dst + ".part")  # 先写临时名, 复制到一半不会被当成壁纸播放
            os.rename(dst + ".part", dst)
    elif t == "scene":
        dst = os.path.join(live, title)
        if os.path.lexists(dst):
            dst = os.path.join(live, f"{title}-{d}")
        shutil.copytree(src, dst + ".part")
        os.rename(dst + ".part", dst)
    else:
        continue                           # 网页类等不支持
    with open(imported, "a") as fp:
        fp.write(d + "\n")
    print("+ " + os.path.basename(dst))
EOF
}

# 显示器: 每块屏都显示同一张壁纸, 各自完整铺满 (视频 / 场景每屏开一个实例, 静态每屏铺一份同样的图片), 不再一张画面横跨几块屏
#   区域和顺序取自 xrandr --listmonitors (主屏在前), 与 feh --bg-fill 多张图片、betterlockscreen 多个 -u 的对应顺序一致;
#   复制模式下几块屏区域相同, 只算一块。第 i 块屏的实例: 视频窗口 livewall<i> (mpv 的 IPC 是 mpv.<i>.sock),
#   场景按命令行里的 --window 区域区分。每行一个区域 WxH+X+Y
monitors() { xrandr --listmonitors 2>/dev/null | awk 'NR > 1 {g = $3; gsub(/\/[0-9]+/, "", g); if (!seen[g]++) print g}'; }

# 第 $1 块屏的区域, 设置 W H X Y
geom() { local g=${MON[$1]}; W=${g%%x*}; g=${g#*x}; H=${g%%+*}; g=${g#*+}; X=${g%%+*}; Y=${g#*+}; }

SCENE_ANY='(^|/)linux-wallpaperengine .*(--screen-root|--window)'
scene_pat() { geom "$1"; echo "(^|/)linux-wallpaperengine .*--window ${X}x${Y}x${W}x${H} "; }
video_pat() { echo "--x11-name=livewall$1 "; }

video_running() { pgrep -f -- "(^|/)xwinwrap .*$(video_pat "$1")" >/dev/null; }
scene_running() { pgrep -f -- "$(scene_pat "$1")" >/dev/null; }
running() { pgrep -x xwinwrap >/dev/null || pgrep -f -- "$SCENE_ANY" >/dev/null; }   # 任一块屏在放动态壁纸
can_live() { [ -n "$(items)" ]; }

# 给第 $1 块屏的 mpv 发一条命令 (JSON IPC), 打印回复中的 data; mpv 不在时什么也不打印
ipc() {
    python3 - "$CACHE/mpv.$1.sock" "${@:2}" 2>/dev/null <<'EOF'
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

# 等第 $1 块屏的 mpv 真正播放 $2 并画出几帧 (之前窗口是透明的), 最多 5 秒
wait_frames() {
    local k
    for k in $(seq 50); do
        [ "$(ipc "$1" get_property path)" = "$2" ] && [ "$(ipc "$1" get_property estimated-frame-number)" -ge 3 ] 2>/dev/null && return
        sleep 0.1
    done
    return 1
}

# 视频窗口: mpv 窗口的父窗口, 即 xwinwrap 建的顶层窗口 (picom 按它算透明度); $1 为屏幕序号, 省略时为所有屏
tops() {
    local w
    for w in $(xdotool search --classname "^livewall${1:-[0-9]*}\$" 2>/dev/null); do
        xwininfo -id "$w" -tree 2>/dev/null | awk '/Parent window id/{print $4}'
    done | sort -u
}

# 第 $1 块屏的场景窗口 (start_scene 记下的), 不在时返回非 0
scene_win() {
    local w
    w=$(cat "$CACHE/scene.$1.win" 2>/dev/null) && [ -n "$w" ] && xwininfo -id "$w" >/dev/null 2>&1 && echo "$w"
}

# 关掉第 $1 块屏的视频
stop_video() {
    local k p
    p=$(video_pat "$1")
    video_running "$1" && hide $(tops "$1")   # 先设为全透明: picom 关闭窗口的动画 (上下合拢) 不受 animation-exclude 管
    pkill -f -- "(^|/)xwinwrap .*$p"          # xwinwrap 收到 SIGTERM 会转给 mpv
    pkill -f -- "(^|/)mpv .*$p"               # 以防万一 mpv 没跟着退出
    # 等它们退出干净
    for k in $(seq 20); do
        pgrep -f -- "(^|/)(xwinwrap|mpv) .*$p" >/dev/null || break
        sleep 0.1
    done
    pkill -9 -f -- "(^|/)(xwinwrap|mpv) .*$p"
    # 进程退出后 X 服务器销毁窗口还会慢一拍, 窗口真正消失后才能开新的
    for k in $(seq 20); do
        xdotool search --classname "^livewall$1\$" >/dev/null 2>&1 || return 0
        sleep 0.1
    done
}

# 关掉第 $1 块屏的场景
stop_scene() {
    local k p
    scene_running "$1" || return 0
    p=$(scene_pat "$1")
    hide $(scene_win "$1")                 # 同 stop_video, 先设为全透明
    pkill -f -- "$p"
    for k in $(seq 20); do
        scene_running "$1" || break
        sleep 0.1
    done
    pkill -9 -f -- "$p"
    rm -f "$CACHE/scene.$1.win"
}

# 关掉所有屏的动态壁纸, 包括屏幕布局改变前开的 (位置、大小已经对不上)
stop_all() {
    local k
    running || return 0
    hide $(tops) $(xdotool search --class '^linux-wallpaperengine$' 2>/dev/null)
    pkill -x xwinwrap
    pkill -f -- '--x11-name=livewall'
    pkill -f -- "$SCENE_ANY"
    for k in $(seq 20); do
        pgrep -x xwinwrap >/dev/null || pgrep -f -- '--x11-name=livewall' >/dev/null || pgrep -f -- "$SCENE_ANY" >/dev/null || break
        sleep 0.1
    done
    pkill -9 -x xwinwrap; pkill -9 -f -- '--x11-name=livewall'; pkill -9 -f -- "$SCENE_ANY"
    for k in $(seq 20); do
        xdotool search --classname '^livewall[0-9]*$' >/dev/null 2>&1 || break
        sleep 0.1
    done
    rm -f "$CACHE"/scene.*.win
}

# 生成锁屏背景很吃 CPU (ImageMagick 处理整屏图片), 等切换动画结束后再做, 避免动画卡顿
lock_bg() { (sleep 2; $DWM/bin/blurlock.sh update) >/dev/null 2>&1 8>&- & }

# 从标准输入的候选 (每行一个) 里随机挑一个, 给 $1 块屏都用它 (打印 $1 行, 每块屏一行): 几块屏显示同一张, 各自铺满;
# 避开第一块屏当前的 (第 2 个参数; 开机第一次时为空, 什么都不避开), 只有这一个候选时照样用它
pick() {
    local n=$1 all new i
    all=$(cat)
    [ -n "$all" ] || return 0
    new=$(grep -vxF -e "${2:-}" <<<"$all" | shuf -n1)
    [ -n "$new" ] || new=$(shuf -n1 <<<"$all")
    for ((i = 0; i < n; i++)); do echo "$new"; done
}

# 静态: 随机换一张图片, 每块屏都铺它 (见 pick), 用 GPU 转场从当前画面过渡过去
#   起始画面见 start_frame (动态壁纸在跑时是定格帧, 原来是静态时是当前图片)
static() {
    local cur
    mapfile -t cur < <(cur_walls)
    mapfile -t TB < <(images | pick ${#MON[@]} "${cur[@]}")
    [ ${#TB[@]} -gt 0 ] || return 0
    start_frame                            # 根窗口上已是当前画面, 下面关掉旧壁纸看不出变化
    stop_all
    transition feh --bg-fill "${TB[@]}" || {
        echo "$(date '+%F %T') 转场失败 (A=${TA[*]} B=${TB[*]}), 直接显示新图片" >> "$CACHE/transition.log"
        feh --bg-fill "${TB[@]}"
    }
    lock_bg
}

# 静态图片列表
images() { find "$STATIC" -type f \( -iname '*.jpg' -o -iname '*.jpeg' -o -iname '*.png' -o -iname '*.webp' \) 2>/dev/null; }

# 根窗口上当前铺的图片 (~/.fehbg 里的路径, 每块屏一张, 每行一个)
cur_walls() { sed -n 's/^feh .*--bg-fill //p' ~/.fehbg 2>/dev/null | grep -o "'[^']*'" | tr -d "'"; }

# 第 $1 块屏当前的动态壁纸 ($CACHE/current 第 $1+1 行 "类型<TAB>路径" 里的路径)
cur_item() { sed -n "$(($1 + 1))p" "$CACHE/current" 2>/dev/null | cut -f2; }

# 动态壁纸 $1 的帧缓存路径: 播放时截的真实画面 (snapshot), 没有时用兜底帧 (frame_of)
frame_path() { echo "$CACHE/frames/$(printf %s "$1" | md5sum | cut -c1-12).jpg"; }

# 兜底帧 (还没播放过时用): 视频从 $START 秒截取; 场景用作者的预览图 (多是小方图, 放大后发糊), 打印缓存路径
frame_of() {
    local kind=$1 src=$2 f
    mkdir -p "$CACHE/frames"
    f=$(frame_path "$src")
    if [ ! -f "$f" ]; then
        if [ "$kind" = scene ]; then
            src=$(find "$src" -maxdepth 1 -iname 'preview.*' | head -1)
            [ -n "$src" ] && nice ffmpeg -v error -y -i "$src" -frames:v 1 -q:v 2 "$f"
        else
            nice ffmpeg -v error -y -ss $START -i "$src" -frames:v 1 -q:v 2 "$f"
        fi
    fi
    echo "$f"
}

# 后台给所有动态壁纸预先截好兜底帧: 切换时不用现场截 (4K 视频要一两秒)
prewarm() { (items | while IFS=$'\t' read -r kind v; do frame_of "$kind" "$v" >/dev/null; done) >/dev/null 2>&1 8>&- & }

# 纯黑画面 (什么都截不到时的起始画面), 打印路径
black() { [ -f "$CACHE/black.jpg" ] || magick -size 16x9 xc:black "$CACHE/black.jpg"; echo "$CACHE/black.jpg"; }

# 截下第 $1 块屏壁纸层的真实画面存到 $2 (全分辨率, 和屏幕上看到的一样), 成功返回 0:
#   视频: mpv 的 window 截图 (含缩放和 Anime4K); 场景: 窗口内容; 静态: 截不了 (用当前图片)
snapshot() {
    local tmp=$2.part.jpg w k s s0=
    rm -f "$tmp"
    if video_running "$1" && [ -n "$(ipc "$1" get_property pid)" ]; then
        ipc "$1" screenshot-to-file "$tmp" window >/dev/null
        for k in $(seq 40); do              # 截图在后台写文件, 等大小不再变化
            s=$(stat -c %s "$tmp" 2>/dev/null)
            [ -n "$s" ] && [ "$s" -gt 0 ] && [ "$s" = "$s0" ] && break
            s0=$s; sleep 0.05
        done
    elif w=$(scene_win "$1"); then
        import -window "$w" -quality 95 "$tmp" 2>/dev/null
    fi
    [ -s "$tmp" ] && mv "$tmp" "$2"
}

# 转场的起始画面 (每块屏一张, 存在数组 TA; 切换前调用, 之后旧壁纸可以直接关掉):
#   动态壁纸在跑: 截下各屏当前画面, 一起铺到根窗口定格 (和屏幕上的一模一样, 看不出变化);
#     截不到的屏用它的帧缓存 (上次播放时截的真实画面), 再没有用纯黑
#   静态: 当前图片 (~/.fehbg 里比屏幕少时, 缺的屏用第一张); 都没有 (开机第一次) 时用纯黑, 相当于从黑屏转场进来
start_frame() {
    local i f
    TA=()
    if running; then
        for i in "${!MON[@]}"; do
            f=$(frame_path "$(cur_item "$i")")
            if snapshot "$i" "$CACHE/snap.$i.jpg"; then TA[i]=$CACHE/snap.$i.jpg
            elif [ -n "$(cur_item "$i")" ] && [ -f "$f" ]; then TA[i]=$f
            else TA[i]=$(black)
            fi
        done
        feh --bg-fill "${TA[@]}"
    else
        mapfile -t TA < <(cur_walls)
        for i in "${!MON[@]}"; do
            [ -f "${TA[i]}" ] || TA[i]=${TA[0]}
            [ -f "${TA[i]}" ] || TA[i]=$(black)
        done
        TA=("${TA[@]:0:${#MON[@]}}")      # 屏幕变少了: 多出来的不要
    fi
}

# 这次用哪个转场着色器, 打印路径; 找不到时为空 (转场失败, 直接显示)
#   省电模式下按 $TRANSITION_SAVE, 否则按 $TRANSITION: random (随机, 不含 fade) / fade (GPU 淡入) / 着色器名
pick_transition() {
    local t=$TRANSITION d=$DWM/config/transitions
    [ -f "$POWERSAVE" ] && t=$TRANSITION_SAVE
    if [ "$t" = random ]; then
        ls "$d"/*.glsl 2>/dev/null | grep -v '/fade\.glsl$' | shuf -n1
    elif [ -f "$d/$t.glsl" ]; then
        echo "$d/$t.glsl"
    fi
}

# 暂停 / 继续第 $1 块屏的新壁纸: 屏幕 video|scene yes|no (转场期间停在截图的那一帧, 转场结束时和截图完全一样, 衔接无跳变)
pause_new() {
    if [ "$2" = video ]; then
        ipc "$1" set pause "$3" >/dev/null
    else
        pkill "-$([ "$3" = yes ] && echo STOP || echo CONT)" -f -- "$(scene_pat "$1")"
    fi
}

# GPU 转场: 每块屏从 TA 里它的那张图片过渡到 TB 里它的那张, 各屏同时播放, 成功返回 0
#   bin/transition.py 在最底层开铺满整个桌面的窗口, 画好 A (和屏幕上的一样) 后回 ready, 让它播放; 播完 (停在 B) 回 done,
#   这时执行回调 $@ 在它下面把最终画面准备好 (和 B 一样), 再让它退出, 露出的就是准备好的画面 (握手原因见 transition.py)
#   起来了但没等到 done (中途出错): 照样执行回调, 记到 transition.log; 根本没起来: 返回非 0, 由调用方直接显示
transition() {
    local shader line ok=1 i args=() in out
    shader=$(pick_transition)
    [ -n "$shader" ] || return 1
    for i in "${!MON[@]}"; do
        [ -f "${TA[i]}" ] && [ -f "${TB[i]}" ] || return 1
        args+=("${MON[i]}" "${TA[i]}" "${TB[i]}")
    done
    coproc TR { exec timeout 10 python3 "$DWM/bin/transition.py" "$shader" "$TRANS_SEC" "${args[@]}" 2>>"$CACHE/transition.log" 8>&-; }
    in=${TR[0]} out=${TR[1]}               # 它退出后 bash 会清掉 TR, 先记下
    if read -r -t 3 line <&"$in" && [ "$line" = ready ]; then
        echo go >&"$out"
        read -r -t 5 line <&"$in" && [ "$line" = done ] ||
            echo "$(date '+%F %T') 转场没有正常播完, 直接显示" >> "$CACHE/transition.log"
        "$@"
        echo ok >&"$out" 2>/dev/null
        ok=0
    else
        kill "$TR_PID" 2>/dev/null
    fi
    wait "$TR_PID" 2>/dev/null
    return $ok
}

# 把壁纸窗口压到转场窗口下面并设为不透明 (transition 的回调)
show_under() {
    local id
    for id in "$@"; do
        xdotool windowlower "$id"
        xprop -id "$id" -f _NET_WM_WINDOW_OPACITY 32c -set _NET_WM_WINDOW_OPACITY 4294967295
    done
}

# 新壁纸出场 (各屏都已画好, 但还是全透明; 类型在 KIND, 兜底帧在 FB):
#   全部暂停并截图 B (截不到时重试, 仍不行用兜底帧), 从 A ($TA, 见 start_frame) 转场到 B,
#   转场期间新壁纸被垫到下面; 结束后继续播放. 转场程序出错时直接显示新壁纸, 并记到 transition.log
reveal() {
    local i k wins=()
    TB=()
    for i in "${!MON[@]}"; do
        pause_new "$i" "${KIND[i]}" yes
        if [ "${KIND[i]}" = video ]; then wins+=($(tops "$i")); else wins+=($(scene_win "$i")); fi
    done
    for i in "${!MON[@]}"; do
        TB[i]=$CACHE/next.$i.jpg
        rm -f "${TB[i]}"
        for k in 1 2 3; do snapshot "$i" "${TB[i]}" && break; sleep 0.2; done
        [ -s "${TB[i]}" ] || TB[i]=${FB[i]}
    done
    transition show_under "${wins[@]}" || {
        echo "$(date '+%F %T') 转场失败 (A=${TA[*]} B=${TB[*]}), 直接显示新壁纸" >> "$CACHE/transition.log"
        show_under "${wins[@]}"
    }
    for i in "${!MON[@]}"; do pause_new "$i" "${KIND[i]}" no; done
}

# 新壁纸出场后: 截各屏的真实画面存为帧缓存并一起铺到根窗口 (锁屏背景、退出后的兜底画面都是全分辨率);
# 截不了的屏铺兜底帧
settle() {
    local i f=()
    for i in "${!MON[@]}"; do
        f[i]=$(frame_path "${ITEM[i]#*$'\t'}")
        snapshot "$i" "${f[i]}" || f[i]=${FB[i]}
    done
    feh --bg-fill "${f[@]}"
}

# 窗口立即设为全透明
hide() { local id; for id in "$@"; do xprop -id "$id" -f _NET_WM_WINDOW_OPACITY 32c -set _NET_WM_WINDOW_OPACITY 0 2>/dev/null; done; }

# 动态: 随机挑一个, 每块屏都放它 (见 pick), 视频 / 场景分别处理
# 切换方式 (定格帧转场): 先把各屏当前画面定格到根窗口, 关掉 / 隐藏旧壁纸, 新壁纸全透明启动, 画出来后一起转场出场;
# 整个过程屏幕上只有真实的全分辨率画面
play() {
    local i cur v
    mapfile -t cur < <(cat "$CACHE/current" 2>/dev/null)
    mapfile -t ITEM < <(items | pick ${#MON[@]} "${cur[@]}")
    start_frame
    # 屏幕布局变了 (monitor.sh 换布局后会调 next): 旧实例的位置、大小都对不上, 全部关掉 (画面已定格在根窗口)
    [ "$(cat "$CACHE/layout" 2>/dev/null)" = "${MON[*]}" ] || stop_all
    KIND=(); FB=(); SH=()
    for i in "${!MON[@]}"; do
        KIND[i]=${ITEM[i]%%$'\t'*}; v=${ITEM[i]#*$'\t'}
        FB[i]=$(frame_of "${KIND[i]}" "$v")
        if [ "${KIND[i]}" = scene ]; then start_scene "$i" "$v"; else start_video "$i" "$v"; fi
    done
    # 等新壁纸画好: 视频等 mpv 真正出帧 (偶尔新开的播放器出不来画面, 如窗口被 X 销毁得晚, 就关掉重开一次);
    # 场景刚启动时贴图还没加载完, 画面不完整, 等 1.5 秒
    for i in "${!MON[@]}"; do
        [ "${KIND[i]}" = video ] || continue
        v=${ITEM[i]#*$'\t'}
        wait_frames "$i" "$v" || { start_player "$i" "$v" "${SH[i]}"; wait_frames "$i" "$v"; }
    done
    printf '%s\n' "${KIND[@]}" | grep -qx scene && sleep 1.5
    reveal
    settle
    printf '%s\n' "${ITEM[@]}" > "$CACHE/current"
    echo "${MON[*]}" > "$CACHE/layout"
    lock_bg
}

# 场景: 在第 $1 块屏开窗口 (窗口模式, 正好盖住这块屏), 改成不受 dwm 管理的最底层窗口 (同视频的 xwinwrap 窗口), 先保持全透明
#   不用根窗口模式 (直接画在根窗口上): 它是在隐藏窗口里渲染再读回画面, NVIDIA 读不到隐藏窗口的内容 (独显直连
#   全黑, 混合模式 PRIME offload 只有左上角 640x480 有画面); 而且截不到画面做转场, 根窗口一变还会崩溃
start_scene() {
    local fps=$SCENE_FPS_INTEL args=() w before
    nv_primary && [ ! -f "$POWERSAVE" ] && fps=$SCENE_FPS_NV
    [ -d "$ASSETS" ] && args+=(--assets-dir "$ASSETS")
    stop_scene "$1"
    stop_video "$1"
    geom "$1"
    before=$(xdotool search --class '^linux-wallpaperengine$' 2>/dev/null)
    # 它按 XDG_SESSION_TYPE 选 X11 / Wayland, startx 启动时这个变量是 tty, 要显式指定
    XDG_SESSION_TYPE=x11 setsid -f linux-wallpaperengine --silent --fps $fps "${args[@]}" \
        --window "${X}x${Y}x${W}x${H}" --scaling fill "$2" >"$CACHE/scene.$1.log" 2>&1 8>&-
    if w=$(scene_window "$before"); then
        echo "$w" > "$CACHE/scene.$1.win"
        desktop_window "$w" "$1" || echo "$(date '+%F %T') 场景窗口转成最底层窗口失败: $2" >> "$CACHE/transition.log"
    else
        echo "$(date '+%F %T') 场景窗口 10 秒内没有出现: $2 (见 $CACHE/scene.$1.log)" >> "$CACHE/transition.log"
    fi
}

# 等新场景的窗口出现 (不在 $1 列出的已有窗口里; 最多 10 秒, 一般 0.2 秒), 打印窗口 id,
# 并马上设为全透明 (还没转成最底层窗口前不让它盖住其他窗口); 超过 1 秒才出现时记到 transition.log
scene_window() {
    local k w
    for k in $(seq 200); do
        w=$(xdotool search --onlyvisible --class '^linux-wallpaperengine$' 2>/dev/null | grep -vxF -f <(printf '%s\n' $1) | head -1)
        if [ -n "$w" ]; then
            xprop -id "$w" -f _NET_WM_WINDOW_OPACITY 32c -set _NET_WM_WINDOW_OPACITY 0
            [ "$k" -gt 20 ] && echo "$(date '+%F %T') 场景窗口 $((k * 50))ms 后才出现" >> "$CACHE/transition.log"
            echo "$w"
            return 0
        fi
        sleep 0.05
    done
    return 1
}

# 把窗口 $1 改成第 $2 块屏的壁纸窗口: 取消映射 (dwm 不再管理) -> 删掉 WM_STATE -> 设 override-redirect ->
# 重新映射、盖住这块屏、放到最底层 -> 输入穿透 (dwm 接管时可能把它挪到了当前屏, 这里放回原位)
#   WM_STATE: dwm 放手时只把它改成 Withdrawn, 不删; picom 见到 WM_STATE 就把窗口当成窗口管理器的客户窗口,
#   规则里的 override_redirect 对它恒为假, 会套上普通窗口的透明度规则, 盖过 _NET_WM_WINDOW_OPACITY (隐藏不了,
#   还挡住压在最底层的转场窗口); 要等 dwm 写完 Withdrawn 再删, 否则会被它写回来
#   不能设成桌面类型 (_NET_WM_WINDOW_TYPE_DESKTOP): xwinwrap 会把之后开的视频窗口建在桌面类型的窗口里,
#   场景一关视频也跟着没了; picom 改按 class_g = 'linux-wallpaperengine' 排除阴影圆角等
desktop_window() {
    local k
    geom "$2"
    xdotool windowunmap --sync "$1" || return 1
    for k in $(seq 20); do
        xprop -id "$1" WM_STATE 2>/dev/null | grep -q Withdrawn && break
        sleep 0.02
    done
    xprop -id "$1" -remove WM_STATE &&
    xdotool set_window --overrideredirect 1 "$1" &&
    xdotool windowmap --sync "$1" windowsize "$1" "$W" "$H" windowmove "$1" "$X" "$Y" windowlower "$1" 2>/dev/null &&
    input_passthrough "$1"
}

# 窗口 $1 的输入区域设为空, 鼠标事件穿过它落到根窗口 (同 xwinwrap -ni)
#   场景窗口自己订阅了鼠标事件, 不穿透的话 dwm 收不到根窗口上的移动和点击: 指针移到空的外接屏不换显示器, Super+滚轮切不了 tag
input_passthrough() {
    python3 - "$1" <<'EOF'
import ctypes as C, sys
x11, xext = C.CDLL("libX11.so.6"), C.CDLL("libXext.so.6")
x11.XOpenDisplay.restype = C.c_void_p
dpy = x11.XOpenDisplay(None)
if not dpy:
    sys.exit(1)
# ShapeInput = 2, ShapeSet = 0, 0 个矩形 = 空区域
xext.XShapeCombineRectangles(C.c_void_p(dpy), C.c_ulong(int(sys.argv[1], 0)), 2, 0, 0, None, 0, 0, 0)
x11.XCloseDisplay(C.c_void_p(dpy))
EOF
}

# 低于所在屏 ($2) 高度的视频 (1080p 等) 用 Anime4K 着色器在独显上放大, 打印 mpv 的 glsl-shaders 列表 (不放大时为空)
#   Mode A (HQ): 先修复压缩造成的模糊, 再用 CNN 放大两倍, 最后缩到屏幕大小
shaders() {
    local h sh=() f
    [ "$ANIME4K" = 1 ] && [ "$(gpu)" = nvidia ] || return 0
    geom "$2"
    # 只取第一个数字: 有的视频 ffprobe 会多输出几行 (附加数据等)
    h=$(ffprobe -v error -select_streams v:0 -show_entries stream=height -of csv=p=0 "$1" 2>/dev/null | grep -m1 -E '^[0-9]+$')
    [ -n "$h" ] && [ "$h" -lt "$H" ] || return 0
    for f in Clamp_Highlights Restore_CNN_VL Upscale_CNN_x2_VL AutoDownscalePre_x2 AutoDownscalePre_x4 Upscale_CNN_x2_M; do
        sh+=("$A4K/Anime4K_$f.glsl")
    done
    (IFS=:; echo "${sh[*]}")
}

# 在第 $1 块屏新开播放器 (先关掉这块屏残留的), 视频 $2, 着色器 $3
#   独显: vulkan 渲染 + nvdec 解码, 高画质缩放 (1080p 放大更锐, 4K 缩小不糊) + 去色带 (天空渐变不起条纹);
#         首次启动要唤醒独显 (2~3 秒), 之后换视频复用同一个播放器
#   核显: opengl + vaapi, 启动约 0.1 秒, 默认画质 (高画质缩放核显扛不住)
start_player() {
    local envs=() vo
    stop_video "$1"
    if [ "$(gpu)" = nvidia ]; then
        nv_primary || envs=("${NV_ENV[@]}")
        vo=(--vo=gpu-next --gpu-api=vulkan --hwdec=nvdec --profile=high-quality --deband)
    else
        vo=(--vo=gpu-next --gpu-api=opengl --hwdec=auto-safe)
    fi
    # -g: 正好盖住这块屏; -b -ov -fdt: 最底层、不受 dwm 管理、桌面类型窗口 (picom 中按 window_type = 'desktop' 排除阴影圆角等)
    # -o 0: 窗口以全透明出现; --panscan=1.0 裁剪铺满屏幕 (同 feh --bg-fill)
    # xwinwrap 会把和整个桌面一样大的窗口当成桌面, 把新窗口建在它里面; 每屏一个的窗口比整个桌面小, 几个 xwinwrap 互不影响
    env "${envs[@]}" setsid -f xwinwrap -g "${MON[$1]}" -ni -s -st -sp -nf -b -un -ov -fdt -o 0 -- \
        mpv --wid=%WID --x11-name=livewall$1 --no-config --input-ipc-server="$CACHE/mpv.$1.sock" \
            --start=$START --loop-file=inf --no-audio \
            --no-osc --no-osd-bar --no-input-default-bindings --input-vo-keyboard=no \
            --panscan=1.0 "${vo[@]}" --glsl-shaders="$3" --screenshot-format=jpg --screenshot-jpeg-quality=95 \
            --really-quiet "$2" >/dev/null 2>&1 8>&-
}

# 第 $1 块屏放视频 $2 (play 已经定格了当前画面, 见 start_frame), 新视频全透明启动, 由 play 等它出帧后转场出场:
#   原来是视频: 隐藏视频窗口 (看到的是定格帧), 同一个播放器换成新视频
#   原来是场景: 关掉场景再开播放器 (只有一块屏时场景窗口和整个桌面一样大, xwinwrap 会把它当成桌面,
#              把视频窗口建在它里面, 场景一关视频也没了)
#   原来是静态: 新开播放器
start_video() {
    SH[$1]=$(shaders "$2" "$1")
    if video_running "$1" && [ -n "$(ipc "$1" get_property pid)" ]; then
        hide $(tops "$1")
        ipc "$1" set glsl-shaders "${SH[$1]}" >/dev/null
        ipc "$1" loadfile "$2" >/dev/null
    else
        stop_scene "$1"
        start_player "$1" "$2" "${SH[$1]}"
    fi
}

mkdir -p "$CACHE"
exec 8>"$CACHE/.lock"
flock 8                                    # 连续快速切换时排队执行, 不让两次动画交叠
mapfile -t MON < <(monitors)
[ ${#MON[@]} -gt 0 ] || MON=("$(xwininfo -root | awk '/-geometry/{print $2}')")

case $1 in
    auto)
        sync_ws >/dev/null
        prewarm
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
            notify-send " 动态壁纸" "没有可用的视频 ($LIVE / Steam Workshop) 或未安装 xwinwrap / mpv"
        fi ;;
    cron)
        running || static ;;
    sync)
        sync_ws
        prewarm ;;
esac

#! /bin/bash
# 显示器布局 (Super+P 菜单 "󰍹 monitor layout", 接了外接屏时才出现)
#   monitor.sh menu       rofi 选择: 外接屏在右 / 左 / 上方、复制、仅外接屏、仅笔记本, 以及外接屏分辨率、缩放
#                         Esc / "↩ 返回" 回到上一层: 子菜单回到本菜单, 本菜单退出码 2 (rofi.sh 据此重新打开 Super+P 主菜单)
#   monitor.sh <布局>     right | left | above | mirror | external | laptop
#   monitor.sh detect     打印已连接的外接屏 (没有则为空)
#   monitor.sh check      插拔检测 (autostart 每 10 秒调用): 拔掉外接屏时点亮笔记本屏 (防止"仅外接屏"后黑屏);
#                         插上用过的显示器时套用它上次的布局, 新显示器弹通知提示从菜单选
#   monitor.sh login      登录时 (xinitrc, dwm 启动前): 套用保存的布局并设好缩放, 打印 Qt 缩放倍数
# 选好的布局按显示器 (EDID) 存成 autorandr 配置, 外接屏位置不固定, 换了位置从菜单重选即可
# 缩放: X11 整个桌面只有一个缩放比例 (Xft.dpi)。笔记本屏 (2560x1440 高密度) 在用时全局 135%, 只用外接屏时全局用外接屏的缩放;
#   外接屏的缩放默认 100%, 可在菜单里给这台显示器手动指定 (记在它的 autorandr 配置目录里)。
#   两块屏同时用 (扩展) 时, 外接屏按 135% / 外接屏缩放 倍放大的画面渲染再缩小显示 (xrandr --scale),
#   这样外接屏上看起来就是它自己的缩放, 代价是文字比原生略软。改了全局缩放会原地重启 dwm (窗口保留), 已打开的程序重开后生效

DWM=${DWM:-$(cd "$(dirname "$0")/.."; pwd)}
STATE=${XDG_CACHE_HOME:-$HOME/.cache}/monitor-connected
CONF=${XDG_CONFIG_HOME:-$HOME/.config}
PROFILES=$CONF/autorandr
NID=9541
BACK="↩ 返回"                                      # 各层菜单最后一项; 与 Esc 一样回到上一层 (退出码 2)
SCALES="115 125 135"                              # 菜单里除"默认 100%"外的选项
LAPTOP_SCALE=135                                   # 笔记本屏在用时的默认缩放, 与 config/Xresources 的 Xft.dpi: 130 一致

# 笔记本屏: NVIDIA 独显直连时名字是 DP-x, 靠 ConnectorType: Panel 识别; 核显下没有这个属性, 按 eDP / LVDS 名字识别
internal() {
    xrandr --current --prop | awk '/^[^ \t]/ {o = $1; c = ($2 == "connected")} c && /ConnectorType: Panel/ {print o; exit}' | grep . \
        || xrandr --current | awk '$2 == "connected" && $1 ~ /^(eDP|LVDS)/ {print $1; exit}'
}
external() { xrandr --current | awk -v i="$(internal)" '$2 == "connected" && $1 != i {print $1; exit}'; }

# 屏幕 $1 的首选分辨率 (xrandr 里带 + 的那个)
preferred() { xrandr --current | awk -v o="$1" '$1 == o {f = 1; next} f && /^[^ ]/ {exit} f && /\+/ {print $1; exit}'; }

# 布局切换后: 刷新率 (--auto 只给首选的 60Hz)、壁纸重新铺满新屏幕 (动态壁纸遇到屏幕变化会崩)、护眼
post() {
    "$DWM/bin/powersave.sh" apply
    setsid -f "$DWM/bin/livewall.sh" next >/dev/null 2>&1
    "$DWM/bin/nightlight.sh" apply >/dev/null 2>&1 || true      # 护眼关着时返回非 0
}

# 这台外接屏的 autorandr 配置名 (按 EDID)
profile() { echo "monitor-$(autorandr --fingerprint 2>/dev/null | awk -v o="$1" '$1 == o {print $2}' | md5sum | cut -c1-8)"; }

# 按这台外接屏的 EDID 保存当前布局, 下次插上同一台自动沿用 (autorandr --save 会清空目录, 先留住手动缩放)
save() {
    local p s
    p=$(profile "$1")
    s=$(cat "$PROFILES/$p/dwm-scale" 2>/dev/null)
    autorandr --save "$p" --force >/dev/null 2>&1
    [ -n "$s" ] && echo "$s" > "$PROFILES/$p/dwm-scale"
}

active() { xrandr --listmonitors | awk '{print $NF}' | grep -qx "$1"; }

# 外接屏自己的缩放: 手动指定过就用它, 否则 100%
extscale() {
    local s
    s=$(cat "$PROFILES/$(profile "$1")/dwm-scale" 2>/dev/null)
    echo "${s:-100}"
}

# 当前布局应有的全局缩放: 笔记本屏在用 135%, 只用外接屏时用外接屏的缩放
wantscale() {
    local ext
    if active "$(internal)"; then echo $LAPTOP_SCALE; return; fi
    ext=$(external)
    [ -n "$ext" ] && extscale "$ext" || echo 100
}

# 扩展布局时外接屏的放大倍数: 全局 135% / 外接屏缩放, 不小于 1
extfactor() { awk -v g=$LAPTOP_SCALE -v s="$(extscale "$1")" 'BEGIN {f = g / s; if (f < 1) f = 1; printf "%.4f\n", f}'; }

# 外接屏用的分辨率: 亮着就沿用当前的 (菜单里改过分辨率), 否则用首选的
extmode() {
    xrandr --current | awk -v o="$1" '$1 == o {f = 1; next} f && /^[^ ]/ {exit} f && /\*/ {print $1; exit}' | grep . || preferred "$1"
}

# 从当前屏幕摆放推断布局 (改分辨率 / 缩放后按原布局重新排)
curlayout() {
    local int ext
    int=$(internal); ext=$(external)
    [ -n "$ext" ] && active "$ext" || { echo laptop; return; }
    active "$int" || { echo external; return; }
    xrandr --listmonitors | awk -v i="$int" -v e="$ext" '
        {split($3, a, /[\/x+]/); x[$NF] = a[5]; y[$NF] = a[6]; h[$NF] = a[3]}
        END {
            if (x[e] == x[i] && y[e] == y[i]) print "mirror"
            else if (y[e] + h[e] <= y[i]) print "above"
            else if (x[e] < x[i]) print "left"
            else print "right"
        }'
}

curdpi() { xrdb -query | awk '$1 == "Xft.dpi:" {print $2}'; }
dpi() { echo $(( ($1 * 96 + 50) / 100 )); }         # 135% -> 130, 100% -> 96

# 原地重启 dwm, 让状态栏按新的 dpi 重新加载字体 (与 bin/reload.sh 相同的保护: 只有 ~/.local/bin/dwm 处理 SIGHUP)
restartdwm() {
    local pid exe
    pid=$(pgrep -x -u "$USER" dwm | head -1) || return
    exe=$(readlink /proc/$pid/exe 2>/dev/null)
    [ "${exe% (deleted)}" = "$(readlink -f ~/.local/bin/dwm)" ] || return
    kill -HUP "$pid"
    "$DWM/bin/statusbar/statusbar.sh" updateall >/dev/null 2>&1   # dwm 阻塞在等 X 事件, 触发一次让它立即重启
}

# 设缩放 $1 (百分比); $2 = norestart 时不重启 dwm (登录时 dwm 还没启动)
# 除了 Xft.dpi, GTK 的 settings.ini (Chrome / Edge / VSCode 在 X11 下也按它缩放) 和 electron-flags.conf 各有一份, 一起改
setscale() {
    local d f changed=
    d=$(dpi "$1")
    for f in "$CONF"/gtk-3.0/settings.ini "$CONF"/gtk-4.0/settings.ini; do
        [ -f "$f" ] && ! grep -qx "gtk-xft-dpi=$((d * 1024))" "$f" \
            && sed -i "s/^gtk-xft-dpi=.*/gtk-xft-dpi=$((d * 1024))/" "$f" && changed=1
    done
    f="$CONF/electron-flags.conf"
    [ -f "$f" ] && ! grep -q -- "--force-device-scale-factor=$(factor "$1")\$" "$f" \
        && sed -i "s/--force-device-scale-factor=[0-9.]*/--force-device-scale-factor=$(factor "$1")/" "$f" && changed=1
    if [ "$(curdpi)" != "$d" ]; then
        echo "Xft.dpi: $d" | xrdb -merge
        changed=1
        [ "$2" = norestart ] || restartdwm
    fi
    [ -n "$changed" ] && [ "$2" != norestart ] \
        && notify-send -r $NID "󰍹 缩放 $1%" "已打开的程序重开后生效 (Chrome / Edge 关窗口后还在后台, 要从菜单完全退出)"
    return 0
}
factor() { awk -v s="$1" 'BEGIN {printf "%.2f\n", s / 100}'; }   # 135 -> 1.35

apply() {
    local int ext args=() o
    int=$(internal)
    ext=$(external)
    [ -n "$int" ] || { notify-send -r $NID "󰍹 显示器" "找不到笔记本屏幕"; return 1; }
    if [ "$1" = laptop ]; then
        # 关掉其它所有输出, 包括已拔掉但还占着画面的
        for o in $(xrandr --current | awk -v i="$int" '$1 != i && ($2 == "connected" || $2 == "disconnected") {print $1}'); do
            args+=(--output "$o" --off)
        done
        xrandr --output "$int" --auto --primary "${args[@]}"
    else
        [ -n "$ext" ] || { notify-send -r $NID "󰍹 显示器" "没有检测到外接显示器"; return 1; }
        local m f iw ih ew eh
        m=$(extmode "$ext")
        f=$(extfactor "$ext")
        iw=$(preferred "$int"); ih=${iw#*x}; iw=${iw%x*}
        ew=$(awk -v w="${m%x*}" -v f="$f" 'BEGIN {printf "%d", w * f + 0.5}')     # 外接屏放大后占的大小
        eh=$(awk -v h="${m#*x}" -v f="$f" 'BEGIN {printf "%d", h * f + 0.5}')
        # 位置自己算: xrandr 的 --left-of / --above 按放大前的尺寸摆, 两块屏会重叠
        local I=(--output "$int" --auto --primary) E=(--output "$ext" --mode "$m" --scale "${f}x${f}")
        case $1 in
            right)    xrandr "${I[@]}" --pos 0x0 "${E[@]}" --pos "${iw}x0" ;;
            left)     xrandr "${E[@]}" --pos 0x0 "${I[@]}" --pos "${ew}x0" ;;
            above)    xrandr "${E[@]}" --pos 0x0 "${I[@]}" --pos "0x${eh}" ;;
            # 两块屏分辨率不同 (2560x1440 / 1920x1080): 外接屏缩放成笔记本屏的大小
            mirror)   xrandr "${I[@]}" --pos 0x0 --output "$ext" --mode "$m" --same-as "$int" --scale-from "${iw}x${ih}" ;;
            external) xrandr --output "$ext" --mode "$m" --scale 1x1 --pos 0x0 --primary --output "$int" --off ;;
            *)        return 1 ;;
        esac
    fi || { notify-send -r $NID "󰍹 显示器" "切换失败"; return 1; }
    [ -n "$ext" ] && save "$ext"
    post
    setscale "$(wantscale)"
}

# 外接屏分辨率: 列出它支持的分辨率, 当前的打勾
resolution() {
    local ext cur choice
    ext=$(external)
    cur=$(xrandr --current | awk -v o="$ext" '$1 == o {f = 1; next} f && /^[^ ]/ {exit} f && /\*/ {print $1; exit}')
    choice=$( { xrandr --current | awk -v o="$ext" -v c="$cur" '$1 == o {f = 1; next} f && /^[^ ]/ {exit} f && !s[$1]++ {print ($1 == c ? "󰄬 " : "   ") $1}'
                echo "$BACK"; } | rofi -dmenu -p "$ext 分辨率") || return 2
    [ "$choice" = "$BACK" ] && return 2
    choice=${choice##* }
    [ -n "$choice" ] && [ "$choice" != "$cur" ] || return
    xrandr --output "$ext" --mode "$choice" || { notify-send -r $NID "󰍹 显示器" "切换分辨率失败"; return 1; }
    apply "$(curlayout)"                       # 按新分辨率重新摆放 (扩展时外接屏的放大和位置要重算)
}

# 外接屏缩放: 自动, 或手动指定 (记在这台显示器的 autorandr 配置里)
scale() {
    local ext f cur choice s
    ext=$(external)
    f="$PROFILES/$(profile "$ext")/dwm-scale"
    [ -d "$(dirname "$f")" ] || save "$ext"      # 还没保存过这台显示器: 先存一份当前布局
    cur=$(cat "$f" 2>/dev/null)
    choice=$( { echo "$([ -z "$cur" ] && echo "󰄬 " || echo "   ")默认 100%"
                for s in $SCALES; do echo "$([ "$cur" = "$s" ] && echo "󰄬 " || echo "   ")$s%"; done
                echo "$BACK"; } \
        | rofi -dmenu -p "$ext 缩放 (当前 $(extscale "$ext")%)") || return 2
    case $choice in
        "$BACK") return 2 ;;
        *默认*) rm -f "$f" ;;
        *%)     s=${choice##* }; echo "${s%\%}" > "$f" ;;
        *)      return ;;
    esac
    apply "$(curlayout)"                       # 扩展时改的是外接屏的放大倍数, 只用外接屏时改的是全局缩放
}

menu() {
    local ext choice
    ext=$(external)
    [ -n "$ext" ] || { notify-send -r $NID "󰍹 显示器" "没有检测到外接显示器"; return; }
    while :; do
        choice=$(printf '%s\n' \
            "󰍺 外接屏在右侧" "󰍺 外接屏在左侧" "󰍺 外接屏在上方" "󰍹 复制 (两块屏显示相同内容)" "󰍹 仅外接屏" "󰌢 仅笔记本" \
            "󰹑 外接屏分辨率" "󰁌 缩放" "$BACK" \
            | rofi -dmenu -p "$ext") || return 2
        case $choice in
            "$BACK") return 2 ;;
            *右侧) apply right ;;
            *左侧) apply left ;;
            *上方) apply above ;;
            *复制*) apply mirror ;;
            *仅外接屏) apply external ;;
            *仅笔记本) apply laptop ;;
            *分辨率) resolution ;;
            *缩放) scale ;;
            *) return ;;
        esac
        [ $? -eq 2 ] || return 0               # 子菜单按了 Esc / 返回: 重新显示本菜单
    done
}

check() {
    local cur last ext prof
    cur=$(xrandr --current | awk '$2 == "connected" {print $1}' | tr '\n' ' ')
    last=$(cat "$STATE" 2>/dev/null)
    [ "$cur" = "$last" ] && return
    echo "$cur" > "$STATE"
    [ -n "$last" ] || return                   # 登录后第一次只记录: autostart 已经 autorandr --change 过
    ext=$(external)
    if [ -z "$ext" ]; then
        apply laptop                           # 外接屏拔掉了: 确保笔记本屏亮着
    elif prof=$(autorandr --detected 2>/dev/null | head -1) && [ -n "$prof" ]; then
        autorandr --load "$prof" >/dev/null 2>&1 && post && setscale "$(wantscale)"
    else
        notify-send -r $NID "󰍹 检测到外接显示器 $ext" "按 Super+P 选择 monitor layout 设置布局"
    fi
}

# 登录时: 套用保存的布局 (没有匹配的就保持原样), 按布局设好 Xft.dpi, 打印 Qt 缩放倍数供 xinitrc 导出
login() {
    local s
    autorandr --change >/dev/null 2>&1
    s=$(wantscale)
    setscale "$s" norestart
    factor "$s"
}

case $1 in
    menu)     menu ;;
    login)    login ;;
    detect)   external ;;
    check)    check ;;
    right|left|above|mirror|external|laptop) apply "$1" ;;
    *)        sed -n '2,8p' "$0"; exit 1 ;;
esac

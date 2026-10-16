#! /bin/bash
# 显示器 (Super+P 菜单 "󰍹 monitor layout")
#   monitor.sh menu       rofi 菜单: 布局 (外接屏在右 / 左 / 上 / 下方、复制、仅外接屏、仅笔记本) + 每块屏一项,
#                         点进某块屏: 分辨率、刷新率、旋转、镜像翻转、缩放、位置 (外接屏) / 开关 (笔记本屏)
#                         只有笔记本屏时直接进它的菜单; 支持多块外接屏 (同一方向的按输出名依次往外排)
#                         Esc / "↩ 返回" 回到上一层: 子菜单回到本菜单, 本菜单退出码 2 (rofi.sh 据此重新打开 Super+P 主菜单)
#   monitor.sh <布局>     right | left | above | below | mirror | external | laptop
#   monitor.sh detect     打印已连接的第一块外接屏 (没有则为空)
#   monitor.sh rate <屏>  打印这块屏手动选的刷新率 (当前分辨率下有效才打印; powersave.sh 不省电时用它)
#   monitor.sh check      插拔检测 (autostart 每 10 秒调用): 套用这种接法上次的布局, 新的接法默认外接屏扩展到右侧;
#                         拔掉外接屏时点亮笔记本屏 (防止"仅外接屏"后黑屏)
#   monitor.sh login      登录时 (xinitrc, dwm 启动前): 套用保存的布局并设好缩放, 打印 Qt 缩放倍数
# 设置存在 ~/.config/dwm-monitor/, 都按 EDID 识别 (同一块屏换了输出名, 如独显直连 DP-4 / 核显 eDP-1, 也认得):
#   out-<EDID>   每块屏: mode= rate= rotate= reflect= scale=, 没写的用默认 (首选分辨率、最高刷新率、不旋转、不翻转)
#   set-<摘要>   每种接法 (连着哪几块屏): 每行 "EDID 位置", 位置相对基准屏 (有笔记本屏时是笔记本屏, 台式机是标 main 的那块):
#                main right left above below mirror off; 关掉的屏按 0 尺寸参与排布, 重新打开时其它屏位置不变
# 缩放: X11 整个桌面只有一个缩放比例 (Xft.dpi), 用第一块亮着的屏 (笔记本屏优先, 其次基准屏) 的缩放;
#   每块屏的缩放都可在菜单里指定: 笔记本屏默认按分辨率猜 (宽 >= 2560 为 135%, 否则 100%), 外接屏默认 100%。
#   其它屏按 全局缩放 / 自己的缩放 倍放大的画面渲染再缩小显示 (xrandr --scale), 看起来就是它自己的缩放,
#   代价是文字比原生略软。改了全局缩放会原地重启 dwm (窗口保留), 已打开的程序重开后生效 (Qt 程序也是, 不用重新登录)
# 测试: MONITOR_DRYRUN=1 只打印 xrandr 命令 (不切换、不换壁纸、不改缩放); MONITOR_FAKE=文件 用它代替 xrandr --current --prop 的输出

DWM=${DWM:-$(cd "$(dirname "$0")/.."; pwd)}
STATE=${XDG_CACHE_HOME:-$HOME/.cache}/monitor-connected
CONF=${XDG_CONFIG_HOME:-$HOME/.config}
DIR=$CONF/dwm-monitor
PROFILES=$DIR/legacy                              # 旧版按 autorandr 存的布局和外接屏缩放 (只读, 沿用; 从 ~/.config/autorandr 搬来)
OLDSCALE=$CONF/dwm-scale                          # 旧版笔记本屏缩放 (只读, 沿用)
NID=9541
BACK="↩ 返回"                                      # 各层菜单最后一项; 与 Esc 一样回到上一层 (退出码 2)
SCALES="100 115 125 135 150 175 200"              # 缩放菜单里除"默认"外的选项
LAPTOP_SCALE=135                                   # 没指定过时高分屏 (宽 >= 2560) 笔记本的缩放, 与 config/Xresources 的 Xft.dpi: 130 一致

xr() { if [ -n "$MONITOR_DRYRUN" ]; then echo xrandr "$@"; else xrandr "$@"; fi; }

# 读一遍当前屏幕状态:
#   OUTS 已连接的输出, INT 笔记本屏, EXTS 外接屏 (按名字排), STALE 已拔掉但还占着画面的输出
#   EDID[屏] PREF[屏] 首选分辨率, CUR[屏] 当前分辨率 (关着为空), GEOM[屏] 当前位置 WxH+X+Y (关着为 -)
#   SETF 这种接法的布局文件, PLACE[屏] 位置, ANCHOR 基准屏, ORDER 排布顺序 (基准屏在前)
# 笔记本屏: NVIDIA 独显直连时名字是 DP-x, 靠 ConnectorType: Panel 识别; 核显下没有这个属性, 按 eDP / LVDS 名字识别
declare -A EDID PREF CUR GEOM PLACE
scan() {
    local o c p e g cur pref
    XRC=$(if [ -n "$MONITOR_FAKE" ]; then cat "$MONITOR_FAKE"; else xrandr --current --prop; fi)
    OUTS=(); EXTS=(); STALE=(); INT=; EDID=(); PREF=(); CUR=(); GEOM=()
    while read -r o c p e g cur pref; do
        if [ "$c" = connected ]; then
            OUTS+=("$o"); GEOM[$o]=$g; PREF[$o]=${pref#-}; CUR[$o]=${cur#-}
            # 与旧版 autorandr --fingerprint 的摘要相同 (EDID 十六进制串 + 换行的 md5)
            if [ "$e" = - ]; then EDID[$o]=name-$o; else EDID[$o]=$(echo "$e" | md5sum | cut -c1-8); fi
            [ -z "$INT" ] && [ "$p" = panel ] && INT=$o
        elif [ "$g" != - ]; then
            STALE+=("$o")
        fi
    done < <(awk '
        function flush() { if (o != "") print o, c, p, (e == "" ? "-" : e), g, (cur == "" ? "-" : cur), (pref == "" ? "-" : pref) }
        /^[^ \t]/ && ($2 == "connected" || $2 == "disconnected") {
            flush(); o = $1; c = $2; p = "-"; e = ""; cur = ""; pref = ""; g = "-"; inedid = 0
            for (i = 3; i <= NF; i++) if ($i ~ /^[0-9]+x[0-9]+\+-?[0-9]+\+-?[0-9]+$/) { g = $i; break }
            next
        }
        /^\tEDID:/ { inedid = 1; next }
        inedid && /^\t\t[0-9a-f]+[ \t]*$/ { e = e $1; next }
        { inedid = 0 }
        /^\tConnectorType: Panel/ { p = "panel" }
        /^   [0-9]+x[0-9]+/ { if ($0 ~ /\*/) cur = $1; if ($0 ~ /\+/ && pref == "") pref = $1 }
        END { flush() }' <<< "$XRC")
    if [ -z "$INT" ]; then
        for o in "${OUTS[@]}"; do [[ $o =~ ^(eDP|LVDS) ]] && { INT=$o; break; }; done
    fi
    for o in $(printf '%s\n' "${OUTS[@]}" | sort -V); do [ "$o" != "$INT" ] && EXTS+=("$o"); done
    SETF=$DIR/set-$(for o in "${OUTS[@]}"; do echo "${EDID[$o]}"; done | sort | md5sum | cut -c1-8)
    loadplaces
}

# 读这种接法保存的位置, 没保存过时: 旧版 autorandr 配置能推出来就用它, 否则外接屏都在右侧
loadplaces() {
    local o
    PLACE=(); LEGACY=
    for o in "${OUTS[@]}"; do
        PLACE[$o]=$(awk -v e="${EDID[$o]}" '$1 == e {print $2; exit}' "$SETF" 2>/dev/null)
    done
    [ -f "$SETF" ] || ! legacy || LEGACY=1
    reorder
}

# 按 PLACE 定基准屏和排布顺序, 补上缺的 / 不合法的位置
reorder() {
    local o
    if [ -n "$INT" ]; then
        ANCHOR=$INT
        [ "${PLACE[$INT]}" = off ] || PLACE[$INT]=main
    else
        ANCHOR=
        for o in "${EXTS[@]}"; do [ "${PLACE[$o]}" = main ] && { ANCHOR=$o; break; }; done
        ANCHOR=${ANCHOR:-${EXTS[0]}}
        PLACE[$ANCHOR]=main
    fi
    ORDER=("$ANCHOR")
    for o in "${EXTS[@]}"; do
        [ "$o" = "$ANCHOR" ] && continue
        ORDER+=("$o")
        case ${PLACE[$o]} in right|left|above|below|mirror|off) ;; *) PLACE[$o]=right ;; esac
    done
}

# 旧版 (autorandr) 只有一块外接屏时保存的布局: 推出外接屏的位置和分辨率
legacy() {
    local ext=${EXTS[0]} f ip ep em ix iy ex ey
    [ -n "$INT" ] && [ ${#EXTS[@]} -eq 1 ] || return 1
    f=$PROFILES/monitor-${EDID[$ext]}/config
    [ -f "$f" ] || return 1
    # 笔记本屏在配置里可能是别的名字 (切换显卡模式后): 取外接屏以外第一块亮着的
    read -r ip ep em < <(awk -v e="$ext" '
        $1 == "output" {o = $2; n[++k] = o} $1 == "pos" {pos[o] = $2} $1 == "mode" {m[o] = $2}
        END {for (j = 1; j <= k; j++) if (n[j] != e && pos[n[j]] != "") {i = n[j]; break}
             print (i == "" ? "off" : pos[i]), (pos[e] == "" ? "off" : pos[e]), (m[e] == "" ? "-" : m[e])}' "$f")
    if [ "$ep" = off ]; then PLACE[$ext]=off
    elif [ "$ip" = off ]; then PLACE[$INT]=off; PLACE[$ext]=right
    elif [ "$ip" = "$ep" ]; then PLACE[$ext]=mirror
    else
        ix=${ip%x*}; iy=${ip#*x}; ex=${ep%x*}; ey=${ep#*x}
        if [ "$ey" -lt "$iy" ]; then PLACE[$ext]=above
        elif [ "$ex" -lt "$ix" ]; then PLACE[$ext]=left
        else PLACE[$ext]=right; fi
    fi
    [ "$em" != - ] && [ "$em" != "${PREF[$ext]}" ] && [ -z "$(get "$ext" mode)" ] && put "$ext" mode "$em"
    return 0
}

saveset() {
    local o
    mkdir -p "$DIR"
    for o in "${OUTS[@]}"; do echo "${EDID[$o]} ${PLACE[$o]}"; done > "$SETF"
}

# 每块屏的设置: get 屏 键 / put 屏 键 值 (值为空: 删掉, 回到默认)
ofile() { echo "$DIR/out-${EDID[$1]}"; }
get() { sed -n "s/^$2=//p" "$(ofile "$1")" 2>/dev/null | tail -1; }
put() {
    local f
    f=$(ofile "$1")
    mkdir -p "$DIR"
    { grep -v "^$2=" "$f" 2>/dev/null; [ -z "$3" ] || echo "$2=$3"; } > "$f.tmp" && mv "$f.tmp" "$f"
}

# 屏幕 $1 支持的分辨率 (去重, 按 xrandr 的顺序)
modes() {
    awk -v o="$1" '/^[^ \t]/ {f = ($1 == o); next} f && /^   [0-9]+x[0-9]+/ && !s[$1]++ {print $1}' <<< "$XRC"
}
# 屏幕 $1 在分辨率 $2 下支持的刷新率, 从高到低
rates() {
    awk -v o="$1" -v m="$2" '/^[^ \t]/ {f = ($1 == o); next}
        f && /^   [0-9]+x[0-9]+/ && $1 == m {for (i = 2; i <= NF; i++) {r = $i; gsub(/[*+]/, "", r); if (r != "" && !s[r]++) print r}}' <<< "$XRC" \
        | sort -rn
}

# 生效的设置: 手动选过且有效就用它, 否则默认
mode() {
    local m
    m=$(get "$1" mode)
    [ -n "$m" ] && modes "$1" | grep -qxF "$m" && { echo "$m"; return; }
    [ -n "${PREF[$1]}" ] && echo "${PREF[$1]}" || modes "$1" | head -1
}
savedrate() {                                      # 手动选的刷新率, 在分辨率 $2 下有效才打印
    local r
    r=$(get "$1" rate)
    [ -n "$r" ] && rates "$1" "$2" | grep -qxF "$r" && echo "$r"
}
rate() {
    local m
    m=$(mode "$1")
    savedrate "$1" "$m" || rates "$1" "$m" | head -1
}
rotation() { local r; r=$(get "$1" rotate); echo "${r:-normal}"; }
reflection() { local r; r=$(get "$1" reflect); echo "${r:-normal}"; }

# 缩放: 手动指定的 (含旧版存的), 否则笔记本屏按分辨率猜、外接屏 100%
oldscale() { [ "$1" = "$INT" ] && echo "$OLDSCALE/laptop-${EDID[$1]}" || echo "$PROFILES/monitor-${EDID[$1]}/dwm-scale"; }
setscale_of() { local s; s=$(get "$1" scale); [ -n "$s" ] && echo "$s" || cat "$(oldscale "$1")" 2>/dev/null; }
defscale() {
    local w
    [ "$1" = "$INT" ] || { echo 100; return; }
    w=${PREF[$1]%x*}
    [ "${w:-0}" -ge 2560 ] && echo $LAPTOP_SCALE || echo 100
}
scaleof() { local s; s=$(setscale_of "$1"); echo "${s:-$(defscale "$1")}"; }

on() { [ "${PLACE[$1]}" != off ]; }
# 决定全局缩放和主屏的屏: 第一块亮着且不是复制的
lead() {
    local o
    for o in "${ORDER[@]}"; do
        case ${PLACE[$o]} in off|mirror) ;; *) echo "$o"; return 0 ;; esac
    done
    return 1
}
wantscale() { local l; l=$(lead) && scaleof "$l" || echo 100; }

# 布局切换后: 刷新率 (省电模式)、壁纸重新铺满新屏幕 (动态壁纸遇到屏幕变化会崩)、护眼
post() {
    "$DWM/bin/powersave.sh" apply
    setsid -f "$DWM/bin/livewall.sh" next >/dev/null 2>&1
    "$DWM/bin/nightlight.sh" apply >/dev/null 2>&1 || true      # 护眼关着时返回非 0
}

curdpi() { xrdb -query | awk '$1 == "Xft.dpi:" {print $2}'; }
dpi() { echo $(( ($1 * 96 + 50) / 100 )); }         # 135% -> 130, 100% -> 96
factor() { awk -v s="$1" 'BEGIN {printf "%.2f\n", s / 100}'; }   # 135 -> 1.35

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
    [ -n "$MONITOR_DRYRUN" ] && return 0
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
        # Qt 倍数: dwm 重启时按 Xft.dpi 自己设 (见 dwm.c syncqtscale), 这里同步给 systemd / dbus 拉起的程序
        systemctl --user set-environment QT_SCALE_FACTOR="$(factor "$1")" QT_SCREEN_SCALE_FACTORS="$(factor "$1")" 2>/dev/null
        changed=1
        [ "$2" = norestart ] || restartdwm
    fi
    [ -n "$changed" ] && [ "$2" != norestart ] \
        && notify-send -r $NID "󰍹 缩放 $1%" "已打开的程序重开后生效 (Chrome / Edge 关窗口后还在后台, 要从菜单完全退出)"
    return 0
}

# 按 PLACE 和每块屏的设置摆放屏幕 (只调 xrandr), 一条命令设好所有屏
# 基准屏在 (0,0), 同一方向的屏按 ORDER 依次往外排 (左右顶对齐, 上下左对齐), 最后整体平移到非负坐标
# 位置自己算: xrandr 的 --right-of 等按放大前的尺寸摆, 屏幕会重叠
layout() {
    local l g o p m w h f R D L=0 U=0 args=()
    local -A X Y F W H
    l=$(lead) || { notify-send -r $NID "󰍹 显示器" "至少要有一块屏亮着"; return 1; }
    g=$(scaleof "$l")
    for o in "${ORDER[@]}"; do
        W[$o]=0; H[$o]=0
        case ${PLACE[$o]} in off|mirror) continue ;; esac
        m=$(mode "$o")
        [ -n "$m" ] || { notify-send -r $NID "󰍹 显示器" "$o 没有可用的分辨率"; return 1; }
        w=${m%x*}; h=${m#*x}; h=${h%%[!0-9]*}
        case $(rotation "$o") in left|right) read -r w h <<< "$h $w" ;; esac
        if [ "$o" = "$l" ]; then f=1
        else f=$(awk -v g="$g" -v s="$(scaleof "$o")" 'BEGIN {f = g / s; if (f < 1) f = 1; printf "%.4f\n", f}'); fi
        F[$o]=$f
        W[$o]=$(awk -v w="$w" -v f="$f" 'BEGIN {printf "%d", w * f + 0.5}')     # 放大后占的大小
        H[$o]=$(awk -v h="$h" -v f="$f" 'BEGIN {printf "%d", h * f + 0.5}')
    done
    X[$ANCHOR]=0; Y[$ANCHOR]=0; R=${W[$ANCHOR]}; D=${H[$ANCHOR]}
    for o in "${ORDER[@]:1}"; do
        case ${PLACE[$o]} in
            right) X[$o]=$R; Y[$o]=0; R=$((R + W[$o])) ;;
            left)  L=$((L + W[$o])); X[$o]=$((-L)); Y[$o]=0 ;;
            above) U=$((U + H[$o])); X[$o]=0; Y[$o]=$((-U)) ;;
            below) X[$o]=0; Y[$o]=$D; D=$((D + H[$o])) ;;
        esac
    done
    for o in "${STALE[@]}"; do args+=(--output "$o" --off); done
    for o in "${ORDER[@]}"; do
        if ! on "$o"; then args+=(--output "$o" --off); continue; fi
        args+=(--output "$o" --mode "$(mode "$o")")
        p=$(rate "$o"); [ -n "$p" ] && args+=(--rate "$p")
        args+=(--rotate "$(rotation "$o")" --reflect "$(reflection "$o")")
        if [ "${PLACE[$o]}" = mirror ]; then
            # 与主屏显示相同内容, 分辨率不同 (2560x1440 / 1920x1080) 时缩放成主屏的大小
            args+=(--same-as "$l" --scale-from "${W[$l]}x${H[$l]}")
        else
            args+=(--scale "${F[$o]}x${F[$o]}" --pos "$((X[$o] + L))x$((Y[$o] + U))")
        fi
        [ "$o" = "$l" ] && args+=(--primary)
    done
    xr "${args[@]}" || { notify-send -r $NID "󰍹 显示器" "切换失败"; return 1; }
}

# 摆放屏幕并记住这种接法的布局, 然后刷新壁纸等、按新布局设全局缩放
apply() {
    layout || return
    saveset
    [ -n "$MONITOR_DRYRUN" ] && return
    scan
    post
    setscale "$(wantscale)"
}

# 布局预设 (主菜单前几项 / 命令行)
preset() {
    local o
    case $1 in
        right|left|above|below)
            for o in "${ORDER[@]:1}"; do PLACE[$o]=$1; done
            [ -n "$INT" ] && PLACE[$INT]=main ;;
        mirror)
            for o in "${ORDER[@]:1}"; do PLACE[$o]=mirror; done
            PLACE[$ANCHOR]=main ;;
        external)
            [ -n "$INT" ] && [ ${#EXTS[@]} -gt 0 ] || { notify-send -r $NID "󰍹 显示器" "没有检测到外接显示器"; return 1; }
            PLACE[$INT]=off
            for o in "${EXTS[@]}"; do case ${PLACE[$o]} in off|mirror) PLACE[$o]=right ;; esac; done ;;
        laptop)
            [ -n "$INT" ] || { notify-send -r $NID "󰍹 显示器" "找不到笔记本屏幕"; return 1; }
            PLACE[$INT]=main
            for o in "${EXTS[@]}"; do PLACE[$o]=off; done ;;
        *) return 1 ;;
    esac
    apply
}

# ---- 菜单 ----

rotname() { case $1 in left) echo "向左转 90°" ;; right) echo "向右转 90°" ;; inverted) echo "转 180°" ;; *) echo "不旋转" ;; esac; }
refname() { case $1 in x) echo "水平翻转" ;; y) echo "垂直翻转" ;; xy) echo "水平+垂直翻转" ;; *) echo "不翻转" ;; esac; }
placename() {
    case $1 in
        main) echo "基准" ;; right) echo "右侧" ;; left) echo "左侧" ;; above) echo "上方" ;; below) echo "下方" ;;
        mirror) echo "复制" ;; off) echo "已关闭" ;;
    esac
}
hz() { [ -n "$1" ] && awk -v r="$1" 'BEGIN {printf "%gHz\n", r}'; }
title() { [ "$1" = "$INT" ] && echo "󰌢 笔记本屏 $1" || echo "󰍹 $1"; }

# 主菜单里每块屏一行: 名字、分辨率、刷新率、方向、位置、缩放
summary() {
    local o=$1 s
    s="$(title "$o")   "
    on "$o" || { echo "$s已关闭"; return; }
    s+="$(mode "$o")  $(hz "$(rate "$o")")"
    [ "$(rotation "$o")" = normal ] || s+="  $(rotname "$(rotation "$o")")"
    [ "$(reflection "$o")" = normal ] || s+="  $(refname "$(reflection "$o")")"
    [ "$o" = "$ANCHOR" ] || s+="  $(placename "${PLACE[$o]}")"
    echo "$s  $(scaleof "$o")%"
}

# rofi 单选: $1 提示, 其余为选项 (最后加 "↩ 返回"); 打印选中项的序号 (从 0 起), Esc / 返回时退出码 2
pick() {
    local p=$1 i
    shift
    i=$(printf '%s\n' "$@" "$BACK" | rofi -dmenu -no-custom -format i -p "$p") || return 2
    [ -n "$i" ] && [ "$i" -ge 0 ] && [ "$i" -lt $# ] || return 2
    echo "$i"
}

# rofi 选一个值: $1 提示, $2 当前值, 其余为 "值|显示文字" (值为空表示默认), 当前的打勾; 打印选中的值, Esc / 返回时退出码 2
choose() {
    local p=$1 cur=$2 v i vals=() labels=()
    shift 2
    for v in "$@"; do
        vals+=("${v%%|*}")
        labels+=("$([ "${v%%|*}" = "$cur" ] && echo "󰄬 " || echo "   ")${v#*|}")
    done
    i=$(pick "$p" "${labels[@]}") || return 2
    echo "${vals[$i]}"
}

# 改一项设置并重新摆放 (值没变就不动)
change() {
    [ "$(get "$1" "$2")" = "$3" ] && return
    put "$1" "$2" "$3"
    apply
}

# 改位置; 不能把所有屏都关掉
moveto() {
    local o=$1 p=$2 old=${PLACE[$1]} a
    [ "$old" = "$p" ] && return
    if [ "$p" = main ]; then                   # 台式机换基准屏: 原基准屏换到相反的一侧, 摆放保持不变
        a=$ANCHOR
        case $old in right) PLACE[$a]=left ;; left) PLACE[$a]=right ;; above) PLACE[$a]=below ;; below) PLACE[$a]=above ;; *) PLACE[$a]=right ;; esac
    fi
    PLACE[$o]=$p
    reorder
    lead >/dev/null || { scan; notify-send -r $NID "󰍹 显示器" "至少要有一块屏亮着"; return 1; }
    apply
}

# 一块屏的菜单: 选完一项立即生效, 回到这个菜单方便接着调; Esc / 返回 回到主菜单
screenmenu() {
    local o=$1 i v m r items opts
    while :; do
        scan
        m=$(mode "$o")
        items=("󰹑 分辨率      $m" "󰓅 刷新率      $(hz "$(rate "$o")")" "󰑵 旋转        $(rotname "$(rotation "$o")")"
               "󰯏 镜像翻转    $(refname "$(reflection "$o")")" "󰁌 缩放        $(scaleof "$o")%")
        if [ "$o" = "$INT" ]; then
            [ ${#EXTS[@]} -gt 0 ] && items+=("󰍹 开关        $(on "$o" && echo 开启 || echo 已关闭)")
        elif [ "$o" != "$ANCHOR" ]; then
            items+=("󰍺 位置        $(placename "${PLACE[$o]}")")
        fi
        i=$(pick "$o" "${items[@]}") || return 2
        case $i in
            0)  opts=("|默认 ${PREF[$o]}")
                for v in $(modes "$o"); do opts+=("$v|$v"); done
                v=$(choose "$o 分辨率" "$(get "$o" mode)" "${opts[@]}") && change "$o" mode "$v" ;;
            1)  r=$(rates "$o" "$m")
                opts=("|默认 (最高 $(hz "$(head -1 <<< "$r")"))")
                for v in $r; do opts+=("$v|$(hz "$v")"); done
                v=$(choose "$o 刷新率 ($m)" "$(savedrate "$o" "$m")" "${opts[@]}") || continue
                change "$o" rate "$v"
                [ "$("$DWM/bin/powersave.sh" status)" = on ] \
                    && notify-send -r $NID "󰍹 刷新率" "省电模式开着, 统一用 60Hz; 关掉省电模式后用这里选的" ;;
            2)  v=$(choose "$o 旋转" "$(get "$o" rotate)" "|不旋转" "left|向左转 90° (逆时针, 竖屏)" "right|向右转 90° (顺时针, 竖屏)" "inverted|转 180° (倒置)") \
                    && change "$o" rotate "$v" ;;
            3)  v=$(choose "$o 镜像翻转" "$(get "$o" reflect)" "|不翻转" "x|水平翻转 (左右镜像)" "y|垂直翻转 (上下镜像)" "xy|水平+垂直翻转") \
                    && change "$o" reflect "$v" ;;
            4)  opts=("|默认 $(defscale "$o")%")
                for v in $SCALES; do opts+=("$v|$v%"); done
                v=$(choose "$o 缩放 (当前 $(scaleof "$o")%)" "$(setscale_of "$o")" "${opts[@]}") || continue
                rm -f "$(oldscale "$o")"           # 旧版存的缩放不再用, 以这里选的为准
                [ "$(get "$o" scale)" = "$v" ] && [ -n "$v" ] || { put "$o" scale "$v"; apply; } ;;
            5)  if [ "$o" = "$INT" ]; then
                    v=$(choose "$o" "${PLACE[$o]}" "main|开启" "off|关闭 (只用外接屏)") && moveto "$o" "$v"
                else
                    v=$(title "$ANCHOR" | cut -d' ' -f2-)
                    opts=("right|在 $v 右侧" "left|在 $v 左侧" "above|在 $v 上方" "below|在 $v 下方" "mirror|复制 (与主屏显示相同内容)" "off|关闭")
                    [ -z "$INT" ] && opts+=("main|设为基准 (其它屏围绕它摆放)")
                    v=$(choose "$o 位置" "${PLACE[$o]}" "${opts[@]}") && moveto "$o" "$v"
                fi ;;
        esac
    done
}

menu() {
    local i o items acts
    scan
    [ ${#OUTS[@]} -gt 0 ] || return 1
    [ ${#EXTS[@]} -eq 0 ] && { screenmenu "$INT"; return; }   # 只有笔记本屏: 直接进它的菜单
    while :; do
        scan
        items=(); acts=()
        if [ -n "$INT" ]; then
            items+=("󰍺 外接屏在右侧" "󰍺 外接屏在左侧" "󰍺 外接屏在上方" "󰍺 外接屏在下方"); acts+=(right left above below)
        fi
        items+=("󰍹 复制 (所有屏显示相同内容)"); acts+=(mirror)
        [ -n "$INT" ] && { items+=("󰍹 仅外接屏" "󰌢 仅笔记本"); acts+=(external laptop); }
        for o in "${ORDER[@]}"; do items+=("$(summary "$o")"); acts+=("screen:$o"); done
        i=$(pick "显示器" "${items[@]}") || return 2
        case ${acts[$i]} in
            screen:*) screenmenu "${acts[$i]#screen:}" ;;
            *)        preset "${acts[$i]}" ;;
        esac
        [ $? -eq 2 ] || return 0               # 子菜单按了 Esc / 返回: 重新显示本菜单
    done
}

check() {
    local cur last
    scan
    cur=$(printf '%s ' "${OUTS[@]}")
    last=$(cat "$STATE" 2>/dev/null)
    [ "$cur" = "$last" ] && return
    echo "$cur" > "$STATE"
    [ -n "$last" ] || return                   # 登录后第一次只记录: login 已经摆放过
    if [ -f "$SETF" ] || [ -n "$LEGACY" ] || [ ${#EXTS[@]} -eq 0 ]; then
        apply                                  # 用过的接法 / 外接屏拔掉了: 确保笔记本屏亮着
    else
        apply && notify-send -r $NID "󰍹 检测到外接显示器 ${EXTS[*]}" "已扩展到右侧, 按 Super+P 选 monitor layout 调整"
    fi
}

# 登录时: 套用保存的布局 (新的接法保持原样), 按布局设好 Xft.dpi, 打印 Qt 缩放倍数供 xinitrc 导出
login() {
    local s
    scan
    if [ -f "$SETF" ] || [ -n "$LEGACY" ] || [ ${#EXTS[@]} -eq 0 ]; then
        layout >/dev/null && saveset
    fi
    s=$(wantscale)
    setscale "$s" norestart
    factor "$s"
}

# 旧版存的 autorandr 布局搬走: 留在 ~/.config/autorandr 里时, autorandr 的系统钩子 (插拔、唤醒时由 udev 触发)
# 会套用这些不带旋转等设置的旧布局, 把这里摆好的屏幕改回去
if compgen -G "$CONF/autorandr/monitor-*" >/dev/null; then
    mkdir -p "$PROFILES" && mv -n "$CONF"/autorandr/monitor-* "$PROFILES"/ 2>/dev/null
fi

case $1 in
    menu)     menu ;;
    login)    login ;;
    detect)   scan; echo "${EXTS[0]}" | grep . ;;
    rate)     scan; [ -n "${CUR[$2]}" ] && savedrate "$2" "${CUR[$2]}" ;;
    check)    check ;;
    right|left|above|below|mirror|external|laptop) scan; preset "$1" ;;
    *)        sed -n '2,10p' "$0"; exit 1 ;;
esac

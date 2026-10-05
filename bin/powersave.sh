#! /bin/bash
# 省电模式 (Super+P 菜单 open / close power save): 开启时
#   1. 壁纸切到静态 (停掉视频 / 场景渲染, 由 livewall.sh toggle 完成, 带转场)
#   2. 屏幕刷新率降到 60Hz (原来 240Hz)
#   3. CPU 节能策略 (EPP) 设为 power: 写 /sys 需要 root, 要配置 sudo 免密 (见 docx/dwm使用说明.md), 没配时跳过这一项
# 关闭时按开启前记下的状态全部恢复; 状态跨重启保留 (autostart 调用 apply 重新应用刷新率和 EPP)
# 不省电时 apply 把屏幕设为最高刷新率 (独显直连时 X 默认选 60Hz)
# 拔电自动开、插电自动关 (autostart 每 10 秒调用 ac): 只自动关闭拔电时自动开的那次, 手动开的 (Super+P) 不动
#   powersave.sh on | off | toggle | apply | status | ac

DWM=${DWM:-$(cd "$(dirname "$0")/.."; pwd)}
export DISPLAY=${DISPLAY:-:0}
DIR=${XDG_CACHE_HOME:-$HOME/.cache}/powersave
ON=$DIR/on                                 # 省电中
PREV=$DIR/prev                             # 开启前的状态: wallmode= / epp= / rate_<屏幕>=
AUTO=$DIR/auto                             # 这次省电是拔电自动开的 (插电时自动关)
LASTAC=$DIR/ac                             # 上次检测到的供电: ac / bat, 只在变化时动作
RATE=60
EPP=power
EPP_FILES=/sys/devices/system/cpu/cpu*/cpufreq/energy_performance_preference

# 是否插着电源 (任一 Mains 类型的电源在线); POWERSAVE_AC=0/1 可覆盖, 用于不拔电测试
ac_online() {
    local s
    [ -n "$POWERSAVE_AC" ] && { [ "$POWERSAVE_AC" = 1 ]; return; }
    for s in /sys/class/power_supply/*; do
        [ "$(cat "$s/type" 2>/dev/null)" = Mains ] && [ "$(cat "$s/online" 2>/dev/null)" = 1 ] && return 0
    done
    return 1
}

# 独显直连 (Xorg 跑在独显上) 时独显不能休眠, 是最大的耗电源
dgpu_direct() { [ "$(nvidia-smi --query-gpu=display_active --format=csv,noheader 2>/dev/null)" = Enabled ]; }

notify() { notify-send -r 9530 "󰌪 省电模式" "$1" 2>/dev/null; echo "$1"; }
wall_live() { pgrep -x xwinwrap >/dev/null || pgrep -x linux-wallpaper >/dev/null; } # 与 livewall.sh 的 running 一致

# 每个已连接屏幕当前的分辨率和刷新率, 每行 "屏幕 分辨率 刷新率"
rates() {
    xrandr --query | awk '/ connected/{o = $1} o && /\*/{for (i = 2; i <= NF; i++) if ($i ~ /\*/) {r = $i; gsub(/[*+]/, "", r); print o, $1, r}; o = ""}'
}

# 屏幕 $1 当前分辨率下支持的最高刷新率
max_rate() {
    xrandr --current | awk -v o="$1" '$1 == o {f = 1; next} f && /^[^ ]/ {exit} f && /\*/ {for (i = 2; i <= NF; i++) {r = $i; gsub(/[*+]/, "", r); if (r + 0 > m + 0) m = r}; print m; exit}'
}

# 只给 --rate 不会生效, 要连同分辨率一起指定
set_rate() { xrandr --output "$1" --mode "$2" --rate "$3" 2>/dev/null; }

# 不省电时: 每个屏幕用最高刷新率 (独显直连时 X 默认选首选的 60Hz, 不是 240Hz)
full_rate() {
    local o m r
    while read -r o m r; do set_rate "$o" "$m" "$(max_rate "$o")"; done < <(rates)
}

# 写 EPP (sudo -n: 没配免密时直接失败, 不会卡住等密码)
set_epp() { echo "$1" | sudo -n tee $EPP_FILES >/dev/null 2>&1; }

# 应用刷新率和 EPP, 打印生效的项
apply_hw() {
    local o m r done=()
    while read -r o m r; do set_rate "$o" "$m" $RATE && done+=("$o ${RATE}Hz"); done < <(rates)
    set_epp $EPP && done+=("CPU 节能") || done+=("CPU 节能未生效: 需配置 sudo 免密, 见 dwm使用说明")
    printf '%s\n' "${done[@]}"
}

on() {
    [ -f "$ON" ] && { notify "已经开启"; return; }
    mkdir -p "$DIR"
    {
        echo "wallmode=$(wall_live && echo live || echo static)"
        echo "epp=$(cat /sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference 2>/dev/null)"
        rates | while read -r o m r; do echo "rate_${o//[^A-Za-z0-9]/_}=$r"; done
    } > "$PREV"
    touch "$ON"
    local msg=
    # 先停壁纸渲染, 再改刷新率; 加超时: livewall 卡住 (如锁没释放) 时省电的其余各项照常生效
    wall_live && timeout 20 $DWM/bin/livewall.sh toggle && msg=$'静态壁纸\n'
    msg+=$(apply_hw)
    dgpu_direct && msg+=$'\n当前为独显直连, 独显常开 (约 25W); 在 BIOS 切回混合模式最省电'
    notify "已开启"$'\n'"$msg"
}

off() {
    [ -f "$ON" ] || { notify "没有开启"; return; }
    local o m r k wallmode= epp=
    [ -f "$PREV" ] && . "$PREV"
    while read -r o m r; do
        k=rate_${o//[^A-Za-z0-9]/_}
        # 没有记录 (切换显卡模式后屏幕改了名字, 如 eDP-1 -> DP-4) 时用最高刷新率
        set_rate "$o" "$m" "${!k:-$(max_rate "$o")}"
    done < <(rates)
    [ -n "$epp" ] && set_epp "$epp"
    rm -f "$ON" "$AUTO"
    [ "$wallmode" = live ] && ! wall_live && timeout 20 $DWM/bin/livewall.sh toggle
    notify "已关闭, 已恢复原来的刷新率$([ -n "$epp" ] && echo "、CPU 策略")$([ "$wallmode" = live ] && echo "和动态壁纸")"
}

case $1 in
    on)     on ;;
    off)    off ;;
    toggle) rm -f "$AUTO"; [ -f "$ON" ] && off || on ;;  # 手动操作: 之后插电不会自动关
    ac)                                                 # 供电变化时: 拔电自动开, 插电只关自动开的那次
        mkdir -p "$DIR"
        now=$(ac_online && echo ac || echo bat)
        [ "$now" = "$(cat "$LASTAC" 2>/dev/null)" ] && exit 0
        echo "$now" > "$LASTAC"
        if [ "$now" = bat ] && [ ! -f "$ON" ]; then
            on && touch "$AUTO"
        elif [ "$now" = ac ] && [ -f "$AUTO" ]; then
            off
        fi ;;
    apply)  if [ -f "$ON" ]; then apply_hw >/dev/null; else full_rate; fi ;;  # 开机时: 刷新率和 EPP 重启后会被重置, 重新应用
    status) [ -f "$ON" ] && echo on || echo off ;;
    *)      echo "用法: $0 on | off | toggle | apply | status | ac" ;;
esac

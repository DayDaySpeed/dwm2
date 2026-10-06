# 打印菜单
call_menu() {
    # 和 livewall.sh toggle 的判断一致: 正在播动态壁纸 (视频 xwinwrap / 场景 linux-wallpaperengine) 就切到静态
    { pgrep -x xwinwrap || pgrep -x linux-wallpaper; } >/dev/null && echo ' switch wallpaper mode to static' || echo ' switch wallpaper mode to live'
    [ -f ~/.cache/powersave/on ] && echo '󰌪 close power save' || echo '󰌪 open power save'
    [ "$(dunstctl is-paused 2>/dev/null)" = true ] && echo '󰂛 close do not disturb' || echo '󰂛 open do not disturb'
    [ "$($DWM/bin/dpms.sh status)" = paused ] && echo '󰌾 resume auto lock' || echo '󰌿 pause auto lock'
    command -v gammastep >/dev/null && { [ "$($DWM/bin/nightlight.sh status)" = on ] && echo '󰖔 close night light' || echo '󰖔 open night light'; }
    [ -n "$($DWM/bin/monitor.sh detect)" ] && echo '󰍹 monitor layout'   # 接了外接屏时才出现
    echo '󰕞 update statusbar'
    command -v daed >/dev/null && { [ "$(ps aux | grep -v grep | grep daed)" ] && echo ' close daed' || echo ' open daed'; }
    [ "$(ps aux | grep picom | grep -v 'grep\|rofi\|nvim')" ] && echo ' close picom' || echo ' open picom'
    [ "$(pgrep -x sunshine)" ] && echo '󱒃 close sunshine' || echo '󱒃 open sunshine'
    [ "$(pgrep -x x11vnc)" ] && echo '󰢹 close vnc' || echo '󰢹 open vnc'
}

# 执行菜单
execute_menu() {
    case $1 in
        ' switch wallpaper mode to static'|' switch wallpaper mode to live')
            $DWM/bin/livewall.sh toggle
            ;;
        '󰌪 open power save'|'󰌪 close power save')
            $DWM/bin/powersave.sh toggle > /dev/null
            ;;
        '󰂛 open do not disturb'|'󰂛 close do not disturb')
            dunstctl set-paused toggle
            [ "$(dunstctl is-paused)" = true ] || notify-send -r 9534 "󰂚 勿扰" "已关闭, 通知恢复显示"
            ;;
        '󰌿 pause auto lock'|'󰌾 resume auto lock')
            $DWM/bin/dpms.sh toggle
            ;;
        '󰖔 open night light'|'󰖔 close night light')
            $DWM/bin/nightlight.sh toggle
            ;;
        '󰍹 monitor layout')
            $DWM/bin/monitor.sh menu || [ $? -ne 2 ] || return 2   # 子菜单按 Esc: 回到主菜单
            ;;
        '󰕞 update statusbar')
            coproc ($DWM/bin/statusbar/statusbar.sh updateall > /dev/null 2>&1)
            ;;
        ' open daed')
            coproc (sudo systemctl start daed > /dev/null && $DWM/bin/statusbar/statusbar.sh updateall > /dev/null)
            ;;
        ' close daed')
            coproc (sudo systemctl stop daed > /dev/null && $DWM/bin/statusbar/statusbar.sh updateall > /dev/null)
            ;;
        ' open picom')
            coproc (picom --config $DWM/config/picom.conf > /dev/null 2>&1)
            ;;
        ' close picom')
            killall picom
            ;;
        '󱒃 open sunshine')
            $DWM/bin/sunshine.sh start > /dev/null
            ;;
        '󱒃 close sunshine')
            $DWM/bin/sunshine.sh stop > /dev/null
            ;;
        '󰢹 open vnc')
            $DWM/bin/vnc.sh start > /dev/null
            ;;
        '󰢹 close vnc')
            $DWM/bin/vnc.sh stop > /dev/null
            ;;
    esac
    return 0                                       # 只有子菜单按 Esc 才返回 2
}

# 子菜单按 Esc 返回 2 时重新打开主菜单; 主菜单按 Esc 才是关闭
while choice=$(call_menu | rofi -dmenu -p ""); do
    execute_menu "$choice"
    [ $? -eq 2 ] || break
done

# 打印菜单
call_menu() {
    echo ' switch wallpaper'
    echo '󰕞 update statusbar'
    command -v daed >/dev/null && { [ "$(ps aux | grep -v grep | grep daed)" ] && echo ' close daed' || echo ' open daed'; }
    [ "$(ps aux | grep picom | grep -v 'grep\|rofi\|nvim')" ] && echo ' close picom' || echo ' open picom'
    [ "$(pgrep -x sunshine)" ] && echo '󱒃 close sunshine' || echo '󱒃 open sunshine'
    [ "$(pgrep -x x11vnc)" ] && echo '󰢹 close vnc' || echo '󰢹 open vnc'
}

# 执行菜单
execute_menu() {
    case $1 in
        ' switch wallpaper')
            $DWM/bin/livewall.sh toggle
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
            coproc (picom --experimental-backends --config $DWM/config/picom.conf > /dev/null 2>&1)
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
}

execute_menu "$(call_menu | rofi -dmenu -p "")"

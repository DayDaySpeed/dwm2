#! /bin/bash
# 电源菜单 (Super+Shift+Esc, 状态栏菜单图标右键): 锁屏 / 睡眠 / 休眠 / 注销 / 重启 / 关机
# 注销、重启、关机会再确认一次, 防止误按丢掉所有窗口

DWM=${DWM:-$(cd "$(dirname "$0")/.."; pwd)}

# 二次确认: 只有选“确定”才返回成功
confirm() { [ "$(printf '取消\n确定' | rofi -dmenu -p "$1?" -window-title confirm)" = 确定 ]; }

case $(printf ' 锁屏\n󰒲 睡眠\n󰄉 休眠\n󰍃 注销\n 重启\n 关机' | rofi -dmenu -p 电源 -window-title power) in
    *锁屏) $DWM/bin/blurlock.sh ;;
    *睡眠) systemctl suspend ;;          # 挂起到内存, 唤醒快; xss-lock 会在睡眠前先锁屏
    *休眠) systemctl hibernate ;;        # 挂起到硬盘, 不耗电
    *注销) confirm 注销 && pkill -x dwm ;; # SIGTERM 结束 dwm, 会话随之退出 (SIGHUP 是原地重启, 不能用)
    *重启) confirm 重启 && reboot ;;
    *关机) confirm 关机 && poweroff ;;
esac

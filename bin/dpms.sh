#!/bin/sh
# dpms.sh — 自动锁屏 / 熄屏的时间, 后台循环守护 (某些应用会在会话中途把 DPMS 改回默认值)
#   无操作 10 分钟:   进入 Super+Z 星系屏保 (bin/galaxysaver.py)
#   无操作 19 分 30 秒: 屏幕调暗提醒 (xss-lock -n bin/dimscreen.sh)
#   无操作 20 分钟:   锁屏 (xss-lock)
#   无操作 22 分钟:   熄屏 (DPMS)
#   dpms.sh                启动守护 (autostart)
#   dpms.sh toggle         暂停 / 恢复自动锁屏和熄屏 (Super+P 菜单 pause / resume auto lock, 看视频、演示时用)
#   dpms.sh status         输出 on (自动锁屏生效) / paused
#   dpms.sh --one-shot     只设置一次

ALERT_SEC=1170                             # 调暗提醒 (10 分钟时已先进入星系屏保, 见 bin/galaxysaver.py)
CYCLE_SEC=30                               # 提醒后再过多久锁屏 (锁屏在 ALERT_SEC + CYCLE_SEC = 1200 秒)
OFF_SEC=1320                               # 熄屏, 与 config/betterlockscreenrc 的 lock_timeout 保持一致
PAUSED=${XDG_CACHE_HOME:-$HOME/.cache}/nolock

apply() {
    if [ -f "$PAUSED" ]; then
        xset s off
        xset -dpms
    else
        xset dpms "$OFF_SEC" "$OFF_SEC" "$OFF_SEC"
        xset +dpms
        xset s on                          # 先开再设时间: xset s on 会把时间重置为默认的 600
        xset s "$ALERT_SEC" "$CYCLE_SEC"
    fi
}

case "$1" in
  --one-shot) apply; exit 0 ;;
  status) [ -f "$PAUSED" ] && echo paused || echo on; exit 0 ;;
  toggle)
    if [ -f "$PAUSED" ]; then
        rm -f "$PAUSED"; apply
        notify-send -r 9532 "󰌾 自动锁屏" "已恢复: 10 分钟无操作进入星系屏保, 20 分钟锁屏"
    else
        mkdir -p "$(dirname "$PAUSED")"; touch "$PAUSED"; apply
        notify-send -r 9532 "󰌿 自动锁屏" "已暂停: 不会进入屏保、自动锁屏和熄屏"
    fi
    exit 0 ;;
esac

apply
# 每 60s 检查并修正, 持久运行
while :; do
  sleep 60
  if [ -f "$PAUSED" ]; then
    [ "$(xset -q 2>/dev/null | awk '/timeout:/ {print $2}')" = 0 ] || apply
  else
    [ "$(xset -q 2>/dev/null | awk '/Standby:/ {print $2}')" = "$OFF_SEC" ] && \
    [ "$(xset -q 2>/dev/null | awk '/timeout:/ {print $2}')" = "$ALERT_SEC" ] || apply
  fi
done

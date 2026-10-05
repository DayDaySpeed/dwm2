#! /bin/bash
# 锁屏前的提醒 (xss-lock -n 调用): 无操作 19 分 30 秒时屏幕调暗, 30 秒内动一下鼠标或键盘
# xss-lock 就会结束本脚本, 亮度恢复; 否则 20 分钟时锁屏 (锁屏后亮度同样恢复). 10 分钟时已先进入星系屏保

DIM_PERCENT=40                             # 调暗到原亮度的百分比

orig=$(brightnessctl get 2>/dev/null) || exit 0
brightnessctl -q set $((orig * DIM_PERCENT / 100))
sleep infinity &
trap 'kill $! 2>/dev/null; brightnessctl -q set "$orig"; exit 0' TERM INT
wait

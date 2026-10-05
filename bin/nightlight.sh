#! /bin/bash
# 护眼 (Super+P 菜单 open / close night light): 屏幕色温调到暖色, 晚上看着不刺眼
#   nightlight.sh toggle | apply | status
# 需要 gammastep (sudo pacman -S gammastep); 开着时重启后由 autostart 调用 apply 重新应用

TEMP=4000                                  # 色温 (K), 越低越暖; 正常约 6500
ON=${XDG_CACHE_HOME:-$HOME/.cache}/nightlight

case $1 in
    toggle)
        if [ -f "$ON" ]; then
            rm -f "$ON"; gammastep -x >/dev/null 2>&1
            notify-send -r 9533 "󰖨 护眼" "已关闭"
        else
            mkdir -p "$(dirname "$ON")"; touch "$ON"; gammastep -P -O $TEMP >/dev/null 2>&1
            notify-send -r 9533 "󰖔 护眼" "已开启 (${TEMP}K)"
        fi ;;
    apply)  [ -f "$ON" ] && gammastep -P -O $TEMP >/dev/null 2>&1 ;;
    status) [ -f "$ON" ] && echo on || echo off ;;
esac

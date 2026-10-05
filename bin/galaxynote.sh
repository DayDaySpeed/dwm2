#!/bin/sh
# galaxynote.sh — dunst 规则调用 (config/dunst.conf 的 [galaxy-note]): 把通知交给 dwm,
# Super+Z 星系正在运行时化作一颗带标题的彗星划过; 星系没运行时 dwm 直接忽略。
#   dunst 传入: 应用名 摘要 正文 图标 紧急程度
#   手动测试:   bin/galaxynote.sh 测试 "Hello 星系"
app=$1
summary=$2
[ -n "$summary" ] || exit 0
xprop -root -f _DWM_GALAXY 8u -set _DWM_GALAXY "note:${app:+$app: }$summary" 2>/dev/null
exit 0

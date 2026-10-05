#!/bin/bash
# 驻留态的天象: 新开窗口诞生新星 / 关窗化作流星 / 通知彗星 / 整点报时. 检查日志里的事件行, 可选录屏.
#   events.sh [录屏.mp4]
# 整点报时要在测试 dwm 启动时带 GALAXY_FAKEHOUR=秒 (驻留这么多秒后假装到了整点), 例如:
#   stop.sh; GALAXY_FAKEHOUR=10 start.sh; events.sh
. "$(dirname "$0")/lib.sh"
fail=0
check() { if grep -q "$1" "$LOG"; then echo "ok   $2"; else echo "FAIL $2"; fail=1; fi; }
: > "$LOG"
[ -n "$1" ] && { "$T/record.sh" "$1" 26 15 1280 & }
pause .5
xdotool key super+z; pause 9.5
victim=$(xdotool search --name '^green-a$' | head -1)
setsid -f xterm -T newborn -bg '#2a5c5c' -fg white -e sh -c 'echo newborn; sleep 30' >/dev/null 2>&1
pause 2.5
[ -n "$victim" ] && xdotool windowkill "$victim"
pause 2
"$REPO/bin/galaxynote.sh" 测试 "Hello 星系: 一颗彗星"
pause 1
"$REPO/bin/galaxynote.sh" 测试 "第二条通知 (排队)"
pause 7
check "galaxy birth: .*captured" "新窗口诞生新星"
check "galaxy death:" "关闭的窗口化作流星"
check 'galaxy note: comet "测试: Hello' "通知彗星"
check 'galaxy note: comet "测试: 第二条' "第二条通知排队后划过"
grep -q "galaxy chime" "$LOG" && echo "ok   整点报时" || echo "skip 整点报时 (启动测试 dwm 时没带 GALAXY_FAKEHOUR)"
xdotool key super+z; pause 3
check "galaxy end: from return restore 1" "回到桌面"
[ "$(overlay)" = 0 ] && echo "ok   遮罩已销毁" || { echo "FAIL 遮罩还在"; fail=1; }
grep -q "xerrors [1-9]" "$LOG" && { echo "FAIL 有 X 错误"; fail=1; }
wait
echo "alive: $(alive)"
exit $fail

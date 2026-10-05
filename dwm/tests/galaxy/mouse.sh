#!/bin/bash
# 驻留态鼠标睡眠 / 唤醒 / 滚轮 / Esc: 只操作独立测试显示器。
set -euo pipefail
. "$(dirname "$0")/lib.sh"

fail() { echo "mouse test: $*" >&2; exit 1; }
waitlog() {
    local i
    for i in $(seq 1 100); do
        grep -q "$1" "$LOG" && return 0
        pause .2
    done
    fail "missing log: $1"
}
[ "$(alive)" = alive ] || fail "test dwm is not running"
[ "$(overlay)" = 0 ] || fail "close any existing galaxy overlay before testing"
cstate > "$RT/mouse-before.txt"
xdotool key super+z
pause 1
xdotool key space                 # 快进到驻留态
waitlog 'galaxy mouse: sleep (orbit start)'

xdotool mousemove 100 100 click 5
pause .3
! grep -q 'galaxy mouse: wake\|galaxy mouse: zoom' "$LOG" || fail "dormant mouse changed interaction"

xdotool click 1                  # 唤醒点击不应选中卡片或核心
waitlog 'galaxy mouse: wake'
! grep -q 'galaxy return:' "$LOG" || fail "first click selected an object"
[ "$(overlay)" = 1 ] || fail "overlay vanished on wake"

xdotool mousedown 3
xdotool mousemove 500 250
pause .08
xdotool mousemove 850 400
pause .08
xdotool mouseup 3
waitlog 'galaxy mouse: drag end'
awk '/galaxy mouse: drag end/{ok = ($6 > 5 || $6 < -5) && ($8 > 5 || $8 < -5)} END{exit !ok}' "$LOG" || fail "right drag did not rotate camera"

xdotool click 5
waitlog 'galaxy mouse: zoom 1.11'
xdotool mousemove 300 350
waitlog 'galaxy mouse: sleep (idle)'
before=$(grep -c 'galaxy mouse: zoom' "$LOG")
xdotool mousemove 400 450 click 5
pause .2
after=$(grep -c 'galaxy mouse: zoom' "$LOG")
[ "$before" = "$after" ] || fail "wheel worked after sleep"

xdotool key a                     # 睡眠时键盘过滤仍可用, Esc 先清过滤
pause .2
xdotool key Escape
pause .2
[ "$(overlay)" = 1 ] || fail "keyboard filtering or Esc collapsed the orbit"

xdotool click 1
pause .2
xdotool key Escape
waitlog 'galaxy mouse: sleep (Esc)'
[ "$(overlay)" = 1 ] || fail "Esc collapsed instead of closing interaction"
xdotool key Escape              # 此时才坍缩
pause .3
xdotool key Escape              # 坍缩中立刻进入壁纸态
pause .2
xdotool key super+z             # 恢复桌面
pause .5
[ "$(overlay)" = 0 ] || fail "overlay was not destroyed"
cstate > "$RT/mouse-after.txt"
diff -q "$RT/mouse-before.txt" "$RT/mouse-after.txt" >/dev/null || fail "window state changed"
echo "mouse test: passed"

#!/bin/bash
# 测试窗口布局: 多个 tag, 颜色 / 内容各不相同 (clickstar.py 按颜色找它们), 外加一个隐藏窗口; 当前 tag 是 1
. "$(dirname "$0")/lib.sh"
W=$REPO/wallpaper/static
term() { setsid -f xterm -T "$2" -bg "$1" -fg white -fa Monospace -fs 13 -e sh -c "$3; sleep 1d" >/dev/null 2>&1 < /dev/null; pause .7; }
img() { setsid -f feh -g 1400x900 --title "$2" "$1" >/dev/null 2>&1 < /dev/null; pause .9; }
move() { xdotool key super+shift+$1; pause .5; }
xdotool key super+1
term '#5c1a1a' red-a 'seq 1 400 | paste - - - - - - - -'; move 3
img $W/steam-1201340849_2560x1440.jpg img-c; move 3
term '#5c3a1a' orange-a 'ls -la /usr/lib | head -60'; move 3
term '#1a5c2a' green-a 'cat /etc/os-release; env | sort | head -40'; move 2
img $W/wallhaven-jelv1w_1920x1080.png img-b; move 2
term '#4a1a5c' purple-a 'ps aux | head -50'; move c
term '#1a4a5c' teal-a 'df -h; free -h; uname -a'; move v
term '#5c5c1a' olive-a 'seq 1000 | xargs -n 12'; move v
img $W/wallhaven-7jxlg3_1920x1080.png img-a
term '#1a2a5c' blue-a 'ls -la /usr/bin | head -70'
term '#b01890' magenta-hidden 'echo hidden window'; xdotool key super+i; pause .5
term '#1a3a5c' blue-b 'cal -y'

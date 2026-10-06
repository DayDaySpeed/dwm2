# Super+Z 星系测试的公共设置和函数, 其他脚本 source 它
T=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)       # 本目录
REPO=$(cd "$T/../../.." && pwd)                       # 仓库根
RT=${XDG_RUNTIME_DIR:-/tmp}/galaxy-test               # 测试副本 / 编译产物 / 临时文件 (不进仓库)
export DISPLAY=${GALAXY_TEST_DISPLAY:-:7}
LOG=~/.cache/dwm-galaxy$DISPLAY.log                   # 测试会话的星系日志 (dwm 按显示号分开写)
mkdir -p "$RT"

overlay() { xwininfo -root -children | grep -c '"dwm-galaxy"'; }
res() { "$RT/xres"; }
dwmpid() { pgrep -n -f "$RT/dwm-galaxytest\$"; }
rss() { awk '/VmRSS/{print $2}' /proc/$(dwmpid)/status; }
alive() { [ -n "$(dwmpid)" ] && echo alive || echo DEAD; }
pause() { python3 -c "import time; time.sleep($1)"; }    # 部分环境禁止前台 sleep, 用 python 代替

# 每个客户端窗口的位置 / 大小 / 映射状态 / WM_STATE, 加上焦点: 动作前后各记一次再比较
cstate() {
    local clients w
    clients=$(xprop -root _NET_CLIENT_LIST) || return 1
    if [[ "$clients" != *'window id # 0x'* ]]; then
        echo "测试会话 $DISPLAY 没有托管窗口: $clients" >&2
        return 1
    fi
    for w in $(printf '%s\n' "$clients" | sed 's/.*# //; s/,//g' | tr ' ' '\n' | sort); do
        g=$(xwininfo -id $w 2>/dev/null | awk '/Absolute upper-left X/{x=$4}/Absolute upper-left Y/{y=$4}/Width/{w=$2}/Height/{h=$2}/Map State/{m=$3}END{print x","y","w","h","m}')
        s=$(xprop -id $w WM_STATE 2>/dev/null | awk '/window state/{print $3}')
        echo "$w $g $s"
    done
    echo "focus $(xdotool getwindowfocus 2>/dev/null)"
}

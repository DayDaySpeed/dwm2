#! /bin/bash
# 远程桌面 (x11vnc): 共享当前 dwm 桌面 :0, 只监听本机, 外部经 SSH 隧道连接
#   vnc.sh start | stop | toggle | status | passwd
# 可通过 ssh 远程执行, 此时没有 $DWM / $DISPLAY, 下面自行推算

DWM=${DWM:-$(cd "$(dirname "$0")/.."; pwd)}
export DISPLAY=${DISPLAY:-:0}
PASSFILE=~/.vnc/passwd
LOGFILE=~/.cache/x11vnc.log
PORT=5900

notify() { notify-send -r 9528 "󰢹 远程桌面" "$1" 2>/dev/null; echo "$1"; }
running() { pgrep -x x11vnc >/dev/null; }

start() {
    running && { notify "已在运行 (localhost:$PORT)"; return; }
    command -v x11vnc >/dev/null || { notify "未安装 x11vnc: sudo pacman -S x11vnc"; exit 1; }
    [ -f $PASSFILE ] || { notify "未设置密码, 请先在终端执行: $DWM/bin/vnc.sh passwd"; exit 1; }
    mkdir -p "$(dirname $LOGFILE)"
    x11vnc -display :0 -auth guess -localhost -rfbport $PORT -rfbauth $PASSFILE \
           -forever -shared -noxdamage -bg -o $LOGFILE >/dev/null 2>&1
    running && notify "已开启 (localhost:$PORT)" || { notify "启动失败, 日志: $LOGFILE"; exit 1; }
}

stop() {
    running || { notify "未在运行"; return; }
    pkill -x x11vnc
    notify "已关闭"
}

case $1 in
    start)  start ;;
    stop)   stop ;;
    toggle) running && stop || start ;;
    status) running && echo "运行中 (localhost:$PORT)" || { echo "未运行"; exit 1; } ;;
    passwd) mkdir -p ~/.vnc && x11vnc -storepasswd $PASSFILE ;;
    *)      sed -n '2,4p' "$0"; exit 1 ;;
esac

#! /bin/bash
# 远程桌面 (x11vnc): 共享当前 dwm 桌面 :0, 局域网内直接连接 <本机IP>:5900 (不加密, 仅限可信网络)
#   vnc.sh start | stop | toggle | status | passwd
# 可通过 ssh 远程执行, 此时没有 $DWM / $DISPLAY, 下面自行推算

DWM=${DWM:-$(cd "$(dirname "$0")/.."; pwd)}
export DISPLAY=${DISPLAY:-:0}
PASSFILE=~/.vnc/passwd
LOGFILE=~/.cache/x11vnc.log
PORT=5900
ALLOW=127.,192.168.,10.,100.    # 只允许本机、局域网与 Tailscale (100.x) 地址连接
SCALE=${VNC_SCALE:-3/4}    # 画面缩放: 2560x1440 * 3/4 = 1920x1080, 与客户端屏幕一致才能全屏铺满; 1 为不缩放

# 局域网 IP: 取物理网卡上 192.168.* / 10.* 的地址 (跳过 docker 网桥与代理虚拟网卡)
lanip() {
    ip -4 -o addr show scope global | awk '$2 !~ /^(docker|br-|veth|Meta|tun)/ {sub(/\/.*/, "", $4); print $4}' \
        | grep -E '^(192\.168|10)\.' | head -1
}

notify() { notify-send -r 9528 "󰢹 远程桌面" "$1" 2>/dev/null; echo "$1"; }
running() { pgrep -x x11vnc >/dev/null; }

start() {
    running && { notify "已在运行 ($(lanip):$PORT)"; return; }
    command -v x11vnc >/dev/null || { notify "未安装 x11vnc: sudo pacman -S x11vnc"; exit 1; }
    [ -f $PASSFILE ] || { notify "未设置密码, 请先在终端执行: $DWM/bin/vnc.sh passwd"; exit 1; }
    mkdir -p "$(dirname $LOGFILE)"
    x11vnc -display :0 -auth "${XAUTHORITY:-$HOME/.Xauthority}" -rfbport $PORT -allow $ALLOW -rfbauth $PASSFILE \
           -scale $SCALE -wait 10 -defer 10 \
           -forever -shared -noxdamage -bg -o $LOGFILE >/dev/null 2>&1
    for _ in $(seq 10); do                                   # 等待端口开始监听 (最多 5 秒)
        ss -ltn | grep -q ":$PORT " && { notify "已开启 ($(lanip):$PORT)"; return; }
        sleep 0.5
    done
    pkill -x x11vnc
    notify "启动失败, 日志: $LOGFILE"; exit 1
}

stop() {
    running || { notify "未在运行"; return; }
    pkill -x x11vnc
    for _ in $(seq 10); do running || break; sleep 0.3; done   # 等进程真正退出
    running && pkill -9 -x x11vnc
    notify "已关闭"
}

case $1 in
    start)  start ;;
    stop)   stop ;;
    toggle) running && stop || start ;;
    status) running && echo "运行中 ($(lanip):$PORT)" || { echo "未运行"; exit 1; } ;;
    passwd) mkdir -p ~/.vnc && x11vnc -storepasswd $PASSFILE ;;
    *)      sed -n '2,4p' "$0"; exit 1 ;;
esac

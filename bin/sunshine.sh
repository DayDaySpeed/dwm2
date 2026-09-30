#! /bin/bash
# 远程桌面 (Sunshine): 以硬件编码的视频流共享当前 dwm 桌面, Windows 上用 Moonlight 连接
#   sunshine.sh start | stop | toggle | status
# 可通过 ssh 远程执行, 此时没有 $DWM / $DISPLAY, 下面自行推算

DWM=${DWM:-$(cd "$(dirname "$0")/.."; pwd)}
export DISPLAY=${DISPLAY:-:0}
export XAUTHORITY=${XAUTHORITY:-$HOME/.Xauthority}
WEBPORT=47990
LOGFILE=~/.config/sunshine/sunshine.log

notify() { notify-send -r 9529 "󱒃 Sunshine" "$1" 2>/dev/null; echo "$1"; }
running() { pgrep -x sunshine >/dev/null; }

# 局域网 IP: 取物理网卡上 192.168.* / 10.* 的地址 (跳过 docker 网桥与代理虚拟网卡)
lanip() {
    ip -4 -o addr show scope global | awk '$2 !~ /^(docker|br-|veth|Meta|tun)/ {sub(/\/.*/, "", $4); print $4}' \
        | grep -E '^(192\.168|10)\.' | head -1
}

start() {
    running && { notify "已在运行 ($(lanip))"; return; }
    command -v sunshine >/dev/null || { notify "未安装 sunshine: yay -S sunshine-bin"; exit 1; }
    setsid -f sunshine >/dev/null 2>&1
    for _ in $(seq 20); do                                   # 等待网页控制台端口开始监听 (最多 10 秒)
        ss -ltn | grep -q ":$WEBPORT " && { notify "已开启, Moonlight 添加主机: $(lanip)"; return; }
        sleep 0.5
    done
    pkill -x sunshine
    notify "启动失败, 日志: $LOGFILE"; exit 1
}

stop() {
    running || { notify "未在运行"; return; }
    pkill -x sunshine
    for _ in $(seq 10); do running || break; sleep 0.3; done   # 等进程真正退出
    running && pkill -9 -x sunshine
    notify "已关闭"
}

case $1 in
    start)  start ;;
    stop)   stop ;;
    toggle) running && stop || start ;;
    status) running && echo "运行中 (Moonlight 添加主机: $(lanip), 控制台 https://localhost:$WEBPORT)" || { echo "未运行"; exit 1; } ;;
    *)      sed -n '2,4p' "$0"; exit 1 ;;
esac

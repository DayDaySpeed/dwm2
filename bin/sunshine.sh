#! /bin/bash
# 远程桌面 (Sunshine): 以硬件编码的视频流共享当前 dwm 桌面, Windows 上用 Moonlight 连接
#   sunshine.sh start | stop | toggle | status | altkey
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
# Tailscale IP: 固定不变, 换网络也能连 (未安装/未登录时为空)
tsip() { tailscale ip -4 2>/dev/null | head -1; }
# 连接地址: 优先 Tailscale IP
addr() { local t; t=$(tsip); echo "${t:-$(lanip)}"; }

# 远程键盘 Alt <-> Super 互换: Windows 会截获 Win 键, 远程时用 Alt 操作 dwm
# 只作用于 Sunshine 的虚拟键盘设备, 本地键盘不受影响
altkey() {
    local id
    for _ in $(seq 20); do                                   # 等待虚拟键盘设备出现 (最多 10 秒)
        id=$(xinput list --id-only "keyboard:libvirtualhid Keyboard" 2>/dev/null) && break
        sleep 0.5
    done
    [ -n "$id" ] || { echo "未找到 Sunshine 虚拟键盘"; return 1; }
    setxkbmap -device "$id" -layout us -option -option altwin:swap_alt_win
}

start() {
    running && { notify "已在运行 ($(addr))"; return; }
    command -v sunshine >/dev/null || { notify "未安装 sunshine: yay -S sunshine-bin"; exit 1; }
    # 网页控制台默认只信任 https://localhost 来源, 从 Windows 用 IP 访问会被 CSRF 保护拦截, 启动时加上局域网 IP 与 Tailscale IP
    local origins="https://$(lanip):$WEBPORT" t
    t=$(tsip); [ -n "$t" ] && origins="$origins,https://$t:$WEBPORT"
    setsid -f sunshine "csrf_allowed_origins=$origins" >/dev/null 2>&1
    for _ in $(seq 20); do                                   # 等待网页控制台端口开始监听 (最多 10 秒)
        ss -ltn | grep -q ":$WEBPORT " && { altkey; notify "已开启, Moonlight 添加主机: $(addr)"; return; }
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
    altkey) altkey ;;
    status) running && echo "运行中 (Moonlight 添加主机: $(addr), 控制台 https://$(addr):$WEBPORT)" || { echo "未运行"; exit 1; } ;;
    *)      sed -n '2,4p' "$0"; exit 1 ;;
esac

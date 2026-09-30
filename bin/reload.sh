#! /bin/bash
# 一键生效 (Super + Shift + R): 打补丁并编译 dwm -> 安装到 ~/.local/bin -> 让正在运行的 dwm 原地重启
# 编译失败时弹出通知, 正在运行的 dwm 保持不变

DWM=${DWM:-$(cd "$(dirname "$0")/.."; pwd)}
LOG=~/.cache/dwm-build.log
NID=9531

notify-send -r $NID -t 3000 " dwm" "编译中..."
if "$DWM/setup.sh" dwm > "$LOG" 2>&1; then
    pid=$(pgrep -x -u "$USER" dwm | head -1)
    # 只有 ~/.local/bin/dwm (带重启补丁) 才处理 SIGHUP; 旧版本收到 SIGHUP 会直接退出, 整个会话随之结束
    if [ "$(readlink -f /proc/$pid/exe 2>/dev/null)" != "$(readlink -f ~/.local/bin/dwm)" ]; then
        notify-send -r $NID -u critical " dwm" "已编译安装到 ~/.local/bin/dwm, 但当前运行的不是它。\n请退出 (Super+Shift+Esc) 后重新 startx 一次, 之后按 Super+Shift+R 即可自动生效"
        exit 0
    fi
    kill -HUP "$pid"
    # dwm 主循环阻塞在等待 X 事件, 刷新一次状态栏 (更新 root 窗口标题) 让它立即处理重启
    "$DWM/bin/statusbar/statusbar.sh" updateall >/dev/null 2>&1
    notify-send -r $NID -t 3000 " dwm" "已重新加载"
else
    notify-send -r $NID -u critical " dwm 编译失败" "$(grep -iE 'error|错误' "$LOG" | head -5)\n\n完整日志: $LOG"
fi

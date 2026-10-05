#! /bin/bash
# 快捷键速查 (Super+/): 解析 config/dwm.h 里每条绑定的行尾注释, 用 rofi 列出, 可搜索, 只看不执行
#   键盘: { MODKEY|..., XK_x, ... },  /* super x | 说明 */
#   鼠标: { ClkXxx, ... },            // 按键 | 位置 | 说明
#   TAGKEYS(XK_x, n, "命令"):         super x 切换到第 n+1 个 tag (为空时启动命令), 另附 shift / ctrl 的通用说明

DWM=${DWM:-$(cd "$(dirname "$0")/.."; pwd)}

list() {
    awk '
    # 键盘绑定: 行尾 /* 按键 | 说明 */
    /^[[:space:]]*\{ *(MODKEY|0|Mod|XF86)/ && match($0, /\/\*[^*]*\|[^*]*\*\//) {
        c = substr($0, RSTART + 2, RLENGTH - 4)
        split(c, a, "|"); gsub(/^ +| +$/, "", a[1]); gsub(/^ +| +$/, "", a[2])
        printf "%-22s %s\n", a[1], a[2]
    }
    # 鼠标绑定: 行尾 // 按键 | 位置 | 说明
    /^[[:space:]]*\{ *Clk/ && match($0, /\/\/.*\|.*\|.*/) {
        c = substr($0, RSTART + 2)
        n = split(c, a, "|"); for (i = 1; i <= n; i++) gsub(/^ +| +$/, "", a[i])
        line = sprintf("%-22s %s", "鼠标 " a[2] " " a[1], a[3])
        if (!seen[line]++) print line                 # 同一操作在多处绑定 (如 super+滚轮) 只列一次
    }
    # tag 跳转
    /^[[:space:]]*TAGKEYS\(XK_/ {
        match($0, /XK_[A-Za-z0-9]+/); key = substr($0, RSTART + 3, RLENGTH - 3)
        match($0, /, *[0-9]+ *,/); n = substr($0, RSTART + 1, RLENGTH - 2) + 1
        cmd = ""; if (match($0, /"[^"]*"/)) cmd = "  (tag 为空时启动 " substr($0, RSTART + 1, RLENGTH - 2) ")"
        printf "%-22s 切换到第 %d 个 tag%s\n", "super " key, n, cmd
    }
    END {
        printf "%-22s %s\n", "super shift <tag键>", "把当前窗口移到该 tag"
        printf "%-22s %s\n", "super ctrl <tag键>", "同时显示 / 取消显示该 tag"
    }' "$DWM/config/dwm.h"
}

list | rofi -dmenu -i -p 快捷键 -window-title keys -no-custom >/dev/null

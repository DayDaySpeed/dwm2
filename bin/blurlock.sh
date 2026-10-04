#! /bin/bash
# 锁屏 (betterlockscreen, 基于 i3lock-color): 当前壁纸调暗 + 模糊作背景, 屏幕中央显示时间、日期和密码圆环
#   blurlock.sh          锁屏 (Super+Ctrl+L, xss-lock 自动锁屏)
#   blurlock.sh update   用当前壁纸重新生成锁屏背景 (autostart 每次换壁纸后在后台调用; 未换壁纸时跳过)
#   blurlock.sh update -f  强制重新生成 (修改 config/betterlockscreenrc 后执行)
# 主题配置: config/betterlockscreenrc

CACHE=${XDG_CACHE_HOME:-$HOME/.cache}/betterlockscreen
SRC=$CACHE/source                          # 记录生成背景所用的壁纸, 未换壁纸时跳过重新生成
LOCKIMG=$CACHE/current/lock_dimblur.png

# 当前壁纸: feh --randomize 把实际显示的那张写在 ~/.fehbg 的第一个路径
curwall() { sed -n "s/^feh .*--bg-fill '\([^']*\)'.*/\1/p" ~/.fehbg 2>/dev/null; }

update() {
    local wall
    wall=$(curwall)
    command -v betterlockscreen >/dev/null && [ -f "$wall" ] || return 1
    [ "$1" != -f ] && [ -f "$LOCKIMG" ] && [ "$(cat "$SRC" 2>/dev/null)" = "$wall" ] && return 0
    mkdir -p "$CACHE"
    exec 9>"$CACHE/.lock"
    flock -n 9 || return 0                 # 已有一次生成在进行
    nice -n 19 betterlockscreen -q -u "$wall" && echo "$wall" > "$SRC"
}

lock() {
    # 未安装 betterlockscreen 时退回 i3lock-color 直接模糊当前画面, 保证总能锁屏
    command -v betterlockscreen >/dev/null || exec i3lock -n --blur 5 --clock --force-clock
    [ -f "$LOCKIMG" ] || update            # 首次使用还没有背景: 先生成 (几秒)
    (sleep 0.5; xdotool mousemove_relative 1 1) & # 解决自动锁屏后未展示锁屏界面的问题 (移动一下鼠标)
    # -- 之后的参数覆盖 betterlockscreen 内置的版面 (内置为左下角小字, 不适合 2560x1440); 星期用中文显示
    # 密码指示器: 铺满屏幕底部的跳动竖条 (沿用原 i3lock 的参数), 输错时底部横带变为半透明白色
    LC_TIME=zh_CN.UTF-8 betterlockscreen -q -l dimblur -- \
        --time-pos="w/2:h/2-60" --time-align 0 --time-size=120 \
        --date-str "%m月%d日 %A" --date-pos="tx:ty+60" --date-align 0 --date-size=28 \
        --date-font="Sarasa UI SC" --date-color=ffffffbb \
        --greeter-pos="w/2:h/2+130" --greeter-align 0 --greeter-size=22 \
        --verif-pos="w/2:h/2+190" --verif-align 0 --verif-size=20 \
        --wrong-pos="w/2:h/2+190" --wrong-align 0 --wrong-size=20 \
        --modif-pos="w/2:h/2+230" --modif-align 0 --modif-size=20 \
        --bar-indicator --redraw-thread \
        --bar-pos="y+h" --bar-direction=1 --bar-max-height=50 --bar-base-width=50 \
        --bar-step=20 --bar-periodic-step=50 --bar-color=00000022 \
        --keyhl-color=ffffffcc --ringver-color=ffffff00 --ringwrong-color=ffffff88
}

case $1 in
    update) update "$2" ;;
    *)      lock ;;
esac

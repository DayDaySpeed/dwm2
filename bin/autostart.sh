#! /bin/bash
# DWM 自启动脚本, 由 dwm 启动时调用 ($DWM = 项目根目录)
# 配置文件统一在 $DWM/config, 脚本统一在 $DWM/bin

settings() {
    [ $1 ] && sleep $1
    xset -b                                   # 关闭蜂鸣器
}

daemons() {
    [ $1 ] && sleep $1
    $DWM/bin/statusbar/statusbar.sh cron &   # 开启状态栏定时更新
    xss-lock -- $DWM/bin/blurlock.sh &       # 开启自动锁屏程序
    fcitx5 &                                  # 开启输入法
    flameshot &                               # 截图要跑一个程序在后台 不然无法将截图保存到剪贴板
    dunst -conf $DWM/config/dunst.conf & # 开启通知server
    picom --experimental-backends --config $DWM/config/picom.conf >> /dev/null 2>&1 & # 开启picom
}

cron() {
    [ $1 ] && sleep $1
    let i=10
    while true; do
        [ $((i % 300)) -eq 0 ] && feh --randomize --bg-fill ~/Pictures/wallpaper/*.png # 每300秒更新壁纸
        sleep 10; let i+=10
    done
}

settings 1 &                                  # 初始化设置项
daemons 3 &                                   # 后台程序项
cron 5 &                                      # 定时任务项

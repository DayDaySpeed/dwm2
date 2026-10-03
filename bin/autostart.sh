#! /bin/bash
# DWM 自启动脚本, 由 dwm 启动时调用 ($DWM = 项目根目录)
# 所有开机启动项都在这里; 环境变量在 $DWM/config/xinitrc
# 配置文件统一在 $DWM/config, 脚本统一在 $DWM/bin

has() { command -v "$1" >/dev/null 2>&1; }

settings() {
    [ $1 ] && sleep $1
    xset -b                                   # 关闭蜂鸣器
    $DWM/bin/livewall.sh auto                 # 设置壁纸 (动态视频 / 静态图片), 并在后台生成锁屏背景
    numlockx on 2>/dev/null || xset led named "Num Lock" # 开启 NumLock
    xinput list --name-only | grep "^SYNA32E2" | while read -r d; do xinput disable "$d"; done # 禁用触摸板 (X 中名为 "SYNA32E2:00 06CB:CEE7 Mouse")
    has syndaemon && syndaemon -i 1 -t -K -R -d # 设置使用键盘时触控板短暂失效(需 xf86-input-synaptics)
    xhost +local:docker >/dev/null            # 允许 Docker 使用显示器
}

daemons() {
    [ $1 ] && sleep $1
    $DWM/bin/statusbar/statusbar.sh cron &    # 开启状态栏定时更新
    xss-lock -- $DWM/bin/blurlock.sh &        # 开启自动锁屏程序
    $DWM/bin/dpms.sh &                        # 10 分钟无操作熄屏 (持续守护, 防止被改回默认)
    fcitx5 &                                  # 开启输入法
    flameshot &                               # 截图要跑一个程序在后台 不然无法将截图保存到剪贴板
    parcellite &                              # 剪贴板管理器
    has lemonade && lemonade server &         # 开启lemonade 远程剪切板支持
    dunst -conf $DWM/config/dunst.conf &      # 开启通知server
    picom --experimental-backends --config $DWM/config/picom.conf >> /dev/null 2>&1 & # 开启picom
}

cron() {
    [ $1 ] && sleep $1
    let i=10
    while true; do
        [ $((i % 300)) -eq 0 ] && $DWM/bin/livewall.sh cron # 每300秒更新静态壁纸 (及锁屏背景); 动态壁纸不打断
        sleep 10; let i+=10
    done
}

settings 1 &                                  # 初始化设置项
daemons 3 &                                   # 后台程序项
cron 5 &                                      # 定时任务项

#! /bin/bash
# DWM 自启动脚本, 由 dwm 启动时调用 ($DWM = 项目根目录)
# 所有开机启动项都在这里; 环境变量在 $DWM/config/xinitrc
# 配置文件统一在 $DWM/config, 脚本统一在 $DWM/bin

has() { command -v "$1" >/dev/null 2>&1; }

settings() {
    [ $1 ] && sleep $1
    xset -b                                   # 关闭蜂鸣器
    has autorandr && autorandr --change >/dev/null 2>&1 # 外接显示器按已保存的布局设置 (改屏幕的都要在启动壁纸之前)
    $DWM/bin/powersave.sh apply               # 刷新率和 CPU 节能策略: 省电模式开着时 60Hz + 节能, 否则用最高刷新率 (重启后会被重置)
    $DWM/bin/livewall.sh auto                 # 设置壁纸 (动态视频 / 静态图片), 并在后台生成锁屏背景; 要在改刷新率之后, 场景壁纸遇到屏幕变化会崩溃
    $DWM/bin/nightlight.sh apply              # 护眼开着时重新应用暖色
    numlockx on 2>/dev/null || xset led named "Num Lock" # 开启 NumLock
    xinput list --name-only | grep "^SYNA32E2" | while read -r d; do xinput disable "$d"; done # 禁用触摸板 (X 中名为 "SYNA32E2:00 06CB:CEE7 Mouse")
    xhost +local:docker >/dev/null            # 允许 Docker 使用显示器
}

daemons() {
    [ $1 ] && sleep $1
    $DWM/bin/statusbar/statusbar.sh cron &    # 开启状态栏定时更新
    xss-lock -l -n $DWM/bin/dimscreen.sh -- $DWM/bin/blurlock.sh &  # 自动锁屏: 先调暗提醒, 睡眠前先锁好屏 (时间见 bin/dpms.sh)
    $DWM/bin/dpms.sh &                        # 无操作 9.5 分钟调暗 / 10 分钟锁屏 / 12 分钟熄屏 (持续守护, 防止被改回默认)
    fcitx5 &                                  # 开启输入法
    flameshot &                               # 截图要跑一个程序在后台 不然无法将截图保存到剪贴板
    { has clipmenud && clipmenud || parcellite; } & # 剪贴板历史 (Super+X 调出; 未装 clipmenu 时退回 parcellite)
    has lemonade && lemonade server &         # 开启lemonade 远程剪切板支持
    dunst -conf $DWM/config/dunst.conf &      # 开启通知server
    picom --experimental-backends --config $DWM/config/picom.conf >> /dev/null 2>&1 & # 开启picom
}

cron() {
    [ $1 ] && sleep $1
    let i=10
    while true; do
        [ $((i % 300)) -eq 0 ] && $DWM/bin/livewall.sh cron # 每300秒更新静态壁纸 (及锁屏背景); 动态壁纸不打断
        $DWM/bin/powersave.sh ac >/dev/null       # 拔电自动开省电模式, 插电自动关 (只关自动开的)
        [ $((i % 60)) -eq 0 ] && $DWM/bin/batalert.sh # 每60秒检查电量: 20% / 10% 提醒, 5% 自动睡眠
        sleep 10; let i+=10
    done
}

settings 1 &                                  # 初始化设置项
daemons 3 &                                   # 后台程序项
cron 5 &                                      # 定时任务项

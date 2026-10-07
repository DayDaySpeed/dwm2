#!/bin/bash
# dwm2 一键安装/编译
#   ./setup.sh             全部: 依赖 + 链接 + dwm st tabbed picom i3lock
#   ./setup.sh deps        安装依赖 (i3lock-color + betterlockscreen, picom / 星系 OpenGL 渲染的编译依赖)
#   ./setup.sh link        建立 ~/.xinitrc 等软链接
#   ./setup.sh dwm|st|tabbed|picom   只编译安装某一个
#   ./setup.sh i3lock      编译 i3lock-color (锁屏输入时显示圆点)
set -e
DWM=$(cd "$(dirname "$0")"; pwd)
cd "$DWM"

step() { printf '\n\033[1;34m==> %s\033[0m\n' "$*"; }

deps() {
    step "依赖"
    if ! pacman -Q i3lock-color >/dev/null 2>&1; then
        pacman -Q i3lock >/dev/null 2>&1 && sudo pacman -Rdd --noconfirm i3lock
        yay -S --needed i3lock-color
    fi
    yay -S --needed betterlockscreen   # 锁屏 (bin/blurlock.sh), 基于 i3lock-color
    yay -S --needed xwinwrap-git       # 动态壁纸 (bin/livewall.sh): 把 mpv 嵌到桌面最底层
    sudo pacman -S --needed meson ninja uthash libconfig libev libxdg-basedir pcre2 libepoxy \
        pixman dbus mesa xcb-util-image xcb-util-renderutil libx11 libxext \
        rofi feh mpv ffmpeg xorg-xwininfo xorg-xprop xdotool python dunst \
        gammastep clipmenu autorandr playerctl brightnessctl flameshot xss-lock fcitx5 pamixer x11vnc \
        intel-media-driver libva-utils \
        pcmanfm \
        ranger ueberzugpp ffmpegthumbnailer translate-shell \
        git-lfs
    yay -S --needed sunshine-bin    # 远程桌面串流 (Windows 端用 Moonlight)
    # 音乐播放器 SPlayer (Super + M): 网易云风格, 支持本地音乐; 官方 AppImage 自带 Electron, 无需编译
    if [ ! -x ~/.local/share/splayer/SPlayer.AppImage ]; then
        mkdir -p ~/.local/share/splayer
        curl -fL -o ~/.local/share/splayer/SPlayer.AppImage \
            https://github.com/imsyy/SPlayer/releases/download/v3.1.1/splayer-3.1.1-x86_64.AppImage
        chmod +x ~/.local/share/splayer/SPlayer.AppImage
    fi
    sudo pacman -S --needed tailscale  # 固定的虚拟 IP, 换网络 (手机热点等) 也能远程连接
    sudo systemctl enable --now tailscaled
}

link() {
    step "软链接"
    ln -sfn "$DWM/config/xinitrc" ~/.xinitrc
    ln -sfn "$DWM/config/Xresources" ~/.Xresources
    # wallpaper/ 是私有子模块 dwm2-wallpaper (Git LFS)。没权限时跳过, 目录留空。
    if [ -f .gitmodules ]; then
        git submodule update --init -- wallpaper \
            || printf '\033[33m壁纸子模块未拉取: 私有仓库, 需要 dwm2-wallpaper 的访问权限, 并已安装 git-lfs\033[0m\n'
    fi
    if [ -d ~/Pictures/wallpaper ] && [ ! -L ~/Pictures/wallpaper ]; then
        mv -n ~/Pictures/wallpaper/* "$DWM/wallpaper/" 2>/dev/null || true
        rmdir ~/Pictures/wallpaper
    fi
    ln -sfn "$DWM/wallpaper" ~/Pictures/wallpaper
    # rofi 主题通过相对路径引用 colors/, 需链接整个目录 (旧版本只链接了 config.rasi 文件)
    [ -d ~/.config/rofi ] && [ ! -L ~/.config/rofi ] && { rm -f ~/.config/rofi/config.rasi; rmdir ~/.config/rofi; }
    ln -sfn "$DWM/config/rofi" ~/.config/rofi
    ln -sfn "$DWM/config/ranger" ~/.config/ranger
    mkdir -p ~/Music
    mkdir -p ~/.config/betterlockscreen
    ln -sfn "$DWM/config/betterlockscreenrc" ~/.config/betterlockscreen/betterlockscreenrc
    mkdir -p ~/.config/sunshine
    ln -sfn "$DWM/config/sunshine.conf" ~/.config/sunshine/sunshine.conf
    # dwm 的 Makefile 要求这三项在源码目录中 (均已被 dwm 的 .gitignore 忽略)
    ln -sfn ../config/dwm.h     dwm/config.h
    ln -sfn ../bin/autostart.sh dwm/autostart.sh
    ln -sfn ../bin/statusbar    dwm/statusbar
}

# st / tabbed 源码直接放在本仓库 (基于 yaocccc/st、yaocccc/tabbed, 已含本仓库的改动), 直接修改源码即可;
# 配置 config.h 软链接到 config/<name>.h
build_suckless() {
    local name=$1
    step "$name"
    ln -sfn "../config/$name.h" "$name/config.h"
    (cd "$name" && make clean && make && sudo make install)
}

# dwm 源码直接放在本仓库 (基于 yaocccc/dwm), 直接修改 dwm/ 下的源码即可
build_dwm() {
    step "dwm"
    link
    # 安装到 ~/.local/bin (无需 sudo, 可由 Super+Shift+R 自动完成); 先写临时文件再改名, 可覆盖正在运行的 dwm
    (cd dwm && make clean && make && mkdir -p ~/.local/bin && install -m755 dwm ~/.local/bin/.dwm.new && mv -f ~/.local/bin/.dwm.new ~/.local/bin/dwm)
}

# picom v13 源码直接放在本仓库 (带 dwm2 的 _DWM_NOANIM 补丁, 见 picom/README.dwm2.md), 安装到 ~/.local (PATH 中排在系统包前面)
build_picom() {
    step "picom v13 (-> ~/.local/bin/picom)"
    cd picom
    rm -rf build
    meson setup --buildtype=release -Dprefix="$HOME/.local" build
    ninja -C build
    ninja -C build install
    cd "$DWM"
}

# i3lock-color 源码直接放在本仓库 (2.13.c.5, 已加入输入时显示圆点的改动), 安装为 /usr/local/bin/i3lock-color
# (betterlockscreen 优先用这个名字); 系统包的 /usr/bin/i3lock 保持不变, 锁屏的 PAM 配置也用系统包的
build_i3lock() {
    step "i3lock-color (-> /usr/local/bin/i3lock-color)"
    (cd i3lock-color && ./build.sh)
    sudo install -m755 i3lock-color/build/i3lock /usr/local/bin/i3lock-color
}

case "${1:-all}" in
    all)    deps; link; build_dwm; build_suckless st; build_suckless tabbed; build_picom; build_i3lock
            step "完成。首次安装请重新 startx; 之后修改 dwm 配置按 Super + Shift + R 即可生效" ;;
    deps)   deps ;;
    link)   link ;;
    dwm)    build_dwm ;;
    st|tabbed) build_suckless "$1" ;;
    picom)  build_picom ;;
    i3lock) build_i3lock ;;
    *)      sed -n '2,8p' "$0"; exit 1 ;;
esac

#!/bin/bash
# dwm2 一键安装/编译
#   ./setup.sh             全部: 依赖 + 链接 + dwm st tabbed picom
#   ./setup.sh deps        安装依赖 (i3lock-color, picom 编译依赖)
#   ./setup.sh link        建立 ~/.xinitrc 等软链接
#   ./setup.sh dwm|st|tabbed|picom   只编译安装某一个
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
    sudo pacman -S --needed meson ninja uthash libconfig libev libxdg-basedir pcre \
        pixman dbus mesa xcb-util-image xcb-util-renderutil libx11 libxext \
        rofi feh dunst flameshot xss-lock fcitx5 pamixer
}

link() {
    step "软链接"
    ln -sfn "$DWM/config/xinitrc" ~/.xinitrc
    ln -sfn "$DWM/config/Xresources" ~/.Xresources
    mkdir -p ~/Pictures/screenshots
    if [ -d ~/Pictures/wallpaper ] && [ ! -L ~/Pictures/wallpaper ]; then
        mv -n ~/Pictures/wallpaper/* "$DWM/wallpaper/" 2>/dev/null || true
        rmdir ~/Pictures/wallpaper
    fi
    ln -sfn "$DWM/wallpaper" ~/Pictures/wallpaper
    mkdir -p ~/.config/rofi
    ln -sfn "$DWM/config/rofi.rasi" ~/.config/rofi/config.rasi
    # dwm 的 Makefile 要求这三项在源码目录中 (均已被 dwm 的 .gitignore 忽略)
    ln -sfn ../config/dwm.h     dwm/config.h
    ln -sfn ../bin/autostart.sh dwm/autostart.sh
    ln -sfn ../bin/statusbar    dwm/statusbar
}

# st / tabbed 的 config.h 被上游仓库跟踪: 编译前放入我们的配置, 编译后还原, 保持子模块干净
build_suckless() {
    local name=$1
    step "$name"
    cp "config/$name.h" "$name/config.h"
    (cd "$name" && make clean && make && sudo make install) || { git -C "$name" checkout -- config.h; exit 1; }
    git -C "$name" checkout -- config.h
}

build_dwm() {
    step "dwm"
    link
    (cd dwm && make clean && make && sudo make install)
}

build_picom() {
    step "picom (yaocccc 动画分支 -> /usr/local)"
    cd picom
    git submodule update --init --recursive
    rm -rf build
    meson setup --buildtype=release -Dprefix=/usr/local build
    ninja -C build
    sudo ninja -C build install
    cd "$DWM"
}

case "${1:-all}" in
    all)    git submodule update --init; deps; link; build_dwm; build_suckless st; build_suckless tabbed; build_picom
            step "完成。退出 dwm (super+ctrl+F12) 后重新 startx 生效" ;;
    deps)   deps ;;
    link)   link ;;
    dwm)    build_dwm ;;
    st|tabbed) build_suckless "$1" ;;
    picom)  build_picom ;;
    *)      sed -n '2,7p' "$0"; exit 1 ;;
esac

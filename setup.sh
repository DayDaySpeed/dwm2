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
        rofi feh dunst flameshot xss-lock fcitx5 pamixer x11vnc \
        intel-media-driver libva-utils \
        pcmanfm lollypop \
        ranger ueberzugpp ffmpegthumbnailer translate-shell
    yay -S --needed sunshine-bin    # 远程桌面串流 (Windows 端用 Moonlight)
    sudo pacman -S --needed tailscale  # 固定的虚拟 IP, 换网络 (手机热点等) 也能远程连接
    sudo systemctl enable --now tailscaled
}

link() {
    step "软链接"
    ln -sfn "$DWM/config/xinitrc" ~/.xinitrc
    ln -sfn "$DWM/config/Xresources" ~/.Xresources
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
    mkdir -p ~/.config/sunshine
    ln -sfn "$DWM/config/sunshine.conf" ~/.config/sunshine/sunshine.conf
    # dwm 的 Makefile 要求这三项在源码目录中 (均已被 dwm 的 .gitignore 忽略)
    ln -sfn ../config/dwm.h     dwm/config.h
    ln -sfn ../bin/autostart.sh dwm/autostart.sh
    ln -sfn ../bin/statusbar    dwm/statusbar
}

# st / tabbed 的源码属于上游子模块: 编译前放入我们的 config.h 并打上 patches/<name>-*.diff,
# 编译后全部还原, 保持子模块干净 (可随时 git pull 上游)
build_suckless() {
    local name=$1 p
    step "$name"
    cp "config/$name.h" "$name/config.h"
    for p in patches/"$name"-*.diff; do
        [ -f "$p" ] && { echo "打补丁: $p"; git -C "$name" apply "$DWM/$p" || { git -C "$name" checkout -- .; exit 1; }; }
    done
    (cd "$name" && make clean && make && sudo make install) || { git -C "$name" checkout -- .; exit 1; }
    git -C "$name" checkout -- .
}

build_dwm() {
    local p
    step "dwm"
    link
    for p in patches/dwm-*.diff; do
        [ -f "$p" ] && { echo "打补丁: $p"; git -C dwm apply "$DWM/$p" || { git -C dwm checkout -- .; exit 1; }; }
    done
    # 安装到 ~/.local/bin (无需 sudo, 可由 Super+Shift+R 自动完成); 先写临时文件再改名, 可覆盖正在运行的 dwm
    (cd dwm && make clean && make && mkdir -p ~/.local/bin && install -m755 dwm ~/.local/bin/.dwm.new && mv -f ~/.local/bin/.dwm.new ~/.local/bin/dwm) \
        || { git -C dwm checkout -- .; exit 1; }
    git -C dwm checkout -- .
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
            step "完成。首次安装请重新 startx; 之后修改 dwm 配置按 Super + Shift + R 即可生效" ;;
    deps)   deps ;;
    link)   link ;;
    dwm)    build_dwm ;;
    st|tabbed) build_suckless "$1" ;;
    picom)  build_picom ;;
    *)      sed -n '2,7p' "$0"; exit 1 ;;
esac

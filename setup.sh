#!/bin/bash
# dwm2 安装入口: 新装的 Arch (base + 用户 + 网络 + sudo) 上克隆本仓库后运行 ./setup.sh, 重启即进入同样的桌面
# 每一步都可以重复运行, 已经满足的项会跳过
#   ./setup.sh             全部: bootstrap deps system link build services, 最后 check
#   ./setup.sh --no-apps   同上, 但不装快捷键对应的应用 (Chrome / Edge / VSCode / Steam / OBS / SPlayer / Sunshine); 也可 SETUP_APPS=0
#   ./setup.sh bootstrap   base-devel git, 开启 multilib, 没有 yay 时从 AUR 编译
#   ./setup.sh deps        按 packages/*.txt 装包, 按检测到的显卡 (Intel / AMD / NVIDIA) 追加驱动
#   ./setup.sh system      写 /etc (sudo): tty1 自动登录、zh_CN locale、省电模式免密、NumLock、N 卡 modeset, 登录 shell 改为 zsh, root 用同一套 zsh 配置
#   ./setup.sh link        ~/.xinitrc、~/.zshrc、~/.config/* 等链接到仓库; 程序会改写的配置只在不存在时复制一份
#   ./setup.sh build       编译安装 dwm st tabbed picom i3lock; 也可单独 ./setup.sh dwm|st|tabbed|picom|i3lock
#   ./setup.sh services    启用 NetworkManager bluetooth tailscaled, 用户服务 sni-watcher
#   ./setup.sh check       只读体检: 缺的包、链接、服务、系统配置, 每项打印 ✓ / ✗
set -e
DWM=$(cd "$(dirname "$0")"; pwd)
cd "$DWM"

APPS=${SETUP_APPS:-1}
args=()
for a in "$@"; do [ "$a" = --no-apps ] && APPS=0 || args+=("$a"); done
set -- "${args[@]}"

step() { printf '\n\033[1;34m==> %s\033[0m\n' "$*"; }
warn() { printf '\033[33m%s\033[0m\n' "$*"; }

# 包清单: 去掉注释, 每行可写多个包
pkglist() { sed 's/#.*//' "$@" | tr -s ' \t' '\n' | grep -v '^$' || true; }

# 显卡厂商 (每行一个: intel / amd / nvidia), 按 PCI 显示控制器 (class 0x03xxxx) 判断, 不依赖 lspci
gpus() {
    local d
    for d in /sys/bus/pci/devices/*; do
        case $(cat "$d/class" 2>/dev/null) in 0x03*) ;; *) continue ;; esac
        case $(cat "$d/vendor" 2>/dev/null) in
            0x8086) echo intel ;;
            0x1002) echo amd ;;
            0x10de) echo nvidia ;;
        esac
    done | sort -u
}
has_gpu() { gpus | grep -qx "$1"; }

# 本次要装的包: 官方仓库 / AUR
repo_pkgs() {
    local g
    pkglist packages/core.txt
    [ "$APPS" = 1 ] && pkglist packages/apps.txt
    for g in $(gpus); do pkglist "packages/$g.txt"; done
    # dkms 驱动要有对应内核的头文件 (linux -> linux-headers, linux-lts -> linux-lts-headers)
    has_gpu nvidia && for g in /usr/lib/modules/*/pkgbase; do [ -f "$g" ] && echo "$(cat "$g")-headers"; done
    return 0
}
aur_pkgs() {
    pkglist packages/core-aur.txt
    [ "$APPS" = 1 ] && pkglist packages/apps-aur.txt
    return 0
}

bootstrap() {
    step "bootstrap: base-devel git multilib yay"
    sudo pacman -S --needed --noconfirm base-devel git
    # multilib: steam 和 lib32-nvidia-utils 在这里
    if ! grep -q '^\[multilib\]' /etc/pacman.conf; then
        sudo sed -i '/^#\[multilib\]/,/^#Include/ s/^#//' /etc/pacman.conf
        sudo pacman -Sy
    fi
    if ! command -v yay >/dev/null; then
        local tmp
        tmp=$(mktemp -d)
        git clone --depth 1 https://aur.archlinux.org/yay-bin.git "$tmp/yay-bin"
        (cd "$tmp/yay-bin" && makepkg -si --noconfirm)
        rm -rf "$tmp"
    fi
}

deps() {
    step "依赖 (显卡: $(gpus | tr '\n' ' ')应用: $([ "$APPS" = 1 ] && echo 装 || echo 不装))"
    # i3lock-color 与 i3lock 冲突, 先删掉 i3lock
    if pacman -Q i3lock >/dev/null 2>&1 && ! pacman -Q i3lock-color >/dev/null 2>&1; then
        sudo pacman -Rdd --noconfirm i3lock
    fi
    # shellcheck disable=SC2046
    sudo pacman -S --needed $(repo_pkgs)
    # shellcheck disable=SC2046
    yay -S --needed $(aur_pkgs)
    # 音乐播放器 SPlayer (Super + M): 官方 AppImage 自带 Electron, 无需编译
    if [ "$APPS" = 1 ] && [ ! -x ~/.local/share/splayer/SPlayer.AppImage ]; then
        mkdir -p ~/.local/share/splayer
        curl -fL -o ~/.local/share/splayer/SPlayer.AppImage \
            https://github.com/imsyy/SPlayer/releases/download/v3.1.1/splayer-3.1.1-x86_64.AppImage
        chmod +x ~/.local/share/splayer/SPlayer.AppImage
    fi
}

# 把模板 $1 (@USER@ 换成当前用户) 装到 $2, 权限 $3; 内容相同时跳过
put_etc() {
    local tmp
    tmp=$(mktemp)
    sed "s/@USER@/$(id -un)/g" "$1" > "$tmp"
    if sudo cmp -s "$tmp" "$2"; then echo "  已是最新: $2"
    else sudo install -D -m "$3" -o root -g root "$tmp" "$2" && echo "  已写入: $2"; fi
    rm -f "$tmp"
}

system() {
    step "系统配置 (/etc, 需要 sudo)"
    put_etc system/getty-autologin.conf /etc/systemd/system/getty@tty1.service.d/autologin.conf 644
    put_etc system/numlock.service /etc/systemd/system/numlock.service 644
    sudo systemctl daemon-reload
    sudo systemctl enable numlock.service
    # sudoers 写错会导致 sudo 不能用: 先 visudo 校验
    local tmp
    tmp=$(mktemp)
    sed "s/@USER@/$(id -un)/g" system/sudoers-powersave > "$tmp"
    visudo -cf "$tmp" >/dev/null && put_etc system/sudoers-powersave /etc/sudoers.d/powersave 440
    rm -f "$tmp"
    # 锁屏的中文星期 (LC_TIME=zh_CN.UTF-8)
    if ! locale -a 2>/dev/null | grep -qi '^zh_CN\.utf8$'; then
        sudo sed -i 's/^#\s*\(zh_CN\.UTF-8 UTF-8\)/\1/' /etc/locale.gen
        sudo locale-gen
    fi
    if has_gpu nvidia; then
        put_etc system/modprobe-nvidia.conf /etc/modprobe.d/nvidia.conf 644
    fi
    # ~/.zprofile 在 tty1 登录后执行 startx, 登录 shell 要是 zsh
    if [ "$(getent passwd "$(id -un)" | cut -d: -f7)" != /usr/bin/zsh ] && [ -x /usr/bin/zsh ]; then
        sudo chsh -s /usr/bin/zsh "$(id -un)"
    fi
    # root (su - / sudo -i) 也用同一套 zsh 配置; 复制而不是链接 (root 的配置不该跟着普通用户可写的文件变),
    # 改了 home/zsh 后重跑 ./setup.sh system 更新。不带 local.zsh (私有密钥)
    put_etc home/zshrc          /root/.zshrc               644
    put_etc home/zsh/zshrc      /root/.config/zsh/zshrc    644
    put_etc home/starship.toml  /root/.config/starship.toml 644
    if [ "$(getent passwd root | cut -d: -f7)" != /usr/bin/zsh ] && [ -x /usr/bin/zsh ]; then
        sudo chsh -s /usr/bin/zsh root
    fi
}

# 链接 $2 -> $1; 目标是普通文件或目录且内容不同时先备份成 <名字>.bak.<时间>
ln_() {
    local src=$1 dst=$2
    mkdir -p "$(dirname "$dst")"
    if [ -e "$dst" ] && [ ! -L "$dst" ]; then
        if [ -f "$dst" ] && cmp -s "$src" "$dst"; then rm -f "$dst"
        else mv "$dst" "$dst.bak.$(date +%Y%m%d%H%M%S)" && warn "  已备份原有的 $dst"; fi
    fi
    ln -sfn "$src" "$dst"
}

# 程序 (或 monitor.sh) 会改写的配置: 只在不存在时复制一份 (@HOME@ 换成家目录), 之后归本机所有
seed() {
    local src=$1 dst=$2
    [ -e "$dst" ] && return
    mkdir -p "$(dirname "$dst")"
    sed "s|@HOME@|$HOME|g" "$src" > "$dst"
    echo "  已复制: $dst"
}

# dwm / st / tabbed 的 Makefile 要求这些在源码目录中 (均已被 .gitignore 忽略)
link_src() {
    ln -sfn ../config/dwm.h     dwm/config.h
    ln -sfn ../bin/autostart.sh dwm/autostart.sh
    ln -sfn ../bin/statusbar    dwm/statusbar
    ln -sfn ../config/st.h      st/config.h
    ln -sfn ../config/tabbed.h  tabbed/config.h
}

link() {
    step "链接配置"
    local f
    ln_ "$DWM" ~/.config/dwm2                  # 固定入口: dunst.conf、sni-watcher.service 等不能用 $DWM 的地方经它找到仓库
    seed config/dwm2.local.example ~/.config/dwm2.local
    ln_ "$DWM/config/xinitrc"     ~/.xinitrc
    ln_ "$DWM/config/Xresources"  ~/.Xresources
    ln_ "$DWM/home/zprofile"      ~/.zprofile
    ln_ "$DWM/home/zshrc"         ~/.zshrc
    ln_ "$DWM/home/zsh/zshrc"     ~/.config/zsh/zshrc
    ln_ "$DWM/home/starship.toml" ~/.config/starship.toml
    ln_ "$DWM/home/kitty/kitty.conf" ~/.config/kitty/kitty.conf
    ln_ "$DWM/config/rofi"        ~/.config/rofi       # rofi 主题通过相对路径引用 colors/, 要链接整个目录
    ln_ "$DWM/config/ranger"      ~/.config/ranger
    ln_ "$DWM/config/betterlockscreenrc" ~/.config/betterlockscreen/betterlockscreenrc
    ln_ "$DWM/config/sunshine.conf"      ~/.config/sunshine/sunshine.conf
    ln_ "$DWM/home/systemd/sni-watcher.service" ~/.config/systemd/user/sni-watcher.service
    for f in home/fcitx5/profile home/fcitx5/config home/fcitx5/conf/*; do seed "$f" ~/.config/"${f#home/}"; done
    for f in home/fcitx5/rime/*; do seed "$f" ~/.local/share/fcitx5/rime/"${f##*/}"; done
    seed home/flameshot/flameshot.ini ~/.config/flameshot/flameshot.ini
    seed home/gtk-3.0/settings.ini    ~/.config/gtk-3.0/settings.ini
    seed home/gtk-4.0/settings.ini    ~/.config/gtk-4.0/settings.ini
    seed home/electron-flags.conf     ~/.config/electron-flags.conf
    seed home/mimeapps.list           ~/.config/mimeapps.list
    mkdir -p ~/Music ~/Pictures ~/.cache
    # wallpaper/ 是私有子模块 dwm2-wallpaper (Git LFS)。没权限时跳过, 目录留空
    if [ -f .gitmodules ]; then
        git lfs install >/dev/null 2>&1 || true
        git submodule update --init -- wallpaper \
            || warn "壁纸子模块未拉取: 私有仓库, 需要 dwm2-wallpaper 的访问权限 (SSH key) 和 git-lfs"
    fi
    if [ -d ~/Pictures/wallpaper ] && [ ! -L ~/Pictures/wallpaper ]; then
        mv -n ~/Pictures/wallpaper/* "$DWM/wallpaper/" 2>/dev/null || true
        rmdir ~/Pictures/wallpaper 2>/dev/null || true
    fi
    ln_ "$DWM/wallpaper" ~/Pictures/wallpaper
    link_src
}

# st / tabbed 源码直接放在本仓库 (基于 yaocccc/st、yaocccc/tabbed, 已含本仓库的改动), 直接修改源码即可;
# 配置 config.h 软链接到 config/<name>.h
build_suckless() {
    local name=$1
    step "$name"
    link_src
    (cd "$name" && make clean && make && sudo make install)
}

# dwm 源码直接放在本仓库 (基于 yaocccc/dwm), 直接修改 dwm/ 下的源码即可
build_dwm() {
    step "dwm"
    link_src
    # 安装到 ~/.local/bin (无需 sudo, 可由 Super+Shift+R 自动完成); 先写临时文件再改名, 可覆盖正在运行的 dwm
    (cd dwm && make clean && make && mkdir -p ~/.local/bin && install -m755 dwm ~/.local/bin/.dwm.new && mv -f ~/.local/bin/.dwm.new ~/.local/bin/dwm)
}

# picom v13 源码直接放在本仓库 (带 dwm2 的 _DWM_NOANIM 补丁, 见 picom/README.dwm2.md), 安装到 ~/.local (xinitrc 把 ~/.local/bin 排在 PATH 最前)
build_picom() {
    step "picom v13 (-> ~/.local/bin/picom)"
    (cd picom && rm -rf build && meson setup --buildtype=release -Dprefix="$HOME/.local" build && ninja -C build && ninja -C build install)
}

# i3lock-color 源码直接放在本仓库 (2.13.c.5, 已加入输入时显示圆点的改动), 安装为 /usr/local/bin/i3lock-color
# (betterlockscreen 优先用这个名字); 系统包的 /usr/bin/i3lock 保持不变, 锁屏的 PAM 配置也用系统包的
build_i3lock() {
    step "i3lock-color (-> /usr/local/bin/i3lock-color)"
    (cd i3lock-color && ./build.sh)
    sudo install -m755 i3lock-color/build/i3lock /usr/local/bin/i3lock-color
}

build() { build_dwm; build_suckless st; build_suckless tabbed; build_picom; build_i3lock; }

services() {
    step "服务"
    # NetworkManager 只 enable 不立即启动: 安装时如果正用 iwd / systemd-networkd 联网, 立即启动会抢网卡导致断网; 重启后生效
    sudo systemctl enable NetworkManager.service
    sudo systemctl enable --now bluetooth.service tailscaled.service
    systemctl --user daemon-reload
    systemctl --user enable sni-watcher.service
}

# ---------- check: 只读体检 ----------
fails=0
ok()  { printf '  \033[32m✓\033[0m %s\n' "$*"; }
bad() { printf '  \033[31m✗\033[0m %s\n' "$*"; fails=$((fails + 1)); }

check_link() { [ "$(readlink -f "$2")" = "$(readlink -f "$1")" ] && ok "$2" || bad "$2 未链接到 $1 (./setup.sh link)"; }

check() {
    local p miss f
    fails=0
    step "体检: 包"
    miss=$(for p in $(repo_pkgs) $(aur_pkgs); do pacman -Q "$p" >/dev/null 2>&1 || pacman -Qg "$p" >/dev/null 2>&1 || echo "$p"; done | tr '\n' ' ')
    [ -z "$miss" ] && ok "packages/*.txt 全部已装" || bad "未安装: $miss(./setup.sh deps)"
    command -v yay >/dev/null && ok "yay" || bad "没有 yay (./setup.sh bootstrap)"

    step "体检: 链接与配置"
    check_link "$DWM" ~/.config/dwm2
    check_link "$DWM/config/xinitrc" ~/.xinitrc
    check_link "$DWM/home/zprofile" ~/.zprofile
    check_link "$DWM/home/zshrc" ~/.zshrc
    check_link "$DWM/config/rofi" ~/.config/rofi
    check_link "$DWM/home/systemd/sni-watcher.service" ~/.config/systemd/user/sni-watcher.service
    for f in ~/.config/dwm2.local ~/.config/fcitx5/profile ~/.config/gtk-3.0/settings.ini ~/.config/electron-flags.conf; do
        [ -e "$f" ] && ok "$f" || bad "$f 不存在 (./setup.sh link)"
    done
    [ -n "$(ls -A wallpaper 2>/dev/null)" ] && ok "壁纸子模块" || bad "wallpaper/ 为空: 私有子模块未拉取"

    step "体检: 编译产物"
    for f in ~/.local/bin/dwm ~/.local/bin/picom /usr/local/bin/st /usr/local/bin/tabbed /usr/local/bin/i3lock-color; do
        [ -x "$f" ] && ok "$f" || bad "$f 不存在 (./setup.sh build)"
    done
    # 旧安装留下的同名程序: xinitrc 已把 ~/.local/bin 排在前面, 只提示
    for f in /usr/local/bin/dwm /usr/local/bin/picom; do
        [ -e "$f" ] && warn "  ! $f 是旧版本残留, ~/.local/bin 里的优先; 确认不用后可 sudo rm $f"
    done

    step "体检: 系统配置与服务"
    [ -f /etc/systemd/system/getty@tty1.service.d/autologin.conf ] && ok "tty1 自动登录" || bad "tty1 自动登录未配置 (./setup.sh system)"
    [ "$(getent passwd "$(id -un)" | cut -d: -f7)" = /usr/bin/zsh ] && ok "登录 shell 是 zsh" || bad "登录 shell 不是 zsh, ~/.zprofile 不会执行 startx (./setup.sh system)"
    locale -a 2>/dev/null | grep -qi '^zh_CN\.utf8$' && ok "zh_CN.UTF-8 locale" || bad "缺 zh_CN.UTF-8 locale (./setup.sh system)"
    f=/sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference
    if [ -f "$f" ]; then
        # 把当前值原样写回去 (不改变状态), 能免密写入说明 sudoers 规则在 (sudo -l 本身要密码, 不能用来判断)
        sudo -n tee "$f" < "$f" >/dev/null 2>&1 \
            && ok "省电模式免密 (sudoers)" || bad "省电模式写 EPP 需要免密 sudo (./setup.sh system)"
    fi
    if has_gpu nvidia; then
        [ -f /etc/modprobe.d/nvidia.conf ] && ok "NVIDIA modeset" || bad "缺 /etc/modprobe.d/nvidia.conf (./setup.sh system)"
    fi
    systemctl is-enabled numlock.service >/dev/null 2>&1 && ok "numlock.service" || bad "numlock.service 未启用 (./setup.sh system)"
    for f in NetworkManager bluetooth tailscaled; do
        systemctl is-enabled "$f.service" >/dev/null 2>&1 && ok "$f" || bad "$f 未启用 (./setup.sh services)"
    done
    systemctl --user is-enabled sni-watcher.service >/dev/null 2>&1 && ok "sni-watcher (用户服务)" || bad "sni-watcher 未启用 (./setup.sh services)"

    step "体检: 仓库"
    if git ls-files 2>/dev/null | grep -qE '(^|/)(local\.zsh|dwm2\.local)$' || git grep -qE 'sk-(or|ant)-[A-Za-z0-9]' -- . ':!setup.sh' 2>/dev/null; then
        bad "仓库里有本机私有文件或密钥, 不要提交"
    else
        ok "仓库里没有密钥和本机私有文件"
    fi

    echo
    [ "$fails" = 0 ] && printf '\033[1;32m全部通过\033[0m\n' || printf '\033[1;31m%d 项未通过\033[0m\n' "$fails"
}

case "${1:-all}" in
    all)       bootstrap; deps; system; link; build; services; check
               step "完成。重启后在 tty1 自动登录并进入桌面; 之后修改 dwm 配置按 Super + Shift + R 即可生效"
               echo "  还需手动完成的事见 docs/tutorials/01-install.md 第 4 节" ;;
    bootstrap) bootstrap ;;
    deps)      deps ;;
    system)    system ;;
    link)      link ;;
    build)     build ;;
    services)  services ;;
    check)     check ;;
    dwm)       build_dwm ;;
    st|tabbed) build_suckless "$1" ;;
    picom)     build_picom ;;
    i3lock)    build_i3lock ;;
    *)         sed -n '2,13p' "$0"; exit 1 ;;
esac

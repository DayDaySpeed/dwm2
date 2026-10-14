# 教程 1：从最小 Arch 到进入桌面

**你会做到：** 在一台新电脑上装好这套 dwm 桌面，开机自动进入。
**需要多久：** Arch 本身 30 分钟左右；`./setup.sh` 主要花在下载和编译上，网速正常时 20～40 分钟。
**前提：** 会按 [Arch Wiki 安装指南](https://wiki.archlinux.org/title/Installation_guide) 装一个最小系统。

---

## 1. 装一个最小 Arch

按 Arch Wiki 装好以下几样就够了，**不要装任何桌面环境**（GNOME、KDE 等），桌面所需的一切都由本仓库安装：

- 基本系统：`pacstrap /mnt base linux linux-firmware`
- 引导程序：GRUB 或 systemd-boot 都可以
- 联网工具：装机时用的 `iwd` / `networkmanager` 任一个都行，装好后 setup.sh 会统一换成 NetworkManager
- 一个普通用户，加入 `wheel` 组，并在 `visudo` 里允许 `wheel` 使用 sudo
- `sudo` 和文本编辑器（如 `vim`）

重启进入新系统，**用这个普通用户登录**，确认能联网（`ping archlinux.org`）。

> 用 linux-lts 等其他内核也可以：有 NVIDIA 显卡时，setup.sh 会按已装的内核自动补上对应的头文件包（如 `linux-lts-headers`）。

## 2. 克隆仓库并运行 setup.sh

```sh
sudo pacman -S --needed git
git clone https://github.com/DayDaySpeed/dwm2.git ~/projs/dwm2   # 放在哪都行
cd ~/projs/dwm2
./setup.sh
```

不想装快捷键对应的那些应用（Chrome、Edge、VSCode、Steam、OBS、SPlayer、Sunshine）时，改用：

```sh
./setup.sh --no-apps
```

之后想装时再运行一次 `./setup.sh deps` 即可。

### 运行过程中会遇到的提示

- **sudo 密码**：会问几次，输当前用户的密码。
- **pacman 的选择题**（"有 N 个包提供 xxx，选哪个"）：直接按回车用默认值。
- **yay 编译 AUR 包时**问要不要看 diff、要不要清理：按回车跳过。
- **壁纸子模块拉取失败**：只打印黄色警告，不会中断。原因见下面的"手动完成的事"。

### setup.sh 做了什么

`./setup.sh` 依次执行下面 7 步。每一步都能单独运行（如 `./setup.sh link`），也都能重复运行，已经做过的会跳过。

| 步骤 | 做什么 |
|---|---|
| `bootstrap` | 装 base-devel、git，开启 multilib 仓库（Steam 和 32 位 N 卡驱动在里面），没有 yay 时从 AUR 编译一个 |
| `deps` | 按 `packages/*.txt` 装包。`core` 是桌面必需的，`apps` 是快捷键对应的应用，再按检测到的显卡追加 `intel` / `amd` / `nvidia`；`-aur` 结尾的清单用 yay 装 |
| `system` | 写 `/etc`：tty1 自动登录、zh_CN.UTF-8 locale、省电模式免密 sudo、tty 下开 NumLock；有 N 卡时写 modeset 配置；把登录 shell 改成 zsh |
| `link` | 把 `~/.xinitrc`、`~/.zshrc`、`~/.config/rofi` 等链接到仓库；原来有内容不同的文件会先备份成 `<文件名>.bak.<时间>`。fcitx5、flameshot、GTK 这类程序自己会改写的配置，只在不存在时复制一份。最后拉壁纸子模块 |
| `build` | 编译安装 dwm 和 picom（装到 `~/.local/bin`），以及 st、tabbed、i3lock-color（装到 `/usr/local/bin`） |
| `services` | 启用 NetworkManager、bluetooth、tailscaled，以及用户服务 sni-watcher |
| `check` | 只读体检，见第 5 节 |

## 3. 重启

```sh
reboot
```

开机后会在 tty1 自动登录，然后自动进入桌面：

1. tty1 自动登录（`/etc/systemd/system/getty@tty1.service.d/autologin.conf`）
2. zsh 读 `~/.zprofile`，在 tty1 上执行 `startx`
3. `~/.xinitrc` 设置环境变量，启动 dwm
4. dwm 运行 `bin/autostart.sh`：壁纸、状态栏、输入法、通知、picom 等

看到顶部的状态栏和壁纸就成功了。接着看 [教程 2：快速上手](02-quickstart.md)。

## 4. setup.sh 做不了、要手动完成的事

| 事项 | 怎么做 |
|---|---|
| 壁纸 | `wallpaper/` 是私有子模块 `dwm2-wallpaper`。先把这台机器的 SSH 公钥加到 GitHub，再运行 `./setup.sh link`。没有权限时目录为空，可以用自己的图片，见 [教程 4：壁纸](04-wallpaper.md) |
| 本机配置 | `~/.config/dwm2.local`（第一次 `link` 时从 `config/dwm2.local.example` 复制）。笔记本要禁用触摸板时，把 `xinput list --name-only` 里触摸板名字的开头填进 `DISABLE_INPUT`，重新登录生效 |
| 显卡模式 | 有核显 + 独显的笔记本，"混合 / 独显直连"在 BIOS 里切换，见 [dwm 手册](../manual/dwm.md) 第 7 节 |
| 远程桌面 | `sudo tailscale up` 登录 Tailscale；第一次打开 Sunshine 要在网页里设用户名密码并配对；`bin/vnc.sh passwd` 设 VNC 密码。见 [远程桌面手册](../manual/remote-desktop.md) |
| 场景壁纸 | Steam 登录后订阅 Wallpaper Engine 壁纸，再运行 `bin/livewall.sh sync` |
| 输入法 | 已配好 fcitx5 + Rime 雾凇拼音（带万象语法模型），左右 Shift 切中英。第一次用时 Rime 要部署词库，稍等十几秒；设置在 `fcitx5-configtool` 里改，Rime 的方案配置在 `~/.local/share/fcitx5/rime/` |

## 5. 用 check 确认结果

```sh
./setup.sh check
```

它只检查不修改，每一项打 ✓ 或 ✗，✗ 后面会写怎么修，例如：

```
==> 体检: 系统配置与服务
  ✓ tty1 自动登录
  ✓ 登录 shell 是 zsh
  ✗ 省电模式写 EPP 需要免密 sudo (./setup.sh system)
```

按提示运行括号里的命令，再 check 一次，直到 `全部通过`。装完之后、更新了仓库之后、遇到问题时，都可以先跑一次 check。

## 6. 从旧电脑迁移

仓库里没有、需要从旧电脑带过来的只有本机私有文件：

- `~/.config/zsh/local.zsh`：密钥、个人工具路径（由 `~/.zshrc` 加载，不入库）
- `~/.config/dwm2.local`：本机配置（触摸板等）
- `~/.config/autorandr/`、`~/.config/dwm-scale/`：外接显示器的布局和缩放（不带也行，插上后重新选一次）

---

**下一步：** [教程 2：快速上手](02-quickstart.md)

# 教程 6：排错

**你会做到：** 出问题时知道先查什么、日志在哪；对照常见问题自己解决。
**前提：** 无。

---

## 1. 先跑 check

```sh
cd $DWM && ./setup.sh check
```

它会检查以下几项，有问题打 ✗，并写明修复命令：

- 包是否装全
- 配置链接是否正确
- 编译产物在不在
- 系统配置和服务

大部分"装完不对劲"的问题，按提示运行一两条命令就能解决。

在 tty 下（桌面没起来时）`$DWM` 还没设置，直接 `cd` 到仓库目录运行即可。

## 2. 日志在哪

| 日志 | 内容 |
|---|---|
| `~/.local/share/xorg/Xorg.0.log` | X 服务器本身：显卡驱动、显示器识别。startx 失败时先看这里 |
| `~/.cache/dwm-error.log` | dwm 运行中的 X 错误、闪退前的最后记录 |
| `~/.cache/dwm-build.log` | `Super + Shift + R` 的编译输出 |
| `~/.cache/dwm-galaxy.log`、`dwm-overview.log` | 星系、窗口总览的运行记录 |
| `~/.cache/livewall/transition.log` | 壁纸切换、转场出错 |
| `~/.cache/livewall/scene.log` | 场景壁纸（linux-wallpaperengine）的输出 |
| `~/.cache/x11vnc.log` | VNC |

用户服务（如 sni-watcher）的日志：`journalctl --user -u sni-watcher -e`。

## 3. 常见问题

### 开机停在 tty 登录界面，或登录后没进桌面

- **原因：** 下面三个环节有一个没生效：tty1 自动登录、登录 shell 是 zsh、`~/.zprofile` 执行 startx。
- **解决：**
  1. `./setup.sh check` 看"tty1 自动登录""登录 shell 是 zsh""~/.zprofile"这几项。
  2. 按提示运行 `./setup.sh system` 或 `./setup.sh link`。
  3. 手动 `startx` 能进桌面，就说明是自动登录这一环的问题。
  4. 手动 `startx` 也报错，看 `~/.local/share/xorg/Xorg.0.log`（通常是显卡驱动）。

### picom 的动画和配置不生效

- **原因：** 运行的不是仓库编译的 v13，而是旧安装残留的另一个 picom。
- **检查：** `readlink /proc/$(pgrep -x picom)/exe`，应该是 `~/.local/bin/picom`。
- **解决：**
  1. `config/xinitrc` 已经把 `~/.local/bin` 排在 PATH 最前，重新登录即可。
  2. `./setup.sh check` 会列出 `/usr/local/bin/picom` 这类残留，确认不用后删掉。

### Super + Shift + R 提示"当前运行的不是它"

- **原因：** 正在运行的 dwm 不是 `~/.local/bin/dwm`（比如是旧安装在 `/usr/local/bin` 的版本），收不到原地重启的信号。
- **解决：** `Super + Shift + Esc` 注销，重新登录一次。之后 `Super + Shift + R` 就正常了。

### 输入法打不出中文

- **解决：**
  1. `./setup.sh check` 确认输入法相关的包都装了：`fcitx5-rime`、`fcitx5-gtk`、`fcitx5-qt`，以及 AUR 的 `rime-ice-git`。
  2. 运行 `fcitx5-configtool`，确认输入法列表里有"中州韵"（Rime，默认），或者自带的"拼音"。
  3. 刚装好时 Rime 要部署词库，十几秒内打不出字属于正常。改了 `~/.local/share/fcitx5/rime/` 里的配置后，在输入法托盘菜单里选"重新部署"。
  4. 只有 Steam 打不出：用 `Super + 0` 启动（`bin/steam.sh` 会改用 XIM 协议），不要直接运行 `steam`。

### 外接屏文字发糊

- **原因：** 这是 X11 只有一个全局缩放带来的取舍，不是故障。
- **解决：** 把外接屏缩放设成和笔记本屏一样就清晰了。见 [教程 5 第 4 节](05-monitors.md#4-缩放和清晰度)。

### 鼠标移到另一块屏，键盘还在控制原来那块

- **原因：** 旧版本里，场景壁纸的窗口会吞掉鼠标事件。现在已改成鼠标事件直接穿过壁纸窗口。
- **解决：** 如果还遇到，左键点状态栏的菜单图标 `󰍜` 换一次壁纸，让新的场景窗口生效；也可以用 `Super + B` 直接切屏。

### 省电模式提示"CPU 节能未生效"

- **原因：** 没有配置免密 sudo 规则。
- **解决：** `./setup.sh system`。CPU 驱动不支持 EPP（如 `acpi-cpufreq`）的机器上，这一项会自动跳过，属于正常。

### Cursor、VSCode 等 Electron 程序闪退

- **原因：** dwm 没有系统托盘协议的 watcher。Electron 会退回旧的托盘实现，而它容易崩溃。
- **解决：** `bin/sni-watcher` 用户服务补上了这个 watcher。检查它是否在运行：`systemctl --user status sni-watcher`；没启用就运行 `./setup.sh services`。

### 星系（Super + Z）黑屏、卡顿或画面闪

- **检查：** `~/.cache/dwm-galaxy.log` 的最后几行。
- **解决：**
  - 画面太闪、不舒服：`touch ~/.cache/galaxy-gentle`，开启减弱动效。
  - 卡顿：开省电模式会进入安静模式（特效少、帧率低）。也可以在 `config/dwm.h` 末尾调星系参数，见 [星系手册](../manual/galaxy.md)。
  - 卡死时：按 `Esc` 或 `Super + Z` 都能退出。

### 状态栏某一块不显示或显示错误

- **检查：** `bash $DWM/bin/statusbar/packages/<模块>.sh`，直接运行看报错。例如 `cpu.sh` 在没有温度传感器的机器上不显示温度，属于正常；台式机没有电池，`bat.sh` 不显示。
- **解决：** 改完后 `Super + P` → `update statusbar`。

### 锁屏后无法解锁

- **原因：** 15 分钟内连续输错 3 次，系统会把账号锁定 10 分钟（faillock），锁定期间输对密码也提示错误。
- **解决：** 等 10 分钟；或按 `Ctrl + Alt + F2` 切到文本终端登录，执行 `faillock --reset`。见 [锁屏手册](../manual/lock-screenshot.md)。

## 4. 还是不行

- 把相关日志的最后几十行和 `./setup.sh check` 的输出一起记下来。
- `git log` 看最近改了什么。用 `git stash` 暂存本地改动，回到干净状态再试一次，就能判断问题是不是自己改出来的。

---

**下一步：** 想改 dwm 本身：[教程 7：二次开发](07-development.md)

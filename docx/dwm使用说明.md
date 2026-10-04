# dwm 使用说明

本文对应 `~/projs/dwm2` 当前的配置（`config/dwm.h`）。下文 **Super** 指 Win 键。

---

## 1. 基本概念

| 概念 | 说明 |
|---|---|
| **tag** | 相当于虚拟桌面。状态栏最左边的图标就是各个 tag，可以同时显示多个 tag |
| **平铺 / 浮动** | 默认平铺：窗口自动排列，不重叠。浮动窗口可以自由移动、调整大小，显示在平铺窗口之上 |
| **主工作区** | 平铺布局下左侧的大窗口区域，右侧是其余窗口组成的栈 |
| **全局窗口** | 在所有 tag 中都显示的窗口，比如 scratchpad 终端 |
| **scratchpad** | 一个随叫随到的浮动终端，按一次显示，再按一次隐藏 |
| **隐藏窗口** | 窗口被收起但没关闭，标题仍留在状态栏上，可以恢复 |

**两种布局：**

| 图标 | 布局 | 说明 |
|---|---|---|
| 󰙀 | tile（主次栈） | 默认。左边是主窗口，右边是其余窗口 |
| 󰕰 | magicgrid（网格） | 所有窗口平均排成网格 |

---

## 2. 快捷键

### 2.1 启动程序

| 按键 | 功能 |
|---|---|
| `Super + Enter` | 打开终端（st，由 tabbed 管理，支持多标签） |
| `Super + Space` | 打开浮动终端 |
| `Super + -` | 打开全局浮动终端（在所有 tag 显示） |
| `Super + S` | 显示 / 隐藏 scratchpad 终端（显示在屏幕顶部） |
| `Super + D` | rofi：启动应用（列出已安装的应用，带图标） |
| `Super + Shift + D` | rofi：运行命令（输入命令名） |
| `Super + P` | rofi 自定义菜单：切换动态 / 静态壁纸、开关省电模式、勿扰模式、暂停自动锁屏、护眼、刷新状态栏、开关 daed、开关 picom |
| `Super + R` | 打开 / 关闭 pcmanfm 文件管理器（浮动在屏幕中央） |
| `Super + Shift + A` | 截图（flameshot），框选后松开鼠标即复制到剪贴板（不保存文件） |
| `Super + Ctrl + L` | 锁屏（模糊锁屏，输入密码解锁） |
| `Super + Y` | 翻译鼠标选中的文字，译文显示在右上角通知里（含汉字 → 英文，否则 → 中文） |
| `Super + Shift + Y` | 弹出输入框，输入文字后回车翻译，译文自动复制到剪贴板 |
| `Super + X` | 剪贴板历史：列出最近复制过的内容，选中的放回剪贴板（需安装 clipmenu） |
| `Super + N` | 重新显示上一条通知（错过的通知可以连按逐条找回） |
| `Super + Shift + N` | 关闭所有通知 |
| `Super + /` | 快捷键速查：列出所有快捷键和鼠标操作，可输入文字搜索 |

### 2.2 窗口焦点与切换

| 按键 | 功能 |
|---|---|
| `Super + Tab` / `Super + ↓` | 聚焦下一个窗口（有浮动窗口时只在浮动窗口之间切换） |
| `Super + Shift + Tab` / `Super + ↑` | 聚焦上一个窗口 |
| `Super + H / J / K / L` | 按方向聚焦：左 / 下 / 上 / 右 |
| `Super + Shift + H / J / K / L` | 与该方向的窗口交换位置（仅平铺窗口） |
| `Super + Shift + Enter` | 把当前窗口设为主窗口 |
| `Super + A` | overview：预览所有窗口 |

### 2.3 窗口状态

| 按键 | 功能 |
|---|---|
| `Super + Q` | 关闭窗口 |
| `Super + Ctrl + Q` | 强制关闭窗口（用于关不掉的窗口） |
| `Super + T` | 当前窗口在浮动和平铺之间切换 |
| `Super + Shift + T` | 所有窗口在浮动和平铺之间切换 |
| `Super + F` | 全屏 / 取消全屏 |
| `Super + G` | 当前窗口在全局（所有 tag 显示）和普通之间切换 |
| `Super + U` | 显示 / 隐藏边框 |
| `Super + I` | 隐藏当前窗口 |
| `Super + Shift + I` | 恢复最后隐藏的窗口；想恢复其他隐藏窗口，按 `Super + A` 在预览里点它 |
| `Super + O` | 只显示当前窗口 ↔ 显示全部窗口 |

### 2.4 移动与调整窗口（键盘）

| 按键 | 功能 |
|---|---|
| `Super + Ctrl + ↑ ↓ ← →` | 移动窗口（与其他窗口之间有吸附效果） |
| `Super + Alt + ↑ ↓ ← →` | 调整窗口大小 |

### 2.5 布局与间距

| 按键 | 功能 |
|---|---|
| `Super + Shift + Space` | 在网格布局和平铺布局之间切换 |
| `Super + ,` / `Super + .` | 缩小 / 放大主工作区 |
| `Super + E` | 主工作区的窗口数在 1 和 2 之间切换 |
| `Super + Ctrl + =` | 缩小窗口间隙（窗口变大） |
| `Super + Ctrl + -` | 增大窗口间隙（窗口变小） |
| `Super + Ctrl + Space` | 窗口间隙恢复默认 |

### 2.6 tag

| 按键 | 功能 |
|---|---|
| `Super + ←` / `Super + →` | 切换到左边 / 右边的 tag |
| ``Super + ` `` | 在最近两个 tag 之间来回切换（比如在 1 和 3 之间：按一下回到上一个 tag，再按一下切回来） |
| `Super + Shift + ←` / `Super + Shift + →` | 把当前窗口移到左边 / 右边的 tag |
| `Super + 对应键` | 跳到对应的 tag（见下表）。如果该 tag 没有窗口，会自动启动对应的程序 |
| `Super + Shift + 对应键` | 把当前窗口移到对应的 tag |
| `Super + Ctrl + 对应键` | 在当前画面中叠加显示 / 取消显示对应的 tag |

| 键 | tag | 用途 | 没有窗口时自动启动 |
|---|---|---|---|
| `1` |  | 终端 1 | — |
| `2` | 󰎧 | 终端 2 | — |
| `3` | 󰎪 | 终端 3 | — |
| `9` | 󰕧 | OBS | `obs` |
| `C` |  | 浏览器 | `google-chrome-stable` |
| `M` | 󰎄 | 音乐 | SPlayer（`~/.local/share/splayer/SPlayer.AppImage`） |
| `0` |  | Steam | `steam`（使用 `~/.local/bin/steam`，已修复 Steam 中的输入法） |
| `W` | 󰇩 | Edge 浏览器 | `microsoft-edge-stable` |
| `V` | 󰨞 | VS Code | `code` |

### 2.7 其他

| 按键 | 功能 |
|---|---|
| `Super + =` | 显示 / 隐藏系统托盘 |
| `Super + Shift + F` | 显示 / 隐藏状态栏 |
| `Super + B` | 焦点移到另一个显示器 |
| `Super + Shift + B` | 把当前窗口移到另一个显示器 |
| `Super + Shift + ↑` / `↓` | 音量加 / 减（以 5% 为步长） |
| `Super + Shift + R` | **让配置生效**：自动编译安装 dwm 并原地重启。所有窗口、所在 tag、先后顺序、浮动 / 隐藏状态和当前查看的 tag 都保留；编译失败会弹通知，当前的 dwm 继续运行 |
| `Super + Shift + Esc` | 电源菜单：锁屏、睡眠、休眠、注销（退出 dwm）、重启、关机。注销 / 重启 / 关机会再确认一次 |
| 媒体键（播放 / 上一首 / 下一首） | 控制正在播放的音乐或视频（`playerctl`）。音量键和亮度键由笔记本固件处理 |

---

## 3. 鼠标操作

| 位置 | 操作 | 功能 |
|---|---|---|
| 窗口上 | `Super + 左键拖动` | 移动窗口（平铺窗口拖动后会变成浮动窗口，按 `Super + T` 可放回平铺） |
| 窗口上 | `Super + 右键拖动` | 调整窗口大小 |
| 任意位置 | `Super + 滚轮` | 切换 tag |
| tag 图标 | 左键 | 切换到该 tag |
| tag 图标 | 右键 | 叠加显示 / 取消显示该 tag |
| tag 图标 | `Super + 左键` | 把当前窗口移到该 tag |
| tag 图标 | 滚轮 | 切换 tag |
| 窗口标题 | 左键 | 只保留这个窗口，隐藏其他窗口 |
| 窗口标题 | 右键 | 隐藏 / 显示这个窗口 |
| 状态栏空白处 | 左键 | rofi：切换窗口 |
| 状态栏空白处 | 右键 | rofi：启动应用 |

---

## 4. 状态栏

状态栏右侧从左到右依次是：菜单图标、音乐、Wi-Fi、CPU、内存、日期、音量、电池。每个模块都可以点击：

| 模块 | 左键 | 中键 | 右键 | 滚轮 |
|---|---|---|---|---|
| 菜单图标 | 显示状态通知，并随机换一张壁纸（动态模式下换一个视频） | — | 电源菜单：关机 / 重启 / 休眠 / 锁定 | — |
| 音乐 | 播放 / 暂停 | — | 播放 / 暂停 | 上一首 / 下一首 |
| Wi-Fi | 通知当前网络 | — | 弹出 `nmtui-connect` 连接网络 | — |
| CPU | 通知 CPU 占用 | — | 弹出 `btop` | — |
| 内存 | 通知内存用量 | — | 弹出 `btop` | — |
| 日期 | 通知日期和待办 | — | 用 nvim 编辑 `~/.todo.md` 待办 | — |
| 音量 | 通知音量 | 静音 / 取消静音 | 打开 / 关闭 pavucontrol | 音量 ±5% |
| 电池 | 通知电量和剩余时间 | — | 打开 / 关闭电源管理设置 | — |

弹出的 btop、nmtui 等小窗口再点一次同一个位置就会关闭。

**状态栏脚本命令**（位于 `bin/statusbar/statusbar.sh`）：

```sh
$DWM/bin/statusbar/statusbar.sh updateall     # 立即刷新所有模块
$DWM/bin/statusbar/statusbar.sh update cpu    # 只刷新某个模块
$DWM/bin/statusbar/statusbar.sh check         # 检查各模块是否正常
```

---

## 5. 让窗口按指定方式打开

启动程序时指定窗口的 class，可以直接让窗口以浮动、全局等状态打开。例如 `st -c float`：

| class | 效果 |
|---|---|
| `float` | 浮动 |
| `global` | 全局 |
| `noborder` | 无边框 |
| `FG` | 浮动 + 全局 |
| `FN` | 浮动 + 无边框 |
| `GN` | 全局 + 无边框 |
| `FGN` | 浮动 + 全局 + 无边框 |

以下程序已经有固定规则：

- **截图、文件管理器**：flameshot、pcmanfm 自动浮动。
- **图片查看和保存对话框**：QQ、微信、Telegram 的图片查看器，以及浏览器的「保存文件」对话框，都会浮动。
- **自动放到指定 tag**：Chrome 放到浏览器 tag，OBS 放到 OBS tag，Steam 放到 Steam tag，Edge 放到 Edge tag，VS Code 放到 VS Code tag。

如果想给其他程序加规则，在 `config/dwm.h` 的 `rules[]` 里添加，添加后要重新编译。

---

## 6. 修改配置

所有配置都在 `~/projs/dwm2` 中：

| 想改什么 | 改哪个文件 | 改完后 |
|---|---|---|
| 快捷键、tag、窗口规则、颜色、间距 | `config/dwm.h` | 按 `Super + Shift + R` |
| 终端字体、透明度 | `config/st.h` | `./setup.sh st` |
| 终端标签栏 | `config/tabbed.h` | `./setup.sh tabbed` |
| 开机启动的程序 | `bin/autostart.sh` | 下次登录生效 |
| 环境变量、缩放、输入法 | `config/xinitrc` | 下次登录生效 |
| 窗口动画、透明、圆角、模糊 | `config/picom.conf` | `Super + P` 关闭 picom 再打开 |
| 通知样式 | `config/dunst.conf` | `killall dunst; dunst -conf $DWM/config/dunst.conf &` |
| rofi 外观 | `config/rofi.rasi` | 下次打开 rofi 时生效 |
| 状态栏模块 | `bin/statusbar/packages/*.sh` | 立即生效（下次刷新时） |
| 壁纸 | 图片放进 `wallpaper/static/`，视频放进 `wallpaper/live/`；Steam 上订阅的 Wallpaper Engine 壁纸执行 `bin/livewall.sh sync`（或重新登录）复制进 `wallpaper/live/`，之后可以在 Steam 取消订阅；不想要的直接从 `wallpaper/live/` 删掉 | 静态壁纸每 5 分钟随机更换；`Super + P` → switch wallpaper mode to live / static 在动态 / 静态之间切换（各自随机挑一个，选择会记住）；左键点状态栏的菜单图标立即换一张（动态模式下换一个）；所有壁纸切换（动态、静态、每 5 分钟换图）都随机播放一种 GPU 转场特效（`config/transitions/`）；在 `bin/livewall.sh` 顶部改 `TRANSITION=` 可固定某一种或改成 `fade`（GPU 淡入），`TRANSITION_SAVE=` 单独设置省电模式下的转场 |

**让 dwm 的改动生效：** 改完 `config/dwm.h` 直接按 `Super + Shift + R`，会自动编译、安装到 `~/.local/bin/dwm` 并原地重启，窗口都会保留，不需要 sudo，也不需要退出。编译日志在 `~/.cache/dwm-build.log`。
只有改了 `config/xinitrc` 或 `bin/autostart.sh`（它们只在登录时执行）才需要 `Super + Shift + Esc` → 注销，再重新 `startx`。

> 改 dwm 本身的功能直接修改 `dwm/` 下的源码（如 `dwm/dwm.c`），同样按 `Super + Shift + R` 生效。

---

## 7. 需要另外安装才能用的功能

| 功能 | 需要安装 |
|---|---|
| `Super + M` 音乐播放器 | SPlayer 的 AppImage（`./setup.sh deps` 会自动下载，无需安装） |
| `Super + R` 文件管理器 | `pcmanfm` |
| 使用键盘时暂时禁用触控板 | `xf86-input-synaptics`（autostart 已写好 `syndaemon`，装好后自动生效） |
| `Super + Y` 翻译 | `translate-shell`（需要联网，使用谷歌翻译等在线服务） |
| `Super + X` 剪贴板历史 | `clipmenu`（装好后重新登录生效；没装时用原来的 parcellite） |
| 护眼 | `gammastep` |
| 外接显示器自动布局 | `autorandr`：接好显示器、用 `xrandr` 调好后执行 `autorandr --save <名字>` 保存，以后插上会自动应用（登录时也会应用）；想插拔时立即生效，再执行 `sudo systemctl enable autorandr`。注意：切换屏幕布局时场景壁纸可能崩溃，按 `Super + P` 重新切一次壁纸即可 |

**Super + P 菜单里的其他开关：**

- **do not disturb（勿扰）**：暂停弹出通知，远程桌面、开会、录屏时用；关闭后，期间收到的通知会补弹出来。
- **pause / resume auto lock（暂停自动锁屏）**：看视频、演示时不会自动锁屏和熄屏；恢复后回到正常的时间（见「锁屏与截图使用说明」）。
- **night light（护眼）**：屏幕调成 4000K 暖色，晚上看着不刺眼；开着时重启后依然有效。需要安装 `gammastep`。

**电量低提醒：** 用电池时，电量降到 20% 弹提醒，10% 弹紧急提醒，5% 时 30 秒后自动睡眠（期间插上电源会取消）。每一档只提醒一次。

**省电模式（`Super + P` → open / close power save）：**

- 开启时：壁纸带转场切到静态（停掉视频和场景渲染）、屏幕刷新率降到 60Hz、CPU 节能策略（EPP）设为 `power`；关闭时全部恢复成开启前的样子。开着省电模式重启后依然有效。
- 拔掉电源时自动开启，插上电源时自动关闭（每 10 秒检测一次）。只会自动关闭拔电时自动开的那次；在 `Super + P` 里手动开的不会被插电关掉。
- 独显直连模式下（BIOS 里的「独显直连 / MUX」），独显一直开着，大约 25W，开启省电时会弹通知提醒。想要真正省电，要在 BIOS 里切回混合模式。
- CPU 节能策略要写 `/sys`，需要 root。配置一次 sudo 免密后才会生效，否则只跳过这一项（会弹通知提示）：

```sh
# 先写到临时文件, visudo 检查语法无误后再装进 /etc/sudoers.d (sudoers 写错会导致 sudo 用不了)
echo "$USER ALL=(root) NOPASSWD: /usr/bin/tee /sys/devices/system/cpu/cpu*/cpufreq/energy_performance_preference" > /tmp/powersave
sudo visudo -cf /tmp/powersave && sudo install -m 440 -o root -g root /tmp/powersave /etc/sudoers.d/powersave
```

**显卡模式（混合 / 独显直连）：**

这台笔记本（HP OMEN 16）有核显和独显（RTX 4070），可以在 BIOS 里切换屏幕由谁驱动。

| | 混合模式（默认） | 独显直连 |
|---|---|---|
| 屏幕、picom 由谁渲染 | 核显 | 独显 |
| 动态视频壁纸 | 独显渲染后拷到核显显示 | 独显 |
| 场景壁纸 | 只能用核显，30 帧 | 独显，60 帧 |
| 省电模式 | 有效：独显可以完全休眠 | 效果打折：独显一直通电，电池续航明显变差 |

- 两种模式共用一套 xorg 配置：`/etc/X11/xorg.conf.d/` 下不放显卡配置，由 Xorg 自动识别，加上 nvidia 驱动自带的 `/usr/share/X11/xorg.conf.d/10-nvidia-drm-outputclass.conf`。原来的 `10-intel.conf`、`10-nvidia.conf` 备份在 `/etc/X11/xorg.conf.d.bak/`。
- **切换方法**：重启时按 `F10` 进 BIOS，在显卡相关选项（Graphics Switching / Display Mode 之类）里选「独显 / Discrete」或「混合 / Hybrid」，保存重启，正常 `startx` 即可。`bin/livewall.sh` 会自动识别当前模式。
- **黑屏恢复**：按 `Ctrl + Alt + F2` 进 tty 登录，执行 `sudo mv /etc/X11/xorg.conf.d.bak/* /etc/X11/xorg.conf.d/` 恢复旧配置后重启；或者进 BIOS 切回混合模式。

安装命令：

```sh
sudo pacman -S pcmanfm clipmenu gammastep autorandr
```

**音乐播放器（SPlayer）：**

- 按 `Super + M` 打开 SPlayer，窗口自动放在音乐 tag。它是网易云风格的第三方客户端（[imsyy/SPlayer](https://github.com/imsyy/SPlayer)），本地音乐和网易云在线歌单都能播放。
- **播放本地歌曲**：左侧进入「本地音乐」，添加 `~/Music` 文件夹即可扫描；支持按歌手 / 专辑分类、编辑歌曲标签和封面。
- **在线**：登录网易云账号后可同步歌单、每日推荐；设置里也可以切换为纯本地模式（不联网）。
- **歌曲没有封面**：封面要嵌在音乐文件里才能显示。执行 `$DWM/scripts/music_covers.py`，会给 `~/Music` 里没有封面的歌自动从网易云搜索并写入专辑封面（按歌手 + 歌名匹配，对不上的跳过；音频内容不变）。可先加 `--dry-run` 只查看匹配结果。写入后在 SPlayer 里重新扫描本地音乐即可。
- 状态栏的音乐模块通过 `playerctl` 显示并控制正在播放的音乐：SPlayer、浏览器里的音视频、Spotify 等都支持（正在播放的优先，音乐软件优先于浏览器）。

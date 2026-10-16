# rofi 使用说明

rofi 是启动器和菜单：用来启动程序、切换窗口，也被用作 dwm 里各种弹出菜单的界面。

- 主题：`config/rofi/config.rasi`（`~/.config/rofi` 整个目录软链接到 `config/rofi/`，所有打开 rofi 的地方都使用这个主题）。样式来自开源合集 [adi1090x/rofi](https://github.com/adi1090x/rofi) 的 type-1/style-5
- 翻译输入框单独使用 `config/rofi/input.rasi`（单行、大字号）
- 自定义菜单脚本：`bin/rofi.sh`

---

## 1. 打开方式

| 操作 | 打开的内容 |
|---|---|
| `Super + D` | 启动应用（列出已安装的桌面应用） |
| `Super + Shift + D` | 运行命令（输入命令名后回车执行） |
| `Super + X` | 剪贴板历史（clipmenu） |
| `Super + /` | 快捷键速查 |
| `Super + Shift + Esc` | 电源菜单 |
| 左键点状态栏空白处 | 切换窗口（列出所有打开的窗口） |
| 右键点状态栏空白处 | 启动应用（列出已安装的桌面应用） |
| `Super + P` | 自定义菜单（见下文） |
| 右键点状态栏最左侧的菜单图标 | 电源菜单：关机 / 重启 / 休眠 / 锁定 |

---

## 2. 在 rofi 中的操作

| 按键 | 功能 |
|---|---|
| 直接输入 | 过滤列表（支持模糊匹配） |
| `↑` / `↓` 或 `Ctrl + P` / `Ctrl + N` | 上下选择 |
| `Tab` | 选择下一项 |
| `Enter` | 确定 |
| `Esc` | 关闭 rofi |

---

## 3. 自定义菜单（`Super + P`）

| 菜单项 | 作用 |
|---|---|
| ` switch wallpaper mode to live / static` | 动态 / 静态壁纸切换（见 [壁纸教程](../tutorials/04-wallpaper.md)） |
| `󰌪 open / close power save` | 省电模式：静态壁纸 + 60Hz + CPU 节能，拔电时自动打开（见 [dwm 手册](dwm.md)） |
| `󰂛 open / close do not disturb` | 勿扰：暂停显示通知 |
| `󰌿 pause auto lock` / `󰌾 resume auto lock` | 暂停 / 恢复自动锁屏和熄屏（看视频、演示时用） |
| `󰖔 open / close night light` | 护眼暖色（gammastep） |
| `󰕞 update statusbar` | 立即刷新状态栏 |
| ` open daed` / ` close daed` | 启动 / 停止 daed 代理服务。需要先安装 daed，并给 `systemctl` 配置免密 sudo，否则从菜单点击没有反应 |
| `󰍹 monitor layout` | 显示器设置。接了外接屏时先是布局：外接屏在右侧 / 左侧 / 上方 / 下方、复制、仅外接屏、仅笔记本，下面每块屏一行；点进某块屏可以设分辨率、刷新率、旋转（左 / 右 90°、180°）、镜像翻转、缩放，外接屏还能设位置（相对笔记本屏的方向、复制、关闭），支持多块外接屏。没接外接屏时直接是笔记本屏的这些设置。缩放：笔记本屏默认按分辨率取 100% 或 135%，外接屏默认 100%；几块屏同时用时，其它屏按放大后的画面渲染再缩小显示，看起来就是它自己的缩放（文字比原生略软，详见 [多显示器教程](../tutorials/05-monitors.md)）；改了全局缩放后状态栏自动刷新，已打开的程序重开后生效。设置按显示器记住，下次插上同样的几块屏自动沿用；拔掉外接屏时自动点亮笔记本屏。子菜单里按 `Esc` 或选 `↩ 返回` 回到上一层，在 Super+P 主菜单按 `Esc` 才是关闭 |
| ` open picom` / ` close picom` | 打开 / 关闭 picom |
| `󱒃 open sunshine` / `󱒃 close sunshine` | 开启 / 关闭 Sunshine 远程桌面（见 [远程桌面使用说明](remote-desktop.md)） |
| `󰢹 open vnc` / `󰢹 close vnc` | 开启 / 关闭远程桌面（见 [远程桌面使用说明](remote-desktop.md)） |

**添加菜单项：** 编辑 `bin/rofi.sh`。

1. 在 `call_menu` 函数里用 `echo` 加一行菜单文字。
2. 在 `execute_menu` 函数的 `case` 里加上同样的文字，并写上要执行的命令。

改完立即生效，不需要编译。

---

## 4. 修改外观（`config/rofi/config.rasi`）

**换配色**：修改文件开头的 `@import "colors/onedark.rasi"`，把 `onedark` 换成 `config/rofi/colors/` 里的其他配色，保存后下次打开 rofi 就生效。可选的配色有：

`adapta` `arc` `black` `catppuccin` `cyberpunk` `dracula` `everforest` `gruvbox` `lovelace` `navy` `nord` `onedark` `paper` `solarized` `tokyonight` `yousai`

| 想改什么 | 改哪一项 |
|---|---|
| 字体和字号 | `font: "JetBrainsMono Nerd Font 13";` |
| 窗口宽度 | `window` 里的 `width: 800px;` |
| 显示几行 | `listview` 里的 `lines` |
| 各模式按钮的图标 | 开头 `configuration` 里的 `display-drun`、`display-run` 等 |

想换成合集里的其他样式，可以到 adi1090x/rofi 仓库的 `files/launchers/` 目录下挑选。

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
| ` set wallpaper` | 从 `wallpaper/` 里随机换一张壁纸 |
| `󰕞 update statusbar` | 立即刷新状态栏 |
| ` open daed` / ` close daed` | 启动 / 停止 daed 代理服务。需要先安装 daed，并给 `systemctl` 配置免密 sudo，否则从菜单点击没有反应 |
| ` open picom` / ` close picom` | 打开 / 关闭 picom |
| `󱒃 open sunshine` / `󱒃 close sunshine` | 开启 / 关闭 Sunshine 远程桌面（见 [远程桌面使用说明](远程桌面使用说明.md)） |
| `󰢹 open vnc` / `󰢹 close vnc` | 开启 / 关闭远程桌面（见 [远程桌面使用说明](远程桌面使用说明.md)） |

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

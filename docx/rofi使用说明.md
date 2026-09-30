# rofi 使用说明

rofi 是启动器和菜单：用来启动程序、切换窗口，也被用作 dwm 里各种弹出菜单的界面。

- 主题：`config/rofi.rasi`（`~/.config/rofi/config.rasi` 软链接到这里，所有打开 rofi 的地方都使用这个主题）
- 自定义菜单脚本：`bin/rofi.sh`

---

## 1. 打开方式

| 操作 | 打开的内容 |
|---|---|
| `Super + D` | 运行命令（输入命令名后回车执行） |
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

**添加菜单项：** 编辑 `bin/rofi.sh`。

1. 在 `call_menu` 函数里用 `echo` 加一行菜单文字。
2. 在 `execute_menu` 函数的 `case` 里加上同样的文字，并写上要执行的命令。

改完立即生效，不需要编译。

---

## 4. 修改外观（`config/rofi.rasi`）

| 想改什么 | 改哪一项 |
|---|---|
| 窗口大小 | `*` 里的 `width: 680px;` `height: 300px;` |
| 背景颜色和透明度 | `transparent: rgba(34,62,79,0.80);`（最后一个数是不透明度） |
| 选中项的颜色 | `transparent-light: rgba(34,82,99);` |
| 字体和字号 | `font: "JetBrainsMono Nerd Font Mono 12.5";` |
| 文字颜色 | `text-color: #f1f1f1;` |
| 显示应用图标 | `configuration` 里的 `show-icons: false;` 改成 `true` |
| 各个模式前的提示图标 | `configuration` 里的 `display-drun`、`display-window`、`display-run` |

保存后，下次打开 rofi 就会生效。

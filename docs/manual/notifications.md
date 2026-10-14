# 通知使用说明（dunst）

dunst 负责显示桌面通知，比如状态栏点击后弹出的 CPU、内存、音量信息，以及其他程序发出的通知。

- 配置文件：`config/dunst.conf`
- 由 `bin/autostart.sh` 开机启动：`dunst -conf $DWM/config/dunst.conf`

---

## 1. 通知的样子

| 项目 | 设置 |
|---|---|
| 位置 | 屏幕右上角，状态栏下方 |
| 宽度 | 最宽 300 像素 |
| 外观 | 圆角 10，背景 `#335566`，字体 JetBrainsMono Nerd Font 9.5 |
| 普通通知 | 显示 10 秒后自动消失 |
| 低优先级通知 | 显示 5 秒后自动消失 |
| 紧急通知 | 红色背景，不会自动消失，要手动关掉 |
| 重复通知 | 自动合并成一条 |
| 离开电脑时 | 超过 2 分钟没有操作，通知不会自动消失，等你回来再看 |

---

## 2. 鼠标操作

| 操作 | 功能 |
|---|---|
| 左键点击通知 | 关闭这一条 |
| 中键点击通知 | 执行通知自带的动作（比如打开对应程序），然后关闭 |
| 右键点击通知 | 关闭所有通知 |

---

## 3. 键盘操作

dunst 1.7 起不再支持在配置文件里设置键盘快捷键，作者配置中的 `[shortcuts]` 段已删除（同时清理了其他已废弃的选项，现在启动时没有任何警告）。

需要用键盘操作时，可以在终端里用 `dunstctl`：

| 命令 | 功能 |
|---|---|
| `dunstctl close` | 关闭最新一条通知 |
| `dunstctl close-all` | 关闭所有通知 |
| `dunstctl history-pop` | 重新显示最近关闭的一条通知（最多保留 20 条历史） |
| `dunstctl context` | 打开通知的动作菜单 |
| `dunstctl set-paused toggle` | 勿扰模式：暂停 / 恢复显示通知 |

如果想用快捷键执行这些命令，可以在 `config/dwm.h` 的 `keys[]` 里添加，比如：

```c
{ MODKEY|Mod1Mask,     XK_space,  spawn, SHCMD("dunstctl close-all") },
```

---

## 4. 修改配置

编辑 `config/dunst.conf`，常改的几项：

| 想改什么 | 改哪一项 |
|---|---|
| 显示位置 | `origin = top-right`、`offset = 6x36` |
| 宽度 | `width = (0, 300)` |
| 显示时长 | 各个 `[urgency_*]` 段里的 `timeout`（单位是秒，0 表示不自动消失） |
| 颜色 | 各个 `[urgency_*]` 段里的 `background`、`foreground` |
| 字体 | `font = JetBrainsMono Nerd Font 9.5` |

改完后重启 dunst 才会生效：

```sh
killall dunst; dunst -conf $DWM/config/dunst.conf &
```

**测试通知：**

```sh
notify-send "标题" "内容"
notify-send -u critical "紧急" "这条不会自动消失"
```

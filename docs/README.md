# dwm2 文档

文档分三类：

- **教程**：按步骤带你做完一件事，第一次用按顺序读。
- **手册**：某个组件的完整说明，用的时候查。
- **开发**：改源码和脚本的人看。

## 我想……

| 我想 | 看这里 |
|---|---|
| 在新电脑上装好这套桌面 | [教程 1：安装](tutorials/01-install.md) |
| 第一次进桌面，不知道怎么操作 | [教程 2：快速上手](tutorials/02-quickstart.md) |
| 改快捷键、窗口规则、状态栏 | [教程 3：自定义](tutorials/03-customize.md) |
| 换壁纸、用动态壁纸 | [教程 4：壁纸](tutorials/04-wallpaper.md) |
| 接外接显示器、调缩放 | [教程 5：多显示器](tutorials/05-monitors.md) |
| 出了问题 | [教程 6：排错](tutorials/06-troubleshooting.md) |
| 改 dwm 源码、加功能 | [教程 7：二次开发](tutorials/07-development.md) |
| 查某个快捷键 | 桌面里按 `Super + /`，或看 [dwm 手册](manual/dwm.md) |

## 教程

| 教程 | 内容 |
|---|---|
| [1. 安装](tutorials/01-install.md) | 最小 Arch → `./setup.sh` → 重启进桌面；手动项；`check` 体检；从旧电脑迁移 |
| [2. 快速上手](tutorials/02-quickstart.md) | tag、平铺、浮动；跟着做一遍最常用的操作 |
| [3. 自定义](tutorials/03-customize.md) | 加快捷键、窗口规则、状态栏模块；本机配置；往安装清单加软件 |
| [4. 壁纸](tutorials/04-wallpaper.md) | 静态 / 视频 / 场景壁纸，加自己的文件，导入 Wallpaper Engine，转场 |
| [5. 多显示器](tutorials/05-monitors.md) | 布局、分辨率、缩放；焦点规则；外接屏清晰度的取舍 |
| [6. 排错](tutorials/06-troubleshooting.md) | check、日志位置、常见问题 |
| [7. 二次开发](tutorials/07-development.md) | 启动链、仓库分工、改 dwm 源码、星系测试会话、扩展点 |

## 手册

| 手册 | 内容 |
|---|---|
| [dwm](manual/dwm.md) | 窗口管理器：快捷键、鼠标、tag、状态栏、窗口规则、修改配置、省电与显卡模式 |
| [星系](manual/galaxy.md) | Super+Z 3D 工作空间星系：操作、天象、减弱动效、参数 |
| [终端](manual/terminal.md) | st + tabbed：标签、复制粘贴、滚动缩放；ranger 文件管理器 |
| [picom](manual/picom.md) | 透明、圆角、模糊、动画的开关和调整 |
| [rofi](manual/rofi.md) | 启动器、切换窗口、Super+P 菜单、外观 |
| [通知](manual/notifications.md) | dunst：通知操作、dunstctl、修改样式 |
| [锁屏与截图](manual/lock-screenshot.md) | 锁屏、自动锁屏、解锁，flameshot 截图 |
| [远程桌面](manual/remote-desktop.md) | 从 Windows 远程操作：Sunshine + Moonlight（主），x11vnc（备用） |

## 最常用的按键

| 按键 | 功能 |
|---|---|
| `Super + Enter` | 打开终端 |
| `Super + D` | 启动应用 |
| `Super + Q` | 关闭窗口 |
| `Super + 1 / 2 / 3` | 切换 tag |
| `Super + Z` | 3D 工作空间星系 / 回到桌面 |
| `Super + A` | 窗口总览 |
| `Super + P` | 设置菜单（壁纸、省电、勿扰、护眼、显示器…） |
| `Super + Shift + A` | 截图 |
| `Super + Ctrl + L` | 锁屏 |
| `Super + Shift + R` | 让 dwm 配置生效（自动编译并原地重启，窗口保留） |
| `Super + Shift + Esc` | 电源菜单（睡眠、注销、重启、关机） |
| `Super + /` | 快捷键速查 |

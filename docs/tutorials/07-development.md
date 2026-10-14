# 教程 7：二次开发

**你会做到：** 了解从开机到桌面的启动链和仓库的分工；能修改 dwm 源码并安全地测试；知道 picom、i3lock-color 的改动在哪；提交前做好检查。
**前提：** 会 C 和 shell；读过 [教程 3：自定义](03-customize.md)。

---

## 1. 启动链

```
开机
 └─ tty1 自动登录           /etc/systemd/system/getty@tty1.service.d/autologin.conf  (system/getty-autologin.conf)
     └─ zsh 读 ~/.zprofile   -> home/zprofile: 在 tty1 上执行 startx
         └─ ~/.xinitrc       -> config/xinitrc
             ├─ DWM=仓库目录 (由 ~/.xinitrc 链接反推), PATH 前置 ~/.local/bin
             ├─ 读 ~/.config/dwm2.local (本机配置)
             ├─ 输入法环境变量, xrdb 读 config/Xresources
             ├─ bin/monitor.sh login: 套用保存的显示器布局, 按布局定 Xft.dpi 和 Qt 缩放
             ├─ 把环境变量导入 systemd 用户会话, 启动 portal 和 polkit 认证代理
             └─ exec ~/.local/bin/dwm
                 └─ bin/autostart.sh (dwm 启动时调用, 原地重启后也会再调用)
                     ├─ settings: 显示器布局, 刷新率, 壁纸, 护眼, NumLock, 禁用触摸板
                     ├─ daemons:  状态栏, 自动锁屏 (xss-lock + dpms.sh), fcitx5, flameshot, 剪贴板, dunst, picom
                     └─ cron:     每 10 秒: 插拔检测, 拔电省电; 每 60 秒: 电量提醒; 每 5 分钟: 换静态壁纸
```

`~/.config/dwm2` 也链接到仓库根目录，给 `dunst.conf`、`sni-watcher.service` 这类不能用环境变量的地方使用。

## 2. 仓库分工

| 目录 | 放什么 | 改完怎么生效 |
|---|---|---|
| `dwm/` | dwm 源码（基于 yaocccc/dwm，改动很多）；`galaxy*.c` 是星系，`overview.c` 是窗口总览 | `Super + Shift + R` |
| `config/` | 各程序的配置；`dwm.h`、`st.h`、`tabbed.h` 在编译时被链接成对应源码目录里的 `config.h` | 见 [教程 3](03-customize.md#4-不用编译的配置) |
| `bin/` | dwm 调用的脚本：自启动、状态栏、Super+P 菜单、壁纸、显示器、省电… | 大多立即生效 |
| `home/` | 个人配置（zsh、kitty、fcitx5…），由 `setup.sh link` 链接或复制到 `~` | 链接的立即生效 |
| `packages/` `system/` | 包清单和 `/etc` 模板 | `./setup.sh deps` / `system` |
| `st/` `tabbed/` | 终端和标签容器源码 | `./setup.sh st` / `tabbed` |
| `picom/` | 上游 picom v13 + 一个补丁 | `./setup.sh picom`，再重启 picom |
| `i3lock-color/` | 锁屏程序源码（加了输入时显示圆点） | `./setup.sh i3lock` |
| `wallpaper/` | 私有子模块，壁纸文件 | — |

## 3. 改 dwm 源码

日常循环：

1. 修改 `dwm/*.c` 或 `config/dwm.h`。
2. `Super + Shift + R`，由 `bin/reload.sh` 执行。

`bin/reload.sh` 做的事：

1. 调用 `./setup.sh dwm` 编译，安装到 `~/.local/bin/dwm`。先写临时文件再改名，所以可以覆盖正在运行的程序。
2. 编译失败：弹通知，正在运行的 dwm 不动，日志在 `~/.cache/dwm-build.log`。
3. 编译成功：给 dwm 发 `SIGHUP`。dwm 先把每块屏每个 tag 的布局、主窗口数、主区比例存进 root 窗口的 `_DWM_LAYOUTS` 属性，再 `execvp` 自身。新进程看到 `DWM_RESTARTED` 后读回这些设置，重新接管现有窗口。X 客户端始终没有断开，所以窗口和 tag 都在。

注意：

- 只有 `~/.local/bin/dwm` 才认 `SIGHUP`。`reload.sh` 会先确认正在运行的是不是它，不是就提示重新登录，不会误杀会话。
- 改了 `dwm.c` 里结构体或状态的含义时，原地重启读回的旧状态可能不兼容。保险起见注销后重新登录一次。
- dwm 运行中的错误记在 `~/.cache/dwm-error.log`。

## 4. 在测试会话里改星系

星系（`dwm/galaxy*.c`）是全屏遮罩加 OpenGL 渲染，直接在正在用的桌面上调试容易卡住整个会话。`dwm/tests/galaxy/` 提供一个独立的 Xvfb 会话（默认显示号 `:7`），里面跑测试副本 dwm，进程名不是 `dwm`，`Super + Shift + R` 不会误中它。

```sh
dwm/tests/galaxy/start.sh          # 起会话和测试窗口 (加 --multi 起双屏)
dwm/tests/galaxy/interrupt.sh      # 16 个中断 / 按键场景, 全部 ok 才算通过
dwm/tests/galaxy/loop.sh 40        # 连续进出 40 次, 内存和 X 资源应保持不变
dwm/tests/galaxy/reload.sh         # 改代码后: 重编并原地重启测试副本
dwm/tests/galaxy/stop.sh           # 结束
```

完整工具列表见 [dwm/tests/galaxy/README.md](../../dwm/tests/galaxy/README.md)，星系的设计和参数见 [星系手册](../manual/galaxy.md)。

**改着色器时：**

- GLSL 源码以字符串形式写在 `dwm/galaxy-gl.c` 里（`#version 330 core`）。编译失败的信息会写进星系日志（测试会话是 `~/.cache/dwm-galaxy:7.log`）。
- 改完先在测试会话里确认能正常打开，再到真实桌面上 `Super + Shift + R`。
- 装了 `glslang` 时，可以把着色器字符串拷出来用 `glslangValidator` 先查语法。
- Xvfb 只有软件渲染，效果和性能以真机为准。

## 5. picom 和 i3lock-color

- **picom**：`picom/` 是上游 v13 源码，加了一个约 20 行的补丁：dwm 在 root 窗口上设置 `_DWM_NOANIM`（毫秒），这段时间内 picom 的窗口动画直接跳到终态。这样星系和窗口总览自己演完"进入窗口"的过渡后，交接给真实窗口时不会被 picom 再动画一遍。补丁的具体位置见 [picom/README.dwm2.md](../../picom/README.dwm2.md)。编译安装：`./setup.sh picom`，然后 `Super + P` 关闭再打开 picom。
- **i3lock-color**：`i3lock-color/` 是 2.13.c.5 源码，加了"输入密码时显示圆点"。`./setup.sh i3lock` 安装到 `/usr/local/bin/i3lock-color`，betterlockscreen 优先用这个名字。系统包的 `/usr/bin/i3lock` 和 PAM 配置保持不变。

## 6. 常见扩展点

| 想加 | 改哪里 |
|---|---|
| 开机启动一个程序 | `bin/autostart.sh` 的 `daemons()`（常驻）或 `settings()`（一次性设置） |
| 定时任务 | `bin/autostart.sh` 的 `cron()`，按 10 秒为单位 |
| Super + P 菜单项 | `bin/rofi.sh`：在 `call_menu()` 里打印一行，在 `execute_menu()` 里加对应的 `case` |
| 状态栏模块 | 见 [教程 3 第 3 节](03-customize.md#3-加一个状态栏模块) |
| 需要新软件 | 加到 `packages/*.txt`，见 [教程 3 第 7 节](03-customize.md#7-加一个软件到安装清单) |
| 需要写 `/etc` | 模板放进 `system/`，在 `setup.sh` 的 `system()` 里用 `put_etc` 安装，在 `check()` 里加一项检查 |
| 需要家目录里的配置 | 文件放进 `home/`，在 `setup.sh` 的 `link()` 里用 `ln_`（只读配置）或 `seed`（程序会改写的配置） |

写脚本时的约定：

- 脚本开头用 `DWM=${DWM:-$(cd "$(dirname "$0")/.."; pwd)}` 取仓库目录，从命令行单独运行也能用。
- 缓存和状态放 `${XDG_CACHE_HOME:-$HOME/.cache}`。
- 可选的程序先 `command -v` 判断，没装时跳过。
- 和硬件有关的写法先检测再用（参考 `setup.sh` 的 `gpus()`、`bin/statusbar/packages/cpu.sh` 的温度读取），不要写死设备名。设备名这类值放进 `~/.config/dwm2.local`。

## 7. 提交前

```sh
./setup.sh check          # 最后一节"仓库"会检查有没有密钥或本机私有文件
git status                # 确认没有 home/ 以外的个人文件、日志、编译产物
```

- 密钥只放 `~/.config/zsh/local.zsh`，本机设置只放 `~/.config/dwm2.local`，两者都不在仓库里。
- 改了脚本的行为，顺手更新对应的手册或教程。

---

**回到：** [文档首页](../README.md)

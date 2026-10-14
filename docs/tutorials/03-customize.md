# 教程 3：自定义

**你会做到：** 加一个快捷键、一条窗口规则、一个状态栏模块；知道其他配置改哪里、怎么生效、哪些只属于本机。
**需要多久：** 30 分钟。
**前提：** 已经会基本操作（[教程 2](02-quickstart.md)）。

下文的 `$DWM` 指仓库所在目录（如 `~/projs/dwm2`）。桌面里的终端已经设置好了这个变量。

---

## 1. 加一个快捷键

目标：`Super + Shift + P` 打开音量控制 `pavucontrol`。

1. 打开 `config/dwm.h`，找到 `static Key keys[]`。在 `spawn + SHCMD` 那一段加一行：

   ```c
   { MODKEY|ShiftMask,    XK_p,      spawn, SHCMD("pavucontrol") },                                           /* super shift p    | 音量控制               */
   ```

   - `MODKEY` 是 Super，`ShiftMask` / `ControlMask` / `Mod1Mask`（Alt）可以用 `|` 组合。
   - 按键名用 `XK_` 加上 `xev` 里看到的键名。
   - **行尾注释照格式写**（`/* 按键 | 说明 */`），`Super + /` 的速查表就是从这里生成的。
2. 按 `Super + Shift + R`：自动编译、安装并原地重启 dwm，窗口和 tag 都保留。
3. 按 `Super + Shift + P` 试一下。

**编译失败怎么办：** 会弹出红色通知，显示前几行错误；完整日志在 `~/.cache/dwm-build.log`。正在运行的 dwm 不受影响，改好再按一次即可。

**先确认按键没被占用：** 按 `Super + /` 搜一下。

## 2. 加一条窗口规则

目标：pavucontrol 打开时浮动，并且显示在屏幕中间。

1. 查窗口的 class：运行 `xprop WM_CLASS`，鼠标变成十字后点一下 pavucontrol 的窗口，输出类似：

   ```
   WM_CLASS(STRING) = "pavucontrol", "pavucontrol"
   ```

   第一个是 instance，第二个是 class。
2. 在 `config/dwm.h` 的 `rules[]` 里，"普通优先度"一段加一行：

   ```c
   {"pavucontrol",          NULL,                 NULL,             0,            1,          0,          0,        -1,      0}, // 音量控制   浮动 居中
   ```

   各列的含义依次是：
   - class、instance、title：用来匹配窗口，写 `NULL` 表示不限
   - tags：要放到哪个 tag。`0` 表示当前 tag，`1 << 4` 表示第 5 个 tag
   - 是否浮动
   - 是否全局（所有 tag 都显示）
   - 是否无边框
   - 显示在哪个显示器：`-1` 表示当前显示器
   - 浮动位置：`0` 是居中，`1`～`9` 按九宫格（1 左上，9 右下）
3. `Super + Shift + R`，重开 pavucontrol 看效果。

规则从上往下匹配，越靠上优先级越高。现有规则和更多例子见 [dwm 手册](../manual/dwm.md) 第 5 节。

## 3. 加一个状态栏模块

状态栏右侧的每一块都是 `bin/statusbar/packages/` 里的一个脚本。`bin/statusbar/statusbar.sh` 定时调用这些脚本，脚本把显示内容写进 `bin/statusbar/temp`，再由它合成状态栏文字。点击某一块时，dwm 会调用 `statusbar.sh <模块名> <按键>`（按键是 `L` / `M` / `R` / `U` / `D`，分别对应左键、中键、右键、滚轮上、滚轮下）。

目标：加一个显示开机时长的 `up` 模块，左键弹通知。

1. 新建 `bin/statusbar/packages/up.sh`（照着 `mem.sh` 的结构写）：

   ```bash
   #! /bin/bash
   # 开机时长

   tempfile=$(cd $(dirname $0);cd ..;pwd)/temp

   this=_up
   icon_color="^c#3B001B^^b#6873790x88^"
   text_color="^c#3B001B^^b#6873790x99^"
   signal=$(echo "^s$this^" | sed 's/_//')

   update() {
       icon=" 󰔟 "
       text=" $(uptime -p | sed 's/up //; s/ hours\?/h/; s/ minutes\?/m/; s/,//g') "
       sed -i '/^export '$this'=.*$/d' $tempfile
       printf "export %s='%s%s%s%s%s'\n" $this "$signal" "$icon_color" "$icon" "$text_color" "$text" >> $tempfile
   }

   click() {
       case "$1" in
           L) notify-send "󰔟 开机时长" "$(uptime -p)" -r 9540 ;;
       esac
   }

   case "$1" in
       click) click $2 ;;
       *) update ;;
   esac
   ```

   - `^s模块名^`：点击时用来识别是哪一块
   - `^c颜色^`：前景色
   - `^b颜色透明度^`：背景色，后两位是透明度
2. 在 `bin/statusbar/statusbar.sh` 里登记这个模块，一共三处：
   - `refresh()`：开头的重置列表里加 `_up=''`，`xsetroot -name` 的字符串里在想放的位置加 `$_up`
   - `cron()`：加一行刷新周期，例如 `[ $((i % 60)) -eq 0 ] && to=(${to[@]} up)`；再把 `up` 加进 `[ $i -lt 30 ]` 那一行
   - `updateall|check)`：在列表里加 `up`
3. 立即看效果：`Super + P` → `update statusbar`。改了 `cron()` 里的周期要下次登录才生效（cron 是登录时启动的）。

## 4. 不用编译的配置

| 想改什么 | 文件 | 怎么生效 |
|---|---|---|
| 窗口动画、透明、圆角、模糊 | `config/picom.conf` | `Super + P` 关闭 picom 再打开 |
| 通知样式 | `config/dunst.conf` | `killall dunst; dunst -conf $DWM/config/dunst.conf &` |
| 启动器外观、配色 | `config/rofi/` | 下次打开 rofi |
| Super + P 菜单 | `bin/rofi.sh` | 立即 |
| 开机启动的程序 | `bin/autostart.sh` | 下次登录 |
| 环境变量、输入法 | `config/xinitrc` | 下次登录 |
| 终端字体、颜色 | `config/st.h` | `./setup.sh st`，新开的终端生效 |

各自的详细说明见 [手册](../README.md#手册)。

## 5. 只属于这台电脑的配置

有些设置不该进仓库，比如密钥、只有这台机器才有的硬件名。它们放在仓库外，不会被提交：

| 文件 | 放什么 | 谁加载 |
|---|---|---|
| `~/.config/dwm2.local` | 本机配置，如 `DISABLE_INPUT`（进桌面时禁用的输入设备名前缀，笔记本触摸板用）。模板是 `config/dwm2.local.example` | `config/xinitrc`，下次登录生效 |
| `~/.config/zsh/local.zsh` | 密钥（API token）、个人工具的 PATH、alias | `~/.zshrc`，新开终端生效 |

**不要把密钥写进仓库里的任何文件。** `./setup.sh check` 会检查仓库里有没有疑似密钥。

## 6. home/ 里的配置：链接和复制

`home/` 放的是 zsh、starship、kitty、fcitx5、flameshot 等个人配置，`./setup.sh link` 按两种方式装到家目录：

- **链接**（`~/.zshrc`、`~/.config/starship.toml`、`~/.config/kitty/kitty.conf` 等）：家目录里的文件就是仓库里的文件。改哪边都一样，改动会出现在 `git status` 里，可以提交。
- **只在不存在时复制**（fcitx5 和 Rime 的配置、flameshot、GTK/Electron 缩放、`mimeapps.list`）：这些程序自己会改写配置，链接的话仓库会一直被改脏。复制之后就归这台电脑所有。想把这台电脑上的改动带进仓库，手动把文件拷回 `home/` 再提交。

## 7. 加一个软件到安装清单

想让新电脑装好后自带某个软件：

1. 加到对应的清单里，每行可以写多个包，`#` 后面是注释：
   - 官方仓库的包：`packages/core.txt`（桌面必需）或 `packages/apps.txt`（应用）
   - AUR 的包：`packages/core-aur.txt` 或 `packages/apps-aur.txt`
   - 只在某种显卡上需要的：`packages/intel.txt`、`amd.txt`、`nvidia.txt`
2. `./setup.sh deps` 安装。
3. `./setup.sh check` 确认。

包名可以先用 `pacman -Si 包名`（官方仓库）或 `yay -Si 包名`（AUR）确认存在。

---

**下一步：** [教程 4：壁纸](04-wallpaper.md)

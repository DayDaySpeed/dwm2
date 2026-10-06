# picom 使用说明

picom 是窗口合成器，负责窗口的透明、圆角、模糊、淡入淡出和打开 / 关闭动画。没有它，dwm 照样能用，只是没有这些效果。

- 使用的版本：上游 picom v13，源码在 `picom/`，带一个 dwm 用的小补丁（见 `picom/README.dwm2.md`），安装在 `~/.local/bin/picom`
- 配置文件：`config/picom.conf`（v13 语法：窗口相关的设置都写在 `rules` 里，动画写在 `animations` 里）
- 由 `bin/autostart.sh` 开机启动：`picom --config $DWM/config/picom.conf`

> 系统里还有一个 pacman 装的 picom（`/usr/bin/picom`，也是 v13，但没有 dwm 的补丁）。
> 执行 `which picom` 应该显示 `~/.local/bin/picom`。

---

## 1. 开关 picom

| 方法 | 说明 |
|---|---|
| `Super + P` → `close picom` / `open picom` | 从 rofi 菜单开关，最方便 |
| `killall picom` | 在终端里关闭 |
| `picom --config $DWM/config/picom.conf -b` | 在终端里启动（`-b` 表示在后台运行） |

**修改配置后：** 先关闭 picom 再打开，新配置才会生效。

---

## 2. 当前效果

| 效果 | 设置 |
|---|---|
| 圆角 | 半径 10（输入法 fcitx 的候选框不加圆角） |
| 透明度 | 当前聚焦的窗口 95%，未聚焦的窗口 92%，全屏窗口 93% |
| 始终不透明的窗口 | VS Code、Chrome、Edge、Typora、平铺的 st 终端（以文字为主，避免透出壁纸），以及 mpv、OBS、GIMP、VNC、画中画 |
| 背景模糊 | dual_kawase，强度 4 |
| 淡入淡出 | 开启（rofi、输入法、screenkey 除外） |
| 打开 / 显示窗口动画 | appear（从中心放大并淡入，0.2 秒） |
| 关闭 / 隐藏窗口动画 | disappear（缩小并淡出，0.15 秒） |
| 位置、大小变化 | geometry-change（平滑过渡 0.2 秒）；切 tag 时窗口从屏幕左右两侧滑进来 |
| 不加动画的窗口 | 输入法、flameshot、dunst 通知、rofi、screenkey、微信 |
| 阴影 | 关闭 |
| 渲染后端 | glx，开启垂直同步 |
| `Super + Z` 星系动画的全屏遮罩（`dwm-galaxy`） | 不加阴影、圆角、模糊、淡入淡出和动画，始终 100% 不透明，也不会因为它全屏而停止合成；否则会出现黑边、二次淡入和闪烁 |
| `Super + A` 窗口总览的遮罩（`dwm-overview`，只盖当前屏） | 同上：不加阴影、圆角、模糊、淡入淡出和动画，始终 100% 不透明，并保持持续合成 |

---

## 3. 常见修改

所有修改都在 `config/picom.conf` 中进行，改完后按「开关 picom」一节的方法重启 picom。

v13 里针对窗口的设置都写成 `rules` 里的一条规则：`match` 是匹配条件，后面跟要设置的选项，后面的规则覆盖前面的。

**调整透明度：**
```
rules = (
    { match = "focused || wmwin || override_redirect"; opacity = 0.95; },   # 聚焦窗口
    { match = "!(focused || wmwin || override_redirect)"; opacity = 0.92; dim = 0.1; },   # 未聚焦窗口
    ...
);
```
想让某个程序始终不透明，在 `rules` 里加一条，比如 `{ match = "class_g = 'kitty'"; opacity = 1; },`。

**窗口 class 怎么查：** 在终端执行 `xprop WM_CLASS`，然后用鼠标点一下目标窗口。输出的第二个值就是 `class_g`。

**调整圆角：**
```
corner-radius = 10;             # 全局; 某个程序不要圆角就加规则 { match = "..."; corner-radius = 0; }
```

**调整模糊强度：**
```
blur-strength = 3;              # 数字越大越模糊
```

**调整或关闭动画：**
```
animations = (
    { triggers = [ "open", "show" ]; preset = "appear"; scale = 0.6; duration = 0.2; },
    { triggers = [ "close", "hide" ]; preset = "disappear"; scale = 0.6; duration = 0.15; },
    { triggers = [ "geometry" ]; preset = "geometry-change"; duration = 0.2; }
);
```
`duration` 是时长（秒）；整段删掉就没有动画。可用的预设还有 `slide-in` / `slide-out`、`fly-in` / `fly-out`，详见 `man picom` 的 ANIMATIONS 一节。
想让某个程序不带动画，在 `rules` 里不加动画的那条规则的 `match` 中加上它。

**关闭淡入淡出：**
```
fading = false;
```

---

## 4. 常见问题

| 现象 | 处理方法 |
|---|---|
| 没有透明、圆角、动画 | picom 没在运行：先执行 `pgrep -a picom` 确认，再用 `Super + P` 打开 |
| 某个程序画面异常或闪烁 | 在 `rules` 里给它加一条规则（例如 `opacity = 1`、`animations` 关掉、`corner-radius = 0`） |
| 感觉卡顿、耗电 | 调小 `blur-strength`，或者删掉 `animations` |
| 配置写错了 | 在终端执行 `picom --config $DWM/config/picom.conf --diagnostics`，看有没有 WARN / ERROR |
| 全屏玩游戏、看视频掉帧 | 全屏时 picom 会自动停止合成（`unredir-if-possible`），一般不需要处理；还不行就先关掉 picom |

---

## 5. 重新编译

picom 源码在 `picom/`。一般不需要重新编译，只有更新了源码时才需要：

```sh
cd ~/projs/dwm2 && ./setup.sh picom
```

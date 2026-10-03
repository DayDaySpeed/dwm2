# picom 使用说明

picom 是窗口合成器，负责窗口的透明、圆角、模糊、淡入淡出和打开 / 关闭动画。没有它，dwm 照样能用，只是没有这些效果。

- 使用的版本：yaocccc/picom（基于 picom v9 的动画分支），安装在 `/usr/local/bin/picom`
- 配置文件：`config/picom.conf`
- 由 `bin/autostart.sh` 开机启动：`picom --experimental-backends --config $DWM/config/picom.conf`

> 系统里还有一个 pacman 装的 picom（`/usr/bin/picom`，v13），它不支持这份配置里的动画选项。
> 执行 `which picom` 应该显示 `/usr/local/bin/picom`。

---

## 1. 开关 picom

| 方法 | 说明 |
|---|---|
| `Super + P` → `close picom` / `open picom` | 从 rofi 菜单开关，最方便 |
| `killall picom` | 在终端里关闭 |
| `picom --experimental-backends --config $DWM/config/picom.conf -b` | 在终端里启动（`-b` 表示在后台运行） |

**修改配置后：** 先关闭 picom 再打开，新配置才会生效。

---

## 2. 当前效果

| 效果 | 设置 |
|---|---|
| 圆角 | 半径 10（输入法 fcitx 的候选框不加圆角） |
| 透明度 | 当前聚焦的窗口 95%，未聚焦的窗口 92%，全屏窗口 93% |
| 始终不透明的窗口 | mpv、OBS、GIMP、VNC、全屏的 Chrome、画中画 |
| 背景模糊 | dual_kawase，强度 4 |
| 淡入淡出 | 开启（rofi、输入法、screenkey 除外） |
| 打开窗口动画 | zoom（从中心放大） |
| 关闭窗口动画 | squeeze（压缩消失） |
| 不加动画的窗口 | 输入法、flameshot、dunst 通知、rofi、screenkey、微信 |
| 阴影 | 关闭 |
| 渲染后端 | glx，开启垂直同步 |

---

## 3. 常见修改

所有修改都在 `config/picom.conf` 中进行，改完后按「开关 picom」一节的方法重启 picom。

**调整透明度：**
```
active-opacity = 0.95;          # 聚焦窗口
opacity-rule = [
    "92:!focused",              # 未聚焦窗口
];
```
想让某个程序始终不透明，在 `opacity-rule` 里加一行，比如 `"100:class_g = 'kitty'"`。

**窗口 class 怎么查：** 在终端执行 `xprop WM_CLASS`，然后用鼠标点一下目标窗口。输出的第二个值就是 `class_g`。

**调整圆角：**
```
corner-radius = 10.0;
```

**调整模糊强度：**
```
blur-strength = 4;              # 数字越大越模糊
```

**调整或关闭动画：**
```
animations = true;                          # 改成 false 关闭动画
animation-for-open-window = "zoom";         # 打开窗口动画
animation-for-unmap-window = "squeeze";     # 关闭窗口动画
animation-stiffness-in-tag = 125;           # 数值越大动画越快
```
想让某个程序不带动画，把它加进 `animation-exclude` 列表。

**关闭淡入淡出：**
```
fading = false;
```

---

## 4. 常见问题

| 现象 | 处理方法 |
|---|---|
| 没有透明、圆角、动画 | picom 没在运行：先执行 `pgrep -a picom` 确认，再用 `Super + P` 打开 |
| 某个程序画面异常或闪烁 | 把它加进对应的 exclude 列表，或者在 `opacity-rule` 里给它设 100 |
| 感觉卡顿、耗电 | 调小 `blur-strength`，或者关闭 `animations` |
| 全屏玩游戏、看视频掉帧 | 全屏时 picom 会自动停止合成（`unredir-if-possible`），一般不需要处理；还不行就先关掉 picom |

---

## 5. 重新编译

picom 源码在 `picom/`。一般不需要重新编译，只有更新了源码时才需要：

```sh
cd ~/projs/dwm2 && ./setup.sh picom
```

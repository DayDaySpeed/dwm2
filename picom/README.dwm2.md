# picom (dwm2 内置版本)

- 来源: 上游 [yshui/picom](https://github.com/yshui/picom) tag `v13` (d87a5ba), 源码直接放在本仓库
- 编译安装: 仓库根目录执行 `./setup.sh picom`, 装到 `~/.local/bin/picom` (PATH 中排在 pacman 装的 `/usr/bin/picom` 前面)
- 配置: `config/picom.conf` (v13 语法: 窗口相关的设置都在 `rules` 里, 动画在 `animations` 里)

## 本仓库的补丁

只有一处: **`_DWM_NOANIM`** (约 20 行)。

dwm 在 root 窗口上设置 `_DWM_NOANIM` (CARDINAL, 单位毫秒, 见 `dwm/dwm.c` 的 `noanim()`),
接下来这段时间内窗口动画直接跳到终态: 不启动新动画, 正在运行的也立即结束。
Super+Z 星系 / Super+A 总览已经自己演完了「进入窗口」的过渡, 交接给真实窗口时不能再被
picom 加一遍放大或滑动, 也不能让交接前还在滑动的窗口继续晃。

- `src/atom.h`: atom 列表加 `_DWM_NOANIM`
- `src/common.h`: `session` 加 `dwm_noanim_until_ms`
- `src/event.c` `ev_property_notify`: root 上的 `_DWM_NOANIM` 变化时记下截止时刻
- `src/wm/win.c` `win_process_animation_and_state_change`: 截止时刻之前不启动新动画, 并结束正在运行的动画

升级上游版本时, 用新源码替换本目录 (保留本文件), 再按上面四处重新打补丁。

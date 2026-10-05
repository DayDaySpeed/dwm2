# Super+Z 星系测试工具

在独立的 Xvfb 会话里测试 `dwm/galaxy*.c`, 不影响正在使用的桌面 (:0)。

- **显示号**: 默认 `:7`, 可用 `GALAXY_TEST_DISPLAY=:8` 改。
- **临时文件位置**: 测试副本 `dwm-galaxytest`、编译产物和临时文件都放在 `${XDG_RUNTIME_DIR:-/tmp}/galaxy-test/`, 不进仓库。
- **测试副本的进程名不是 `dwm`**: `bin/reload.sh` (Super+Shift+R) 用 `pgrep -x dwm` 找要重载的进程, 不会误中它。

## 常用流程

```sh
dwm/tests/galaxy/start.sh          # 起会话并建好测试窗口 (加 --multi 起 Xinerama 双屏)
dwm/tests/galaxy/interrupt.sh      # 16 个中断 / 按键场景, 全部 ok 才算通过
dwm/tests/galaxy/mouse.sh          # 驻留态鼠标睡眠 / 唤醒 / 8 秒超时 / Esc
dwm/tests/galaxy/loop.sh 40        # 连续进出 40 次, RSS 和 X 资源应保持不变
dwm/tests/galaxy/events.sh         # 驻留态天象: 新窗口诞生 / 关窗流星 / 通知彗星 / 整点报时
# 改了代码之后:
dwm/tests/galaxy/reload.sh         # 重编, 替换测试副本并原地重启 (窗口保留)
dwm/tests/galaxy/stop.sh           # 结束会话
```

测试会话的星系日志在 `~/.cache/dwm-galaxy:7.log`, 正式桌面的是 `~/.cache/dwm-galaxy.log`。

## 工具一览

| 文件 | 用途 |
|---|---|
| `start.sh` / `stop.sh` | 起 / 停测试会话: Xvfb、xrender 后端的 picom、测试副本 dwm、测试窗口 |
| `setup.sh` | 测试窗口布局: tag 1 / 2 / 3 / 5 / 9 上的彩色 xterm 和图片窗口, 外加一个隐藏窗口 |
| `reload.sh` | 重新编译并原地重启测试副本 |
| `lib.sh` | 公共变量和函数: `overlay` / `alive` / `rss` / `res` / `cstate` / `pause` |
| `interrupt.sh` | 开场 / 驻留 / 坍缩 / 回程 / 壁纸态下的 Esc、Super+Z、普通键、点击; 检查遮罩销毁、窗口状态恢复、日志里的结束路径 |
| `mouse.sh` | 驻留态首次左键只唤醒、右键拖动视角、睡眠时忽略滚轮和移动、8 秒自动休眠、Esc 先关闭交互 |
| `loop.sh` | 连续进出 N 次, 检查内存、X 资源和窗口状态 |
| `events.sh` | 驻留中开一个 xterm (诞生新星)、关掉 green-a (化作流星)、发两条通知 (`bin/galaxynote.sh`, 第二条排队), 检查日志里的 birth / death / note / chime 行。`events.sh out.mp4` 同时录屏 |
| `clickstar.py` | 先唤醒鼠标，再按颜色找窗口卡片并点击，验证点击跳转。例: `clickstar.py 74 26 92 40` 对应 purple-a |
| `record.sh` | 录测试会话: `record.sh out.mp4 秒数 [帧率] [宽度]` |
| `jumps.py` | 在录屏里找跳帧: 列出与前后一秒中位数相比变化最突兀的帧 |
| `grab.sh` | 从录屏抽帧: `grab.sh out.mp4 目录 9.5 12 20` |
| `xres.c` | 查询 dwm 在 X 服务器上占用的资源 (直接发 X-Resource 请求, 不依赖 libXRes), 由 `start.sh` 编译 |
| `xrbench.c` | 在当前显示上对 XRender 各合成路径计时 (离屏, 不显示任何东西)。正式桌面上运行: `DISPLAY=:0 ./xrbench` |

## 测试用的环境变量

启动测试 dwm 时带上 (例如 `stop.sh; GALAXY_VARIANT=A start.sh`), `reload.sh` 重启后仍然有效:

- `GALAXY_VARIANT=A|B|C`: 固定开场 (A 桌面碎成星尘 / B 星门 / C 大爆炸), 不设时随机轮换且不连续重复。日志的 `galaxy variant:` 行记录本次用的是哪套
- `GALAXY_FAKEHOUR=秒`: 驻留这么多秒后假装到了整点, 触发报时
- `GALAXY_TRACE=1`: 每帧把镜头参数写进日志, 用 `camtrace.py` 分析

## 注意

- Xvfb 是纯软件渲染, 帧率只用来对比前后变化; 真实帧率以 :0 日志的 `galaxy orbit:` / `galaxy dive:` 行为准。
- 软件渲染偶尔慢一帧, 在 `jumps.py` 里也会表现为单帧高值, 需要抽帧看画面确认。
- 测试窗口里有 987x121 这样的细长窗口, 用 `clickstar.py` 按颜色找时可能找不到, 换一个颜色即可。

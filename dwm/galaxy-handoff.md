# Super+Z 3D 工作空间星系: 交接说明

给接手 `dwm/galaxy.c` 的 agent。写于 2026-10-04。

## 0. 先读这几条

- **回复用户一律用简体中文。**
- **不要 git commit, 不要 push。** 工作区里还有用户自己没提交的修改 (`git status` 里的 `config/picom.conf`、`docx/picom使用说明.md`、删掉的 `wallpaper/static/wallhaven-d8drrl_2560x1080.png` 等), 不要覆盖、回退或"顺手整理"它们。
- 不能动真实窗口: 动画期间不移动、不隐藏、不映射、不 raise、不改焦点。每个窗口只在开始时截一次图, 之后只在离屏 3D 场景里画。真实的 tag / 焦点 / 布局 / 隐藏状态只在结束时恢复, 或按点击切换。
- 不要另起独立程序, 不要引入 3D 引擎。只用 Xlib + XRender + 软件矩阵。
- 文件已从 `relax.c` 改名为 `galaxy.c`, 但**函数和类型的前缀仍是 `relax*` / `Relax*`**, 快捷键绑定的函数名也还是 `relax`。如果用户要求连标识符一起改名, 需要同时改 `dwm.c` 里的前向声明和 `config/dwm.h:142`。

## 1. 当前状态

功能已经完成, 在 Xvfb 测试会话里验证过。

**当前桌面尚未加载这次增强**：只在独立 Xvfb `:17` 测试了新二进制，没有重启用户的 `:0` dwm。用户可在准备好时按 Super+Shift+R 重载。

用户重载并试过 Super+Z 之后, 先读 `~/.cache/dwm-galaxy.log`, 看这两行:

- `galaxy setup: ...`: 按键到第一帧的准备耗时;
- `galaxy first frames (start ms / render ms): ...`: 开场前 24 帧的时刻和渲染耗时。

如果这两行不存在, 说明用户跑的仍是旧版本。

大卡片倾斜时优先一次透视采样。用户 :0 的日志里 4×4 分块会把全屏窗口从约 8ms 打到 40ms 以上；分块只留作单应矩阵放不下时的退路。开场仍与桌面重合的卡片改为无变换拷贝。若探测到软件透视很慢，大卡片改画进半分辨率再放大。

## 2. 用户确认过的交互设计

| 状态 | Esc | Super+Z | 其他键 | 左键 | 滚轮 / 鼠标移动 |
|---|---|---|---|---|---|
| Intro (开场约 5.7s) | 从当前画面接入约 2.35s 收束坍缩 | 从当前画面回程 | 快进到驻留 (0.6s 时间扭曲) | 快进 | – |
| Orbit (驻留, 不限时) | 坍缩 | 回程到原 tag 和焦点 | 忽略 | 点窗口星: 跳到它的 tag 并聚焦它 (隐藏窗口会被恢复); 点核心: 只切 tag | 推拉镜头 / 视差, 悬停高亮 |
| Collapse (约 2.35s，星系群同向划过一段弧线后坍缩) | 立即进入 Rest | 立即取消并恢复 | – | – | – |
| Return (约 1.6s) | 立即结束 | 立即结束 | – | – | – |
| Rest (纯壁纸) | 恢复 | 恢复 | 恢复 | 恢复 | – |

其他已确认的设计:

- 窗口形态按深度 LOD 混合: 近处是截图小面板, 远处退化成光点。
- 轨道环必须看得见: 同一 ring 的星共享一个平面, 3D 椭圆环按前后半圈分别做深度排序, 星身后有彗星弧。tag 核心沿两条倾斜的星系群轨道公转，群轨道也按前后深度排序。
- 驻留时不抓键盘和鼠标, 锁屏程序能盖在遮罩之上。无输入 90 秒后降到 30fps, 熄屏 (DPMS) 时暂停渲染。

## 3. 代码结构

### 和 dwm 的接口

`galaxy.c` 不是独立编译单元, 由 `dwm.c` 在 `Pertag` 定义之后 `#include "galaxy.c"` (`dwm.c:443`)。`Makefile` 里有 `dwm.o: galaxy.c` 依赖, `config.mk` 加了 `-lm`。

`dwm.c` 里和星系相关的改动:

- 前向声明: `relax`、`relaxevent`、`relaxpost`、`relaxtick`、`relaxtimeout`、`relaxcleanup`、`relaxactive`。
- `run()` 主循环: 动画期间改用 `select` 加超时, 不再阻塞在 `XNextEvent`。每批事件先交给 `relaxevent`, 返回 0 才走 dwm 原本的 handler, 之后再调用 `relaxpost`。处理完一批后调用 `relaxtick` 渲染一帧, 超时由 `relaxtimeout` 给出。
- `cleanup()` 开头调用 `relaxcleanup()`。

`dwm.c` 里还有用户自己的修改, 例如 `previewallwin` 和删掉的 `quit`, 和星系无关, 不要动。

### 时钟

- `scene`: 场景时间。真实秒除以 `tscale = RELAXINTRO = 1.35`。
- `stage = min(scene, RELAXHOLD = 4.2)`: 驱动所有关键帧曲线 (镜头、卡片尺寸、亮度等)。驻留时停在 4.2。
- `motion = scene`: 一直往前走, 驱动轨道角、进动、全局自转、巡航扫掠 (`RELAXCRUISE` 18s)。
- Esc 退场有独立的 2.35s 真实时间轴：开场按 Esc 时先在 0.45s 内从当前 `stage` 平滑进入 `RELAXHOLD`；冻结星系群全局朝向，tag 核心沿共用轨道同向转过约 207°，同时淡出局部轨道和尾迹；最后 1.1s 把 `stage` 从 `RELAXEXIT` (4.75) 播到 `RELAXEND` (6.0)。**所有关键帧在 4.2 到 4.8 之间必须是平的**, 否则驻留进入坍缩会跳变。
- 快进: `relaxwarp` 把 scene 时钟用 easeInOutCubic 在 `RELAXWARP` (0.6s) 内推到 HOLD。
- 回程有自己的时钟 u (0 到 1, `RELAXRETURN` 1.6s)。开始时把所有星和镜头的状态冻结进 `r*` 字段, 之后不再依赖世界旋转。

关键帧曲线是 `RelaxKey` 数组, 用单调三次 Hermite 插值 (`relaxcurve` / `RELAXCURVE`), 定义在 `galaxy.c` 约 160 行之后。

### 函数分区 (行号是近似值)

| 区域 | 函数 |
|---|---|
| 数学 | `relaxcurve`、向量 / 矩阵 / 四元数 (`relaxeuler`、`relaxquat`、`relaxblend` 做 slerp) (210–420) |
| 镜头与投影 | `relaxproject`、`relaxsetcamera`、`relaxupdatecamera` (驻留运镜 + 视差 + zoom) (425–490) |
| 世界与星体 | `relaxworldat`、`relaxgalaxyat`、`relaxstarat` (纯函数, 尾迹靠回溯时间重算)、`relaxupdate*`、`relaxsortdepth` (painter's algorithm) (495–750) |
| 回程 | `relaxupdatereturn` (Bezier + slerp 回到 home, 最后交叉淡入桌面截图) (754) |
| 渲染 | `relaxhomography` (小卡片透视)、`relaxrendertiled` (大卡片 4×4 仿射块与 A1 蒙版)、`relaxrenderwindow` (卡片合成与着色)、`relaxrenderring` / `relaxrendercluster` / `relaxband` / `relaxflushbands` (局部与星系群轨道)、`relaxrenderbackground` (带缓存)、`relaxrender` |
| 资源 | 截图 `relaxcapture` (总预算 320MB, 超出的截半尺寸)、`relaxbuildmips`、`relaxfree*` (1310–1600) |
| 场景构建 | `relaxbuildgalaxies` (椭球布局, 当前 tag 在前, 空 tag 在后景壳层)、`relaxbuildorbits`、`relaxbuilddust`、壁纸和背景截图 (1600–1800) |
| 日志 | `relaxlogstart`、`relaxlogseg` (分阶段统计 fps / 1% low / 渲染耗时, 驻留时每 10s 一行)、`relaxlogfirst` (1796–1875) |
| 状态机 | `relaxend`、`relaxcancel`、`relaxfinish` (进入 Rest)、`relaxorbitstart`、`relaxcollapsestart`、`relaxwarp`、`relaxreturnstart`、`relaxpick`、`relaxtick`、`relaxtimeout`、`relaxevent`、`relaxpost`、入口 `relax()` (1876 到文件末尾) |

### 容易踩的坑

- **遮罩窗口**: `WM_CLASS` 是 `dwm-galaxy`。用户的 `config/picom.conf` 已对它排除阴影、圆角、透明、淡入淡出、动画、模糊和 unredir。这些是用户已有的未提交修改, 不需要再改 picom。
- **`relaxpost` 抬升遮罩**: 只在普通窗口 (非 override_redirect, 或是 dwm 管理的 client) map 或 configure 时才 raise 遮罩, 否则会盖住 i3lock。
- **窗口关闭检测**: 必须用事件里真正的窗口字段, 即 `xdestroywindow.window`、`xunmap.window`, 不能用 `xany.window`。
- **帧内不 malloc**: 三角形和投影点数组都在 `relax()` 里按上限预分配。
- **透视大卡片**: `relaxhomography` 的 k 缩放不能随便改, 改坏了会出现条纹。
- **防重影 (卡顿修复的核心)**:
  - 当前 tag 的卡片第 0 帧就与真实窗口像素对齐，聚焦放大须等离开桌面后再生效;
  - 桌面截图只在 stage 0 到 0.1 内淡出 (`r->desk`);
  - 卡片浮起从 stage 约 0.1 开始 (`relaxstarat` 里的 `u1`)。
  - 不要让桌面截图和已经开始移动的卡片同时可见。大卡片倾斜时走一次透视采样；不要为了 Xvfb 改回 4×4 分块，那会在用户的 NVIDIA 会话里把旋转帧率打下去。

### 日志

- 正式会话 (:0) 写 `~/.cache/dwm-galaxy.log`, 测试会话 (:7) 写 `~/.cache/dwm-galaxy:7.log`。每次 Super+Z 覆盖重写。
- 结束路径会记成 `galaxy end: from <mode> restore <0|1>`, 测试脚本靠它判定结果。

## 4. 测试方法

不要在用户的 :0 会话里跑自动化测试。用独立的 Xvfb 会话 :7。

> 下面提到的脚本都在 `/tmp/gx/` 和上一轮 agent 的 scratchpad (`/tmp/claude-1000/-home-jiang-projs-dwm2/cbce6cbe-.../scratchpad/`) 里, 重启后可能已经不存在。不存在时按下面的说明重建。

### 启动测试会话 (`/tmp/gx/start2.sh`)

```bash
export DISPLAY=:7
Xvfb :7 -screen 0 2560x1440x24 +extension Composite +extension RENDER -nolisten tcp &
sleep 1
feh --bg-fill /home/jiang/projs/dwm2/wallpaper/static/wallhaven-7jxlg3_1920x1080.png
picom --config /tmp/gx/picom-test.conf &   # backend = "xrender"; vsync/shadow/fading = false
sleep .5
DWM_RESTARTED=1 DWM=/home/jiang/projs/dwm2 /home/jiang/projs/dwm2/dwm/dwm > /tmp/gx/dwm7.log 2>&1 &
```

- 重载测试 dwm: 先 `make`, 再对 :7 上的 dwm 进程发 `kill -HUP`, 然后 `xsetroot -name reload` 唤醒它的主循环。
- 测试布局 (`setup.sh`): 用不同背景色的 xterm 和 feh 图片窗口, 分散到 tag 1、2、3、c、v, 并在当前 tag 放一个用 Super+I 隐藏的窗口。脚本里用到的 `wallhaven-d8drrl_2560x1080.png` 已被用户删除, 要换一张存在的图片。
- 判定状态 (`lib.sh` 的 `cstate`): 对 `_NET_CLIENT_LIST` 里每个窗口记录 xwininfo 的位置、大小、Map State 和 WM_STATE, 再加上 `xdotool getwindowfocus`。动作前后各记一次, 用 diff 比较。另外检查 `xwininfo -root -children | grep -c '"dwm-galaxy"'` 为 0 (遮罩已销毁)。

### 已通过的测试 (修复卡顿后重跑过)

- 早期 `interrupt.sh` 的 16 个用例适用于修改前行为，开场 Esc 预期已改为约 2.35s 坍缩，脚本旧断言不能直接复用。当前节奏已在 `:17` 测过开场 0.1 / 2.5s 按 Esc、驻留后完整坍缩，以及坍缩中按 Super+Z；窗口状态和 X 资源恢复一致。
  旧用例包括：
  - 开场 0.4、1.2、2.5、3.5、4.8s 时按 Esc;
  - 开场中按 Return、space、a 或点击快进;
  - 开场中按 Super+Z;
  - 驻留中按 a, 再按 Super+Z;
  - Esc 后 Esc / Super+Z;
  - 回程中按 Esc;
  - Rest 态下点击 / 按键。
- 旧 `loop.sh`: 60 次混合循环 (回程、坍缩→Rest→恢复、开场取消、快进后回程)。其中开场取消路径已变更，需要更新脚本后重跑。
- 点击跳转 (`clickstar.py`: 截图后按卡片颜色定位再点击): 同 tag 的窗口、其他 tag 的窗口、隐藏窗口、星系核心都验证过。
- 锁屏 (`fakelock`) 在驻留时能盖在遮罩之上。无输入时降到 30fps。0 个窗口和 32 个窗口都正常。
- 录屏和抽帧: `rec.sh out.mp4 秒数 "key super+z@0.8" ...` (用 ffmpeg x11grab), 用 `frames.py` 或 ffmpeg `-ss` 抽帧, 然后用 Read 查看。

### 性能参考

| 环境 | 开场 | 驻留 |
|---|---|---|
| 用户 :0 (NVIDIA, picom glx vsync, 18 个窗口, 旧版本日志) | 平均约 100fps, 1% low 68 | 稳定 59–60fps, 渲染平均约 12ms, 最大约 23ms |
| Xvfb :7 (12 个窗口) | 平卡阶段每帧 8–17ms, 开始透视倾斜后 35–77ms | 约 40fps |
| Xvfb :7 (32 个窗口) | – | 约 23.5fps |

Xvfb 没有 GPU, 只用来验证逻辑, 性能结论以用户 :0 的日志为准。

## 5. 文档

用户可见的说明已经更新为新流程, 改动这些功能时要同步更新:

- `README.md:48`, 其中实现路径已改成 `dwm/galaxy.c`;
- `docx/dwm使用说明.md`;
- `config/dwm.h:142` 的注释。

## 6. 让用户的修改生效

用户按 Super+Shift+R 会执行 `bin/reload.sh`: 编译、安装到 `~/.local/bin/dwm`、向 dwm 发 SIGHUP 原地重启, 窗口会保留。不要未经同意替用户重启 :0 上的 dwm。

## 2026-10-04 驻留轨道和性能调整

- `RELAXARCS=8` 把每条 72 段的主轨道和局部轨道分成 9 段短弧参与画家深度排序；相邻同层光带共用 `relaxflushbands`，两条主轨道每条只有一段短流光。空 tag 保持原有点击范围和较低亮度。
- 驻留卡片按投影尺寸选择近处清晰截图、中景低 mip、远处光点，最大边限制在 300px，悬停平滑增到 430px 并在卡片旁显示名称；小卡走仿射采样，驻留时不额外盖白色 tint / 黑色 quad。
- 质量级 0–3 仅调节装饰：隔帧成本的指数均值超过 16.8ms 连续 45 帧且距上次切换超过 2.5s 后降低细节；低于 13.5ms 连续 150 帧后恢复。切换效果通过 `qualityvisual` 平滑。主轨道、近处窗口和点击始终绘制。
- 日志分开打印 update、background、trails、items、present、XSync；Xvfb 2560×1440、11 窗口的瓶颈集中在 XSync（服务器合成）。本轮没有重启用户的 `:0` dwm，真实硬件的 58 FPS / 50 FPS 目标仍需用户下次重载后看 `~/.cache/dwm-galaxy.log`。
- 独立 Xvfb `:17` 检查过 Esc 坍缩和资源恢复；`:18` 检查过 11 窗口、悬停标题、滚轮、回程、重复进入退出。Xft 首次绘字会多留一个进程级 Picture 缓存，第二次进入退出后数量没有继续增加；`xerrors=0`。

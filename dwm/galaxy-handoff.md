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
| Orbit (驻留, 不限时) | 坍缩 | 回程到原 tag 和焦点 | 忽略 | 点窗口星: 朝镜头前推约 0.42s 后跳到它的 tag 并聚焦 (隐藏窗口会被恢复); 点核心: 核心亮起、周围淡出约 0.3s 后只切 tag | 推拉镜头 / 视差, 悬停高亮 |
| Collapse (约 2.35s，星系群同向划过一段弧线后坍缩) | 立即进入 Rest | 立即取消并恢复 | – | – | – |
| Return (Super+Z 约 1.6s 飞回; 点击走前推 / 淡出) | 立即结束 | 立即结束 | – | – | – |
| Rest (纯壁纸) | 恢复 | 恢复 | 恢复 | 恢复 | – |

其他已确认的设计:

- 窗口形态按深度 LOD 混合: 近处是截图小面板, 远处退化成光点。
- 轨道环必须看得见: 同一 ring 的星共享一个平面, 3D 椭圆环按前后半圈分别做深度排序, 星身后有彗星弧。tag 核心绕中心光源沿三条开普勒椭圆群轨道公转 (近点快远点慢), 每个核心身后拖长曝光星轨; 群轨道和星轨都按短弧深度排序。
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
- `motion = scene`: 一直往前走, 驱动轨道角、进动、全局漂移、翻转 / 涟漪 / 呼吸的时间表。驻留时用户在操作鼠标, motion 放慢到 30%。
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
| 回程 | `relaxupdatereturn` (Super+Z: Bezier + slerp 回到 home)、`relaxupdatepick` (点击窗口前推 / 点击核心淡出) |
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

## 2026-10-04 开普勒椭圆星轨、星系动作、导演镜头

- **群轨道** (`relaxlanes[]`, `relaxlanepoint`, `relaxkepler`, `relaxanomaly`): 3 条椭圆, 焦点都在原点 (中心光源, `RelaxSunItem`)。基准平面是水平盘面 xz, 按升交点 / 倾角 / 近点角摆放。平近点角随时间匀速增长, 用 Newton 迭代解开普勒方程。
  - 分配在 `relaxbuildlanes`: 有窗口的 tag 放内 / 中轨, 空 tag 放外轨。当前 tag 的相位会转到进入驻留时靠镜头的一侧。
  - Esc 退场的 `exitspin` 沿各自轨道方向叠加到平近点角上。
- **星轨** (`RelaxStreakItem`, `relaxupdatecluster` / `relaxrenderstreak`): 从当前平近点角往回取 `RELAXSTREAKM`, 共 24 段, 每个核心分 3 段参与深度排序。
  - 时间均匀采样, 所以近点处长、远点处短。
  - 椭圆底线驻留时只剩 alpha 约 .06 的细线。涟漪经过时, 底线上的对应位置会亮起。
- **动作**: 都是 motion 的纯函数 (交会闪光除外), 星体尾迹回溯仍然成立。
  - 近点亮度 `g->peri`。
  - 交会闪光与互相吸引 `g->flare` / `g->nudge`: 在 `relaxupdategalaxies` 里逐对计算。
  - 星系翻转 `relaxflipat`: 驻留后每 11s 一次, 用 3.2s 绕自身轨道平面 x 轴转一圈。
  - 涟漪 `relaxupdateripple`: 每 14s 一次, 以 .4 屏宽/秒扩散。
  - 轨道呼吸 `relaxbreathe`: 环半径和星体轨道半径共用, 保证星体始终在画出的环上。
  - 日志里有 `galaxy flip` / `galaxy ripple` 行。
- **导演镜头** (`relaxupdatecamera`, `relaxshotat`, `relaxshotseq`):
  - 6 个机位: wide / edge / below / follow / cross / top。每个停 12s, 机位之间 3s 过渡, 切换时记一行 `galaxy shot` 日志。
  - follow 机位用 `relaxsetcameraat` 把目标点移到即将过近点的核心 (`relaxfollowpick`)。回程冻结 `rctarget` 并插值回原点。
  - 最近 4s 有指针移动 / 滚轮, 或正在悬停时, `dspeed` 平滑降到 0: 导演时钟停住, 轨道放慢到 30%。
  - 自动取景 `dfit` 让核心保持在屏幕内, follow 机位不参与。
  - 关键帧末段 pitch 为 -14°。旧的 `relaxcruise` 已移除。
- **测试** (Xvfb :7):
  - `interrupt.sh` 16/16 通过。脚本已按「开场 Esc → 收束 → 壁纸」的现行行为更新。
  - 点击测试: 找到目标的点击都跳到了正确的窗口。注意测试布局里有 987x121 的细长窗口, 颜色匹配容易找不到。
  - zsh 不会拆分 `$var` 参数, 测试驱动请用 python 或 bash 脚本。

## 2026-10-04 开场 v3: 俯冲铺满全屏和旋转前后的节拍

- **时钟**: 开场时钟 `scene` 走到 `RELAXIEND` (5.7, 真实约 7.7s) 才进入驻留。
  - 关键帧 stage 由 `relaxintrostage` 映射: 3.3 之前恒等; 3.45 处停住, 留给俯冲和点名; 4.5 起落定到 `RELAXHOLD`。
  - 按 motion 排程的东西 (漂移、翻转、涟漪、呼吸、holdw、当前 tag 相位) 都以 `RELAXIEND` 为起点。
  - 尾迹回溯在开场中用 `relaxintrostage(scene − k·dt)`。
- **节拍** (`relaxupdatebeats`, `relaxrenderfx`; 时间轴是 `r->iclock`): 强度乘 `relaxbeatw()`, 即开场 / 坍缩中的 `beatfade`。
  - 快进时 `beatfade` 很快降到 0, 跳过俯冲。
  - Esc 时 `iclock` 冻结, `beatfade` 在 1s 内淡出, 镜头回到关键帧。
- **节拍一览**:
  - 起飞屏幕光环;
  - 超空间跃迁: 尘埃拉成光线 (`warpfx`), 镜头前冲;
  - 核心点火: `g->ignite` 加 3D 冲击环, 顺序是 `g->rank`;
  - 轨道描绘: `lanereveal`, `relaxringreveal`, 带笔尖光点;
  - 旋涡扭转: `relaxtwist`, 同时作用于核心、群轨道和星轨;
  - 转速峰值爆闪和盘面冲击环;
  - 俯冲 / 回缩镜头: `relaxbeatdist` / `relaxbeatpitch` / `relaxbeatyaw` / `relaxbeatroll`;
  - 逐个点名: `g->callout`, 核心脉冲, 局部环加亮, 卡片外弹;
  - 星轨拉出: `g->streakreveal`;
  - 卡片翻面: `s->flipcard`, 带 tint / glow 高光;
  - 落定涟漪: 在 `relaxupdatebeats` 末尾, 不乘 holdw。
- **镜头衔接**: 回缩终点 = 关键帧 4.2 = 驻留第一个机位 wide 的起点 (dist 1.0, pitch -11, yaw -4, roll `RELAXDIAG`), 进入驻留时不会再多一次运镜。
- **测试**:
  - `interrupt.sh` 里等待进入驻留的 sleep 改成 9.5, 16/16 通过;
  - `escdive.py` 录「俯冲中 Esc」和「快进」, 抽帧确认没有跳变。

## 2026-10-04 俯冲性能、对角线驻留

- **俯冲卡顿**: 用户 :0 (NVIDIA) 上开场最慢一帧 245ms, 时间基本都花在服务器绘制 (XSync) 上。原因有两个:
  - 卡片倾斜且最大 1100px, 走透视采样;
  - 核心光晕半径是核心的 7 倍, 近距离时一个光晕有几百万像素。
- **对应的改动**:
  - 开场时钟 3.1–3.5 起卡片转为朝向镜头 (`relaxupdatestars` 里 hb 取 `beatw` 的最大值)。
  - 非驻留时, 透视误差小于 4px 的卡片走仿射近似 (`relaxrenderwindow`, `persp < 4`)。
  - 俯冲时卡片上限 700px。
  - `relaxrenderglow` 的屏幕半径上限 80px。
  - `relaxband` 的半宽上限 9px。
  - 日志新增 `galaxy dive: frames N render avg / max` (开场时钟 3.2–5.0)。Xvfb 上开场平均 26→38fps, 最慢帧 88→60ms。
- **对角线驻留**:
  - 所有驻留机位和关键帧 4.2 / 4.8 的 roll 为 `RELAXDIAG` (30°), 压扁的盘面沿屏幕对角线铺开。卡片朝向镜头, 仍保持正立。
  - 群轨道倾角降到 8 / -10 / 14°, wide 机位 pitch -11°, top 机位 pitch -34°。
  - 驻留时群轨道底线 alpha 提到约 .11, 局部环降到 .13, 星轨覆盖 `RELAXSTREAKM` 1.5 rad。
- **测试副本**:
  - Xvfb :7 的测试 dwm 改用 `/tmp/gx/dwm-galaxytest`, 进程名不是 `dwm`, 用户的 `bin/reload.sh` (`pgrep -x dwm | head -1`) 不会误中它。
  - 重新编译后用 `/tmp/gx/reloadtest.sh` 覆盖副本并原地重启。

## 2026-10-04 光带改为软件画布; 驻留铺满全屏

- **基准** (`scratchpad/xrbench.c`, 在用户 :0 上对离屏 Pixmap 计时, 每项后读回 1 像素作为 GPU 栅栏):
  - sprite 放大到 1100px 0.06ms, 700px 卡片仿射 / 透视 ≤0.2ms, 整屏合成 0.15ms, 都是 GPU 加速;
  - `XRenderCompositeTriangles` 约 7µs / 个细三角形, 拆小调用也没用: 600 个 4.4ms, 8 桶 × 600 个 30ms;
  - 大三角形按面积计费: 一张 700×380 卡片的 tint quad 0.84ms;
  - 128×128 a8 块的 XPutImage + 合成: 整屏 240 块 2.5ms。
- **光带** (`relaxband` / `relaxflushbands`):
  - 进程内 a8 画布 (`bandbuf`), 按点到线段的距离算抗锯齿覆盖率, 取最大值写入 (等价于原来同桶 a8 蒙版不叠亮)。
  - 脏块按 128×128 记录; flush 时同一行相邻的块合并成一次 XPutImage + 白色合成, 然后清零。
  - 只在「卡片之前且与它相交」时 flush (`relaxbandpending`); 光点之间的先后不影响观感。
  - 透明度桶和三角形数组都已删除。
  - 日志 `bands A+Bms (N tiles in M flushes)`: A 是软件栅格化, B 是上传加合成。
- **卡片的压暗 / 发光叠加**: 用卡片自己的 mip (已设好变换) 当蒙版合成纯色。pixelcopy 卡片直接画矩形。只有 tiled / half 路径还用 `relaxfillquad`。
- **铺满全屏**:
  - 群轨道半长轴 .20 / .30 / .40 屏宽;
  - 有窗口的星系轮流放进三条轨道, 空 tag 交替补到外 / 中轨;
  - 自动取景目标 .93, dist 下限 .75;
  - wide .9、edge .85、below .88, 关键帧 4.2 / 4.8 和节拍镜头终点 dist .9。
- **三层尘埃** (`relaxbuilddust`, 70–140 颗): 远景星空球壳; 盘面尘带 (`d->disk`, 投影时乘 world, 随星系群转); 前景浮尘。
- **Xvfb 数据**: 俯冲 36→23ms; 驻留在质量 0 下 58–59fps。

## 2026-10-04 驻留再撑满; 滚轮默认最大放大

- **滚轮**: `zoom` 默认 1 表示最近 (最大放大), 范围 [1, 1.8]。向上滚到底不再变化, 向下滚 ÷.9 拉远。「最近」已经做进机位和落定距离里, 所以落定后不会再推一次镜头。
- **距离**:
  - 机位: wide .72 (pitch -14°)、edge .68、below .70、cross .76→.68、top .90;
  - 关键帧 4.2 / 4.8 和节拍终点都是 .72 / -14°;
  - 群轨道 .22 / .33 / .45 屏宽。
- **自动取景**: 偏移乘以当前 zoom 做归一, 不再抵消用户的缩放; 目标 .98, 范围 [.8, 1.25]。
- **验证**: 在 Xvfb 截图比对: 向上滚无变化; 向下滚拉远, 停手 10s 仍保持; 再向上滚回到最近。

## 2026-10-05 驻留「星轨遨游」镜头和密集特效

- **镜头** (`relaxshotseq`, `relaxshotat`, `relaxtourgalaxy`, `relaxtourtrack`):
  - 机位序列: wide → tour → tour → edge → tour → below → tour → cross → tour → top → tour。每个停 9s, 过渡 4s。
  - tour: 目标点是轮到的星系核心 (按 `popord` 轮转), dist .5, 在核心轨道外侧绕它转, pitch 缓慢起伏。
  - 两个跟踪槽 `dtour[2]` / `dtyaw[2]` / `dtg[2]` 分别平滑跟踪当前和下一个机位的星系; 换机位时槽位整体前移。
  - 与 tour 相关的过渡距离加 `.35·sin(π·mix)`, 形成飞越。
  - 全景机位的 dist 随 `dclock` 呼吸 ±12% (周期 20s)。
  - 自动取景只在全景机位起作用; 镜头最近 .35F。驻留卡片上限 460 / 600px (悬停)。
- **节奏** (`relaxcycle(motion, offset, every, &k, &local)`, 单位是真实秒, 以进入驻留为起点):

  | 特效 | offset | 间隔 |
  |---|---|---|
  | 流星 | .8 | 2.6 |
  | 星座 | 2 | 4 |
  | 翻转 | 1.5 | 5 |
  | 涟漪 | 4 | 6 |
  | 光桥 | 3 | 7 |
  | 彗星 | 6 | 15 |
  | 超新星 | 9 | 20 |

  - 日志有 flip / ripple / comet / supernova 行。
- **状态** `relaxupdateholdfx`:
  - `g->nova`, `g->bridge`, `s->constel`, 以及尘埃 `d->boost` (超新星附近的盘面尘带变亮);
  - 光桥连接的两个核心在每次事件开始时选定 (不同轨道上相距最近, 带随机), 存在 `bri` / `brj`。
- **绘制** `relaxrenderholdfx`, 在卡片之后:
  - 轨道光流: 群轨道每条 6 个光点, 局部环每条 2 个;
  - 星座连线 (按轨道角排序);
  - 光桥: 光线加跑动的亮点;
  - 超新星的两圈冲击环;
  - 彗星: 世界空间, 尾巴背向中心;
  - 流星: 屏幕空间, 沿对角线。
  - 远景星点的闪烁在 dust 绘制分支里 (`d->tw`)。回程开始时清零 nova / bridge。
- **测试环境**:
  - 机器重启后 /tmp 被清空。测试工具已重建到 scratchpad 的 `gx/`: `start.sh`, `setup.sh`, `reloadtest.sh`, `lib.sh`, `interrupt.sh`, `loop.sh`, `clickstar.py`, `grab.sh`, `xres`。
  - `xres` 直接发 X-Resource 扩展请求, 因为系统没有 libXRes。

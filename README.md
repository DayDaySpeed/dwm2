# dwm2

基于 [yaocccc](https://github.com/yaocccc) 的 dwm 桌面环境, 所有配置集中在本仓库。

```
config/   所有配置 (只改这里)
  dwm.h  st.h  tabbed.h      编译时使用的 config.h
  picom.conf  dunst.conf     由 bin/autostart.sh 加载
  ranger/                    终端文件管理器, ~/.config/ranger 软链接到这里
  sunshine.conf              远程桌面串流, ~/.config/sunshine/sunshine.conf 软链接到这里
  betterlockscreenrc         锁屏主题, ~/.config/betterlockscreen/betterlockscreenrc 软链接到这里
  rofi/                      rofi 主题 (adi1090x style-5) 与配色, ~/.config/rofi 软链接到这里
  xinitrc                    环境变量与会话服务, ~/.xinitrc 软链接到这里
  Xresources                 Xft.dpi 等, ~/.Xresources 软链接到这里
bin/      dwm 调用的脚本 (快捷键、状态栏、自启动)
  autostart.sh               dwm 启动时执行, 所有开机启动项都在这里
  statusbar/                 状态栏
  livewall.sh                动态 / 静态壁纸 (xwinwrap + mpv 播放视频)
  powersave.sh               省电模式: 静态壁纸 + 60Hz + CPU 节能 (Super+P 开关)
  power.sh keys.sh           电源菜单 (Super+Shift+Esc), 快捷键速查 (Super+/)
  batalert.sh dimscreen.sh   低电量提醒, 锁屏前调暗提醒
  nightlight.sh              护眼暖色 (Super+P 开关)
  blurlock.sh rofi.sh set_vol.sh dpms.sh ...
scripts/                     手动运行的工具脚本 (music_covers.py 补专辑封面)
docx/                        使用说明 (dwm、终端、picom、rofi、通知、锁屏截图), 入口 docx/README.md
wallpaper/                   壁纸; ~/Pictures/wallpaper 软链接到这里
  static/                    静态图片, 每 5 分钟随机轮换
  live/                      动态壁纸视频 (mp4 等, 不提交到 git)
dwm/ st/ tabbed/ picom/      源码 (基于 yaocccc 的版本, 已含本仓库的改动), 直接修改, ./setup.sh <名字> 编译安装
i3lock-color/                锁屏用的 i3lock-color 源码 (输入密码时显示圆点), ./setup.sh i3lock 编译安装
setup.sh                     安装/编译入口
```

`$DWM` 指向本仓库根目录 (在 `config/xinitrc` 中导出)。

## 使用

```sh
git clone --recursive <本仓库> ~/projs/dwm2
cd ~/projs/dwm2 && ./setup.sh          # 依赖 + 链接 + 编译全部

# 改了 config/dwm.h 后: 按 Super + Shift + R (自动编译安装并原地重启)
./setup.sh st | tabbed | picom         # 改了对应配置后
```

脚本和 picom/dunst 配置修改后无需编译; 改 dwm/st/tabbed 后需重新编译并重启 dwm。

`Super + Z` 进入 DWM 内的 3D 工作空间星系：约 8 秒开场（窗口截图带着一圈冲击波脱离桌面，其他 tag 的窗口在超空间跃迁的光线中飞来，各 tag 核心依次点火并放出冲击环，轨道像光笔一样被画出；整个星系群高速旋转并被拧成螺旋，转速最高时中心光源爆闪；随后镜头俯冲进星系群内部铺满全屏，各星系逐个点名闪亮，再沿弧线拉回，星轨从核心向后拉出，窗口卡片依次翻面亮相，落定时中心光源发出一圈大涟漪），然后**停在星系轨道态**，不限时（镜头带一定翻滚，压扁的轨道盘面沿屏幕对角线铺满全屏，有窗口的星系分布在三条轨道上，远景星空、盘面尘带和前景浮尘构成空间层次，窗口卡片仍保持正立）：tag 核心带着窗口星绕中心光源沿三条倾角、偏心率各不相同的开普勒椭圆轨道公转（近点快、远点慢，每个核心身后拖一段渐隐的长曝光星轨），窗口星还会沿各自核心周围的 3D 轨道运行；星系不时整体翻转、轨道环缓慢呼吸，中心光源周期性发出沿轨道扩散的涟漪，两个核心交会时同时闪光；另有流星沿对角线划过、彗星穿过星系群、光点沿轨道流动、同一 tag 的窗口连成星座、两个核心之间亮起光桥、远景星空闪烁、偶尔一个核心爆发成超新星，平均每一两秒就有一个特效；镜头像在星轨中遨游：在全景、侧掠、仰视、穿越盘面、俯瞰等机位之间，穿插飞到各个星系身边绕着它转、再飞越到下一个星系的巡游镜头，全景时也会缓慢推拉，移动鼠标或滚动滚轮时镜头停住、轨道放慢，停手约 4 秒后继续；局部轨道与核心、窗口星按短弧深度穿插；窗口近处显示有尺寸上限的清晰截图，中景使用缩略图，远处退为光点。移动鼠标有视差，滚轮推拉镜头（默认就是最近、铺满全屏的状态，向下滚拉远，向上滚回到最近），鼠标悬停会平滑放大窗口并显示名称，点击窗口星时该卡片朝镜头前推，约 0.4 秒后切到它所在的 tag 并聚焦它（隐藏窗口会被恢复），点击星系核心时核心亮起、周围淡出后切到该 tag。驻留时按 `Super + Z` 回程：窗口沿 3D 曲线飞回原位置，无缝露出原桌面；开场或驻留时按 `Esc`：各 tag 星系沿共用轨道划过一段弧线，再向中心收束成一个光点，整段约 2.35 秒，之后只剩壁纸；再按 `Super + Z`（或任意键 / 点击）恢复原桌面。退场时再按 `Esc` 可立即进入壁纸态。开场中按其他键或点击快进到驻留态。动画只使用开始时截的一次窗口图，期间不移动、隐藏或聚焦任何真实窗口；驻留时不独占键盘鼠标，锁屏照常；渲染超预算时逐级减去远景尘埃、尾迹和次要轨道柔光，保留主轨道与窗口；无操作 90 秒降到 30 帧，熄屏时暂停渲染（实现见 `dwm/galaxy.c`）。`Super + P` 仍为原有 rofi 自定义菜单。

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

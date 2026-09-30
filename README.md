# dwm2

基于 [yaocccc](https://github.com/yaocccc) 的 dwm 桌面环境, 所有配置集中在本仓库。

```
config/   所有配置 (只改这里)
  dwm.h  st.h  tabbed.h      编译时使用的 config.h
  picom.conf  dunst.conf     由 bin/autostart.sh 加载
  xinitrc                    ~/.xinitrc 软链接到这里
bin/      所有脚本
  autostart.sh               dwm 启动时执行
  statusbar/                 状态栏
  blurlock.sh rofi.sh set_vol.sh music_player.sh dpms.sh ...
dwm/ st/ tabbed/ picom/      上游源码 (git submodule, 不直接修改)
setup.sh                     安装/编译入口
```

`$DWM` 指向本仓库根目录 (在 `config/xinitrc` 中导出)。

## 使用

```sh
git clone --recursive <本仓库> ~/projs/dwm2
cd ~/projs/dwm2 && ./setup.sh          # 依赖 + 链接 + 编译全部

./setup.sh dwm                         # 改了 config/dwm.h 后
./setup.sh st | tabbed | picom         # 改了对应配置后
```

脚本和 picom/dunst 配置修改后无需编译; 改 dwm/st/tabbed 后需重新编译并重启 dwm。

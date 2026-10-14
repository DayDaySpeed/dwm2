#!/bin/sh
# Steam (Super+0): 界面由沙盒化的 CEF (steamwebhelper) 渲染, 容器内没有
# fcitx5 的 GTK 输入法模块 (im-fcitx5.so),导致 GTK_IM_MODULE=fcitx5
# 加载失败、无法切换输入法。容器内自带 im-xim.so,改用 XIM 协议
# (通过 X11 直接和 fcitx5 通讯,不依赖容器内的模块文件) 即可正常工作。
export GTK_IM_MODULE=xim
exec /usr/bin/steam "$@"

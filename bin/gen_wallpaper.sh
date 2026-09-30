#!/bin/bash

# --- 配置区域 ---
RESOLUTION="2560x1440" # 你的屏幕分辨率
BG_COLOR="#2E3440"     # Nord 深色背景
TEXT_COLOR="#ECEFF4"   # Nord 米白文字
TITLE_COLOR="#88C0D0"  # Nord 淡蓝标题
# FONT="JetBrains-Mono-Bold" # 建议使用的字体，如果没安装，可以暂时注释掉用系统默认
TITLE_FONT_SIZE=48
FONT_SIZE_MAIN=22                                # 主体文字大小
DWM=${DWM:-~/projs/dwm2}
OUTPUT_FILE="$DWM/wallpaper/nvim_cheatsheet.png" # 最终输出路径 (壁纸目录)

# ==========================================
# 左栏内容：系统配置路径
# ==========================================
read -d '' CONTENT_PATHS <<EOF
[ SYSTEM CONFIG PATHS ]

> DWM2 (~/projs/dwm2, 需编译: ./setup.sh dwm|st|tabbed)
DWM:       config/dwm.h
St:        config/st.h
Tabbed:    config/tabbed.h
Autostart: bin/autostart.sh
Statusbar: bin/statusbar/

> COMPOSITOR & UI
Picom:     config/picom.conf
Rofi:      config/rofi.rasi
Dunst:     config/dunst.conf
Xinitrc:   config/xinitrc
GTK2:      ~/.gtkrc-2.0
GTK3:      ~/.config/gtk-3.0/settings.ini

> TERMINAL & SHELL
Kitty:     ~/.config/kitty/kitty.conf
Alacritty: ~/.config/alacritty/alacritty.yml
Zsh:       ~/.zshrc
Alias:     ~/.zsh_aliases
Starship:  ~/.config/starship.toml
EOF

# ==========================================
# 右栏内容：Neovim 精选速查表
# 注意：为了对齐美观，描述文字前尽量用空格对齐
# 我们在这里把 % 转义为 \% 以避免 ImageMagick 警告
# ==========================================
read -d '' CONTENT_NVIM <<EOF
[ NEOVIM ESSENTIALS ]

> 模式切换 (MODES)
i / a       : 在光标前 / 后插入
I / A       : 在行首 / 行尾插入
o / O       : 在下方 / 上方新开一行插入
v / V       : 字符可视化 / 行可视化
Ctrl+v      : 块可视化模式
Esc / C-[   : 返回普通模式

> 核心编辑 (EDITING)
u / C-r     : 撤销 (Undo) / 重做 (Redo)
dd / yy     : 删除(剪切)行 / 复制行
p / P       : 在光标后 / 前粘贴
x / X       : 删除光标处字符 / 前字符
cw / ciw    : 修改一个词 (Change Word)
dt" / ci"   : 删除直到 " / 修改 "" 内部内容
.           : 重复上一次编辑操作

> 快速移动 (MOVEMENT)
h j k l     : 左 下 上 右
w / b       : 下个词首 / 上个词首
0 / $       : 行首 / 行尾
^           : 行首非空字符
gg / G      : 文件首 / 文件尾
:123        : 跳转到第 123 行
\%          : 匹配括号跳转 ()[]{}
C-d / C-u   : 向下翻半页 / 向上翻半页

> 窗口与标签 (WINDOWS/TABS)
:sp / :vsp  : 水平分割 / 垂直分割
C-w + hjkl  : 在窗口间切换焦点
C-w + c     : 关闭当前窗口
:tabnew     : 新建标签页
gt / gT     : 切换下一个 / 上一个标签页

> 保存与退出 (EX COMMANDS)
:w          : 保存
:q / :q!    : 退出 / 强制退出
:wq / ZZ    : 保存并退出
:e file     : 打开新文件
EOF

# --- 重要修复：确保输出目录存在 ---
# dirname 命令会提取 OUTPUT_FILE 的目录部分 (即 /home/jiang/Pictures)
# mkdir -p 会确保这个目录存在，如果不存在就创建它
mkdir -p "$(dirname "$OUTPUT_FILE")"

# --- 生成图像的魔法命令 (双栏版) ---
# 如果你没有安装 JetBrains Mono 字体，可以去掉 -font "$FONT" 这部分，使用默认字体
magick -size "$RESOLUTION" xc:"$BG_COLOR" \
  -font "$FONT" -antialias \
  -fill "$TITLE_COLOR" -pointsize "$TITLE_FONT_SIZE" -draw "gravity north text 0,100 'ARCH DWM & NEOVIM CHEAT SHEET'" \
  -fill "$TEXT_COLOR" -pointsize "$FONT_SIZE_MAIN" \
  -gravity west -annotate +200+100 "$CONTENT_PATHS" \
  -gravity east -annotate +200+100 "$CONTENT_NVIM" \
  "$OUTPUT_FILE"

# --- 完成通知 ---
if [ $? -eq 0 ]; then
  echo -e "\033[32m[SUCCESS]\033[0m 双栏壁纸已成功生成!"
  echo "位置: $OUTPUT_FILE"
  # 这里可以尝试自动设置壁纸，取决于你用什么工具设定壁纸，例如 feh:
  # feh --bg-scale "$OUTPUT_FILE"
else
  echo -e "\033[31m[ERROR]\033[0m 壁纸生成失败，请检查上方错误信息。"
fi

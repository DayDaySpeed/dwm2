#!/bin/bash
# 结束测试会话 (Xvfb / picom / 测试副本 dwm / 测试窗口都在这个显示上, 随 Xvfb 一起退出)
. "$(dirname "$0")/lib.sh"
pkill -f "^Xvfb $DISPLAY "; pkill -f "^Xephyr $DISPLAY "; pkill -f "^Xvfb ${GALAXY_TEST_HOST:-:17} "; pkill -f "$RT/dwm-galaxytest\$"; true

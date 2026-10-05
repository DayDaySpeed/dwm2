#!/bin/bash
# 改完代码后: 重新编译, 替换测试副本并原地重启 (SIGHUP, 窗口和 tag 保留)
. "$(dirname "$0")/lib.sh"
make -C "$REPO/dwm" >/dev/null || exit 1
cp "$REPO/dwm/dwm" "$RT/dwm-galaxytest.new" && mv "$RT/dwm-galaxytest.new" "$RT/dwm-galaxytest"
kill -HUP "$(dwmpid)" && xsetroot -name reload

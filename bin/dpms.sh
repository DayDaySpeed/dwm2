#!/bin/sh
# dpms.sh — 后台循环守护，持续确保 DPMS 保持为 600s（10分钟）
# 启动方式：nohup $DWM/bin/dpms.sh >/dev/null 2>&1 &

IDLE_SEC=600

case "$1" in
  --one-shot)
    xset dpms "$IDLE_SEC" "$IDLE_SEC" "$IDLE_SEC"
    xset +dpms
    xset s "$IDLE_SEC"
    xset s on
    exit 0
    ;;
esac

# 首次立即设置
xset dpms "$IDLE_SEC" "$IDLE_SEC" "$IDLE_SEC"
xset +dpms
xset s "$IDLE_SEC"
xset s on

# 每 60s 检查并修正，持久运行（某些应用会在会话中途把 DPMS 改回默认值）
while :; do
  sleep 60
  # 检查实际 DPMS 值是否被改
  current=$(xset -q 2>/dev/null | awk '/Standby:/ {print $2}')
  if [ "$current" != "$IDLE_SEC" ]; then
    xset dpms "$IDLE_SEC" "$IDLE_SEC" "$IDLE_SEC"
    xset +dpms
    xset s "$IDLE_SEC"
    xset s on
  fi
done

#! /bin/bash
# 低电量提醒 (autostart 每 60 秒调用): 只在用电池时生效, 每档只提醒一次, 插上电源后重置
#   20%: 提醒    10%: 紧急提醒    5%: 紧急提醒, 30 秒后自动睡眠 (期间插上电源则取消)
# 测试用环境变量: BATALERT_CAP=电量 BATALERT_AC=0/1 覆盖实际读数, BATALERT_DRYRUN=1 不真的睡眠

STATE=${XDG_CACHE_HOME:-$HOME/.cache}/batalert   # 已提醒过的最低一档
NID=9531

ac_online() {
    local s
    [ -n "$BATALERT_AC" ] && { [ "$BATALERT_AC" = 1 ]; return; }
    for s in /sys/class/power_supply/*; do
        [ "$(cat "$s/type" 2>/dev/null)" = Mains ] && [ "$(cat "$s/online" 2>/dev/null)" = 1 ] && return 0
    done
    return 1
}

capacity() {
    [ -n "$BATALERT_CAP" ] && { echo "$BATALERT_CAP"; return; }
    cat /sys/class/power_supply/BAT*/capacity 2>/dev/null | head -1
}

if ac_online; then rm -f "$STATE"; exit 0; fi
cap=$(capacity)
[ -n "$cap" ] || exit 0
done_=$(cat "$STATE" 2>/dev/null || echo 100)

level=
for l in 5 10 20; do [ "$cap" -le $l ] && { level=$l; break; }; done
[ -n "$level" ] && [ "$level" -lt "$done_" ] || exit 0       # 没到新的一档
echo "$level" > "$STATE"

case $level in
    20) notify-send -r $NID -u normal "󰁻 电量低" "剩余 ${cap}%, 建议接上电源" ;;
    10) notify-send -r $NID -u critical "󰁺 电量很低" "剩余 ${cap}%, 请尽快接上电源" ;;
    5)
        notify-send -r $NID -u critical "󰂎 电量即将耗尽" "剩余 ${cap}%, 30 秒后自动睡眠, 接上电源可取消"
        sleep 30
        ac_online && { notify-send -r $NID "󰂄 已接上电源" "取消自动睡眠"; exit 0; }
        [ "$BATALERT_DRYRUN" = 1 ] && { echo "dry-run: systemctl suspend"; exit 0; }
        systemctl suspend ;;
esac

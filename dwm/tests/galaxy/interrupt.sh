#!/bin/bash
# 中断 / 跳过 / 各状态下的按键: 每个用例前后窗口状态一致, 遮罩已销毁, 日志里的结束路径正确 (共 16 个)
. "$(dirname "$0")/lib.sh"
pass=0; fail=0
case_() {
    local name=$1 expect=$2 bad= res a n; shift 2
    cstate > "$RT/c0.txt"
    for a in "$@"; do
        case $a in
            sleep*) pause ${a#sleep } ;;
            check-overlay*) n=$(overlay); [ "$n" = "${a#check-overlay }" ] || { echo "  [$name] overlay=$n, expected ${a#check-overlay }"; bad=1; } ;;
            *) xdotool $a ;;
        esac
    done
    pause 2.2
    cstate > "$RT/c1.txt"
    res=ok
    [ "$(overlay)" = 0 ] || res="OVERLAY LEFT"
    diff -q "$RT/c0.txt" "$RT/c1.txt" >/dev/null || res="$res STATE DIFF"
    [ "$(alive)" = alive ] || res="DWM DEAD"
    grep -q -- "$expect" "$LOG" || res="$res (log lacks '$expect')"
    [ -n "$bad" ] && res="$res overlay-check-failed"
    if [ "$res" = ok ]; then pass=$((pass+1)); else fail=$((fail+1)); fi
    printf "%-46s %s\n" "$name" "$res"
}
xdotool mousemove 1280 720
for t in 0.4 1.2 2.5 3.5 4.8; do
    case_ "Esc in intro @${t}s -> collapse -> rest" "end: from rest restore 1" "key super+z" "sleep $t" "key Escape" "sleep 3" "check-overlay 1" "key super+z"
done
for k in Return space a; do
    case_ "key '$k' in intro -> warp to orbit" "intro (warped)" "key super+z" "sleep 2" "key $k" "sleep 1.2" "check-overlay 1" "key super+z"
done
# 开场里鼠标默认休眠: 第一次点击只唤醒, 第二次才快进
case_ "click in intro -> warp to orbit" "intro (warped)" "key super+z" "sleep 2" "click 1" "sleep .3" "click 1" "sleep 1.2" "check-overlay 1" "key super+z"
case_ "Super+Z in intro -> return" "end: from return restore 1" "key super+z" "sleep 2" "key super+z"
case_ "orbit: typing 'a', then Super+Z -> return" "end: from return restore 1" "key super+z" "sleep 9.5" "key a" "sleep 1" "check-overlay 1" "key super+z"
case_ "orbit: Esc -> collapse; Esc again -> rest" "end: from rest restore 1" "key super+z" "sleep 9.5" "key Escape" "sleep 0.5" "key Escape" "sleep 0.5" "check-overlay 1" "key super+z"
case_ "orbit: Esc -> collapse; Super+Z -> cancel" "end: from collapse restore 1" "key super+z" "sleep 9.5" "key Escape" "sleep 0.6" "key super+z"
case_ "orbit: Super+Z -> return; Esc -> end now" "end: from return restore 1" "key super+z" "sleep 9.5" "key super+z" "sleep 0.5" "key Escape"
case_ "rest: click -> restore" "end: from rest restore 1" "key super+z" "sleep 9.5" "key Escape" "sleep 2.5" "check-overlay 1" "click 1"
case_ "rest: other key -> restore" "end: from rest restore 1" "key super+z" "sleep 9.5" "key Escape" "sleep 2.5" "key Return"
echo "passed $pass failed $fail"
[ $fail = 0 ]

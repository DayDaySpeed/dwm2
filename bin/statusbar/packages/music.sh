#! /bin/bash
# music 脚本: 通过 playerctl (MPRIS) 显示/控制正在播放的音乐 (SPlayer、浏览器、Spotify 等)

tempfile=$(cd $(dirname $0);cd ..;pwd)/temp

this=_music
icon_color="^c#3B102B^^b#6873790x88^"
text_color="^c#3B102B^^b#6873790x99^"
signal=$(echo "^s$this^" | sed 's/_//')
maxlen=30

# 选择要显示的播放器: 正在播放的优先, 其次暂停的; 同状态下音乐软件优先于浏览器
pick_player() {
    command -v playerctl >/dev/null || return
    local p s best="" best_score=0 score
    for p in $(playerctl -l 2>/dev/null); do
        s=$(playerctl -p "$p" status 2>/dev/null)
        case $s in Playing) score=4 ;; Paused) score=2 ;; *) continue ;; esac
        case $p in chromium*|chrome*|edge*|firefox*|brave*) ;; *) score=$((score + 1)) ;; esac
        [ $score -gt $best_score ] && best=$p && best_score=$score
    done
    echo "$best"
}

update() {
    local player text status
    player=$(pick_player)
    if [ -n "$player" ]; then
        text=$(playerctl -p "$player" metadata --format '{{artist}} - {{title}}' 2>/dev/null | sed 's/^ - //')
        status=$(playerctl -p "$player" status 2>/dev/null)
    fi

    sed -i '/^export '$this'=.*$/d' $tempfile
    [ -z "$text" ] && return
    [ ${#text} -gt $maxlen ] && text="${text:0:$maxlen}…"
    text=$(printf '%s' "$text" | sed "s/'/'\\\\''/g")   # 单引号转义, 以便写入 temp 后被 source
    icon=" 󰝚 "; [ "$status" = Paused ] && icon=" 󰐎 "
    printf "export %s='%s%s%s%s %s '\n" $this "$signal" "$icon_color" "$icon" "$text_color" "$text" >> $tempfile
}

click() {
    local player; player=$(pick_player)
    if [ -n "$player" ]; then
        case "$1" in
            L|R) playerctl -p "$player" play-pause ;;
            U)   playerctl -p "$player" previous ;;
            D)   playerctl -p "$player" next ;;
        esac
    fi >/dev/null 2>&1
}

case "$1" in
    click) click $2 ;;
    *) update ;;
esac

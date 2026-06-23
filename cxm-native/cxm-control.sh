#!/bin/bash
# CXM remote control — tab/search/raise commands via xdotool
export DISPLAY=:0
export XAUTHORITY=/home/dakboard/.Xauthority

WIN=$(xdotool search --class "cxm-leads" 2>/dev/null | head -1)
[ -z "$WIN" ] && WIN=$(xdotool search --name "CXM Leads" 2>/dev/null | head -1)

if [ -z "$WIN" ]; then
    echo "error: CXM app window not found"
    exit 1
fi

# Get window position for absolute clicks
GEOM=$(xdotool getwindowgeometry "$WIN" 2>/dev/null)
WX=$(echo "$GEOM" | grep Position | awk '{print $2}' | cut -d, -f1)
WY=$(echo "$GEOM" | grep Position | awk '{print $2}' | cut -d, -f2)

focus_app() {
    xdotool windowraise "$WIN"
    # Click centre of content area to ensure keyboard focus lands on the notebook
    xdotool mousemove $((WX + 960)) $((WY + 500))
    xdotool click 1
    sleep 0.15
}

cmd="$1"
arg="$2"

case "$cmd" in
  tab)
    TAB="${arg:-0}"
    focus_app
    # Go to first tab, then advance
    for i in 1 2 3 4; do
        xdotool key ctrl+Prior
        sleep 0.04
    done
    for i in $(seq 1 "$TAB"); do
        xdotool key ctrl+Next
        sleep 0.04
    done
    echo "ok: tab $TAB"
    ;;

  search)
    xdotool windowraise "$WIN"
    # Click the search entry (165px from left, 30px from window top in header)
    xdotool mousemove $((WX + 165)) $((WY + 30))
    xdotool click 1
    sleep 0.15
    xdotool key ctrl+a
    xdotool key Delete
    if [ -n "$arg" ]; then
        xdotool type --clearmodifiers --delay 30 "$arg"
    fi
    echo "ok: search '$arg'"
    ;;

  clear)
    xdotool windowraise "$WIN"
    xdotool mousemove $((WX + 165)) $((WY + 30))
    xdotool click 1
    sleep 0.15
    xdotool key ctrl+a
    xdotool key Delete
    echo "ok: cleared"
    ;;

  raise)
    xdotool windowraise "$WIN"
    echo "ok: raised"
    ;;

  status)
    echo "ok: window=$WIN pos=${WX},${WY}"
    ;;
esac

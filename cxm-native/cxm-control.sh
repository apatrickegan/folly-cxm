#!/bin/bash
# CXM remote control — sends commands via Unix socket to the GTK app
SOCK="/tmp/cxm-control.sock"

if [ ! -S "$SOCK" ]; then
    echo "error: CXM socket not found (app not running?)"
    exit 1
fi

cmd="$1"
arg="$2"

case "$cmd" in
  tab)
    echo "tab ${arg:-0}" | nc -q1 -U "$SOCK"
    echo "ok: tab ${arg:-0}"
    ;;
  search)
    echo "search $arg" | nc -q1 -U "$SOCK"
    echo "ok: search '$arg'"
    ;;
  clear)
    echo "clear" | nc -q1 -U "$SOCK"
    echo "ok: cleared"
    ;;
  refresh)
    echo "refresh" | nc -q1 -U "$SOCK"
    echo "ok: refresh"
    ;;
  raise)
    export DISPLAY=:0; export XAUTHORITY=/home/dakboard/.Xauthority
    WIN=$(xdotool search --class "cxm-leads" 2>/dev/null | head -1)
    [ -n "$WIN" ] && xdotool windowraise "$WIN"
    echo "ok: raised"
    ;;
  status)
    if [ -S "$SOCK" ]; then echo "ok: socket ready"; else echo "error: no socket"; fi
    ;;
esac

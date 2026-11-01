#!/bin/sh
# KUAL returns immediately while the guarded launcher owns the session.
app_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd) || exit 1
mkdir -p "$app_dir/data"
nohup sh "$app_dir/run.sh" >> "$app_dir/data/moonlight.log" 2>&1 < /dev/null &

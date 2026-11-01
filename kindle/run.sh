#!/bin/sh
set -u
app_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd) || exit 1
mkdir -p "$app_dir/data"
printf '\nMoonlight 0.1: checking installation in %s\n' "$app_dir"
missing=0
for required in runtime/ld-linux-armhf.so.3 bin/moonlight-kindle runtime/lib/libfbink.so.1 runtime/lib/libgtk-x11-2.0.so.0 runtime/lib/libc.so.6 runtime/fonts/DejaVuSans.ttf; do
    if [ ! -s "$app_dir/$required" ]; then
        printf 'INCOMPLETE INSTALLATION: missing %s\n' "$app_dir/$required" >&2
        missing=1
    fi
done
if [ "$missing" -ne 0 ]; then
    printf 'Extract the COMPLETE package to the Kindle USB root and merge folders. Preserve moonlight/data.\n' >&2
    exit 1
fi
# USB filesystems may ignore chmod; the complete archive stores executable modes.
chmod +x "$app_dir/runtime/ld-linux-armhf.so.3" "$app_dir/bin/moonlight-kindle" 2>/dev/null || :
# A PID check also recovers after a reboot or an interrupted previous session.
if [ -f "$app_dir/data/pid" ]; then
    read -r old_pid < "$app_dir/data/pid"
    case "$old_pid" in *[!0-9]*|'') ;; *) if kill -0 "$old_pid" 2>/dev/null; then exit 0; fi ;; esac
fi
printf '%s\n' "$$" > "$app_dir/data/pid"
saved_orientation=$(lipc-get-prop com.lab126.winmgr orientationLock 2>/dev/null || :)
case "$saved_orientation" in U|D|L|R) ;; *) saved_orientation=U ;; esac
cleanup() {
    lipc-set-prop com.lab126.winmgr orientationLock "$saved_orientation" 2>/dev/null || :
    lipc-set-prop com.lab126.powerd preventScreenSaver 0 2>/dev/null || :
    rm -f "$app_dir/data/pid"
}
trap cleanup EXIT
trap 'test -z "${child_pid:-}" || kill -TERM "$child_pid" 2>/dev/null; exit 0' HUP INT TERM
export DISPLAY=${DISPLAY:-:0}
# Keep pairing identity outside the replaceable extension directory.
persistent_data=${MOONLIGHT_DATA_DIR:-/mnt/us/moonlight-data}
if ! mkdir -p "$persistent_data"; then
    printf 'Could not create the credentials directory: %s\n' "$persistent_data" >&2
    exit 1
fi
chmod 700 "$persistent_data" 2>/dev/null || :
if [ ! -e "$persistent_data/keys" ] && [ -d "$app_dir/data/keys" ]; then
    cp -R "$app_dir/data/keys" "$persistent_data/keys" || exit 1
fi
if [ ! -e "$persistent_data/settings.ini" ] && [ -f "$app_dir/data/settings.ini" ]; then
    cp "$app_dir/data/settings.ini" "$persistent_data/settings.ini" || exit 1
fi
export MOONLIGHT_DATA_DIR="$persistent_data"
cat > "$app_dir/data/fonts.conf" <<FONTCONF
<?xml version="1.0"?>
<fontconfig><dir>$app_dir/runtime/fonts</dir><cachedir>$app_dir/data/font-cache</cachedir>
<alias><family>sans-serif</family><prefer><family>DejaVu Sans</family></prefer></alias></fontconfig>
FONTCONF
export FONTCONFIG_FILE="$app_dir/data/fonts.conf"
export FONTCONFIG_PATH="$app_dir/runtime"
export GTK2_RC_FILES="$app_dir/runtime/gtkrc"
export GTK_MODULES=""
export GDK_PIXBUF_MODULE_FILE="$app_dir/runtime/loaders.cache"
export LC_ALL=C
lipc-set-prop com.lab126.powerd preventScreenSaver 1 2>/dev/null || :
"$app_dir/runtime/ld-linux-armhf.so.3" --library-path "$app_dir/runtime/lib" "$app_dir/bin/moonlight-kindle" --kindle &
child_pid=$!
wait "$child_pid"

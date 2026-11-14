#!/bin/sh
# Build into dist/ and generate the app icons.
#   dist/FanControl      Linux x86_64 (needs only glibc + GTK 3 at runtime)
#   dist/FanControl.exe  Windows x64 (static; needs PawnIO and LpcIO.bin, see README)
#
# Build requirements (Ubuntu): sudo apt install gcc libgtk-3-dev gcc-mingw-w64-x86-64
# The Windows build is skipped when MinGW is not installed.
set -e
cd "$(dirname "$0")"
mkdir -p dist assets

echo "== Linux"
gcc -O2 -Wall -Wextra -o dist/FanControl src/fan_control_linux.c \
    $(pkg-config --cflags --libs gtk+-3.0) -lm -Wl,--as-needed
strip dist/FanControl
dist/FanControl --make-icon assets/icon-256.png

if command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
    echo "== Windows"
    # A .ico file can hold the PNG as it is (Windows Vista and later).
    python3 - <<'PY'
import struct
png = open('assets/icon-256.png', 'rb').read()
open('assets/icon.ico', 'wb').write(
    struct.pack('<HHH', 0, 1, 1) + struct.pack('<BBBBHHII', 0, 0, 0, 0, 1, 32, len(png), 22) + png)
PY
    x86_64-w64-mingw32-windres -I src src/app.rc -O coff -o src/app.res
    x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -municode -mwindows -static -s \
        -o dist/FanControl.exe src/fan_control_win.c src/app.res \
        -lcomctl32 -lshell32 -lntdll
    rm -f src/app.res
else
    echo "== Windows: skipped (x86_64-w64-mingw32-gcc not found)"
fi

ls -l dist assets

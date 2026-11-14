#!/bin/sh
# Install Fan Control for the current user (no root needed).
#
#   ./install.sh            install (uses dist/FanControl, or ./FanControl)
#   ./install.sh uninstall  remove it again
set -e
cd "$(dirname "$0")"

BIN_DIR="$HOME/.local/bin"
APP_DIR="$HOME/.local/share/applications"
ICON_DIR="$HOME/.local/share/icons/hicolor/256x256/apps"
DESKTOP="$APP_DIR/io.github.ideapadfancontrol.desktop"

if [ "$1" = "uninstall" ]; then
    rm -f "$BIN_DIR/FanControl" "$DESKTOP" "$ICON_DIR/ideapad-fan-control.png"
    update-desktop-database "$APP_DIR" 2>/dev/null || true
    echo "Removed. (The boot service, if any, is turned off by choosing Automatic first.)"
    exit 0
fi

for f in dist/FanControl ./FanControl; do
    [ -f "$f" ] && BIN="$f" && break
done
if [ -z "$BIN" ]; then
    echo "FanControl binary not found. Run ./build.sh first." >&2
    exit 1
fi

mkdir -p "$BIN_DIR" "$APP_DIR" "$ICON_DIR"
install -m 755 "$BIN" "$BIN_DIR/FanControl"
[ -f assets/icon-256.png ] && install -m 644 assets/icon-256.png "$ICON_DIR/ideapad-fan-control.png"

cat > "$DESKTOP" <<EOF
[Desktop Entry]
Type=Application
Name=Fan Control
Comment=Choose the cooling fan mode of a Lenovo IdeaPad
Exec=$BIN_DIR/FanControl
Icon=ideapad-fan-control
Terminal=false
Categories=Settings;HardwareSettings;
Keywords=fan;cooling;lenovo;ideapad;temperature;silent;
StartupWMClass=FanControl
EOF
update-desktop-database "$APP_DIR" 2>/dev/null || true
gtk-update-icon-cache -q "$HOME/.local/share/icons/hicolor" 2>/dev/null || true

echo "Installed. Open \"Fan Control\" from your app menu, or run: $BIN_DIR/FanControl"

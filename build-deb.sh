#!/bin/sh
# Build a Debian/Ubuntu package of the Linux app into dist/.
#
#   ./build-deb.sh            version from the latest git tag (v2.2.0 -> 2.2.0)
#   ./build-deb.sh 2.3.0      explicit version
#
# Build requirements (Ubuntu): sudo apt install gcc libgtk-3-dev dpkg-dev fakeroot
# The package installs:
#   /usr/bin/FanControl
#   /usr/share/applications/io.github.ideapadfancontrol.desktop
#   /usr/share/icons/hicolor/256x256/apps/ideapad-fan-control.png
#   /usr/share/ideapad-fan-control/kmod/   (source of the optional kernel driver;
#                                           run: sudo /usr/share/ideapad-fan-control/kmod/install.sh)
set -e
cd "$(dirname "$0")"
umask 022

VERSION=${1:-$(git describe --tags --abbrev=0 2>/dev/null | sed 's/^v//')}
[ -n "$VERSION" ] || { echo "No version given and no git tag found." >&2; exit 1; }
PKG=ideapad-fan-control
ARCH=amd64
BUILD=build/deb
ROOT=$BUILD/${PKG}_${VERSION}_$ARCH

rm -rf "$BUILD"
mkdir -p "$ROOT/DEBIAN" "$ROOT/usr/bin" "$ROOT/usr/share/applications" \
         "$ROOT/usr/share/icons/hicolor/256x256/apps" \
         "$ROOT/usr/share/ideapad-fan-control/kmod" \
         "$ROOT/usr/share/doc/$PKG" dist

echo "== Compiling"
gcc -O2 -Wall -Wextra -o "$ROOT/usr/bin/FanControl" src/fan_control_linux.c \
    $(pkg-config --cflags --libs gtk+-3.0) -lm -Wl,--as-needed
strip "$ROOT/usr/bin/FanControl"
"$ROOT/usr/bin/FanControl" --make-icon "$ROOT/usr/share/icons/hicolor/256x256/apps/ideapad-fan-control.png"

cat > "$ROOT/usr/share/applications/io.github.ideapadfancontrol.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Fan Control
Comment=Choose the cooling fan mode of a Lenovo IdeaPad
Exec=/usr/bin/FanControl
Icon=ideapad-fan-control
Terminal=false
Categories=Settings;HardwareSettings;
Keywords=fan;cooling;lenovo;ideapad;temperature;silent;
StartupWMClass=FanControl
EOF

install -m 644 kmod/Makefile kmod/ideapad_fan_pwm.c "$ROOT/usr/share/ideapad-fan-control/kmod/"
install -m 755 kmod/install.sh "$ROOT/usr/share/ideapad-fan-control/kmod/"
install -m 644 README.md "$ROOT/usr/share/doc/$PKG/"
install -m 644 LICENSE "$ROOT/usr/share/doc/$PKG/copyright"

echo "== Dependencies"
mkdir -p "$BUILD/debian"
: > "$BUILD/debian/control"
DEPS=$(cd "$BUILD" && dpkg-shlibdeps -O "${ROOT#$BUILD/}/usr/bin/FanControl" 2>/dev/null \
       | sed -n 's/^shlibs:Depends=//p')
[ -n "$DEPS" ] || DEPS="libc6, libgtk-3-0t64 | libgtk-3-0"

SIZE=$(du -sk "$ROOT" | cut -f1)
cat > "$ROOT/DEBIAN/control" <<EOF
Package: $PKG
Version: $VERSION
Section: utils
Priority: optional
Architecture: $ARCH
Installed-Size: $SIZE
Depends: $DEPS, pkexec | policykit-1
Recommends: systemd
Suggests: build-essential, linux-headers-generic, mokutil, sbsigntool
Maintainer: richardyusan <richardyusan@users.noreply.github.com>
Homepage: https://github.com/richardyusan/ideapad-fan-control
Description: Fan speed control for the Lenovo IdeaPad 3 14ALC6
 A small GTK 3 app and command-line tool (FanControl) to choose the cooling
 fan mode of a Lenovo IdeaPad: automatic, a manual speed from 30 to 100
 percent, or dust cleaning. A manual speed is kept across reboots by the
 ideapad-fan-control systemd service the app sets up.
 .
 For a steady fan speed with Secure Boot on, build and sign the optional
 kernel driver with: sudo /usr/share/ideapad-fan-control/kmod/install.sh
EOF

cat > "$ROOT/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
if [ "$1" = "configure" ]; then
    command -v update-desktop-database >/dev/null && update-desktop-database -q /usr/share/applications || true
    command -v gtk-update-icon-cache >/dev/null && gtk-update-icon-cache -q /usr/share/icons/hicolor || true
fi
exit 0
EOF
cat > "$ROOT/DEBIAN/postrm" <<'EOF'
#!/bin/sh
set -e
if [ "$1" = "remove" ] || [ "$1" = "purge" ]; then
    command -v update-desktop-database >/dev/null && update-desktop-database -q /usr/share/applications || true
    command -v gtk-update-icon-cache >/dev/null && gtk-update-icon-cache -q /usr/share/icons/hicolor || true
fi
exit 0
EOF
cat > "$ROOT/DEBIAN/prerm" <<'EOF'
#!/bin/sh
# Stop the boot service the app may have created (it runs a copy of the
# binary from /usr/local/libexec, so it would otherwise keep running).
set -e
if [ "$1" = "remove" ] && command -v systemctl >/dev/null; then
    systemctl disable --now ideapad-fan-control.service >/dev/null 2>&1 || true
fi
exit 0
EOF
chmod 755 "$ROOT/DEBIAN/postinst" "$ROOT/DEBIAN/postrm" "$ROOT/DEBIAN/prerm"

echo "== Packaging"
OUT=dist/${PKG}_${VERSION}_$ARCH.deb
fakeroot dpkg-deb --build -Zxz "$ROOT" "$OUT" >/dev/null
rm -rf build
dpkg-deb --info "$OUT"
ls -l "$OUT"

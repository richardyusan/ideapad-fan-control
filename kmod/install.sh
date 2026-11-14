#!/bin/sh
# Builds, signs and installs the ideapad_fan_pwm kernel driver, so Fan
# Control can hold a steady fan speed with Secure Boot ON.
#
#   sudo kmod/install.sh             build, sign, install, load at boot
#   sudo kmod/install.sh uninstall   remove the driver (the signing key stays)
#
# Signing uses a Machine Owner Key (MOK), the same mechanism Ubuntu uses for
# DKMS drivers such as NVIDIA's. If this machine has no MOK yet, one is
# created in /var/lib/shim-signed/mok/ (readable by root only). The key must
# be enrolled once: this script asks you to choose a one-time password, and
# on the next reboot the blue "MOK management" screen asks for it.
#
# Run this again after a kernel update (the driver is built per kernel).
# Until then Fan Control still works, with the pulsing fallback.
set -eu
PATH=/usr/sbin:/usr/bin:/sbin:/bin
cd "$(dirname "$0")"

NAME=ideapad_fan_pwm
KVER=$(uname -r)
DEST=/lib/modules/$KVER/updates
AUTOLOAD=/etc/modules-load.d/ideapad-fan-pwm.conf
MOKDIR=/var/lib/shim-signed/mok
KEY=$MOKDIR/MOK.priv
CERT=$MOKDIR/MOK.der

if [ "$(id -u)" -ne 0 ]; then
    echo "Run this with sudo: sudo $0 $*" >&2
    exit 1
fi

if [ "${1:-}" = "uninstall" ]; then
    systemctl stop ideapad-fan-control.service 2>/dev/null || true
    rmmod $NAME 2>/dev/null || true
    rm -f "$DEST/$NAME.ko" "$AUTOLOAD"
    depmod -a
    echo "Removed the $NAME driver. The signing key in $MOKDIR was left in place."
    exit 0
fi

if [ ! -d "/lib/modules/$KVER/build" ]; then
    echo "Kernel headers are missing. Install them first:" >&2
    echo "  sudo apt install linux-headers-$KVER" >&2
    exit 1
fi

echo "== Building for kernel $KVER"
make -s clean >/dev/null 2>&1 || true
make -s 2>&1 | grep -vE 'warning: (the compiler|pahole)|The kernel was built|You are using|Skipping BTF' || true
[ -f $NAME.ko ] || { echo "Build failed." >&2; exit 1; }

echo "== Signing"
if [ ! -f "$KEY" ] || [ ! -f "$CERT" ]; then
    echo "No Machine Owner Key yet; creating one in $MOKDIR"
    mkdir -p "$MOKDIR"
    openssl req -new -x509 -newkey rsa:2048 -nodes -days 36500 \
        -subj "/CN=$(hostname) Secure Boot Module Signature key/" \
        -addext "extendedKeyUsage=codeSigning" \
        -keyout "$KEY" -outform DER -out "$CERT" 2>/dev/null
    chmod 600 "$KEY"
fi
kmodsign sha512 "$KEY" "$CERT" $NAME.ko

echo "== Installing"
install -D -m 644 $NAME.ko "$DEST/$NAME.ko"
depmod -a
echo $NAME > "$AUTOLOAD"

ENROLLED=0
mokutil --test-key "$CERT" 2>/dev/null | grep -q "already enrolled" && ENROLLED=1

if [ "$ENROLLED" = 0 ]; then
    echo
    echo "== Enrolling the signing key (one time)"
    echo "Choose a one-time password now. You will type it once more on the"
    echo "blue MOK management screen at the next reboot."
    mokutil --import "$CERT"
fi

rmmod $NAME 2>/dev/null || true
if modprobe $NAME 2>/dev/null; then
    echo
    echo "The driver is loaded now."
else
    echo
    echo "The driver is installed but could not be loaded yet (the key is not enrolled)."
fi

if [ "$ENROLLED" = 0 ]; then
    cat <<EOF

Next steps:
  1. Reboot. On the blue "Perform MOK management" screen choose:
       Enroll MOK -> Continue -> Yes -> type the password -> Reboot
  2. You can turn Secure Boot back on in the BIOS (F2 -> Security), now or
     later. The signed driver loads either way.
EOF
fi

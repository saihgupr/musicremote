#!/bin/bash
# Installs the newest built .deb on a device and restarts Music so the tweak
# loads. Build first: THEOS=/opt/theos make package
#
# Usage: ./deploy.sh [iphone|ipad] [--no-install]
# Privileged work (dpkg) needs the device's mobile password in RC_PASSWORD,
# same variable RemoteCompanion's deploy.sh uses. ssh itself uses your key.
set -uo pipefail

HOST="iphone"
NO_INSTALL=0
for arg in "$@"; do
    case "$arg" in
        iphone|ipad) HOST="$arg" ;;
        --no-install) NO_INSTALL=1 ;;
        *) echo "usage: ./deploy.sh [iphone|ipad] [--no-install]"; exit 1 ;;
    esac
done
[ "$HOST" = "ipad" ] && TARGET=ipad.local || TARGET=iphone.local

DEB=$(ls -t packages/*.deb 2>/dev/null | head -1)
if [ -z "$DEB" ]; then
    echo "no package found, run: THEOS=/opt/theos make package"
    exit 1
fi
if [ "$NO_INSTALL" = "1" ]; then
    echo "built only: $DEB"
    exit 0
fi

PASSWORD="${RC_PASSWORD:-}"
if [ -z "$PASSWORD" ]; then
    echo "RC_PASSWORD is not set (the mobile user's password on $TARGET)"
    exit 1
fi

ssh "$HOST" 'rm -f /tmp/MusicRemote.deb' || exit 1
scp -q "$DEB" "$HOST:/tmp/MusicRemote.deb" || exit 1

# Remote side: run dpkg under sudo, keep dpkg's status even while trimming output.
ssh "$HOST" "printf '%s\n' '$PASSWORD' | sudo -S -p '' /var/jb/usr/bin/dpkg -i /tmp/MusicRemote.deb > /tmp/mr_dpkg.log 2>&1; rc=\$?; tail -3 /tmp/mr_dpkg.log; rm -f /tmp/mr_dpkg.log /tmp/MusicRemote.deb; exit \$rc"
rc=$?
if [ $rc -ne 0 ]; then
    echo "dpkg failed on $HOST (exit $rc)"
    exit $rc
fi

# The CLI moved from /usr/local/bin to /usr/bin; drop the old copy so login
# shells, which search /usr/local/bin first, do not keep running a stale build.
ssh "$HOST" "printf '%s\n' '$PASSWORD' | sudo -S -p '' rm -f /var/jb/usr/local/bin/musicremote; printf '%s\n' '$PASSWORD' | sudo -S -p '' killall -9 SpringBoard 2>/dev/null; true"
sleep 7
echo "installed $DEB on $HOST (resprung)"

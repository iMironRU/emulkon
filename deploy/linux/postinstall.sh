#!/bin/sh
# После установки и обновления пакета emulkon: служба включена и запущена (обновление — перезапуск).
set -e
command -v systemctl > /dev/null 2>&1 || exit 0
[ -d /run/systemd/system ] || exit 0
systemctl daemon-reload || true
# deb: «configure» с прежней версией в $2; rpm: $1 > 1 — обновление.
if { [ "$1" = "configure" ] && [ -n "$2" ]; } || { [ "$1" -gt 1 ] 2> /dev/null; }; then
  systemctl try-restart emulkon.service || true
else
  systemctl enable --now emulkon.service || true
fi

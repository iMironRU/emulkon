#!/bin/sh
# Перед удалением пакета emulkon (не при обновлении): остановить и выключить службу. Данные в /var/lib/emulkon
# остаются — удалить вручную (systemd хранит их в /var/lib/private/emulkon).
set -e
command -v systemctl > /dev/null 2>&1 || exit 0
[ -d /run/systemd/system ] || exit 0
# deb: «remove»; rpm: $1 = 0 — удаление, 1 — обновление.
if [ "$1" = "remove" ] || [ "$1" = "0" ]; then
  systemctl disable --now emulkon.service || true
fi

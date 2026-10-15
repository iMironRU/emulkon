#!/usr/bin/env bash
# check-driver.sh — до 1С: видит ли ВМ сервер Эмулкона и отдаёт ли он драйвер для Linux.
#
#   ./check-driver.sh http://<адрес сервера>:8737
#
# Сервер — служба Эмулкона на компьютере с Hyper-V в режиме «Кассы в сети» (консоль: Служба → Свойства) или
# любой другой сервер. Скачивает пакет драйвера в ~/emulkon, проверяет SHA-256 по каталогу сервера и что в
# пакете есть библиотека Linux x86_64.
set -euo pipefail

server="${1:-}"
[ -n "$server" ] || { echo "использование: $0 http://<сервер>:8737"; exit 2; }
server="${server%/}"
out="$HOME/emulkon"
mkdir -p "$out"

echo "Сервер: $(curl -fsS "$server/api/v1/health")"
catalog="$(curl -fsS "$server/api/v1/downloads")"
pkg_url="$(printf '%s' "$catalog" | grep -o '"driver_package":{[^}]*}' | grep -o '"url":"[^"]*"' | cut -d'"' -f4)"
pkg_sha="$(printf '%s' "$catalog" | grep -o '"driver_package":{[^}]*}' | grep -o '"sha256":"[^"]*"' | cut -d'"' -f4)"
[ -n "$pkg_url" ] || { echo "Сервер не публикует пакет драйвера"; exit 1; }
name="$(basename "$pkg_url")"
curl -fsS -o "$out/$name" "$server$pkg_url"
echo "$pkg_sha  $out/$name" | sha256sum -c -
unzip -o -q "$out/$name" -d "$out/package"
grep -q 'os="Linux"' "$out/package/MANIFEST.XML" || { echo "В пакете нет драйвера для Linux (сервер старше 0.9.0?)"; exit 1; }
so="$(ls "$out"/package/libEmulkonDriver_*_lin64.so)"
echo "Драйвер Linux: $so"
file "$so" 2>/dev/null || true
if ldd "$so" | grep -q 'not found'; then
  ldd "$so"
  echo "Не хватает системных библиотек"
  exit 1
fi
echo
echo "Пакет для 1С: $out/$name — «Драйверы оборудования» → «Добавить новый драйвер из файла»."

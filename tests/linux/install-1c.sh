#!/usr/bin/env bash
# install-1c.sh — платформа 1С:Предприятие в тестовой ВМ Linux из ваших дистрибутивов (ИТС).
#
#   sudo ./install-1c.sh <каталог с дистрибутивом>
#
# Понимает оба вида поставки: setup-full-<версия>-x86_64.run (8.3.20 и новее) — ставит тонкий и толстый
# клиент без вопросов; или deb-пакеты 1c-enterprise-* (старые версии). Лицензирование — ваше (README.md).
set -euo pipefail

dir="${1:-}"
[ -n "$dir" ] && [ -d "$dir" ] || { echo "использование: sudo $0 <каталог с дистрибутивом 1С>"; exit 2; }
[ "$(id -u)" -eq 0 ] || { echo "нужен sudo"; exit 2; }

run="$(find "$dir" -maxdepth 2 -name 'setup-full-*-x86_64.run' | sort | tail -n 1)"
if [ -n "$run" ]; then
  echo "Установщик: $run"
  chmod +x "$run"
  "$run" --mode unattended --enable-components client_full,client_thin,ru
elif ls "$dir"/*.deb > /dev/null 2>&1; then
  echo "Пакеты: $dir/*.deb"
  apt-get update -qq
  apt-get install -y "$dir"/*.deb
else
  echo "В $dir нет ни setup-full-*-x86_64.run, ни .deb"
  exit 1
fi

# Шрифты и библиотеки, без которых клиент 1С на «голой» Ubuntu не стартует или рисует квадраты.
apt-get install -y -qq fonts-dejavu ttf-mscorefonts-installer libgtk-3-0 > /dev/null 2>&1 || true

echo "Готово. Клиент: /opt/1cv8/x86_64/<версия>/1cv8c (тонкий), 1cv8 (толстый)."

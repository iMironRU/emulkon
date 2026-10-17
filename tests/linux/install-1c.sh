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
  # v8_install_deps — зависимости платформы; server — ради ibcmd (база из .dt без клиента), служба не нужна.
  "$run" --mode unattended --enable-components client_full,client_thin,server,v8_install_deps,ru
  systemctl disable --now "srv1cv8-*" > /dev/null 2>&1 || true
elif ls "$dir"/*.deb > /dev/null 2>&1; then
  echo "Пакеты: $dir/*.deb"
  apt-get update -qq
  apt-get install -y "$dir"/*.deb
else
  echo "В $dir нет ни setup-full-*-x86_64.run, ни .deb"
  exit 1
fi

# Платформа кладёт рядом свою libstdc++ (8.5.1 — от GCC 10): на Ubuntu 24.04 она перекрывает системную, новее,
# и клиент не стартует («GLIBCXX_3.4.30 not found» у webkit2gtk, icu). Встроенную отключаем — берётся системная.
for d in /opt/1cv8/x86_64/*/; do
  if [ -L "$d/libstdc++.so.6" ] && ldd "$d/1cv8c" 2>&1 | grep -q "GLIBCXX.*not found"; then
    mv "$d/libstdc++.so.6" "$d/libstdc++.so.6.bundled-off"
    echo "Отключена встроенная libstdc++: $d"
  fi
done

# Шрифты и библиотеки, без которых клиент 1С на «голой» Ubuntu не стартует или рисует квадраты.
apt-get install -y -qq fonts-dejavu ttf-mscorefonts-installer libgtk-3-0 > /dev/null 2>&1 || true

echo "Готово. Клиент: /opt/1cv8/x86_64/<версия>/1cv8c (тонкий), 1cv8 (толстый)."

#!/usr/bin/env bash
# screen.sh — экран :99 в ВМ для 1С: Xvfb, openbox, x11vnc и noVNC. Всё слушает только localhost ВМ: с компьютера
# экран открывается через туннель SSH (run-1c.ps1 -Screen → http://localhost:6080/vnc.html). Нужен человеку один раз —
# активировать лицензию разработчика 1С (логин developer.1c.ru вводит он) и подтвердить «Предупреждение безопасности».
#
#   bash screen.sh          # запустить (уже запущенное не трогает)
#   bash screen.sh stop     # остановить
set -euo pipefail
export DISPLAY=:99

if [ "${1:-}" = "stop" ]; then
  pkill -f "websockify.*6080" || true
  pkill -x x11vnc || true
  pkill -x openbox || true
  pkill -x Xvfb || true
  exit 0
fi

start() {  # start <процесс для pgrep> <лог> <команда…>: отдельно от сеанса SSH, переживает его закрытие
  local name="$1" log="$2"
  shift 2
  pgrep -f "$name" > /dev/null || setsid -f "$@" > "$log" 2>&1 < /dev/null
}
start "Xvfb :99" /tmp/emulkon-xvfb.log Xvfb :99 -screen 0 1600x1000x24 -nolisten tcp
for _ in $(seq 1 20); do xdpyinfo > /dev/null 2>&1 && break; sleep 0.2; done
start "^openbox" /tmp/emulkon-openbox.log openbox
start "x11vnc -display :99" /tmp/emulkon-x11vnc.log x11vnc -display :99 -localhost -nopw -forever -shared -quiet -noxdamage  # без XDAMAGE — иначе картинка с Xvfb застывает
start "websockify.*6080" /tmp/emulkon-novnc.log websockify --web /usr/share/novnc 127.0.0.1:6080 127.0.0.1:5900
sleep 1
pgrep -a Xvfb | head -1
pgrep -a x11vnc | head -1
pgrep -af "websockify.*6080" | head -1

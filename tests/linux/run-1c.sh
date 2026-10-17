#!/usr/bin/env bash
# run-1c.sh — прогон обработки ТестЭмулкона в 1С под Linux (в ВМ; запускает tests/linux/run-1c.ps1 с компьютера).
# То же, что tests/1c/run.ps1 на Windows: тонкий клиент, /Execute обработки, параметры — в /C. Экран — :99
# (screen.sh), его видно через noVNC.
#
#   bash run-1c.sh <платформа> <база> <пользователь> <обработка> <параметр /C> <отчёт> <таймаут, с>
#
# Сообщение «Внешняя компонента успешно установлена» (одна кнопка) закрывается, когда обработка поставила метку
# <отчёт>.status — как на Windows. Остальные окна не трогаем: «Предупреждение безопасности» и активацию лицензии
# подтверждает человек на экране. Не закрылась за таймаут — снимок экрана <отчёт>.png и код 124.
set -uo pipefail
platform="$1" base="$2" user="$3" epf="$4" param="$5" report="$6" timeout="${7:-600}"
export DISPLAY=:99
rm -f "$report" "$report.status" "$report.png" "$report.out"

# Сеанс D-Bus — для GTK/WebKit клиента (в сеансе SSH его нет).
dbus-run-session -- "/opt/1cv8/x86_64/$platform/1cv8c" ENTERPRISE "/F$base" "/N$user" "/Execute$epf" "/C$param" \
  /DisableStartupMessages /DisableStartupDialogs "/Out$report.out" &
pid=$!
deadline=$((SECONDS + timeout))
while kill -0 "$pid" 2> /dev/null; do
  # окна ищем у самого клиента (pid выше — dbus-run-session)
  cpid=$(pgrep -P "$pid" -x 1cv8c || pgrep -x 1cv8c | head -1)
  if [ $SECONDS -ge $deadline ]; then
    import -window root -display :99 "$report.png" 2> /dev/null || true
    echo "1С не закрылась за $timeout с — снимок экрана: $report.png; окна:"
    xdotool search --onlyvisible --pid "${cpid:-$pid}" --name . getwindowname %@ 2> /dev/null || true
    exit 124
  fi
  if [ -f "$report.status" ]; then
    for w in $(xdotool search --onlyvisible --pid "${cpid:-$pid}" --name . 2> /dev/null); do
      read -r width height < <(xdotool getwindowgeometry "$w" 2> /dev/null | awk '/Geometry/ {split($2, g, "x"); print g[1], g[2]}')
      if [ -n "${width:-}" ] && [ "$width" -lt 450 ] && [ "$height" -lt 200 ] && [ "$width" -gt 100 ]; then
        echo "закрываю сообщение: $(xdotool getwindowname "$w")"
        xdotool windowclose "$w" 2> /dev/null || true
      fi
    done
  fi
  sleep 2
done
wait "$pid"
code=$?
[ -f "$report.out" ] && iconv -f utf-8 -t utf-8 -c "$report.out" | tail -n 20
exit $code

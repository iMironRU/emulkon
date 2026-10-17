#!/usr/bin/env bash
# linux-driver.sh — драйвер для Linux в CI: сборка libEmulkonDriver_lin64.so, модульные тесты и стенд «как
# платформа 1С» без сервера и с живым сервером эмуляции (собирается здесь же из server/).
# Запускается в контейнере manylinux_2_28 (glibc 2.28): собранная там .so работает и на дистрибутивах новее.
#
# Нужны: ONEC_SDK_B64 (zip заголовков SDK Native API 1С в base64; без него — пропуск), go в PATH.
# Результат: dist-linux/libEmulkonDriver_lin64.so (x86_64) или libEmulkonDriver_linarm64.so (aarch64, issue #12).
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
out="$root/dist-linux"
mkdir -p "$out"

if [ -z "${ONEC_SDK_B64:-}" ]; then
  echo "::notice::Секрета ONEC_SDK_B64 нет — драйвер Linux не собирается"
  exit 0
fi

work="$(mktemp -d)"
trap 'kill "${server_pid:-0}" 2>/dev/null || true; rm -rf "$work"' EXIT

# SDK: только символы base64 (секрет мог быть записан с BOM и переводами строк).
printf '%s' "$ONEC_SDK_B64" | tr -cd 'A-Za-z0-9+/=' | base64 -d > "$work/sdk.zip"
mkdir -p "$work/sdk"
unzip -q "$work/sdk.zip" -d "$work/sdk"
sdk="$(dirname "$(find "$work/sdk" -name ComponentBase.h | head -n 1)")"
[ -f "$sdk/ComponentBase.h" ] || { echo "В секрете ONEC_SDK_B64 нет ComponentBase.h"; exit 1; }

# Checkout в контейнере принадлежит другому пользователю — git (штамп VCS в go build) иначе отказывается.
git config --global --add safe.directory "$root"

# Сервер эмуляции для стенда: свой порт и временные данные.
(cd "$root/server" && go build -o "$work/emulkon" ./cmd/emulkon)
"$work/emulkon" -addr 127.0.0.1:8799 -data "$work/data" -downloads "$work" > "$work/server.log" 2>&1 &
server_pid=$!
for _ in $(seq 1 50); do
  curl -fsS http://127.0.0.1:8799/api/v1/health > /dev/null 2>&1 && break
  sleep 0.2
done
curl -fsS http://127.0.0.1:8799/api/v1/health > /dev/null || { cat "$work/server.log"; exit 1; }

export EMULKON_TEST_SERVER=http://127.0.0.1:8799
gen=Ninja; command -v ninja > /dev/null || gen="Unix Makefiles"
cmake -S "$root/component" -B "$work/build" -G "$gen" -DCMAKE_BUILD_TYPE=Release "-DONEC_SDK=$sdk"
cmake --build "$work/build"
ctest --test-dir "$work/build" --output-on-failure

so="$work/build/libEmulkonDriver_lin64.so"
# Наружу — только экспорты Native API; зависимости — системные библиотеки, без libstdc++.
nm -D --defined-only "$so" | awk '{print $3}' | sort
if ldd "$so" | grep -q 'libstdc++'; then
  echo "libstdc++ должна быть внутри .so (-static-libstdc++)"
  exit 1
fi
readelf -h "$so" | grep -E 'Class|Machine'

# Драйвер называет серверу свою архитектуру (driver_arch) — по компилятору, а не по разрядности (issue #12).
arch="$(uname -m)"
if ! curl -fsS "http://127.0.0.1:8799/control/v1/journal?method=session.open&limit=50" | grep -q "\"driver_arch\":\"$arch\""; then
  echo "в журнале стенда нет session.open с driver_arch=$arch"
  exit 1
fi
echo "driver_arch на стенде: $arch"

# Имя по архитектуре: x86_64 — libEmulkonDriver_lin64.so (пакет, установщик, релиз), aarch64 — ..._linarm64.so.
name=libEmulkonDriver_lin64.so
[ "$arch" = aarch64 ] && name=libEmulkonDriver_linarm64.so
cp "$so" "$out/$name"
echo "Драйвер Linux ($arch): $out/$name ($(stat -c %s "$so") байт)"

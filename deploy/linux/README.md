# Сервер Эмулкона на Linux (пакет .deb / .rpm)

Пакет `emulkon` ставит сервер эмуляции службой systemd на Linux x86_64: Debian, Ubuntu, Astra (`.deb`); RHEL,
Rocky, РЕД ОС, Альт (`.rpm`). Драйвер 1С для касс (Windows и Linux) — внутри: страница `/downloads` и
самообновление драйверов работают так же, как с установщиком Windows.

```bash
sudo apt install ./emulkon_<версия>-1_amd64.deb      # Debian, Ubuntu, Astra
sudo dnf install ./emulkon-<версия>-1.x86_64.rpm     # RHEL, Rocky, РЕД ОС
```

Служба включается и запускается сразу. Веб-интерфейс — http://127.0.0.1:8737/.

| Что | Где |
|---|---|
| Программа | `/usr/bin/emulkon` |
| Служба | `emulkon.service` (`systemctl status emulkon`), свой пользователь на время работы (`DynamicUser`) |
| Настройки | `/etc/emulkon/emulkon.env` (только root; при обновлении пакета не перезаписываются) |
| Данные: устройства, сценарии, журнал | `/var/lib/emulkon` |
| Лог | `journalctl -u emulkon` |
| Драйвер для касс | `/usr/share/emulkon/driver` → `http://<сервер>:8737/downloads` |

**Кассы в сети.** В `/etc/emulkon/emulkon.env` — `EMULKON_ADDR=0.0.0.0:8737`, затем `sudo systemctl restart emulkon` и
порт 8737 в брандмауэре (`ufw allow 8737/tcp` или `firewall-cmd --add-port=8737/tcp --permanent`). Сервер, открытый
в сеть, закройте токенами: в файл пишется SHA-256 токена (`EMULKON_CONTROL_TOKEN_HASH`, `EMULKON_DEVICE_TOKEN_HASH`),
сам токен — у администратора и в настройках драйвера.

**Удаление** — `sudo apt remove emulkon` / `sudo dnf remove emulkon`: служба останавливается, данные остаются
(`/var/lib/private/emulkon`), `apt purge` убирает и настройки.

## Сборка пакета

`deploy\linux\package.ps1` (Windows или Linux, нужен go; nfpm скачивается `go run` с зафиксированной версией):
сервер под linux/amd64 без cgo, пакет драйвера из `dist\downloads` (сначала `.\build.ps1 -LinuxDriver <.so>`), результат
— `dist\linux\`. Без драйвера — `-NoDriver`. CI собирает пакеты в задаче Windows и ставит их на чистую Ubuntu
(служба, запрос к серверу, удаление) и в контейнер Rocky Linux 9; релиз кладёт оба пакета рядом с установщиком.

Файлы: `nfpm.yaml` (состав и права), `emulkon.service`, `emulkon.env`, `postinstall.sh`, `preremove.sh` — только LF.

# Образ сервера Эмулкон. Сборка из корня репозитория:
#   docker build -f deploy/docker/Dockerfile -t emulkon:dev .
#
# Драйвер для касс (пакет для БПО и библиотеки самообновления Windows и Linux) — из deploy/docker/downloads/:
# CI кладёт туда dist/downloads сборки Windows, локально — скопируйте сами (после .\build.ps1). Пусто — образ
# без драйвера: сервер работает, страница загрузок пустая. Готовый образ — ghcr.io/imironru/emulkon:<версия>.
FROM golang:1.24-alpine AS build
WORKDIR /src
COPY server/go.mod ./
RUN go mod download
COPY server/ ./
COPY VERSION ./VERSION
RUN CGO_ENABLED=0 go build -trimpath -ldflags "-s -w -X main.version=$(cat VERSION)" -o /out/emulkon ./cmd/emulkon

FROM gcr.io/distroless/static-debian12:nonroot
LABEL org.opencontainers.image.title="Эмулкон" \
      org.opencontainers.image.description="ЭМУЛЯТОР подключаемого оборудования 1С:БПО — сервер эмуляции" \
      org.opencontainers.image.source="https://github.com/iMironRU/emulkon" \
      org.opencontainers.image.url="https://imiron.ru/emulkon/" \
      org.opencontainers.image.licenses="Apache-2.0"
COPY --from=build /out/emulkon /emulkon
COPY deploy/docker/downloads/ /downloads/
ENV EMULKON_ADDR=:8737 \
    EMULKON_DATA_DIR=/data \
    EMULKON_DOWNLOADS_DIR=/downloads
VOLUME ["/data"]
EXPOSE 8737
USER nonroot:nonroot
ENTRYPOINT ["/emulkon"]

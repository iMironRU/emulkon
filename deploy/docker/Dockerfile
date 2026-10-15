# Образ сервера Эмулкон. Сборка из корня репозитория:
#   docker build -f deploy/docker/Dockerfile -t emulkon:dev .
FROM golang:1.24-alpine AS build
WORKDIR /src
COPY server/go.mod ./
RUN go mod download
COPY server/ ./
COPY VERSION ./VERSION
RUN CGO_ENABLED=0 go build -trimpath -ldflags "-s -w -X main.version=$(cat VERSION)" -o /out/emulkon ./cmd/emulkon

FROM gcr.io/distroless/static-debian12:nonroot
COPY --from=build /out/emulkon /emulkon
ENV EMULKON_ADDR=:8737 \
    EMULKON_DATA_DIR=/data
VOLUME ["/data"]
EXPOSE 8737
USER nonroot:nonroot
ENTRYPOINT ["/emulkon"]

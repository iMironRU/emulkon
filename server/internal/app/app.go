// Package app собирает сервер эмуляции из модулей (ARCHITECTURE §4.2).
package app

import (
	"context"
	"fmt"
	"log/slog"
	"net/http"
	"path/filepath"
	"time"

	"emulkon/internal/control"
	"emulkon/internal/deviceapi"
	"emulkon/internal/downloads"
	"emulkon/internal/httpx"
	"emulkon/internal/journal"
	"emulkon/internal/models"
	"emulkon/internal/models/acquiring"
	"emulkon/internal/models/display"
	"emulkon/internal/models/input"
	"emulkon/internal/models/kkt"
	"emulkon/internal/models/labelscales"
	"emulkon/internal/models/printers"
	"emulkon/internal/models/recognizer"
	"emulkon/internal/models/rfid"
	"emulkon/internal/models/scales"
	"emulkon/internal/models/tsd"
	"emulkon/internal/scenarios"
	"emulkon/internal/secret"
	"emulkon/internal/sessions"
	"emulkon/web"
)

// Config — параметры сервера.
type Config struct {
	Version             string
	DataDir             string
	DeviceToken         secret.Token
	ControlToken        secret.Token
	DefaultTimeout      time.Duration
	JournalMaxFileBytes int64
	JournalMaxFiles     int
	SessionIdleTTL      time.Duration
	// DownloadsDirs — каталоги дистрибутива (драйвер, установщик); пусто — загрузок нет.
	DownloadsDirs []string
}

// Записи журнала об экране устройства (models.Prompter): трей и веб-интерфейс по ним
// показывают экран терминала с кнопками.
const (
	MethodPrompt     = "device.prompt"
	MethodPromptDone = "device.prompt_done"
)

// App — собранный сервер.
type App struct {
	Handler http.Handler
	Journal *journal.Journal
	Devices *deviceapi.API
	Sess    *sessions.Manager
}

func New(cfg Config) (*App, error) {
	j, err := journal.Open(journal.Options{
		Dir:          filepath.Join(cfg.DataDir, "journal"),
		MaxFileBytes: cfg.JournalMaxFileBytes,
		MaxFiles:     cfg.JournalMaxFiles,
	})
	if err != nil {
		return nil, fmt.Errorf("открыть журнал: %w", err)
	}

	reg := Registry(cfg.Version)
	sess := sessions.NewManager(sessions.Config{
		DeviceToken: cfg.DeviceToken, IdleTTL: cfg.SessionIdleTTL,
		StorePath: filepath.Join(cfg.DataDir, "registry.json"),
		OnPrompt: func(d *sessions.Device, p models.Prompt, done *models.PromptResult) {
			e := journal.Entry{DeviceID: d.ID, WorkstationID: d.WorkstationID, Direction: journal.DirSystem}
			if done == nil {
				e.Method = MethodPrompt
				e.Payload, _ = journal.Marshal(p)
			} else {
				e.Method = MethodPromptDone
				e.Payload, _ = journal.Marshal(map[string]any{"prompt_id": p.ID, "operation": p.Operation, "result": done})
			}
			if _, err := j.Append(e); err != nil {
				slog.Error("запись в журнал", "method", e.Method, "err", err)
			}
		},
		OnEvent: func(d *sessions.Device, e sessions.EventRecord, listeners int) {
			payload, _ := journal.Marshal(map[string]any{"event_id": e.ID, "name": e.Name, "data": e.Data, "listeners": listeners})
			if _, err := j.Append(journal.Entry{DeviceID: d.ID, WorkstationID: d.WorkstationID, Direction: journal.DirEvent,
				Method: e.Name, Payload: payload}); err != nil {
				slog.Error("запись в журнал", "method", e.Name, "err", err)
			}
		},
	}, reg)
	if err := sess.Load(); err != nil {
		j.Close()
		return nil, err
	}
	scen, err := scenarios.New(filepath.Join(cfg.DataDir, "scenarios.json"))
	if err != nil {
		j.Close()
		return nil, err
	}
	dl := downloads.New(downloads.Config{
		Dirs: cfg.DownloadsDirs, Version: cfg.Version, Types: reg.Types(),
		FullModels: map[string]bool{"kkt": true, "acquiring": true, "scales": true, "label_scales": true,
			"receipt_printer": true, "label_printer": true, "scanner": true, "card_reader": true,
			"display": true, "tsd": true, "rfid": true, "recognizer": true},
		Page: web.Page("downloads.html"),
	}, j)
	dev := deviceapi.New(deviceapi.Config{
		Version: cfg.Version, DefaultTimeout: cfg.DefaultTimeout, LatestDriver: dl.LatestDriver, Scenarios: scen,
	}, sess, j)
	ctl := control.New(control.Config{
		Token: cfg.ControlToken, Version: cfg.Version,
		DeviceOpenMode: sess.OpenMode(), EquipmentTypes: reg.Types(),
		OnReset: dev.ResetIdempotency, DefaultTimeout: cfg.DefaultTimeout,
		JournalMaxBytes: cfg.JournalMaxFileBytes, JournalMaxFiles: cfg.JournalMaxFiles, Scenarios: scen,
	}, sess, j)

	mux := http.NewServeMux()
	dev.Register(mux)
	ctl.Register(mux)
	dl.Register(mux)
	mux.Handle("/", web.Handler())

	return &App{Handler: httpx.Recover(mux), Journal: j, Devices: dev, Sess: sess}, nil
}

// Registry — реестр моделей: настоящие модели всех типов оборудования требований 4.8 — ККТ
// (этап 2), эквайринг (этап 3), весы, принтеры, устройства ввода, дисплей, ТСД, RFID,
// распознавание (этап 4).
func Registry(version string) *models.Registry {
	reg := models.DefaultRegistry(version)
	reg.Register("kkt", kkt.Factory(version))
	reg.Register("acquiring", acquiring.Factory(version))
	reg.Register("scales", scales.Factory(version))
	reg.Register("label_scales", labelscales.Factory(version))
	reg.Register("receipt_printer", printers.ReceiptFactory(version))
	reg.Register("label_printer", printers.LabelFactory(version))
	reg.Register("scanner", input.ScannerFactory(version))
	reg.Register("card_reader", input.CardReaderFactory(version))
	reg.Register("display", display.Factory(version))
	reg.Register("tsd", tsd.Factory(version))
	reg.Register("rfid", rfid.Factory(version))
	reg.Register("recognizer", recognizer.Factory(version))
	return reg
}

// Run — фоновые задачи до отмены ctx: обслуживание сессий и запись активности в реестр.
func (a *App) Run(ctx context.Context) {
	go func() {
		t := time.NewTicker(time.Minute)
		defer t.Stop()
		for {
			select {
			case <-ctx.Done():
				return
			case <-t.C:
				a.Sess.Flush()
			}
		}
	}()
	a.Devices.Run(ctx)
}

// Close записывает реестр устройств и закрывает журнал (и тем самым живые потоки журнала).
func (a *App) Close() error {
	a.Sess.Flush()
	return a.Journal.Close()
}

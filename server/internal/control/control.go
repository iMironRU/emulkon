// Package control — Control API (ARCHITECTURE §9.1): управление эмуляцией
// извне — веб-UI и автотесты. Защищён отдельным токеном (не токеном устройств).
package control

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"net/http"
	"strconv"
	"strings"
	"time"

	"emulkon/internal/deviceapi"
	"emulkon/internal/httpx"
	"emulkon/internal/journal"
	"emulkon/internal/models"
	"emulkon/internal/scenarios"
	"emulkon/internal/secret"
	"emulkon/internal/sessions"
)

const (
	defaultJournalLimit = 200
	maxJournalLimit     = 5000
	heartbeatInterval   = 15 * time.Second
)

// Config — параметры Control API.
type Config struct {
	Token           secret.Token // не задан — без авторизации (только для локального запуска)
	Version         string
	DeviceOpenMode  bool                   // токен устройств не задан — для предупреждения в UI
	EquipmentTypes  []models.EquipmentType // поддерживаемые типы оборудования
	OnReset         func()                 // дополнительные действия при сбросе (кэш идемпотентности)
	DefaultTimeout  time.Duration
	JournalMaxBytes int64
	JournalMaxFiles int
	Scenarios       *scenarios.Engine // правила сбоев (ARCHITECTURE §7)
}

// API — обработчики Control API.
type API struct {
	cfg  Config
	sess *sessions.Manager
	j    *journal.Journal
}

func New(cfg Config, sess *sessions.Manager, j *journal.Journal) *API {
	return &API{cfg: cfg, sess: sess, j: j}
}

// Register регистрирует маршруты /control/v1 за проверкой токена.
func (a *API) Register(mux *http.ServeMux) {
	inner := http.NewServeMux()
	inner.HandleFunc("GET /control/v1/info", a.info)
	inner.HandleFunc("GET /control/v1/devices", a.devices)
	inner.HandleFunc("PUT /control/v1/devices/{device_id}/state", a.setState)
	inner.HandleFunc("GET /control/v1/devices/{device_id}/prints", a.prints)
	inner.HandleFunc("POST /control/v1/devices/{device_id}/prompt", a.prompt)
	inner.HandleFunc("POST /control/v1/devices/{device_id}/actions/{name}", a.action)
	inner.HandleFunc("POST /control/v1/devices/{device_id}/events", a.event)
	inner.HandleFunc("PATCH /control/v1/devices/{device_id}", a.deviceMeta)
	inner.HandleFunc("DELETE /control/v1/devices/{device_id}", a.deleteDevice)
	inner.HandleFunc("POST /control/v1/devices/{device_id}/settings/reset", a.resetSettings)
	inner.HandleFunc("GET /control/v1/workstations", a.workstations)
	inner.HandleFunc("PATCH /control/v1/workstations/{workstation_id}", a.workstationMeta)
	inner.HandleFunc("PUT /control/v1/workstations/{workstation_id}/printers", a.workstationPrinters)
	inner.HandleFunc("POST /control/v1/workstations/{workstation_id}/token", a.issueToken)
	inner.HandleFunc("DELETE /control/v1/workstations/{workstation_id}/token", a.revokeToken)
	inner.HandleFunc("POST /control/v1/devices/{device_id}/reprint", a.reprint)
	inner.HandleFunc("GET /control/v1/types", a.types)
	inner.HandleFunc("PUT /control/v1/types/{key}/defaults", a.typeDefaults)
	inner.HandleFunc("GET /control/v1/scenarios", a.scenarioList)
	inner.HandleFunc("POST /control/v1/scenarios", a.scenarioPut)
	inner.HandleFunc("PUT /control/v1/scenarios/{id}", a.scenarioPut)
	inner.HandleFunc("DELETE /control/v1/scenarios/{id}", a.scenarioDelete)
	inner.HandleFunc("GET /control/v1/sessions", a.sessions)
	inner.HandleFunc("GET /control/v1/journal", a.journalQuery)
	inner.HandleFunc("GET /control/v1/journal/stream", a.journalStream)
	inner.HandleFunc("POST /control/v1/reset", a.reset)
	inner.HandleFunc("/control/", httpx.NotFound)
	mux.Handle("/control/", httpx.RequireToken(a.cfg.Token, inner))
}

// --- info ---

type infoResponse struct {
	Version           string                 `json:"version"`
	Emulator          bool                   `json:"emulator"`
	DeviceOpenMode    bool                   `json:"device_open_mode"`
	ControlOpenMode   bool                   `json:"control_open_mode"`
	WorkstationTokens int                    `json:"workstation_tokens"` // рабочих мест с токеном
	EquipmentTypes    []models.EquipmentType `json:"equipment_types"`
	DefaultTimeoutMS  int64                  `json:"default_timeout_ms"`
	Journal           journal.Stats          `json:"journal"`
	JournalMaxBytes   int64                  `json:"journal_max_file_bytes"`
	JournalMaxFiles   int                    `json:"journal_max_files"`
}

func (a *API) info(w http.ResponseWriter, _ *http.Request) {
	httpx.WriteJSON(w, http.StatusOK, infoResponse{
		Version: a.cfg.Version, Emulator: true,
		DeviceOpenMode: a.cfg.DeviceOpenMode, ControlOpenMode: a.cfg.Token.Empty(), WorkstationTokens: a.sess.WorkstationTokens(),
		EquipmentTypes: a.cfg.EquipmentTypes, DefaultTimeoutMS: a.cfg.DefaultTimeout.Milliseconds(),
		Journal: a.j.Stats(), JournalMaxBytes: a.cfg.JournalMaxBytes, JournalMaxFiles: a.cfg.JournalMaxFiles,
	})
}

// --- устройства и сессии ---

func (a *API) devices(w http.ResponseWriter, _ *http.Request) {
	httpx.WriteJSON(w, http.StatusOK, map[string]any{"devices": a.sess.Devices()})
}

// setState — PUT /control/v1/devices/{id}/state: изменить состояние модели
// (бумага, крышка, ФН, ОФД, …). Ключи — у каждой модели свои (docs/models/).
func (a *API) setState(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("device_id")
	d, ok := a.sess.Device(id)
	if !ok {
		httpx.WriteError(w, http.StatusNotFound, httpx.CodeNotFound, "устройство "+id+" не найдено")
		return
	}
	setter, ok := d.Model.(models.StateSetter)
	if !ok {
		httpx.WriteError(w, http.StatusBadRequest, httpx.CodeBadRequest, "модель устройства "+id+" не поддерживает задание состояния")
		return
	}
	var patch map[string]any
	if err := httpx.DecodeJSON(w, r, &patch); err != nil {
		httpx.WriteError(w, http.StatusBadRequest, httpx.CodeBadRequest, err.Error())
		return
	}
	if len(patch) == 0 {
		httpx.WriteError(w, http.StatusBadRequest, httpx.CodeBadRequest, "пустой набор изменений")
		return
	}
	// Модель может разобрать изменение «на месте» — настройки берём из копии.
	saved := make(map[string]any, len(patch))
	for k, v := range patch {
		saved[k] = v
	}
	if err := setter.SetState(patch); err != nil {
		httpx.WriteError(w, http.StatusBadRequest, httpx.CodeBadRequest, err.Error())
		return
	}
	a.sess.SaveSettings(d, saved) // настройки класса — в реестр (ARCHITECTURE §4.3)
	if _, err := a.j.Append(journal.Entry{
		DeviceID: id, WorkstationID: d.WorkstationID, Direction: journal.DirSystem, Method: "control.set_state",
		Payload: mustJSON(map[string]any{"patch": patch, "remote_addr": r.RemoteAddr}),
	}); err != nil {
		slog.Error("запись в журнал", "err", err)
	}
	info, _ := a.sess.DeviceInfo(id)
	httpx.WriteJSON(w, http.StatusOK, info)
}

// promptRequest — ответ с экрана устройства (docs/protocol.md §2.0).
type promptRequest struct {
	PromptID string `json:"prompt_id"`
	models.PromptAnswer
}

// prompt — POST /control/v1/devices/{id}/prompt: человек нажал кнопку на экране
// терминала (приложил карту, отказ, отмена). Итог пишет в журнал модель (device.prompt_done).
func (a *API) prompt(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("device_id")
	d, ok := a.sess.Device(id)
	if !ok {
		httpx.WriteError(w, http.StatusNotFound, httpx.CodeNotFound, "устройство "+id+" не найдено")
		return
	}
	p, ok := d.Model.(models.Prompter)
	if !ok {
		httpx.WriteError(w, http.StatusBadRequest, httpx.CodeBadRequest, "у устройства "+id+" нет экрана, который ждёт ответа")
		return
	}
	var req promptRequest
	if err := httpx.DecodeJSON(w, r, &req); err != nil {
		httpx.WriteError(w, http.StatusBadRequest, httpx.CodeBadRequest, err.Error())
		return
	}
	err := p.Respond(req.PromptID, req.PromptAnswer)
	switch {
	case errors.Is(err, models.ErrPromptGone):
		httpx.WriteError(w, http.StatusConflict, httpx.CodePromptGone, err.Error())
		return
	case err != nil:
		httpx.WriteError(w, http.StatusBadRequest, httpx.CodeBadRequest, err.Error())
		return
	}
	httpx.WriteJSON(w, http.StatusOK, map[string]any{"ok": true})
}

// action — POST /control/v1/devices/{id}/actions/{name}: человек что-то сделал на самом
// устройстве — напечатал этикетку на весах (docs/protocol.md §2.0). Тело — аргументы
// действия (JSON-объект, можно пустое). Напечатанное попадает в журнал как device.print.
func (a *API) action(w http.ResponseWriter, r *http.Request) {
	id, name := r.PathValue("device_id"), r.PathValue("name")
	d, ok := a.sess.Device(id)
	if !ok {
		httpx.WriteError(w, http.StatusNotFound, httpx.CodeNotFound, "устройство "+id+" не найдено")
		return
	}
	args := map[string]any{}
	if r.ContentLength != 0 {
		if err := httpx.DecodeJSON(w, r, &args); err != nil {
			httpx.WriteError(w, http.StatusBadRequest, httpx.CodeBadRequest, err.Error())
			return
		}
		if args == nil {
			args = map[string]any{}
		}
	}
	res, printed, err := d.Act(r.Context(), name, args)
	if errors.Is(err, context.Canceled) || errors.Is(err, context.DeadlineExceeded) {
		return // клиент ушёл, пока ждал очереди устройства
	}
	logged := map[string]any{"action": name, "args": args, "remote_addr": r.RemoteAddr}
	if err != nil {
		logged["error"] = err.Error()
	}
	a.journal(journal.Entry{DeviceID: id, WorkstationID: d.WorkstationID, Direction: journal.DirSystem,
		Method: "control.action", Payload: mustJSON(logged)})
	for _, doc := range printed {
		a.journal(journal.Entry{DeviceID: id, WorkstationID: d.WorkstationID, Direction: journal.DirSystem,
			Method: deviceapi.MethodPrint, Payload: deviceapi.PrintPayload(d, doc, false)})
	}
	// Сканер или считыватель в режиме клавиатуры: код вводит трей рабочего места.
	if kb, ok := res.(models.Keyboarder); ok && err == nil && kb.KeyboardInput() != nil {
		a.journal(journal.Entry{DeviceID: id, WorkstationID: d.WorkstationID, Direction: journal.DirSystem,
			Method: deviceapi.MethodKeyboard, Payload: mustJSON(kb.KeyboardInput())})
	}
	switch {
	case errors.Is(err, models.ErrUnknownAction):
		httpx.WriteError(w, http.StatusNotFound, httpx.CodeNotFound, fmt.Sprintf("у устройства %s нет действия %q", id, name))
	case err != nil:
		httpx.WriteError(w, http.StatusBadRequest, httpx.CodeBadRequest, err.Error())
	default:
		httpx.WriteJSON(w, http.StatusOK, map[string]any{"result": res})
	}
}

func (a *API) journal(e journal.Entry) {
	if _, err := a.j.Append(e); err != nil {
		slog.Error("запись в журнал", "method", e.Method, "err", err)
	}
}

// event — POST /control/v1/devices/{id}/events {"name", "data"}: событие устройства для 1С
// как есть (автотесты, ошибка драйвера). Имя — из требований (models.EventNames). Модели
// со своими событиями (сканер, считыватель) удобнее вызывать действиями: они собирают данные.
func (a *API) event(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("device_id")
	d, ok := a.sess.Device(id)
	if !ok {
		httpx.WriteError(w, http.StatusNotFound, httpx.CodeNotFound, "устройство "+id+" не найдено")
		return
	}
	var e models.DeviceEvent
	if err := httpx.DecodeJSON(w, r, &e); err != nil {
		httpx.WriteError(w, http.StatusBadRequest, httpx.CodeBadRequest, err.Error())
		return
	}
	if !models.EventNames[e.Name] {
		httpx.WriteError(w, http.StatusBadRequest, httpx.CodeBadRequest,
			fmt.Sprintf("событие %q не из требований: Barcode, TracksData, KeyPress, RFIDData, DriverError, Weighting, OnClick…", e.Name))
		return
	}
	rec := a.sess.Emit(d, e)
	httpx.WriteJSON(w, http.StatusOK, map[string]any{"event": rec, "listeners": d.Listeners()})
}

// prints — GET /control/v1/devices/{id}/prints?limit=N: лента печати, новые первыми.
func (a *API) prints(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("device_id")
	d, ok := a.sess.Device(id)
	if !ok {
		httpx.WriteError(w, http.StatusNotFound, httpx.CodeNotFound, "устройство "+id+" не найдено")
		return
	}
	printer, ok := d.Model.(models.Printer)
	if !ok {
		httpx.WriteJSON(w, http.StatusOK, map[string]any{"prints": []models.PrintedDocument{}})
		return
	}
	limit := 20
	if s := r.URL.Query().Get("limit"); s != "" {
		n, err := strconv.Atoi(s)
		if err != nil || n <= 0 {
			httpx.WriteError(w, http.StatusBadRequest, httpx.CodeBadRequest, "limit: положительное целое")
			return
		}
		limit = min(n, 200)
	}
	httpx.WriteJSON(w, http.StatusOK, map[string]any{"prints": printer.Prints(limit)})
}

func (a *API) sessions(w http.ResponseWriter, _ *http.Request) {
	httpx.WriteJSON(w, http.StatusOK, map[string]any{"sessions": a.sess.Sessions()})
}

// --- сброс ---

func (a *API) reset(w http.ResponseWriter, r *http.Request) {
	n := a.sess.ResetDevices()
	a.cfg.Scenarios.ResetCounters() // «на 3-м вызове» снова считается с начала
	if a.cfg.OnReset != nil {
		a.cfg.OnReset()
	}
	if _, err := a.j.Append(journal.Entry{
		Direction: journal.DirSystem, Method: "control.reset",
		Payload: mustJSON(map[string]any{"devices": n, "remote_addr": r.RemoteAddr}),
	}); err != nil {
		slog.Error("запись в журнал", "err", err)
	}
	httpx.WriteJSON(w, http.StatusOK, map[string]any{"reset_devices": n})
}

// --- журнал ---

// parseFilter разбирает параметры фильтра журнала из query string.
func parseFilter(r *http.Request) (journal.Filter, error) {
	q := r.URL.Query()
	f := journal.Filter{
		DeviceID: q.Get("device_id"), WorkstationID: q.Get("workstation_id"),
		SessionID: q.Get("session_id"), Direction: q.Get("direction"),
		Method: q.Get("method"), CorrelationID: q.Get("correlation_id"), Text: q.Get("q"),
	}
	var err error
	if f.Since, err = parseTime(q.Get("since")); err != nil {
		return f, fmt.Errorf("since: %w", err)
	}
	if f.Until, err = parseTime(q.Get("until")); err != nil {
		return f, fmt.Errorf("until: %w", err)
	}
	if f.AfterSeq, err = parseInt(q.Get("after_seq")); err != nil {
		return f, fmt.Errorf("after_seq: %w", err)
	}
	if f.BeforeSeq, err = parseInt(q.Get("before_seq")); err != nil {
		return f, fmt.Errorf("before_seq: %w", err)
	}
	return f, nil
}

type journalResponse struct {
	Entries []journal.Entry `json:"entries"`
	LastSeq int64           `json:"last_seq"`
	HasMore bool            `json:"has_more"`
}

// journalQuery — GET /control/v1/journal: последние limit записей по фильтру
// (по возрастанию seq). Листание назад — before_seq = seq первой полученной записи.
// format=jsonl — выгрузка всех подходящих записей файлом.
func (a *API) journalQuery(w http.ResponseWriter, r *http.Request) {
	f, err := parseFilter(r)
	if err != nil {
		httpx.WriteError(w, http.StatusBadRequest, httpx.CodeBadRequest, err.Error())
		return
	}
	if r.URL.Query().Get("format") == "jsonl" {
		a.journalExport(w, f)
		return
	}
	limit := defaultJournalLimit
	if s := r.URL.Query().Get("limit"); s != "" {
		n, err := strconv.Atoi(s)
		if err != nil || n <= 0 {
			httpx.WriteError(w, http.StatusBadRequest, httpx.CodeBadRequest, "limit: положительное целое")
			return
		}
		limit = min(n, maxJournalLimit)
	}
	last := a.j.Seq()
	if f.BeforeSeq == 0 {
		f.BeforeSeq = last + 1 // фиксируем верхнюю границу, чтобы ответ был согласован с last_seq
	}
	// Берём на одну запись больше, чтобы честно ответить has_more.
	entries, err := a.j.Tail(f, limit+1)
	if err != nil {
		httpx.WriteError(w, http.StatusInternalServerError, httpx.CodeInternal, err.Error())
		return
	}
	hasMore := len(entries) > limit
	if hasMore {
		entries = entries[1:]
	}
	if entries == nil {
		entries = []journal.Entry{}
	}
	httpx.WriteJSON(w, http.StatusOK, journalResponse{Entries: entries, LastSeq: last, HasMore: hasMore})
}

func (a *API) journalExport(w http.ResponseWriter, f journal.Filter) {
	name := "emulkon-journal-" + time.Now().UTC().Format("20060102T150405Z") + ".jsonl"
	w.Header().Set("Content-Type", "application/x-ndjson; charset=utf-8")
	w.Header().Set("Content-Disposition", `attachment; filename="`+name+`"`)
	err := a.j.Scan(f, func(_ *journal.Entry, raw []byte) bool {
		if _, err := w.Write(raw); err != nil {
			return false
		}
		_, err := w.Write([]byte{'\n'})
		return err == nil
	})
	if err != nil {
		slog.Error("выгрузка журнала", "err", err)
	}
}

// journalStream — GET /control/v1/journal/stream: живой поток записей (SSE).
// Курсор — after_seq или заголовок Last-Event-ID (переподключение EventSource);
// без курсора — tail последних записей (по умолчанию 0). Затем новые записи.
// Отставший клиент получает событие lagged и должен переподключиться с курсором.
func (a *API) journalStream(w http.ResponseWriter, r *http.Request) {
	f, err := parseFilter(r)
	if err != nil {
		httpx.WriteError(w, http.StatusBadRequest, httpx.CodeBadRequest, err.Error())
		return
	}
	if id := r.Header.Get("Last-Event-ID"); id != "" {
		if n, err := strconv.ParseInt(id, 10, 64); err == nil {
			f.AfterSeq = n
		}
	}
	hasCursor := f.AfterSeq > 0
	tail, _ := strconv.Atoi(r.URL.Query().Get("tail"))
	tail = max(0, min(tail, maxJournalLimit))

	flusher, ok := w.(http.Flusher)
	if !ok {
		httpx.WriteError(w, http.StatusInternalServerError, httpx.CodeInternal, "потоковая передача не поддерживается")
		return
	}
	sub, err := a.j.Subscribe(1024)
	if err != nil {
		httpx.WriteError(w, http.StatusServiceUnavailable, httpx.CodeUnavailable, err.Error())
		return
	}
	defer sub.Close()

	h := w.Header()
	h.Set("Content-Type", "text/event-stream; charset=utf-8")
	h.Set("Cache-Control", "no-store")
	h.Set("X-Accel-Buffering", "no")
	w.WriteHeader(http.StatusOK)

	send := func(e *journal.Entry) bool {
		b, err := journal.Marshal(e)
		if err != nil {
			return true
		}
		_, err = fmt.Fprintf(w, "id: %d\nevent: entry\ndata: %s\n\n", e.Seq, b)
		return err == nil
	}

	// Дочитка до момента подписки: всё с seq <= StartSeq, дальше — из канала.
	back := f
	back.BeforeSeq = sub.StartSeq + 1
	switch {
	case hasCursor:
		err = a.j.Scan(back, func(e *journal.Entry, _ []byte) bool { return send(e) })
	case tail > 0:
		var entries []journal.Entry
		if entries, err = a.j.Tail(back, tail); err == nil {
			for i := range entries {
				if !send(&entries[i]) {
					break
				}
			}
		}
	}
	if err != nil {
		slog.Error("поток журнала: дочитка", "err", err)
		return
	}
	fmt.Fprintf(w, "event: ready\ndata: {\"seq\":%d}\n\n", sub.StartSeq)
	flusher.Flush()

	ping := time.NewTicker(heartbeatInterval)
	defer ping.Stop()
	ctx := r.Context()
	pending := false // отправлено, но не сброшено клиенту
	for {
		select {
		case <-ctx.Done():
			return
		case <-ping.C:
			if _, err := fmt.Fprint(w, ": ping\n\n"); err != nil {
				return
			}
			flusher.Flush()
		case e, ok := <-sub.C:
			if !ok {
				if sub.Lagged() {
					fmt.Fprint(w, "event: lagged\ndata: {}\n\n")
					flusher.Flush()
				}
				return
			}
			if f.MatchEntry(&e) {
				if !send(&e) {
					return
				}
				pending = true
			}
			// Сбрасываем, когда очередь опустела, — даже если последняя запись не прошла
			// фильтр: иначе подходящее событие ждало бы следующего ping (15 с).
			if pending && len(sub.C) == 0 {
				flusher.Flush()
				pending = false
			}
		}
	}
}

// --- служебное ---

func parseTime(s string) (time.Time, error) {
	if s == "" {
		return time.Time{}, nil
	}
	return time.Parse(time.RFC3339Nano, s)
}

func parseInt(s string) (int64, error) {
	if s == "" {
		return 0, nil
	}
	n, err := strconv.ParseInt(strings.TrimSpace(s), 10, 64)
	if err == nil && n < 0 {
		err = fmt.Errorf("отрицательное значение")
	}
	return n, err
}

func mustJSON(v any) json.RawMessage {
	b, _ := journal.Marshal(v)
	return b
}

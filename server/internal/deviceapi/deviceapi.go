// Package deviceapi — Device API (ARCHITECTURE §5): протокол между
// компонентой-драйвером и сервером. Спецификация — docs/protocol.md, docs/openapi.yaml.
package deviceapi

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"net/http"
	"time"
	"unicode"

	"emulkon/internal/downloads"
	"emulkon/internal/httpx"
	"emulkon/internal/journal"
	"emulkon/internal/models"
	"emulkon/internal/scenarios"
	"emulkon/internal/sessions"
)

// SessionHeader — заголовок с идентификатором сессии в командах.
const SessionHeader = "X-Emulkon-Session"

// MethodPrint — служебная запись журнала: документ, «напечатанный» устройством.
const MethodPrint = "device.print"

// MethodKeyboard — ввод с клавиатуры на рабочем месте: сканер или считыватель в режиме
// клавиатуры, трей вводит текст в активное окно (docs/models/input.md).
const MethodKeyboard = "device.keyboard"

// MethodReprint — «напечатать ещё раз» из карточки устройства: трей печатает документ (docs/protocol.md §2.3).
const MethodReprint = "device.reprint"

// PrintPayload — payload device.print и device.reprint: документ и решение сервера, показать ли
// его на экране кассы и напечатать ли на принтер (настройка вывода устройства, ARCHITECTURE §4.3).
func PrintPayload(d *sessions.Device, doc models.PrintedDocument, reprint bool) json.RawMessage {
	out := d.OutputFor(doc.Kind)
	if reprint {
		out.Screen, out.Print = false, true
	}
	return mustJSON(struct {
		models.PrintedDocument
		Output models.OutputDecision `json:"output"`
	}{doc, out})
}

// Config — параметры Device API.
type Config struct {
	Version        string
	DefaultTimeout time.Duration // рекомендуемый таймаут компоненты; 0 — 30 с
	IdempotencyTTL time.Duration // 0 — 15 минут
	IdempotencyMax int           // 0 — 10000 ответов
	Now            func() time.Time
	// LatestDriver — самая новая DLL драйвера для ОС (пусто — Windows) и архитектуры (каталог загрузок);
	// nil — обновления драйвера не предлагаются.
	LatestDriver func(osName, arch string) (downloads.File, bool)
	// Scenarios — правила сбоев (ARCHITECTURE §7); nil — без сценариев.
	Scenarios *scenarios.Engine
}

// API — обработчики Device API.
type API struct {
	cfg     Config
	sess    *sessions.Manager
	j       *journal.Journal
	idem    *idemCache
	started time.Time
}

func New(cfg Config, sess *sessions.Manager, j *journal.Journal) *API {
	if cfg.DefaultTimeout <= 0 {
		cfg.DefaultTimeout = 30 * time.Second
	}
	if cfg.IdempotencyTTL <= 0 {
		cfg.IdempotencyTTL = 15 * time.Minute
	}
	if cfg.IdempotencyMax <= 0 {
		cfg.IdempotencyMax = 10000
	}
	if cfg.Now == nil {
		cfg.Now = time.Now
	}
	return &API{
		cfg: cfg, sess: sess, j: j,
		idem:    newIdemCache(cfg.IdempotencyTTL, cfg.IdempotencyMax, cfg.Now),
		started: cfg.Now(),
	}
}

// Register регистрирует маршруты /api/v1.
func (a *API) Register(mux *http.ServeMux) {
	mux.HandleFunc("GET /api/v1/health", a.health)
	mux.HandleFunc("POST /api/v1/sessions", a.openSession)
	mux.HandleFunc("DELETE /api/v1/sessions/{session_id}", a.deleteSession)
	mux.HandleFunc("POST /api/v1/devices/{device_id}/commands", a.command)
	mux.HandleFunc("GET /api/v1/devices/{device_id}/events", a.events)
	mux.HandleFunc("/api/", httpx.NotFound)
}

// Run — фоновое обслуживание: просрочка сессий и кэша идемпотентности.
func (a *API) Run(ctx context.Context) {
	t := time.NewTicker(time.Minute)
	defer t.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-t.C:
			a.idem.cleanup()
			for _, s := range a.sess.ExpireIdle() {
				a.journal(journal.Entry{
					SessionID: s.ID, WorkstationID: s.WorkstationID, DeviceID: s.DeviceID,
					Direction: journal.DirSystem, Method: "session.expired",
				})
			}
		}
	}
}

// ResetIdempotency очищает кэш ответов (часть сброса перед тестом).
func (a *API) ResetIdempotency() { a.idem.reset() }

// --- health ---

type healthResponse struct {
	Status        string        `json:"status"`
	Version       string        `json:"version"`
	StartedAt     time.Time     `json:"started_at"`
	UptimeSeconds int64         `json:"uptime_s"`
	Journal       journal.Stats `json:"journal"`
}

func (a *API) health(w http.ResponseWriter, _ *http.Request) {
	httpx.WriteJSON(w, http.StatusOK, healthResponse{
		Status: "ok", Version: a.cfg.Version, StartedAt: a.started.UTC(),
		UptimeSeconds: int64(a.cfg.Now().Sub(a.started).Seconds()),
		Journal:       a.j.Stats(),
	})
}

// --- сессии (§5.1) ---

// SessionResponse — ответ на открытие сессии.
type SessionResponse struct {
	SessionID     string `json:"session_id"`
	DeviceID      string `json:"device_id"`
	Limits        Limits `json:"limits"`
	ServerVersion string `json:"server_version"`
	Emulator      bool   `json:"emulator"`
	// DriverUpdate — DLL драйвера новее той, что открыла сессию (docs/protocol.md §1.5).
	DriverUpdate *downloads.File `json:"driver_update,omitempty"`
}

type Limits struct {
	DefaultTimeoutMS int64 `json:"default_timeout_ms"`
}

func (a *API) openSession(w http.ResponseWriter, r *http.Request) {
	var req sessions.OpenRequest
	if err := httpx.DecodeJSON(w, r, &req); err != nil {
		httpx.WriteError(w, http.StatusBadRequest, httpx.CodeBadRequest, err.Error())
		return
	}
	s, d, isNew, err := a.sess.Open(req)

	// Журналируем и отказы: попытки с неверным токеном и неизвестных мест видны оператору (§10).
	payload := map[string]any{
		"equipment_type":     req.EquipmentType,
		"driver_version":     req.DriverVersion,
		"driver_arch":        req.DriverArch,
		"driver_os":          req.DriverOS,
		"interface_revision": req.InterfaceRevision,
		"remote_addr":        r.RemoteAddr,
	}
	// Экземпляр в 1С (ARCHITECTURE §4.3) — только если компонента его передала.
	for k, v := range map[string]string{"instance_id": req.InstanceID, "instance_name": req.InstanceName, "infobase": req.Infobase} {
		if v != "" {
			payload[k] = v
		}
	}
	if req.InstanceCopy {
		payload["instance_copy"] = true
	}
	e := journal.Entry{WorkstationID: req.WorkstationID, Direction: journal.DirSystem, Method: "session.open"}
	if err != nil {
		payload["rejected"] = err.Error()
		e.Payload = mustJSON(payload)
		a.journal(e)

		status, code := http.StatusBadRequest, httpx.CodeBadRequest
		switch {
		case errors.Is(err, sessions.ErrBadToken):
			status, code = http.StatusUnauthorized, httpx.CodeBadToken
		case errors.Is(err, sessions.ErrUnknownType):
			code = httpx.CodeUnknownType
		}
		httpx.WriteError(w, status, code, err.Error())
		return
	}
	resp := SessionResponse{
		SessionID: s.ID, DeviceID: d.ID, ServerVersion: a.cfg.Version, Emulator: true,
		Limits: Limits{DefaultTimeoutMS: a.cfg.DefaultTimeout.Milliseconds()},
	}
	if a.cfg.LatestDriver != nil && req.DriverArch != "" {
		if f, ok := a.cfg.LatestDriver(req.DriverOS, req.DriverArch); ok && downloads.Compare(f.Version, req.DriverVersion) > 0 {
			resp.DriverUpdate = &f
			payload["driver_update"] = f.Version
		}
	}
	payload["new_workstation"] = isNew
	payload["auth"] = s.Auth
	if s.WorkstationID != req.WorkstationID {
		payload["workstation_requested"] = req.WorkstationID // место назвал токен
	}
	e.WorkstationID = s.WorkstationID
	e.SessionID, e.DeviceID, e.Payload = s.ID, d.ID, mustJSON(payload)
	a.journal(e)

	httpx.WriteJSON(w, http.StatusOK, resp)
}

// resolve проверяет сессию из заголовка для устройства из пути.
func (a *API) resolve(w http.ResponseWriter, r *http.Request) (*sessions.Session, *sessions.Device, bool) {
	sid := r.Header.Get(SessionHeader)
	if sid == "" {
		httpx.WriteError(w, http.StatusUnauthorized, httpx.CodeSessionUnknown, "нет заголовка "+SessionHeader)
		return nil, nil, false
	}
	s, d, err := a.sess.Resolve(sid, r.PathValue("device_id"))
	switch {
	case errors.Is(err, sessions.ErrUnknownSession):
		httpx.WriteError(w, http.StatusUnauthorized, httpx.CodeSessionUnknown, "сессия не найдена — откройте новую (POST /api/v1/sessions)")
		return nil, nil, false
	case errors.Is(err, sessions.ErrWrongDevice):
		httpx.WriteError(w, http.StatusForbidden, httpx.CodeWrongDevice, err.Error())
		return nil, nil, false
	case err != nil:
		httpx.WriteError(w, http.StatusInternalServerError, httpx.CodeInternal, err.Error())
		return nil, nil, false
	}
	return s, d, true
}

// --- команды (docs/protocol.md §1.2) ---

// MaxParams — предел числа позиционных параметров метода.
const MaxParams = 64

// CommandRequest — тело команды: вызов метода драйвера как его сделала 1С.
type CommandRequest struct {
	RequestID string `json:"request_id"`
	// Method — английское имя метода из требований к драйверам (ProcessCheck, GetLastError, …).
	Method string `json:"method"`
	// Params — позиционные параметры: string, число, bool или null; выходные — любым значением.
	Params   json.RawMessage `json:"params"`
	ClientTS string          `json:"client_ts,omitempty"`
}

// CommandResponse — результат вызова.
type CommandResponse struct {
	RequestID string `json:"request_id"`
	// Result — возвращаемое значение метода: bool (BOOL), целое (LONG) или null.
	Result any `json:"result"`
	// Params — значения всех параметров после вызова, той же длины, что в запросе.
	// Компонента записывает их обратно — так 1С получает выходные параметры.
	Params []any `json:"params"`
	// ErrorCode, ErrorText — последняя ошибка устройства (то же вернёт GetLastError).
	ErrorCode int    `json:"error_code"`
	ErrorText string `json:"error_text"`
}

// parseParams разбирает позиционные параметры. Отсутствие — пустой список
// (методы без параметров). Числа сохраняются как json.Number — без потери точности.
func parseParams(raw json.RawMessage) ([]any, error) {
	if len(bytes.TrimSpace(raw)) == 0 || string(bytes.TrimSpace(raw)) == "null" {
		return []any{}, nil
	}
	dec := json.NewDecoder(bytes.NewReader(raw))
	dec.UseNumber()
	var params []any
	if err := dec.Decode(&params); err != nil {
		return nil, fmt.Errorf("params: ожидается массив позиционных параметров: %v", err)
	}
	if len(params) > MaxParams {
		return nil, fmt.Errorf("params: больше %d параметров", MaxParams)
	}
	for i, p := range params {
		switch p.(type) {
		case nil, string, bool, json.Number:
		default:
			return nil, fmt.Errorf("params[%d]: допустимы строка, число, булево или null", i)
		}
	}
	return params, nil
}

func (a *API) command(w http.ResponseWriter, r *http.Request) {
	s, d, ok := a.resolve(w, r)
	if !ok {
		return
	}
	var req CommandRequest
	if err := httpx.DecodeJSONLimit(w, r, &req, httpx.MaxCommandBytes); err != nil {
		httpx.WriteError(w, http.StatusBadRequest, httpx.CodeBadRequest, err.Error())
		return
	}
	if err := checkToken("request_id", req.RequestID); err != nil {
		httpx.WriteError(w, http.StatusBadRequest, httpx.CodeBadRequest, err.Error())
		return
	}
	if err := checkToken("method", req.Method); err != nil {
		httpx.WriteError(w, http.StatusBadRequest, httpx.CodeBadRequest, err.Error())
		return
	}
	params, err := parseParams(req.Params)
	if err != nil {
		httpx.WriteError(w, http.StatusBadRequest, httpx.CodeBadRequest, err.Error())
		return
	}

	base := journal.Entry{
		CorrelationID: req.RequestID, SessionID: s.ID, WorkstationID: s.WorkstationID,
		DeviceID: d.ID, Method: req.Method,
	}
	inPayload := mustJSON(map[string]any{"request_id": req.RequestID, "params": journal.ClipParams(params), "client_ts": req.ClientTS})

	key, fp := idemKey(d.ID, req.RequestID), fingerprint(req.Method, req.Params)
	for {
		entry, owner := a.idem.acquire(key, fp)
		if entry.fingerprint != fp {
			httpx.WriteError(w, http.StatusConflict, httpx.CodeRequestConflict,
				"request_id уже использован с другим методом или параметрами")
			return
		}
		if !owner {
			// Повтор: отдаём результат первого выполнения (дождавшись его при необходимости).
			in := base
			in.Direction, in.Payload, in.Replay = journal.DirIn, inPayload, true
			a.journal(in)
			start := time.Now()
			resp, err := a.idem.wait(r.Context(), entry)
			if err != nil {
				return // клиент ушёл, пока ждал
			}
			if resp == nil {
				continue // первое выполнение не началось — выполняем заново
			}
			a.respond(w, base, resp, time.Since(start), true)
			a.closeAfter(s, req.Method, base) // повтор отключения из новой сессии закрывает и её
			return
		}

		// Сценарии — один раз на первое выполнение: повтор с тем же request_id получит
		// сохранённый ответ, как с настоящим устройством (ARCHITECTURE §7).
		var plan scenarios.Plan
		if a.cfg.Scenarios != nil {
			plan = a.cfg.Scenarios.Plan(scenarios.Call{Device: d.ID, Type: d.Type.Key, Workstation: s.WorkstationID,
				Method: req.Method, Params: params})
			base.AppliedRules = plan.Applied
		}
		in := base
		in.Direction, in.Payload = journal.DirIn, inPayload
		if _, err := a.j.Append(in); err != nil {
			a.idem.finish(key, entry, nil)
			slog.Error("журнал недоступен, команда не выполнена", "err", err)
			httpx.WriteError(w, http.StatusServiceUnavailable, httpx.CodeUnavailable, "журнал недоступен: "+err.Error())
			return
		}
		start := time.Now()
		a.applyBefore(d, plan)
		var res models.Result
		var printed []models.PrintedDocument
		if plan.Error != nil {
			res, err = d.InjectError(r.Context(), params, plan.Error.ErrorCode, plan.Error.ErrorText, a.cfg.Now().UTC())
		} else {
			res, printed, err = d.Exec(r.Context(), req.Method, params, a.cfg.Now().UTC())
		}
		if err != nil {
			a.idem.finish(key, entry, nil) // не дождались очереди устройства — команда не выполнялась
			return
		}
		// «Напечатанное» — в журнал целиком: по нему трей и автотесты показывают чеки
		// (поток /control/v1/journal/stream?method=device.print, docs/protocol.md §2.1).
		for _, doc := range printed {
			p := base
			p.Direction, p.Method, p.Payload = journal.DirSystem, MethodPrint, PrintPayload(d, doc, false)
			a.journal(p)
		}
		resp := toResponse(req.RequestID, res)
		a.idem.finish(key, entry, resp)
		a.closeAfter(s, req.Method, base)
		if plan.Late > 0 {
			time.Sleep(plan.Late) // ответ после таймаута компоненты: она повторит запрос и получит этот же
		}
		if plan.Drop {
			// Команда выполнена, ответ «потерялся»: соединение обрывается без ответа. Повтор
			// с тем же request_id получит сохранённый результат (идемпотентность, protocol.md §1.2).
			out := base
			ms := time.Since(start).Milliseconds()
			out.Direction, out.Payload, out.LatencyMS = journal.DirOut, mustJSON(map[string]any{"dropped": true, "result": resp.Result}), &ms
			a.journal(out)
			dropConnection(w)
			return
		}
		a.respond(w, base, resp, time.Since(start), false)
		return
	}
}

// closeAfter закрывает сессию после DisconnectEquipment (docs/protocol.md §1.1): устройство
// отключено. Ответ уже сохранён: повтор с тем же request_id из новой сессии получит его же.
func (a *API) closeAfter(s *sessions.Session, method string, base journal.Entry) {
	if method != "DisconnectEquipment" {
		return
	}
	a.closeSession(s, base, method)
}

func (a *API) closeSession(s *sessions.Session, base journal.Entry, reason string) {
	if !a.sess.CloseSession(s.ID) {
		return
	}
	e := base
	e.CorrelationID, e.Direction, e.Method, e.AppliedRules = "", journal.DirSystem, "session.closed", nil
	e.Payload = mustJSON(map[string]any{"reason": reason})
	a.journal(e)
}

// deleteSession — DELETE /api/v1/sessions/{session_id}: компонента закончила с сессией
// (пробное подключение «Теста устройства»). Неизвестная сессия — тоже 200: цель достигнута.
func (a *API) deleteSession(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("session_id")
	if s, ok := a.sess.Session(id); ok {
		a.closeSession(s, journal.Entry{SessionID: s.ID, WorkstationID: s.WorkstationID, DeviceID: s.DeviceID}, "client")
	}
	httpx.WriteJSON(w, http.StatusOK, map[string]any{"ok": true})
}

// applyBefore — действия сценария до выполнения: состояние модели, события в 1С, задержка.
func (a *API) applyBefore(d *sessions.Device, plan scenarios.Plan) {
	if setter, ok := d.Model.(models.StateSetter); ok {
		for _, st := range plan.States {
			patch := make(map[string]any, len(st))
			for k, v := range st {
				patch[k] = v
			}
			if err := setter.SetState(patch); err != nil {
				slog.Warn("сценарий: состояние не применено", "device", d.ID, "err", err)
			}
		}
	}
	for _, e := range plan.Events {
		a.sess.Emit(d, e)
	}
	if plan.Delay > 0 {
		time.Sleep(plan.Delay) // начатая команда доводится до конца и без клиента (§5.2)
	}
}

// dropConnection обрывает соединение без ответа (сценарий drop).
func dropConnection(w http.ResponseWriter) {
	if hj, ok := w.(http.Hijacker); ok {
		if conn, _, err := hj.Hijack(); err == nil {
			conn.Close()
			return
		}
	}
	panic(http.ErrAbortHandler)
}

func (a *API) respond(w http.ResponseWriter, base journal.Entry, resp *CommandResponse, latency time.Duration, replay bool) {
	out := base
	ms := latency.Milliseconds()
	logged := *resp
	logged.Params = journal.ClipParams(resp.Params)
	out.Direction, out.Payload, out.LatencyMS, out.Replay = journal.DirOut, mustJSON(&logged), &ms, replay
	a.journal(out)
	httpx.WriteJSON(w, http.StatusOK, resp)
}

func toResponse(requestID string, r models.Result) *CommandResponse {
	params := r.Params
	if params == nil {
		params = []any{}
	}
	return &CommandResponse{RequestID: requestID, Result: r.Return, Params: params, ErrorCode: r.ErrorCode, ErrorText: r.ErrorText}
}

// --- служебное ---

func (a *API) journal(e journal.Entry) {
	if _, err := a.j.Append(e); err != nil {
		slog.Error("запись в журнал", "method", e.Method, "err", err)
	}
}

func mustJSON(v any) json.RawMessage {
	b, err := journal.Marshal(v)
	if err != nil {
		b, _ = json.Marshal(map[string]string{"marshal_error": err.Error()})
	}
	return b
}

func checkToken(field, v string) error {
	if v == "" {
		return fmt.Errorf("%s: обязателен", field)
	}
	if len(v) > 128 {
		return fmt.Errorf("%s: длиннее 128 байт", field)
	}
	for _, r := range v {
		if !unicode.IsPrint(r) {
			return fmt.Errorf("%s: непечатаемые символы", field)
		}
	}
	return nil
}

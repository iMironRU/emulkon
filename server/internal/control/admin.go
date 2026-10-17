package control

// Учёт устройств: подписи, удаление, настройки, рабочие места, классы (docs/protocol.md §2.3).

import (
	"errors"
	"net/http"

	"emulkon/internal/deviceapi"
	"emulkon/internal/httpx"
	"emulkon/internal/journal"
	"emulkon/internal/models"
	"emulkon/internal/sessions"
)

// adminError — ответ на ошибку методов учёта.
func adminError(w http.ResponseWriter, err error, what string) {
	switch {
	case errors.Is(err, sessions.ErrNotFound):
		httpx.WriteError(w, http.StatusNotFound, httpx.CodeNotFound, what+" не найдено")
	case errors.Is(err, sessions.ErrBadRequest):
		httpx.WriteError(w, http.StatusBadRequest, httpx.CodeBadRequest, err.Error())
	default:
		httpx.WriteError(w, http.StatusInternalServerError, httpx.CodeInternal, err.Error())
	}
}

// deviceMeta — PATCH /control/v1/devices/{id} {"alias", "note", "output"}; в журнал — control.device_meta.
func (a *API) deviceMeta(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("device_id")
	var mt sessions.Meta
	if err := httpx.DecodeJSON(w, r, &mt); err != nil {
		httpx.WriteError(w, http.StatusBadRequest, httpx.CodeBadRequest, err.Error())
		return
	}
	if err := a.sess.SetDeviceMeta(id, mt); err != nil {
		adminError(w, err, "устройство "+id)
		return
	}
	d, _ := a.sess.Device(id)
	a.journal(journal.Entry{DeviceID: id, WorkstationID: d.WorkstationID, Direction: journal.DirSystem,
		Method: "control.device_meta", Payload: mustJSON(map[string]any{"meta": mt, "remote_addr": r.RemoteAddr})})
	info, _ := a.sess.DeviceInfo(id)
	httpx.WriteJSON(w, http.StatusOK, info)
}

// deleteDevice — DELETE /control/v1/devices/{id}: только руками (ARCHITECTURE §4.3).
func (a *API) deleteDevice(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("device_id")
	d, ok := a.sess.Device(id)
	if !ok {
		httpx.WriteError(w, http.StatusNotFound, httpx.CodeNotFound, "устройство "+id+" не найдено")
		return
	}
	if err := a.sess.DeleteDevice(id); err != nil {
		adminError(w, err, "устройство "+id)
		return
	}
	a.journal(journal.Entry{DeviceID: id, WorkstationID: d.WorkstationID, Direction: journal.DirSystem,
		Method: "control.delete_device", Payload: mustJSON(map[string]any{"remote_addr": r.RemoteAddr})})
	httpx.WriteJSON(w, http.StatusOK, map[string]any{"deleted": id})
}

// resetSettings — POST /control/v1/devices/{id}/settings/reset: настройки класса по умолчанию.
func (a *API) resetSettings(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("device_id")
	if err := a.sess.ResetDeviceSettings(id); err != nil {
		adminError(w, err, "устройство "+id)
		return
	}
	d, _ := a.sess.Device(id)
	a.journal(journal.Entry{DeviceID: id, WorkstationID: d.WorkstationID, Direction: journal.DirSystem,
		Method: "control.reset_settings", Payload: mustJSON(map[string]any{"remote_addr": r.RemoteAddr})})
	info, _ := a.sess.DeviceInfo(id)
	httpx.WriteJSON(w, http.StatusOK, info)
}

func (a *API) workstations(w http.ResponseWriter, _ *http.Request) {
	httpx.WriteJSON(w, http.StatusOK, map[string]any{"workstations": a.sess.Workstations()})
}

// workstationMeta — PATCH /control/v1/workstations/{id} {"alias", "note"}; неизвестное место заводится.
// В журнал — control.workstation_meta.
func (a *API) workstationMeta(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("workstation_id")
	var mt sessions.Meta
	if err := httpx.DecodeJSON(w, r, &mt); err != nil {
		httpx.WriteError(w, http.StatusBadRequest, httpx.CodeBadRequest, err.Error())
		return
	}
	if err := a.sess.SetWorkstationMeta(id, mt); err != nil {
		adminError(w, err, "рабочее место "+id)
		return
	}
	a.journal(journal.Entry{WorkstationID: id, Direction: journal.DirSystem, Method: "control.workstation_meta",
		Payload: mustJSON(map[string]any{"meta": mt, "remote_addr": r.RemoteAddr})})
	for _, ws := range a.sess.Workstations() {
		if ws.WorkstationID == id {
			httpx.WriteJSON(w, http.StatusOK, ws)
			return
		}
	}
	httpx.WriteError(w, http.StatusNotFound, httpx.CodeNotFound, "рабочее место "+id+" не найдено")
}

// issueToken — POST /control/v1/workstations/{id}/token: новый токен рабочего места, виден один
// раз; прежний перестаёт действовать. В журнал — факт выдачи, без токена.
func (a *API) issueToken(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("workstation_id")
	t, err := a.sess.IssueWorkstationToken(id)
	if err != nil {
		adminError(w, err, "рабочее место "+id)
		return
	}
	a.journal(journal.Entry{WorkstationID: id, Direction: journal.DirSystem, Method: "control.token_issue",
		Payload: mustJSON(map[string]any{"hint": t.Hint, "remote_addr": r.RemoteAddr})})
	httpx.WriteJSON(w, http.StatusOK, t)
}

// revokeToken — DELETE /control/v1/workstations/{id}/token.
func (a *API) revokeToken(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("workstation_id")
	if err := a.sess.RevokeWorkstationToken(id); err != nil {
		adminError(w, err, "рабочее место "+id)
		return
	}
	a.journal(journal.Entry{WorkstationID: id, Direction: journal.DirSystem, Method: "control.token_revoke",
		Payload: mustJSON(map[string]any{"remote_addr": r.RemoteAddr})})
	httpx.WriteJSON(w, http.StatusOK, map[string]any{"ok": true})
}

func (a *API) types(w http.ResponseWriter, _ *http.Request) {
	httpx.WriteJSON(w, http.StatusOK, map[string]any{"types": a.sess.Types()})
}

// typeDefaults — PUT /control/v1/types/{key}/defaults: настройки класса целиком.
func (a *API) typeDefaults(w http.ResponseWriter, r *http.Request) {
	key := r.PathValue("key")
	var defaults map[string]any
	if err := httpx.DecodeJSON(w, r, &defaults); err != nil {
		httpx.WriteError(w, http.StatusBadRequest, httpx.CodeBadRequest, err.Error())
		return
	}
	if err := a.sess.SetTypeDefaults(key, defaults); err != nil {
		adminError(w, err, "класс "+key)
		return
	}
	a.journal(journal.Entry{Direction: journal.DirSystem, Method: "control.type_defaults",
		Payload: mustJSON(map[string]any{"type": key, "defaults": defaults, "remote_addr": r.RemoteAddr})})
	for _, t := range a.sess.Types() {
		if t.Key == key {
			httpx.WriteJSON(w, http.StatusOK, t)
			return
		}
	}
	httpx.WriteJSON(w, http.StatusOK, map[string]any{"ok": true})
}

// workstationPrinters — PUT /control/v1/workstations/{id}/printers {"printers", "default"}:
// принтеры Windows рабочего места, их сообщает трей (docs/protocol.md §2.3).
func (a *API) workstationPrinters(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("workstation_id")
	var req struct {
		Printers []string `json:"printers"`
		Default  string   `json:"default"`
	}
	if err := httpx.DecodeJSON(w, r, &req); err != nil {
		httpx.WriteError(w, http.StatusBadRequest, httpx.CodeBadRequest, err.Error())
		return
	}
	if err := a.sess.SetWorkstationPrinters(id, req.Printers, req.Default); err != nil {
		adminError(w, err, "рабочее место "+id)
		return
	}
	httpx.WriteJSON(w, http.StatusOK, map[string]any{"ok": true})
}

// reprint — POST /control/v1/devices/{id}/reprint: последний документ ленты ещё раз на принтер
// рабочего места. Пишется запись device.reprint — её печатает трей (docs/protocol.md §2.3).
func (a *API) reprint(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("device_id")
	d, ok := a.sess.Device(id)
	if !ok {
		httpx.WriteError(w, http.StatusNotFound, httpx.CodeNotFound, "устройство "+id+" не найдено")
		return
	}
	printer, ok := d.Model.(models.Printer)
	var docs []models.PrintedDocument
	if ok {
		docs = printer.Prints(1)
	}
	if len(docs) == 0 {
		httpx.WriteError(w, http.StatusConflict, httpx.CodeBadRequest, "устройство "+id+" ещё ничего не печатало")
		return
	}
	a.journal(journal.Entry{DeviceID: id, WorkstationID: d.WorkstationID, Direction: journal.DirSystem,
		Method: deviceapi.MethodReprint, Payload: deviceapi.PrintPayload(d, docs[0], true)})
	httpx.WriteJSON(w, http.StatusOK, map[string]any{"reprinted": docs[0].Seq, "kind": docs[0].Kind})
}

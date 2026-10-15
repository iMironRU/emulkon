package control

// Сценарии сбоев (ARCHITECTURE §7, docs/protocol.md §2.4): правила включаются на лету.

import (
	"errors"
	"net/http"

	"emulkon/internal/httpx"
	"emulkon/internal/journal"
	"emulkon/internal/scenarios"
)

func (a *API) scenarioError(w http.ResponseWriter, err error) {
	switch {
	case errors.Is(err, scenarios.ErrNotFound):
		httpx.WriteError(w, http.StatusNotFound, httpx.CodeNotFound, err.Error())
	case errors.Is(err, scenarios.ErrBadRule):
		httpx.WriteError(w, http.StatusBadRequest, httpx.CodeBadRequest, err.Error())
	default:
		httpx.WriteError(w, http.StatusInternalServerError, httpx.CodeInternal, err.Error())
	}
}

func (a *API) scenarioList(w http.ResponseWriter, _ *http.Request) {
	httpx.WriteJSON(w, http.StatusOK, map[string]any{"rules": a.cfg.Scenarios.Rules()})
}

// scenarioPut — POST /control/v1/scenarios (новое) и PUT /control/v1/scenarios/{id} (замена).
func (a *API) scenarioPut(w http.ResponseWriter, r *http.Request) {
	var rule scenarios.Rule
	if err := httpx.DecodeJSON(w, r, &rule); err != nil {
		httpx.WriteError(w, http.StatusBadRequest, httpx.CodeBadRequest, err.Error())
		return
	}
	create := r.Method == http.MethodPost // POST — новое (id из тела или новый), PUT — замена по id из пути
	if !create {
		rule.ID = r.PathValue("id")
	}
	saved, err := a.cfg.Scenarios.Put(rule, create)
	if err != nil {
		a.scenarioError(w, err)
		return
	}
	a.journal(journal.Entry{Direction: journal.DirSystem, Method: "control.scenario",
		Payload: mustJSON(map[string]any{"rule": saved, "remote_addr": r.RemoteAddr})})
	httpx.WriteJSON(w, http.StatusOK, saved)
}

func (a *API) scenarioDelete(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	if err := a.cfg.Scenarios.Delete(id); err != nil {
		a.scenarioError(w, err)
		return
	}
	a.journal(journal.Entry{Direction: journal.DirSystem, Method: "control.scenario",
		Payload: mustJSON(map[string]any{"deleted": id, "remote_addr": r.RemoteAddr})})
	httpx.WriteJSON(w, http.StatusOK, map[string]any{"deleted": id})
}

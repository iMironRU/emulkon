package deviceapi

import (
	"fmt"
	"net/http"
	"time"

	"emulkon/internal/httpx"
	"emulkon/internal/journal"
)

// eventsHeartbeat — пинг потока событий: держит соединение и продлевает сессию.
var eventsHeartbeat = 15 * time.Second

// events — GET /api/v1/devices/{device_id}/events: поток SSE событий устройства для компоненты
// (docs/protocol.md §1.4). Курсор — Last-Event-ID или ?cursor=; без курсора — только новые
// события. Каждое событие: «id: <event_id>», «event: device», data — {event_id, name, data, time}.
func (a *API) events(w http.ResponseWriter, r *http.Request) {
	s, d, ok := a.resolve(w, r)
	if !ok {
		return
	}
	flusher, ok := w.(http.Flusher)
	if !ok {
		httpx.WriteError(w, http.StatusInternalServerError, httpx.CodeInternal, "потоковая передача не поддерживается")
		return
	}
	cursor := r.Header.Get("Last-Event-ID")
	if cursor == "" {
		cursor = r.URL.Query().Get("cursor")
	}
	backlog, ch, cancel := d.SubscribeEvents(cursor)
	defer cancel()

	h := w.Header()
	h.Set("Content-Type", "text/event-stream; charset=utf-8")
	h.Set("Cache-Control", "no-store")
	h.Set("X-Accel-Buffering", "no")
	w.WriteHeader(http.StatusOK)

	send := func(id string, v any) bool {
		b, err := journal.Marshal(v)
		if err != nil {
			return true
		}
		_, err = fmt.Fprintf(w, "id: %s\nevent: device\ndata: %s\n\n", id, b)
		return err == nil
	}
	for _, e := range backlog {
		if !send(e.ID, e) {
			return
		}
	}
	fmt.Fprint(w, "event: ready\ndata: {}\n\n")
	flusher.Flush()

	ping := time.NewTicker(eventsHeartbeat)
	defer ping.Stop()
	for {
		select {
		case <-r.Context().Done():
			return
		case <-ping.C:
			// Сессия жива, пока её слушают; истекла — поток закрывается, компонента откроет новую.
			if _, _, err := a.sess.Resolve(s.ID, d.ID); err != nil {
				return
			}
			if _, err := fmt.Fprint(w, ": ping\n\n"); err != nil {
				return
			}
			flusher.Flush()
		case e := <-ch:
			if !send(e.ID, e) {
				return
			}
			flusher.Flush()
		}
	}
}

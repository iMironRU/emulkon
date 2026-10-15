package sessions

import (
	"fmt"
	"strconv"
	"strings"
	"sync"
	"time"

	"emulkon/internal/models"
)

// Буфер событий устройства для компоненты (docs/protocol.md §1.4). Компонента слушает поток
// SSE и при переподключении присылает курсор — id последнего полученного события.
// Курсор «эпоха.номер»: эпоха — время создания устройства. После перезапуска сервера
// устройство создаётся заново, и курсор прежней эпохи значит «отдай всё, что накопилось».

const eventBufferSize = 200

// EventRecord — событие в буфере устройства.
type EventRecord struct {
	ID   string    `json:"event_id"`
	Seq  int64     `json:"-"`
	Time time.Time `json:"time"`
	models.DeviceEvent
}

type eventBuffer struct {
	mu   sync.Mutex
	seq  int64
	buf  []EventRecord
	subs map[chan EventRecord]struct{}
}

func (d *Device) epoch() string { return strconv.FormatInt(d.CreatedAt.UnixNano(), 36) }

// Emit кладёт событие в буфер и раздаёт подписчикам. Возвращает запись и число слушателей.
func (d *Device) Emit(e models.DeviceEvent, now time.Time) (EventRecord, int) {
	b := &d.events
	b.mu.Lock()
	defer b.mu.Unlock()
	b.seq++
	rec := EventRecord{ID: fmt.Sprintf("%s.%d", d.epoch(), b.seq), Seq: b.seq, Time: now, DeviceEvent: e}
	b.buf = append(b.buf, rec)
	if len(b.buf) > eventBufferSize {
		b.buf = b.buf[len(b.buf)-eventBufferSize:]
	}
	for ch := range b.subs {
		select {
		case ch <- rec:
		default: // медленный слушатель: событие дочитает по курсору после переподключения
		}
	}
	return rec, len(b.subs)
}

// SubscribeEvents — подписка на события. cursor — id последнего полученного события
// (пусто — только новые). backlog — пропущенные события, затем — канал новых.
func (d *Device) SubscribeEvents(cursor string) (backlog []EventRecord, ch <-chan EventRecord, cancel func()) {
	b := &d.events
	c := make(chan EventRecord, 64)
	b.mu.Lock()
	if cursor != "" {
		after := int64(0) // курсор другой эпохи — всё из буфера
		if ep, n, ok := strings.Cut(cursor, "."); ok && ep == d.epoch() {
			after, _ = strconv.ParseInt(n, 10, 64)
		}
		for _, r := range b.buf {
			if r.Seq > after {
				backlog = append(backlog, r)
			}
		}
	}
	if b.subs == nil {
		b.subs = map[chan EventRecord]struct{}{}
	}
	b.subs[c] = struct{}{}
	b.mu.Unlock()
	return backlog, c, func() {
		b.mu.Lock()
		delete(b.subs, c)
		b.mu.Unlock()
	}
}

// Listeners — сколько компонент сейчас слушают события устройства.
func (d *Device) Listeners() int {
	d.events.mu.Lock()
	defer d.events.mu.Unlock()
	return len(d.events.subs)
}

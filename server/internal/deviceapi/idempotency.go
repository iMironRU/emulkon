package deviceapi

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"sync"
	"time"
)

// idemCache — кэш ответов по (device_id, request_id) для идемпотентности (§5.2).
// Повтор с тем же request_id получает тот же ответ, даже если первый запрос
// ещё выполняется (повтор ждёт его завершения) — это и есть сценарий
// «ответ пришёл после таймаута клиента».
type idemCache struct {
	ttl time.Duration
	max int
	now func() time.Time

	mu    sync.Mutex
	m     map[string]*idemEntry
	queue []string // завершённые ключи в порядке завершения — для вытеснения
}

type idemEntry struct {
	fingerprint string
	done        chan struct{}
	resp        *CommandResponse // nil — выполнение прервано до начала, повтор выполнит заново
	finishedAt  time.Time
}

func newIdemCache(ttl time.Duration, max int, now func() time.Time) *idemCache {
	return &idemCache{ttl: ttl, max: max, now: now, m: map[string]*idemEntry{}}
}

func idemKey(deviceID, requestID string) string { return deviceID + "\x00" + requestID }

// fingerprint — отпечаток метода и параметров: повтор request_id с другим
// содержимым — ошибка клиента, а не повтор.
func fingerprint(method string, params json.RawMessage) string {
	var buf bytes.Buffer
	if len(params) > 0 && json.Compact(&buf, params) != nil {
		buf.Reset()
		buf.Write(params)
	}
	h := sha256.New()
	h.Write([]byte(method))
	h.Write([]byte{0})
	h.Write(buf.Bytes())
	return hex.EncodeToString(h.Sum(nil))
}

// acquire возвращает (entry, owner). owner=true — вызывающий выполняет команду
// и обязан вызвать finish. owner=false — запись уже есть: дождаться wait.
func (c *idemCache) acquire(key, fp string) (*idemEntry, bool) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if e, ok := c.m[key]; ok {
		return e, false
	}
	e := &idemEntry{fingerprint: fp, done: make(chan struct{})}
	c.m[key] = e
	return e, true
}

// finish завершает выполнение. resp == nil — команда не начиналась, запись удаляется.
func (c *idemCache) finish(key string, e *idemEntry, resp *CommandResponse) {
	c.mu.Lock()
	defer c.mu.Unlock()
	e.resp = resp
	if resp == nil {
		delete(c.m, key)
	} else {
		e.finishedAt = c.now()
		c.queue = append(c.queue, key)
	}
	close(e.done)
	c.evictLocked()
}

// wait ждёт завершения чужого выполнения.
func (c *idemCache) wait(ctx context.Context, e *idemEntry) (*CommandResponse, error) {
	select {
	case <-e.done:
		return e.resp, nil
	case <-ctx.Done():
		return nil, ctx.Err()
	}
}

// cleanup удаляет просроченные записи.
func (c *idemCache) cleanup() {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.evictLocked()
}

// reset очищает завершённые записи (сброс перед тестом); выполняющиеся не трогает.
func (c *idemCache) reset() {
	c.mu.Lock()
	defer c.mu.Unlock()
	for _, k := range c.queue {
		delete(c.m, k)
	}
	c.queue = nil
}

func (c *idemCache) evictLocked() {
	cutoff := c.now().Add(-c.ttl)
	n := 0
	for n < len(c.queue) {
		k := c.queue[n]
		e, ok := c.m[k]
		if ok && e.finishedAt.After(cutoff) && len(c.queue)-n <= c.max {
			break
		}
		if ok && e.resp != nil {
			delete(c.m, k)
		}
		n++
	}
	if n > 0 {
		c.queue = append(c.queue[:0:0], c.queue[n:]...)
	}
}

func (c *idemCache) len() int {
	c.mu.Lock()
	defer c.mu.Unlock()
	return len(c.m)
}

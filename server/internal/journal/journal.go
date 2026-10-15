package journal

import (
	"bufio"
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
)

const (
	filePrefix = "journal-"
	fileSuffix = ".jsonl"

	// DefaultMaxFileBytes — порог ротации текущего файла.
	DefaultMaxFileBytes = 32 << 20
	// DefaultMaxFiles — сколько файлов (включая текущий) держим на диске.
	DefaultMaxFiles = 8

	maxLineBytes = 8 << 20
)

// ErrClosed — журнал закрыт.
var ErrClosed = errors.New("журнал закрыт")

// Options — параметры журнала.
type Options struct {
	Dir          string
	MaxFileBytes int64 // 0 — DefaultMaxFileBytes
	MaxFiles     int   // 0 — DefaultMaxFiles
	Now          func() time.Time
}

type fileInfo struct {
	num      int
	path     string
	firstSeq int64 // 0 — файл пуст или первая запись неизвестна
}

// Journal — append-only журнал в каталоге Dir. Файлы именуются
// journal-NNNNNN.jsonl; текущий — с наибольшим номером. Ротация не
// переименовывает файлы (на Windows открытый читателем файл не переименовать),
// а открывает следующий номер; старые сверх MaxFiles удаляются.
type Journal struct {
	opts Options

	mu      sync.Mutex
	files   []fileInfo
	cur     *os.File
	curSize int64
	seq     int64
	subs    map[*Subscription]struct{}
	closed  bool
}

// Open открывает (или создаёт) журнал и восстанавливает последний seq.
func Open(opts Options) (*Journal, error) {
	if opts.Dir == "" {
		return nil, errors.New("журнал: не задан каталог")
	}
	if opts.MaxFileBytes <= 0 {
		opts.MaxFileBytes = DefaultMaxFileBytes
	}
	if opts.MaxFiles <= 0 {
		opts.MaxFiles = DefaultMaxFiles
	}
	if opts.Now == nil {
		opts.Now = time.Now
	}
	if err := os.MkdirAll(opts.Dir, 0o755); err != nil {
		return nil, fmt.Errorf("создать каталог журнала %s: %w", opts.Dir, err)
	}
	j := &Journal{opts: opts, subs: map[*Subscription]struct{}{}}
	if err := j.load(); err != nil {
		return nil, err
	}
	return j, nil
}

// load находит файлы журнала, восстанавливает seq и открывает текущий файл.
func (j *Journal) load() error {
	ents, err := os.ReadDir(j.opts.Dir)
	if err != nil {
		return fmt.Errorf("прочитать каталог журнала: %w", err)
	}
	for _, de := range ents {
		name := de.Name()
		if de.IsDir() || !strings.HasPrefix(name, filePrefix) || !strings.HasSuffix(name, fileSuffix) {
			continue
		}
		n, err := strconv.Atoi(strings.TrimSuffix(strings.TrimPrefix(name, filePrefix), fileSuffix))
		if err != nil || n <= 0 {
			continue
		}
		j.files = append(j.files, fileInfo{num: n, path: filepath.Join(j.opts.Dir, name)})
	}
	sort.Slice(j.files, func(a, b int) bool { return j.files[a].num < j.files[b].num })

	for i := range j.files {
		j.files[i].firstSeq = firstSeq(j.files[i].path)
	}
	// Последний seq — из самого нового файла, где есть хотя бы одна валидная запись.
	for i := len(j.files) - 1; i >= 0 && j.seq == 0; i-- {
		j.seq = lastSeq(j.files[i].path)
	}

	if len(j.files) == 0 {
		j.files = append(j.files, fileInfo{num: 1, path: j.pathFor(1)})
	}
	return j.openCurrent()
}

func (j *Journal) pathFor(num int) string {
	return filepath.Join(j.opts.Dir, fmt.Sprintf("%s%06d%s", filePrefix, num, fileSuffix))
}

func (j *Journal) openCurrent() error {
	cur := j.files[len(j.files)-1]
	f, err := os.OpenFile(cur.path, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0o644)
	if err != nil {
		return fmt.Errorf("открыть файл журнала %s: %w", cur.path, err)
	}
	st, err := f.Stat()
	if err != nil {
		f.Close()
		return fmt.Errorf("stat %s: %w", cur.path, err)
	}
	j.cur, j.curSize = f, st.Size()
	// После аварийной остановки строка могла остаться недописанной — закрываем её,
	// чтобы следующая запись легла с новой строки (битая строка при чтении пропускается).
	if j.curSize > 0 && !endsWithNewline(cur.path, j.curSize) {
		n, err := f.Write([]byte{'\n'})
		if err != nil {
			return fmt.Errorf("дописать перевод строки в %s: %w", cur.path, err)
		}
		j.curSize += int64(n)
	}
	return nil
}

// Append добавляет запись: присваивает seq, при пустом TS ставит текущее время (UTC).
func (j *Journal) Append(e Entry) (Entry, error) {
	j.mu.Lock()
	defer j.mu.Unlock()
	if j.closed {
		return e, ErrClosed
	}
	e.Seq = j.seq + 1
	if e.TS.IsZero() {
		e.TS = j.opts.Now().UTC()
	}
	line, err := Marshal(&e)
	if err != nil {
		return e, fmt.Errorf("сериализовать запись журнала: %w", err)
	}
	line = append(line, '\n')

	if j.curSize > 0 && j.curSize+int64(len(line)) > j.opts.MaxFileBytes {
		if err := j.rotate(); err != nil {
			return e, err
		}
	}
	n, err := j.cur.Write(line)
	j.curSize += int64(n)
	if err != nil {
		return e, fmt.Errorf("записать в журнал: %w", err)
	}
	j.seq = e.Seq
	if last := &j.files[len(j.files)-1]; last.firstSeq == 0 {
		last.firstSeq = e.Seq
	}
	j.broadcast(e)
	return e, nil
}

// rotate закрывает текущий файл, открывает следующий и удаляет лишние старые.
func (j *Journal) rotate() error {
	if err := j.cur.Close(); err != nil {
		return fmt.Errorf("закрыть файл журнала: %w", err)
	}
	num := j.files[len(j.files)-1].num + 1
	j.files = append(j.files, fileInfo{num: num, path: j.pathFor(num)})
	if err := j.openCurrent(); err != nil {
		return err
	}
	for len(j.files) > j.opts.MaxFiles {
		old := j.files[0]
		j.files = j.files[1:]
		// Файл может быть открыт читателем (Windows) — тогда он останется на диске
		// и будет подхвачен при следующем старте. Журнал от этого не ломается.
		if err := os.Remove(old.path); err != nil && !errors.Is(err, os.ErrNotExist) {
			slog.Warn("журнал: не удалось удалить старый файл", "path", old.path, "err", err)
		}
	}
	return nil
}

// Seq — номер последней записи.
func (j *Journal) Seq() int64 {
	j.mu.Lock()
	defer j.mu.Unlock()
	return j.seq
}

// Stats — сводка для /health и UI.
type Stats struct {
	LastSeq      int64 `json:"last_seq"`
	Files        int   `json:"files"`
	CurrentBytes int64 `json:"current_bytes"`
	Subscribers  int   `json:"subscribers"`
}

func (j *Journal) Stats() Stats {
	j.mu.Lock()
	defer j.mu.Unlock()
	return Stats{LastSeq: j.seq, Files: len(j.files), CurrentBytes: j.curSize, Subscribers: len(j.subs)}
}

// Close закрывает файл и все подписки.
func (j *Journal) Close() error {
	j.mu.Lock()
	defer j.mu.Unlock()
	if j.closed {
		return nil
	}
	j.closed = true
	for s := range j.subs {
		close(s.ch)
	}
	j.subs = nil
	return j.cur.Close()
}

// --- чтение ---

type snapshotFile struct {
	path  string
	limit int64 // -1 — читать целиком
}

// snapshot фиксирует набор файлов и размер текущего: читаем только целые строки,
// записанные до вызова, без удержания блокировки на время чтения.
func (j *Journal) snapshot(afterSeq int64) ([]snapshotFile, error) {
	j.mu.Lock()
	defer j.mu.Unlock()
	if j.closed {
		return nil, ErrClosed
	}
	var out []snapshotFile
	for i, fi := range j.files {
		// Все записи файла i имеют seq < firstSeq файла i+1 — если он <= afterSeq+1, файл не нужен.
		if i+1 < len(j.files) {
			if next := j.files[i+1].firstSeq; next > 0 && next <= afterSeq+1 {
				continue
			}
		}
		limit := int64(-1)
		if i == len(j.files)-1 {
			limit = j.curSize
		}
		out = append(out, snapshotFile{path: fi.path, limit: limit})
	}
	return out, nil
}

// Scan перебирает подходящие записи по возрастанию seq. fn возвращает false — остановка.
// raw — исходная строка JSON (без перевода строки), действительна только внутри fn.
func (j *Journal) Scan(f Filter, fn func(e *Entry, raw []byte) bool) error {
	files, err := j.snapshot(f.AfterSeq)
	if err != nil {
		return err
	}
	for _, sf := range files {
		cont, err := scanFile(sf, &f, fn)
		if err != nil {
			return err
		}
		if !cont {
			return nil
		}
	}
	return nil
}

// Tail возвращает последние limit подходящих записей по возрастанию seq.
func (j *Journal) Tail(f Filter, limit int) ([]Entry, error) {
	if limit <= 0 {
		return nil, nil
	}
	ring := make([]Entry, 0, limit)
	start := 0
	err := j.Scan(f, func(e *Entry, _ []byte) bool {
		if len(ring) < limit {
			ring = append(ring, *e)
		} else {
			ring[start] = *e
			start = (start + 1) % limit
		}
		return true
	})
	if err != nil {
		return nil, err
	}
	return append(ring[start:], ring[:start]...), nil
}

func scanFile(sf snapshotFile, f *Filter, fn func(e *Entry, raw []byte) bool) (bool, error) {
	fh, err := os.Open(sf.path)
	if err != nil {
		if errors.Is(err, os.ErrNotExist) { // удалён ротацией между снимком и чтением
			return true, nil
		}
		return false, fmt.Errorf("открыть %s: %w", sf.path, err)
	}
	defer fh.Close()
	var r io.Reader = fh
	if sf.limit >= 0 {
		r = io.LimitReader(fh, sf.limit)
	}
	sc := bufio.NewScanner(r)
	sc.Buffer(make([]byte, 0, 64<<10), maxLineBytes)
	for sc.Scan() {
		raw := sc.Bytes()
		if len(bytes.TrimSpace(raw)) == 0 {
			continue
		}
		var e Entry
		if err := json.Unmarshal(raw, &e); err != nil {
			continue // битая строка (обрыв записи) — пропускаем
		}
		if !f.Match(&e) || !f.matchText(raw) {
			continue
		}
		if !fn(&e, raw) {
			return false, nil
		}
	}
	if err := sc.Err(); err != nil {
		return false, fmt.Errorf("прочитать %s: %w", sf.path, err)
	}
	return true, nil
}

// --- подписка ---

// Subscription — живой поток новых записей. Канал C закрывается при Close журнала,
// при отписке или если подписчик не успевает читать (тогда Lagged() == true,
// и клиент должен дочитать пропущенное через Scan с AfterSeq).
type Subscription struct {
	j  *Journal
	ch chan Entry
	C  <-chan Entry
	// StartSeq — последний seq на момент подписки: в канал придут записи с seq > StartSeq,
	// всё до него включительно дочитывается через Scan без пропусков и дублей.
	StartSeq int64
	lagged   bool
}

// Subscribe подписывает на новые записи. buf — размер буфера канала.
func (j *Journal) Subscribe(buf int) (*Subscription, error) {
	if buf <= 0 {
		buf = 256
	}
	j.mu.Lock()
	defer j.mu.Unlock()
	if j.closed {
		return nil, ErrClosed
	}
	ch := make(chan Entry, buf)
	s := &Subscription{j: j, ch: ch, C: ch, StartSeq: j.seq}
	j.subs[s] = struct{}{}
	return s, nil
}

// Close отписывает. Повторный вызов безопасен.
func (s *Subscription) Close() {
	s.j.mu.Lock()
	defer s.j.mu.Unlock()
	if _, ok := s.j.subs[s]; ok {
		delete(s.j.subs, s)
		close(s.ch)
	}
}

// Lagged — подписка закрыта из-за переполнения буфера.
func (s *Subscription) Lagged() bool {
	s.j.mu.Lock()
	defer s.j.mu.Unlock()
	return s.lagged
}

// broadcast вызывается под j.mu.
func (j *Journal) broadcast(e Entry) {
	for s := range j.subs {
		select {
		case s.ch <- e:
		default:
			s.lagged = true
			delete(j.subs, s)
			close(s.ch)
		}
	}
}

// --- восстановление ---

func firstSeq(path string) int64 {
	var seq int64
	_ = scanRaw(path, func(e *Entry) bool { seq = e.Seq; return false })
	return seq
}

func lastSeq(path string) int64 {
	var seq int64
	_ = scanRaw(path, func(e *Entry) bool {
		if e.Seq > seq {
			seq = e.Seq
		}
		return true
	})
	return seq
}

func scanRaw(path string, fn func(e *Entry) bool) error {
	_, err := scanFile(snapshotFile{path: path, limit: -1}, &Filter{AfterSeq: -1 << 62}, func(e *Entry, _ []byte) bool { return fn(e) })
	return err
}

func endsWithNewline(path string, size int64) bool {
	f, err := os.Open(path)
	if err != nil {
		return true
	}
	defer f.Close()
	b := make([]byte, 1)
	if _, err := f.ReadAt(b, size-1); err != nil {
		return true
	}
	return b[0] == '\n'
}

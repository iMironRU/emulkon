package journal

import (
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
	"time"
)

func openTest(t *testing.T, dir string, maxBytes int64, maxFiles int) *Journal {
	t.Helper()
	j, err := Open(Options{Dir: dir, MaxFileBytes: maxBytes, MaxFiles: maxFiles})
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	return j
}

func mustAppend(t *testing.T, j *Journal, e Entry) Entry {
	t.Helper()
	out, err := j.Append(e)
	if err != nil {
		t.Fatalf("Append: %v", err)
	}
	return out
}

func TestAppendAssignsSeqAndPersistsAcrossReopen(t *testing.T) {
	dir := t.TempDir()
	j := openTest(t, dir, 0, 0)
	for i := 0; i < 3; i++ {
		e := mustAppend(t, j, Entry{Direction: DirIn, DeviceID: "kkt-01", Method: "M"})
		if e.Seq != int64(i+1) {
			t.Fatalf("seq = %d, want %d", e.Seq, i+1)
		}
		if e.TS.IsZero() {
			t.Fatal("TS не проставлен")
		}
	}
	j.Close()

	j = openTest(t, dir, 0, 0)
	defer j.Close()
	if got := j.Seq(); got != 3 {
		t.Fatalf("после переоткрытия seq = %d, want 3", got)
	}
	if e := mustAppend(t, j, Entry{Direction: DirOut}); e.Seq != 4 {
		t.Fatalf("seq = %d, want 4", e.Seq)
	}
}

func TestRotationKeepsSeqOrderAndPrunes(t *testing.T) {
	dir := t.TempDir()
	j := openTest(t, dir, 300, 3)
	defer j.Close()
	for i := 0; i < 40; i++ {
		mustAppend(t, j, Entry{Direction: DirIn, DeviceID: "kkt-01", Method: "Ping", Payload: json.RawMessage(`{"n":1}`)})
	}
	files, _ := filepath.Glob(filepath.Join(dir, "journal-*.jsonl"))
	if len(files) != 3 {
		t.Fatalf("файлов %d, want 3 (MaxFiles)", len(files))
	}
	all, err := j.Tail(Filter{}, 1000)
	if err != nil {
		t.Fatal(err)
	}
	if len(all) == 0 || all[len(all)-1].Seq != 40 {
		t.Fatalf("последняя запись не 40: %+v", all)
	}
	for i := 1; i < len(all); i++ {
		if all[i].Seq != all[i-1].Seq+1 {
			t.Fatalf("разрыв seq: %d → %d", all[i-1].Seq, all[i].Seq)
		}
	}
}

func TestFilterAndTail(t *testing.T) {
	j := openTest(t, t.TempDir(), 0, 0)
	defer j.Close()
	base := time.Date(2026, 10, 8, 12, 0, 0, 0, time.UTC)
	for i := 0; i < 10; i++ {
		dev := "kkt-01"
		if i%2 == 1 {
			dev = "scales-01"
		}
		mustAppend(t, j, Entry{
			TS: base.Add(time.Duration(i) * time.Minute), Direction: DirIn, DeviceID: dev,
			Method: "Ping", Payload: json.RawMessage(`{"label":"Чек №` + string(rune('0'+i)) + `"}`),
		})
	}

	got, _ := j.Tail(Filter{DeviceID: "scales-01"}, 2)
	if len(got) != 2 || got[0].Seq != 8 || got[1].Seq != 10 {
		t.Fatalf("tail по устройству: %+v", got)
	}

	got, _ = j.Tail(Filter{Since: base.Add(3 * time.Minute), Until: base.Add(5 * time.Minute)}, 100)
	if len(got) != 2 || got[0].Seq != 4 || got[1].Seq != 5 {
		t.Fatalf("фильтр по времени: %+v", got)
	}

	got, _ = j.Tail(Filter{Text: "ЧЕК №7"}, 100)
	if len(got) != 1 || got[0].Seq != 8 {
		t.Fatalf("поиск текста: %+v", got)
	}

	got, _ = j.Tail(Filter{AfterSeq: 7, BeforeSeq: 10}, 100)
	if len(got) != 2 || got[0].Seq != 8 || got[1].Seq != 9 {
		t.Fatalf("диапазон seq: %+v", got)
	}
}

func TestTornLastLineIsSkipped(t *testing.T) {
	dir := t.TempDir()
	j := openTest(t, dir, 0, 0)
	mustAppend(t, j, Entry{Direction: DirIn})
	mustAppend(t, j, Entry{Direction: DirOut})
	j.Close()

	// Имитация обрыва записи посреди строки.
	path := filepath.Join(dir, "journal-000001.jsonl")
	f, _ := os.OpenFile(path, os.O_WRONLY|os.O_APPEND, 0o644)
	f.WriteString(`{"seq":3,"ts":"2026-`)
	f.Close()

	j = openTest(t, dir, 0, 0)
	defer j.Close()
	if j.Seq() != 2 {
		t.Fatalf("seq = %d, want 2", j.Seq())
	}
	mustAppend(t, j, Entry{Direction: DirSystem})
	got, _ := j.Tail(Filter{}, 10)
	if len(got) != 3 || got[2].Seq != 3 || got[2].Direction != DirSystem {
		t.Fatalf("после обрыва: %+v", got)
	}
}

func TestSubscribeReceivesAndLags(t *testing.T) {
	j := openTest(t, t.TempDir(), 0, 0)
	defer j.Close()

	s, err := j.Subscribe(2)
	if err != nil {
		t.Fatal(err)
	}
	mustAppend(t, j, Entry{Direction: DirIn})
	if e := <-s.C; e.Seq != 1 {
		t.Fatalf("получен seq %d", e.Seq)
	}

	// Переполнение: буфер 2, пишем 3 без чтения — подписка закрывается как отставшая.
	for i := 0; i < 3; i++ {
		mustAppend(t, j, Entry{Direction: DirIn})
	}
	n := 0
	for range s.C {
		n++
	}
	if n != 2 || !s.Lagged() {
		t.Fatalf("прочитано %d, lagged=%v", n, s.Lagged())
	}
	s.Close() // повторное закрытие безопасно
}

func TestScanSkipsOldFilesByAfterSeq(t *testing.T) {
	j := openTest(t, t.TempDir(), 200, 50)
	defer j.Close()
	for i := 0; i < 30; i++ {
		mustAppend(t, j, Entry{Direction: DirIn, Method: "X"})
	}
	files, _ := j.snapshot(25)
	all, _ := j.snapshot(0)
	if len(files) >= len(all) {
		t.Fatalf("snapshot(25) не отбросил старые файлы: %d из %d", len(files), len(all))
	}
	var seqs []int64
	j.Scan(Filter{AfterSeq: 25}, func(e *Entry, _ []byte) bool { seqs = append(seqs, e.Seq); return true })
	if len(seqs) != 5 || seqs[0] != 26 {
		t.Fatalf("after_seq=25: %v", seqs)
	}
}

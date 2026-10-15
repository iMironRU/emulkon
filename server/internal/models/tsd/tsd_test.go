package tsd

import (
	"context"
	"encoding/base64"
	"strings"
	"testing"

	"emulkon/internal/models"
)

func call(m *Model, method string, params ...any) models.Result {
	return m.Handle(context.Background(), method, params)
}

func ok(r models.Result) bool { b, _ := r.Return.(bool); return b }

func b(s string) string { return base64.StdEncoding.EncodeToString([]byte(s)) }

func table(full bool, recs ...string) string {
	f := "false"
	if full {
		f = "true"
	}
	return `<?xml version="1.0" encoding="UTF-8"?><Table FullLoad="` + f + `">` + strings.Join(recs, "") + `</Table>`
}

func rec(code, name, price string) string {
	return `<Record BarCodeBase64="` + b(code) + `" Name="` + name + `" UnitOfMeasurement="шт" Price="` + price + `" Quantity="7"/>`
}

func TestUploadDownload(t *testing.T) {
	m := New(Options{Version: "test"})
	// две части: first и last (как БПО: first, regular…, last)
	if r := call(m, "UploadTable", "emulkon-1", table(true, rec("2000000000053", "Блокнот", "25,30")), "first"); !ok(r) {
		t.Fatalf("first: %+v", r)
	}
	if r := call(m, "UploadTable", "emulkon-1", table(true, rec("4600000000008", "Финики", "95.50")), "last"); !ok(r) {
		t.Fatalf("last: %+v", r)
	}
	st := m.State()
	goods := st["goods"].([]Good)
	if st["goods_count"] != 2 || goods[0].Price != 25.3 || goods[1].Name != "Финики" || st["upload"].(*upload).Packages != 2 {
		t.Fatalf("товары: %+v", st)
	}
	// частичная выгрузка дополняет и заменяет по штрихкоду
	call(m, "UploadTable", "emulkon-1", table(false, rec("4600000000008", "Финики в банке", "99")), "last")
	if st := m.State(); st["goods_count"] != 2 || st["goods"].([]Good)[1].Name != "Финики в банке" {
		t.Fatalf("частичная: %+v", st)
	}
	// полная очищает
	call(m, "UploadTable", "emulkon-1", table(true, rec("1", "Один", "1")), "last")
	if m.State()["goods_count"] != 1 {
		t.Fatal("полная выгрузка не очистила товары")
	}

	m.Act("collect", map[string]any{"barcode": "1", "quantity": 2.0})
	m.Act("collect", map[string]any{"barcode": "1", "quantity": 0.5})
	m.Act("collect", map[string]any{"barcode": "4607000000001"})
	r := call(m, "DownloadTable", "emulkon-1", "")
	s, _ := r.Params[1].(string)
	if !ok(r) || !strings.Contains(s, `BarCodeBase64="`+b("1")+`" Quantity="2.5"`) || !strings.Contains(s, `BarCodeBase64="`+b("4607000000001")+`" Quantity="1"`) {
		t.Fatalf("загрузка: %s", s)
	}
	if doc := m.State()["document"].([]Item); doc[0].Name != "Один" || doc[1].Name != "" {
		t.Fatalf("имена в документе: %+v", doc)
	}
	if r := call(m, "ClearTable", "emulkon-1"); !ok(r) || m.State()["goods_count"] != 0 || len(m.State()["document"].([]Item)) != 2 {
		t.Fatal("ClearTable очищает товары, документ остаётся")
	}
	m.Act("clear_document", nil)
	r = call(m, "DownloadTable", "emulkon-1", "")
	if s, _ := r.Params[1].(string); !strings.Contains(s, "<Table></Table>") {
		t.Fatalf("пустой документ: %s", s)
	}
}

func TestUploadErrors(t *testing.T) {
	m := New(Options{Version: "test"})
	for _, c := range []struct {
		xml, status string
		code        int
	}{
		{table(true, rec("1", "x", "1")), "next", ErrPackageStatus},
		{`<Table>`, "last", ErrBadParams},
		{table(true, `<Record BarCodeBase64="не base64!" Name="x"/>`), "last", ErrBadParams},
		{table(true, `<Record Name="без штрихкода"/>`), "last", ErrBadParams},
	} {
		if r := call(m, "UploadTable", "emulkon-1", c.xml, c.status); ok(r) || r.ErrorCode != c.code {
			t.Errorf("%s %s: %+v", c.xml, c.status, r)
		}
	}
	if _, err := m.Act("collect", map[string]any{"barcode": "1", "quantity": -1.0}); err == nil {
		t.Error("отрицательное количество")
	}
}

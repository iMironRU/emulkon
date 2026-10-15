package printers

import (
	"context"
	"encoding/base64"
	"encoding/json"
	"strings"
	"testing"
	"time"

	"emulkon/internal/models"
)

var testNow = func() time.Time { return time.Date(2026, 10, 8, 18, 5, 0, 0, time.Local) }

func call(m models.Model, method string, params ...any) models.Result {
	return m.Handle(context.Background(), method, params)
}

func ok(r models.Result) bool { b, _ := r.Return.(bool); return b }

func b64(s string) string { return base64.StdEncoding.EncodeToString([]byte(s)) }

func lastDoc(t *testing.T, p models.Printer) models.PrintedDocument {
	t.Helper()
	docs := p.Prints(1)
	if len(docs) != 1 {
		t.Fatalf("лента пуста")
	}
	return docs[0]
}

// --- принтер чеков ---

func TestReceiptPrintText(t *testing.T) {
	m := NewReceipt(Options{Version: "test", Now: testNow})
	doc := `<?xml version="1.0" encoding="UTF-8"?><Document><Positions>
<TextString Text="Участие в дисконтной системе"/>
<TextString Text="Очень длинная строка, которая не помещается в ширину ленты принтера чеков"/>
<Barcode Type="EAN13" ValueBase64="` + b64("2000021262157") + `"/>
<Barcode Type="QR" ValueBase64="` + b64("https://example.test/q") + `"/>
</Positions></Document>`
	if r := call(m, "PrintTextDocument", "emulkon-1", doc); !ok(r) {
		t.Fatalf("печать: %+v", r)
	}
	d := lastDoc(t, m)
	text := strings.Join(d.Lines, "\n")
	for _, want := range []string{emuMark, "Участие в дисконтной системе", "[EAN13: 2000021262157]", "[QR: https://example.test/q]", "08.10.2026 18:05"} {
		if !strings.Contains(text, want) {
			t.Errorf("нет %q:\n%s", want, text)
		}
	}
	for _, l := range d.Lines {
		if n := len([]rune(l)); n > 42 {
			t.Errorf("строка длиннее ленты (%d): %q", n, l)
		}
	}
	if d.Kind != "text" || d.Seq != 1 || m.State()["documents"] != 1 {
		t.Fatalf("документ: %+v %+v", d, m.State())
	}

	// старые ревизии: BarcodeType/Barcode
	old := `<Document><Positions><Barcode BarcodeType="EAN13" Barcode="4600000000008"/></Positions></Document>`
	if r := call(m, "PrintTextDocument", "emulkon-1", old); !ok(r) || !strings.Contains(strings.Join(lastDoc(t, m).Lines, "\n"), "[EAN13: 4600000000008]") {
		t.Fatalf("старый штрихкод: %+v", r)
	}
}

func TestReceiptErrors(t *testing.T) {
	m := NewReceipt(Options{Version: "test", Now: testNow})
	text := `<Document><Positions><TextString Text="Тест"/></Positions></Document>`
	cases := []struct {
		patch map[string]any
		doc   string
		code  int
	}{
		{map[string]any{"paper": false}, text, ErrNoPaper},
		{map[string]any{"cover_open": true}, text, ErrCoverOpen},
		{nil, `<Document><Positions><Barcode Type="AZTEC" ValueBase64="MQ=="/></Positions></Document>`, ErrBarcodeType},
		{map[string]any{"barcodes": false}, `<Document><Positions><Barcode Type="EAN13" ValueBase64="MQ=="/></Positions></Document>`, ErrBarcodeType},
		{nil, `<Document>`, ErrBadParams},
		{nil, ``, ErrBadParams},
	}
	for _, c := range cases {
		m.Reset()
		if c.patch != nil {
			if err := m.SetState(c.patch); err != nil {
				t.Fatal(err)
			}
		}
		if r := call(m, "PrintTextDocument", "emulkon-1", c.doc); ok(r) || r.ErrorCode != c.code {
			t.Errorf("%v %q: %+v, ожидается %d", c.patch, c.doc, r, c.code)
		}
	}
	if len(m.Prints(10)) != 0 {
		t.Fatal("ошибочный документ попал на ленту")
	}
	if r := call(m, "ProcessCheck", "emulkon-1", false, "", ""); ok(r) || r.ErrorCode != ErrNotSupported {
		t.Fatalf("фискальный метод: %+v", r)
	}
}

func TestReceiptDrawerAndLineLength(t *testing.T) {
	m := NewReceipt(Options{Version: "test", Now: testNow})
	if err := m.SetState(map[string]any{"line_length": 32.0}); err != nil {
		t.Fatal(err)
	}
	r := call(m, "GetLineLength", "emulkon-1", 0)
	if !ok(r) || r.Params[1] != int64(32) {
		t.Fatalf("ширина: %+v", r)
	}
	call(m, "OpenCashDrawer", "emulkon-1")
	call(m, "OpenCashDrawer", "emulkon-1")
	if st := m.State(); st["drawer_opens"] != 2 || st["drawer_opened_at"] == nil {
		t.Fatalf("ящик: %+v", st)
	}
	for _, bad := range []map[string]any{{"line_length": 10.0}, {"line_length": 40.5}, {"paper": "нет"}, {"color": 1.0},
		{"paper": false, "line_length": 100.0}} {
		if err := m.SetState(bad); err == nil {
			t.Errorf("%v принят", bad)
		}
	}
	if st := m.State(); st["paper"] != true || st["line_length"] != 32 {
		t.Fatalf("неудачный SetState изменил состояние: %+v", st)
	}
	r = call(m, "EquipmentTest", "", "", "", "")
	if s, _ := r.Params[2].(string); !ok(r) || !strings.Contains(s, "ЭМУЛЯТОР принтера чеков") {
		t.Fatalf("тест: %+v", r)
	}
}

// --- принтер этикеток ---

const formattingXML = `<Formatting Width="60" Height="40">
<Text FieldName="Наименование" Left="0" Top="0" Width="58" Height="10" FontName="Tahoma" FontSize="8" FontStyle="Bold"/>
<Text FieldName="Цена" Left="0" Top="11" Width="30" Height="10" FontSize="12" Align="Right"/>
<Text FieldName="Магазин" Left="31" Top="11" Width="27" Height="10" FontSize="6" Static="true" Value="ЭМУЛ-МАРКЕТ"/>
<Barcode FieldName="Штрихкод" Type="EAN13" Left="0" Top="22" Height="15" PrintHRI="true" FontSize="8"/>
<Image FieldName="Лого" Left="50" Top="0" Width="8" Height="8"/>
<UserData FieldName="Служебное" Value="x"/>
</Formatting>`

func labelsXML(withFormatting bool, labels string) string {
	f := ""
	if withFormatting {
		f = formattingXML
	}
	return `<?xml version="1.0" encoding="UTF-8"?><Data>` + f + `<Labels>` + labels + `</Labels></Data>`
}

const twoLabels = `<Label Quantity="2"><Record FieldName="Наименование" Value="Блокнот для заметок"/><Record FieldName="Цена" Value="43.5"/><Record FieldName="Штрихкод" Value="4008110271538"/></Label>
<Label Quantity="3"><Record FieldName="Наименование" Value="Яблоки антоновка в банке"/><Record FieldName="Цена" Value="78.5"/><Record FieldName="Штрихкод" ValueBase64="` + "MjkwMDAwMTM1NTY3OQ==" + `"/><Record FieldName="Магазин" Value="не меняется"/></Label>`

func TestLabelPrintJob(t *testing.T) {
	m := NewLabel(Options{Version: "test", Now: testNow})
	if r := call(m, "InitializePrinter", "emulkon-1"); !ok(r) || m.State()["initialized"] != true {
		t.Fatalf("инициализация: %+v", r)
	}
	// задание из трёх пакетов, как шлёт БПО 3.3.4.30: first, first (без разметки), last
	steps := []struct {
		fmt    bool
		status string
	}{{true, "first"}, {false, "first"}, {false, "last"}}
	for _, s := range steps {
		if r := call(m, "PrintLabels", "emulkon-1", labelsXML(s.fmt, twoLabels), s.status); !ok(r) {
			t.Fatalf("пакет %v %s: %+v", s.fmt, s.status, r)
		}
	}
	st := m.State()
	j := st["job"].(job)
	if j.Packages != 3 || j.Labels != 6 || j.Copies != 15 || j.Open || j.Status != "last" || st["jobs"] != 1 || st["labels_printed"] != 15 {
		t.Fatalf("задание: %+v %+v", j, st)
	}
	d := lastDoc(t, m)
	text := strings.Join(d.Lines, "\n")
	for _, want := range []string{"ЭМУЛЯТОР · ЭТИКЕТКИ", "ЭТИКЕТКА 60×40 ММ", "ПАКЕТ 3 (last): ЭТИКЕТОК 2, КОПИЙ 5",
		"[1] × 2", "Наименование: Блокнот для заметок", "Цена: 43.5", "Штрихкод: [EAN13: 4008110271538]",
		"[2] × 3", "Штрихкод: [EAN13: 2900001355679]", "Магазин: ЭМУЛ-МАРКЕТ", "Лого: [картинки нет]"} {
		if !strings.Contains(text, want) {
			t.Errorf("нет %q:\n%s", want, text)
		}
	}
	if strings.Contains(text, "не меняется") || strings.Contains(text, "Служебное") {
		t.Errorf("Static или UserData напечатаны неверно:\n%s", text)
	}
	if d.Kind != "label" || d.Layout == nil || d.Layout.WidthMM != 60 || len(d.Layout.Fields) != 5 || len(d.Layout.Labels) != 2 || d.Layout.Total != 2 {
		t.Fatalf("layout: %+v", d.Layout)
	}
	if v := d.Layout.Labels[1].Values; v["Штрихкод"] != "2900001355679" || v["Магазин"] != "ЭМУЛ-МАРКЕТ" || d.Layout.Labels[1].Quantity != 3 {
		t.Fatalf("значения layout: %+v", d.Layout.Labels[1])
	}
	if f := d.Layout.Fields[3]; f.Type != "barcode" || f.BarcodeType != "EAN13" || !f.HRI || f.Top != 22 {
		t.Fatalf("поле штрихкода: %+v", f)
	}

	// задание закрыто: пакет без разметки — 94
	if r := call(m, "PrintLabels", "emulkon-1", labelsXML(false, twoLabels), "regular"); ok(r) || r.ErrorCode != ErrNoLayout {
		t.Fatalf("без разметки после last: %+v", r)
	}
	// один пакет — last с разметкой
	if r := call(m, "PrintLabels", "emulkon-1", labelsXML(true, twoLabels), "last"); !ok(r) || m.State()["jobs"] != 2 {
		t.Fatalf("одиночный пакет: %+v", r)
	}
}

func TestLabelErrors(t *testing.T) {
	m := NewLabel(Options{Version: "test", Now: testNow})
	cases := []struct {
		name   string
		patch  map[string]any
		xml    string
		status string
		code   int
	}{
		{"статус", nil, labelsXML(true, twoLabels), "middle", ErrPackageStatus},
		{"без разметки", nil, labelsXML(false, twoLabels), "first", ErrNoLayout},
		{"нет Labels", nil, `<Data>` + formattingXML + `</Data>`, "last", ErrBadParams},
		{"чужое поле", nil, labelsXML(true, `<Label><Record FieldName="Вес" Value="1"/></Label>`), "last", ErrBadParams},
		{"штрихкод", nil, strings.Replace(labelsXML(true, twoLabels), `Type="EAN13"`, `Type="AZTEC"`, 1), "last", ErrBarcodeType},
		{"размер", nil, strings.Replace(labelsXML(true, twoLabels), `Width="60"`, `Width="0"`, 1), "last", ErrBadParams},
		{"количество", nil, labelsXML(true, `<Label Quantity="-1"/>`), "last", ErrBadParams},
		{"бумага", map[string]any{"paper": false}, labelsXML(true, twoLabels), "last", ErrNoPaper},
		{"крышка", map[string]any{"cover_open": true}, labelsXML(true, twoLabels), "last", ErrCoverOpen},
		{"не XML", nil, `<Data>`, "last", ErrBadParams},
	}
	for _, c := range cases {
		m.Reset()
		if c.patch != nil {
			if err := m.SetState(c.patch); err != nil {
				t.Fatal(err)
			}
		}
		if r := call(m, "PrintLabels", "emulkon-1", c.xml, c.status); ok(r) || r.ErrorCode != c.code {
			t.Errorf("%s: %+v, ожидается %d", c.name, r, c.code)
		}
		if len(m.Prints(5)) != 0 || m.State()["job"] != nil {
			t.Errorf("%s: ошибка изменила состояние: %+v", c.name, m.State())
		}
	}
	if err := m.SetState(map[string]any{"line_length": 40.0}); err == nil {
		t.Error("у принтера этикеток нет ширины строки")
	}
}

func TestLabelImageInLayout(t *testing.T) {
	m := NewLabel(Options{Version: "test", Now: testNow})
	small := b64("\x89PNG small")
	big := strings.Repeat("A", layoutImageMax+4)
	lbl := `<Label><Record FieldName="Лого" Value="` + small + `"/></Label><Label><Record FieldName="Лого" Value="` + big + `"/></Label>`
	if r := call(m, "PrintLabels", "emulkon-1", labelsXML(true, lbl), "last"); !ok(r) {
		t.Fatalf("%+v", r)
	}
	d := lastDoc(t, m)
	if d.Layout.Labels[0].Values["Лого"] != small {
		t.Fatalf("маленькая картинка: %+v", d.Layout.Labels[0])
	}
	if _, has := d.Layout.Labels[1].Values["Лого"]; has {
		t.Fatal("большая картинка попала в layout")
	}
	if text := strings.Join(d.Lines, "\n"); !strings.Contains(text, "Лого: [картинка, 49 КБ]") {
		t.Fatalf("размер картинки:\n%s", text)
	}
}

// Старая схема (БПО УТ 10.3): чек строками между ОткрытьЧек и ЗакрытьЧек.
func TestReceiptLegacyCheck(t *testing.T) {
	m := NewReceipt(Options{Version: "test", Now: testNow})
	if r := call(m, "PrintNonFiscalString", "emulkon-1", "до открытия"); ok(r) || r.ErrorCode != ErrCheckState {
		t.Fatalf("строка без открытого чека: %+v", r)
	}
	r := call(m, "OpenCheck", "emulkon-1", false, false, true, 0, 0)
	if !ok(r) || r.Params[4] != int64(1) {
		t.Fatalf("ОткрытьЧек: %+v", r)
	}
	if r := call(m, "OpenCheck", "emulkon-1", false, false, true, 0, 0); ok(r) || r.ErrorCode != ErrCheckState {
		t.Fatalf("второй ОткрытьЧек: %+v", r)
	}
	for _, c := range [][]any{
		{"PrintNonFiscalString", "emulkon-1", "Товарный чек"},
		{"PrintFiscalString", "emulkon-1", "Молоко", json.Number("2"), json.Number("50"), json.Number("100"), 0, 20},
		{"PrintBarCode", "emulkon-1", "EAN13", "4600000000008"},
		{"CloseCheck", "emulkon-1", json.Number("100"), 0, 0, 0},
	} {
		if r := call(m, c[0].(string), c[1:]...); !ok(r) {
			t.Fatalf("%s: %+v", c[0], r)
		}
	}
	text := strings.Join(lastDoc(t, m).Lines, "\n")
	for _, want := range []string{emuMark, "Товарный чек", "Молоко", "= 100.00", "[EAN13: 4600000000008]", "НАЛИЧНЫМИ"} {
		if !strings.Contains(text, want) {
			t.Errorf("нет %q:\n%s", want, text)
		}
	}
	// ОтменитьЧек: собранное не печатается.
	call(m, "OpenCheck", "emulkon-1", false, false, true, 0, 0)
	call(m, "PrintNonFiscalString", "emulkon-1", "отменённая строка")
	if r := call(m, "CancelCheck", "emulkon-1"); !ok(r) || m.State()["documents"] != 1 {
		t.Fatalf("ОтменитьЧек: %+v %v", r, m.State()["documents"])
	}
}

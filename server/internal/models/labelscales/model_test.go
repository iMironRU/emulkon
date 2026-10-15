package labelscales

import (
	"context"
	"encoding/base64"
	"errors"
	"strings"
	"testing"
	"time"

	"emulkon/internal/models"
)

func newModel() *Model {
	now := time.Date(2026, 10, 8, 15, 39, 41, 0, time.Local)
	return New("label-scales-01", Options{Version: "test", Now: func() time.Time { return now }})
}

func call(m *Model, method string, params ...any) models.Result {
	return m.Handle(context.Background(), method, params)
}

func ok(r models.Result) bool { b, _ := r.Return.(bool); return b }

const goodsXML = `<?xml version="1.0" encoding="UTF-8"?><Table>
<Record PLU="1" Code="1" IsWeightGoods="true" Name="Сыр Российский" Price="95,50" Description="Молоко, соль, закваска" ShelfLife="30"><AdditionalPrices Price1="90" Price2="0" Price3="0" Price4="0"/></Record>
<Record PLU="2" Code="17" IsWeightGoods="false" Name="Пирожок с капустой" Price="40" WeightPieceOfGoods="0.12"/>
</Table>`

func TestEANCheck(t *testing.T) {
	if got := eanCheck("400638133393"); got != 1 {
		t.Fatalf("4006381333931: контрольная %d", got)
	}
	if got := WeightBarcode("21", 1, 0.532); got != "2100001005320" {
		t.Fatalf("весовой штрихкод: %s", got)
	}
	if got := WeightBarcode("22", 12345, 1.5); got != "2212345015009" {
		t.Fatalf("весовой штрихкод: %s", got)
	}
}

func TestUploadGoodsPackages(t *testing.T) {
	m := newModel()
	if r := call(m, "UploadGoods", "emulkon-1", goodsXML, "first"); !ok(r) {
		t.Fatalf("first: %+v", r)
	}
	more := `<Table><Record PLU="2" Code="17" IsWeightGoods="false" Name="Пирожок с картошкой" Price="45"/><Record PLU="3" Code="3" IsWeightGoods="true" Name="Колбаса" Price="600.00"/></Table>`
	if r := call(m, "UploadGoods", "emulkon-1", more, "last"); !ok(r) {
		t.Fatalf("last: %+v", r)
	}
	st := m.State()
	if st["goods_count"] != 3 {
		t.Fatalf("товары дополняются по PLU: %+v", st)
	}
	goods := st["goods"].([]*Good)
	if goods[0].Price != 95.5 || goods[0].Prices[0] != 90 || goods[1].Name != "Пирожок с картошкой" || goods[1].IsWeight {
		t.Fatalf("товары: %+v %+v", goods[0], goods[1])
	}
	up := st["upload"].(*upload)
	if up.Packages != 2 || up.Records != 4 || up.Status != "last" {
		t.Fatalf("выгрузка: %+v", up)
	}
	// новая выгрузка после last начинает статистику заново, таблица остаётся
	if r := call(m, "UploadGoods", "emulkon-1", more, "regular"); !ok(r) {
		t.Fatalf("regular: %+v", r)
	}
	if up := m.State()["upload"].(*upload); up.Packages != 1 || up.Records != 2 {
		t.Fatalf("новая выгрузка: %+v", up)
	}

	for _, c := range []struct {
		xml, status string
		code        int
	}{
		{goodsXML, "middle", ErrPackageStatus},
		{"", "last", ErrBadParams},
		{"<Table>", "last", ErrBadParams},
		{`<Table><Record PLU="0" Name="Без PLU"/></Table>`, "last", ErrBadParams},
		{`<Table><Record PLU="5" Price="1"/></Table>`, "last", ErrBadParams},
	} {
		if r := call(m, "UploadGoods", "emulkon-1", c.xml, c.status); ok(r) || r.ErrorCode != c.code {
			t.Errorf("%q %s: %+v, ожидается %d", c.xml, c.status, r, c.code)
		}
	}
	if r := call(m, "ClearGoods", "emulkon-1"); !ok(r) || m.State()["goods_count"] != 0 {
		t.Fatalf("ClearGoods: %+v", r)
	}
}

// png — минимальный PNG 1×1.
var png = base64.StdEncoding.EncodeToString([]byte{0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0, 0, 0, 13, 'I', 'H', 'D', 'R'})

func TestImages(t *testing.T) {
	m := newModel()
	imgs := `<Table><Record PLU="1" Code="1" Picture="` + png + `"/><Record PLU="9" Code="9" Picture="` + png + `"/></Table>`
	if r := call(m, "UploadProductImages", "emulkon-1", imgs, "last"); !ok(r) {
		t.Fatalf("картинки: %+v", r)
	}
	if got := m.State()["images"].([]int64); len(got) != 2 || got[0] != 1 || got[1] != 9 {
		t.Fatalf("картинки по PLU: %v", got)
	}
	if up := m.State()["images_upload"].(*upload); up.Records != 2 || m.State()["upload"].(*upload) != nil {
		t.Fatalf("выгрузка картинок учитывается отдельно от товаров: %+v", up)
	}
	res, err := m.Act("image", map[string]any{"plu": 9.0})
	if err != nil || res.(map[string]any)["png_base64"] != png {
		t.Fatalf("image: %v %v", res, err)
	}
	if _, err := m.Act("image", map[string]any{"plu": 2.0}); err == nil {
		t.Fatal("картинки PLU 2 нет")
	}
	bad := `<Table><Record PLU="1" Code="1" Picture="` + base64.StdEncoding.EncodeToString([]byte("GIF89a-not-png")) + `"/></Table>`
	if r := call(m, "UploadProductImages", "emulkon-1", bad, "last"); ok(r) || r.ErrorCode != ErrBadParams {
		t.Fatalf("не PNG: %+v", r)
	}
	if r := call(m, "UploadProductImages", "emulkon-1", imgs, "next"); ok(r) || r.ErrorCode != ErrPackageStatus {
		t.Fatalf("статус: %+v", r)
	}
	call(m, "ClearGoods", "emulkon-1")
	if got := m.State()["images"].([]int64); len(got) != 0 {
		t.Fatalf("ClearGoods оставил картинки: %v", got)
	}
}

func TestPrintLabel(t *testing.T) {
	m := newModel()
	call(m, "UploadGoods", "emulkon-1", goodsXML, "last")

	res, err := m.Act("print_label", map[string]any{"plu": 1.0, "weight": 0.532})
	if err != nil {
		t.Fatal(err)
	}
	doc := res.(models.PrintedDocument)
	text := strings.Join(doc.Lines, "\n")
	for _, want := range []string{"ЭМУЛЯТОР", "Сыр Российский", "95.50 RUB", "0.532 КГ", "50.81 RUB", "08.10.26 15:39", "ГОДЕН ДО", "07.11.26", "Молоко", "[EAN-13 2100001005320]"} {
		if !strings.Contains(text, want) {
			t.Errorf("на этикетке нет %q:\n%s", want, text)
		}
	}
	if doc.Kind != "label" || doc.Seq != 1 {
		t.Fatalf("документ: %+v", doc)
	}

	// штучный товар: вес единицы из карточки
	res, err = m.Act("print_label", map[string]any{"plu": 2.0})
	if err != nil {
		t.Fatal(err)
	}
	if text := strings.Join(res.(models.PrintedDocument).Lines, "\n"); !strings.Contains(text, "2100017001200") || !strings.Contains(text, "4.80 RUB") {
		t.Fatalf("штучный товар:\n%s", text)
	}

	if err := m.SetState(map[string]any{"barcode_prefix": "23"}); err != nil {
		t.Fatal(err)
	}
	res, _ = m.Act("print_label", map[string]any{"plu": 1.0, "weight": 1.0})
	if text := strings.Join(res.(models.PrintedDocument).Lines, "\n"); !strings.Contains(text, "[EAN-13 23000010100") {
		t.Fatalf("префикс 23:\n%s", text)
	}

	_, err = m.Act("print_label", map[string]any{"plu": 7.0, "weight": 1.0})
	var de *devError
	if !errors.As(err, &de) || de.code != ErrNoGood {
		t.Fatalf("PLU 7 не загружен: %v", err)
	}
	for _, args := range []map[string]any{{"plu": 1.0}, {"plu": 1.0, "weight": 100.0}, {"weight": 1.0}, {"plu": "1"}} {
		if _, err := m.Act("print_label", args); err == nil {
			t.Errorf("%v принят", args)
		}
	}
	if _, err := m.Act("tare", map[string]any{"plu": 1.0}); !errors.Is(err, models.ErrUnknownAction) {
		t.Fatalf("неизвестное действие: %v", err)
	}

	if got := m.Prints(10); len(got) != 3 || got[0].Seq != 3 {
		t.Fatalf("лента: %+v", got)
	}
	if got := m.PrintsSince(1); len(got) != 2 || got[0].Seq != 2 {
		t.Fatalf("PrintsSince: %+v", got)
	}
	if m.State()["labels"] != 3 {
		t.Fatalf("счётчик этикеток: %+v", m.State())
	}
}

func TestPrefixValidation(t *testing.T) {
	m := newModel()
	for _, bad := range []map[string]any{{"barcode_prefix": "2"}, {"barcode_prefix": "30"}, {"barcode_prefix": 21.0}, {"weight": 1.0}} {
		if err := m.SetState(bad); err == nil {
			t.Errorf("%v принят", bad)
		}
	}
	if m.State()["barcode_prefix"] != "21" {
		t.Fatal("префикс изменился")
	}
}

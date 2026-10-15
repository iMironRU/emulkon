package input

import (
	"context"
	"encoding/base64"
	"strings"
	"testing"
	"time"

	"emulkon/internal/models"
)

var testNow = func() time.Time { return time.Date(2026, 10, 8, 19, 0, 0, 0, time.Local) }

type sink struct{ got []models.DeviceEvent }

func (s *sink) hook(e models.DeviceEvent) { s.got = append(s.got, e) }

func validEAN(s string) bool { return checkDigit(s[:len(s)-1]) == s[len(s)-1:] }

func TestScannerScan(t *testing.T) {
	m := NewScanner(Options{Version: "test", Now: testNow})
	var s sink
	m.SetEventHook(s.hook)
	if _, err := m.Act("scan", map[string]any{"barcode": "4600000000008"}); err != nil {
		t.Fatal(err)
	}
	if len(s.got) != 1 || s.got[0] != (models.DeviceEvent{Name: "Barcode", Data: "4600000000008"}) {
		t.Fatalf("событие: %+v", s.got)
	}
	for _, kind := range ScanKinds {
		res, err := m.Act("scan", map[string]any{"kind": kind})
		if err != nil {
			t.Fatalf("%s: %v", kind, err)
		}
		code := res.(scanRecord).Barcode
		switch kind {
		case "ean13", "weight":
			if len(code) != 13 || !validEAN(code) {
				t.Errorf("%s: %q", kind, code)
			}
			if kind == "weight" && !strings.HasPrefix(code, "21") {
				t.Errorf("весовой без префикса 21: %q", code)
			}
		case "ean8":
			if len(code) != 8 || !validEAN(code) {
				t.Errorf("ean8: %q", code)
			}
		case "datamatrix":
			gtin := code[2:16]
			if !strings.HasPrefix(code, "01") || !validEAN(gtin) || code[16:18] != "21" || !strings.Contains(code, GS+"93") || len(code) != 38 {
				t.Errorf("код маркировки: %q", code)
			}
		}
	}
	if validEAN("4600000000009") || !validEAN("4006381333931") {
		t.Fatal("контрольная цифра")
	}
	if err := m.SetState(map[string]any{"event_format": "base64"}); err != nil {
		t.Fatal(err)
	}
	m.Act("scan", map[string]any{"barcode": "01046" + GS + "93ab"})
	last := s.got[len(s.got)-1]
	if raw, _ := base64.StdEncoding.DecodeString(last.Data); last.Name != "BarcodeBase64" || string(raw) != "01046"+GS+"93ab" {
		t.Fatalf("Base64: %+v", last)
	}
	for _, bad := range []map[string]any{{}, {"kind": "aztec"}, {"barcode": "  "}, {"barcode": 5.0}} {
		if _, err := m.Act("scan", bad); err == nil {
			t.Errorf("%v принят", bad)
		}
	}
	if _, err := m.Act("swipe", nil); err != models.ErrUnknownAction {
		t.Fatal("чужое действие")
	}
	st := m.State()
	if st["scans"] != 8 || len(st["last"].([]scanRecord)) != 8 {
		t.Fatalf("состояние: %+v", st)
	}
	r := m.Handle(context.Background(), "GetWeight", []any{"emulkon-1", 0})
	if r.Return != false || r.ErrorCode != ErrNotSupported {
		t.Fatalf("метод у сканера: %+v", r)
	}
	r = m.Handle(context.Background(), "EquipmentTest", []any{"", "", "", ""})
	if s, _ := r.Params[2].(string); r.Return != true || !strings.Contains(s, "ЭМУЛЯТОР сканера") {
		t.Fatalf("тест: %+v", r)
	}
}

func TestCardReaderTracks(t *testing.T) {
	m := NewCardReader(Options{Version: "test", Now: testNow})
	var s sink
	m.SetEventHook(s.hook)
	// по умолчанию — как в БПО: дорожка 2 с суффиксом CR
	if _, err := m.Act("swipe", map[string]any{"card": "discount"}); err != nil {
		t.Fatal(err)
	}
	if s.got[0] != (models.DeviceEvent{Name: "TracksData", Data: "7770000000017=2912\r"}) {
		t.Fatalf("дорожка 2: %q", s.got[0])
	}
	err := m.SetState(map[string]any{"tracks": []any{
		map[string]any{"use": true, "prefix": 37.0, "suffix": 63.0},
		map[string]any{"use": true, "prefix": 59.0, "suffix": 63.0},
		map[string]any{"use": false},
	}})
	if err != nil {
		t.Fatal(err)
	}
	m.Act("swipe", map[string]any{"track1": "B1^TEST^", "track2": "123=45", "track3": "999"})
	if got := s.got[1].Data; got != "%B1^TEST^?;123=45?" {
		t.Fatalf("дорожки 1 и 2: %q", got)
	}
	if _, err := m.Act("swipe", map[string]any{"track3": "999"}); err == nil {
		t.Fatal("только выключенная дорожка — ошибка")
	}
	for _, bad := range []map[string]any{{"tracks": []any{}}, {"tracks": []any{map[string]any{"prefix": 300.0}, map[string]any{}, map[string]any{}}}, {"x": 1.0}} {
		if err := m.SetState(bad); err == nil {
			t.Errorf("%v принят", bad)
		}
	}
	m.Reset()
	if st := m.State(); st["swipes"] != 0 || st["tracks"].([3]Track)[1].Suffix != 13 {
		t.Fatalf("сброс: %+v", st)
	}
}

// Клавиатурный режим (docs/models/input.md): код уходит трею для ввода, событие — по режиму.
func TestKeyboardMode(t *testing.T) {
	m := NewScanner(Options{Version: "test", Now: testNow})
	s := &sink{}
	m.SetEventHook(s.hook)
	res, _ := m.Act("scan", map[string]any{"barcode": "4601234567893"})
	if kb := res.(models.Keyboarder).KeyboardInput(); kb != nil || len(s.got) != 1 {
		t.Fatalf("по умолчанию — только драйвер: %+v %d", kb, len(s.got))
	}
	if err := m.SetState(map[string]any{"input_mode": "keyboard", "keyboard_suffix": "tab", "keyboard_delay_ms": 0.0}); err != nil {
		t.Fatal(err)
	}
	res, _ = m.Act("scan", map[string]any{"barcode": "4601234567893"})
	kb := res.(models.Keyboarder).KeyboardInput()
	if kb == nil || kb.Text != "4601234567893" || kb.Suffix != "tab" || kb.DelayMS != 0 || len(s.got) != 1 {
		t.Fatalf("клавиатура без события: %+v, событий %d", kb, len(s.got))
	}
	m.SetState(map[string]any{"input_mode": "both"})
	m.Act("scan", map[string]any{"barcode": "1"})
	if len(s.got) != 2 {
		t.Fatal("both — и событие")
	}
	for _, bad := range []map[string]any{{"input_mode": "hid"}, {"keyboard_suffix": "space"}, {"keyboard_delay_ms": 20000.0}} {
		if err := m.SetState(bad); err == nil {
			t.Errorf("%v — ошибка", bad)
		}
	}
	m.Reset()
	if m.State()["input_mode"] != ModeDriver {
		t.Fatal("сброс — режим драйвера")
	}

	r := NewCardReader(Options{Version: "test", Now: testNow})
	r.SetState(map[string]any{"input_mode": "keyboard"})
	res, err := r.Act("swipe", map[string]any{"card": "discount"})
	if err != nil {
		t.Fatal(err)
	}
	if kb := res.(models.Keyboarder).KeyboardInput(); kb == nil || kb.Text != "7770000000017=2912\r" || kb.Suffix != "none" {
		t.Fatalf("карта клавиатурой: суффикс дорожки (CR) уже в данных: %+v", kb)
	}
}

// Библиотеки тестовых данных (docs/models/input.md).
func TestLibraries(t *testing.T) {
	m := NewScanner(Options{Version: "test", Now: testNow})
	lib := []any{map[string]any{"title": "Молоко", "barcode": "4601234567893"}, map[string]any{"title": "КМ сигареты", "barcode": "0104600000000000215ABCDE\x1d93abcd"}}
	if err := m.SetState(map[string]any{"library": lib}); err != nil {
		t.Fatal(err)
	}
	if got := m.State()["library"].([]Barcode); len(got) != 2 || got[1].Title != "КМ сигареты" {
		t.Fatalf("библиотека сканера: %+v", got)
	}
	for _, bad := range []any{
		[]any{map[string]any{"title": "", "barcode": "1"}},
		[]any{map[string]any{"title": "a", "barcode": ""}},
		[]any{map[string]any{"title": "a", "barcode": "1"}, map[string]any{"title": "a", "barcode": "2"}},
		"не массив",
	} {
		if err := m.SetState(map[string]any{"library": bad}); err == nil {
			t.Errorf("%v — ошибка", bad)
		}
	}
	if len(m.State()["library"].([]Barcode)) != 2 {
		t.Fatal("ошибка не меняет библиотеку")
	}

	r := NewCardReader(Options{Version: "test", Now: testNow})
	s := &sink{}
	r.SetEventHook(s.hook)
	if err := r.SetState(map[string]any{"library": []any{map[string]any{"title": "Бонусная", "track2": "7770000000024=2912"}}}); err != nil {
		t.Fatal(err)
	}
	if _, err := r.Act("swipe", map[string]any{"card": "Бонусная"}); err != nil || len(s.got) != 1 || s.got[0].Data != "7770000000024=2912\r" {
		t.Fatalf("карта из библиотеки по подписи: %v %+v", err, s.got)
	}
	if _, err := r.Act("swipe", map[string]any{"card": "Нет такой"}); err == nil {
		t.Fatal("неизвестная карта — ошибка")
	}
	if err := r.SetState(map[string]any{"library": []any{map[string]any{"title": "Пустая"}}}); err == nil {
		t.Fatal("карта без дорожек — ошибка")
	}
}

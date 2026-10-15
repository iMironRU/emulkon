package recognizer

import (
	"bytes"
	"context"
	"encoding/base64"
	"encoding/json"
	"image/png"
	"strings"
	"testing"

	"emulkon/internal/models"
)

func call(m *Model, method string, params ...any) models.Result {
	return m.Handle(context.Background(), method, params)
}

func ok(r models.Result) bool { b, _ := r.Return.(bool); return b }

func TestRecognize(t *testing.T) {
	m := New(ModelOptions{Version: "test"})
	err := m.SetState(map[string]any{"candidates": []any{
		map[string]any{"code": 1023.0, "percent": 0.15},
		map[string]any{"code": 1003.0, "percent": 0.95},
		map[string]any{"code": 2001.0, "percent": 0.4},
	}})
	if err != nil {
		t.Fatal(err)
	}
	r := call(m, "Recognize", "emulkon-1", true, json.Number("2"), "")
	s, _ := r.Params[3].(string)
	if !ok(r) || !strings.Contains(s, `<Table><Record Code="1003" RecognizePercent="0.95"></Record><Record Code="2001" RecognizePercent="0.4">`) || strings.Contains(s, "1023") {
		t.Fatalf("распознавание: %s", s)
	}
	if m.State()["awaiting_confirm"] != true {
		t.Fatal("с обучением ждёт подтверждения")
	}
	if r := call(m, "ConfirmRecognize", "emulkon-1", json.Number("2001")); !ok(r) || m.State()["awaiting_confirm"] != false {
		t.Fatalf("подтверждение: %+v", r)
	}
	if r := call(m, "Recognize", "emulkon-1", false, json.Number("0"), ""); ok(r) {
		t.Fatal("Variants=0 принят")
	}
	r = call(m, "RecognizerOptions", "emulkon-1", "")
	if s, _ := r.Params[1].(string); !strings.Contains(s, `<RecognizerParamters GetImage="true" HardwareRecognizer="true">`) {
		t.Fatalf("параметры: %s", s)
	}
	r = call(m, "GetImage", "emulkon-1", "")
	raw, _ := base64.StdEncoding.DecodeString(r.Params[1].(string))
	if img, err := png.Decode(bytes.NewReader(raw)); err != nil || img.Bounds().Dx() != 160 {
		t.Fatalf("изображение: %v", err)
	}
	m.SetState(map[string]any{"get_image": false})
	if r := call(m, "GetImage", "emulkon-1", ""); ok(r) || r.ErrorCode != ErrNoImage {
		t.Fatalf("без изображения: %+v", r)
	}
	for _, bad := range []map[string]any{{"candidates": []any{map[string]any{"code": 1.0, "percent": 2.0}}}, {"candidates": "x"}, {"x": 1.0}} {
		if err := m.SetState(bad); err == nil {
			t.Errorf("%v принят", bad)
		}
	}
	// пусто на весах — пустая таблица, не ошибка
	m.SetState(map[string]any{"candidates": []any{}})
	r = call(m, "Recognize", "emulkon-1", false, json.Number("3"), "")
	if s, _ := r.Params[3].(string); !ok(r) || !strings.HasSuffix(s, "<Table></Table>") {
		t.Fatalf("пусто: %q", s)
	}
}

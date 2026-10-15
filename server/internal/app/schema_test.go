package app

import (
	"encoding/json"
	"strings"
	"testing"

	"emulkon/internal/models"
)

// valueAt — значение состояния по пути «faults.paper».
func valueAt(state map[string]any, path string) (any, bool) {
	cur := any(state)
	for _, part := range strings.Split(path, ".") {
		m, ok := cur.(map[string]any)
		if !ok {
			return nil, false
		}
		if cur, ok = m[part]; !ok {
			return nil, false
		}
	}
	return cur, true
}

// TestSchemaMatchesModels — схема форм (models.Schema) и модели не разошлись: у каждого поля
// есть текущее значение в State() по Path, и модель принимает его обратно через SetState.
// Так форма карточки показывает и отправляет то, что модель понимает (docs/protocol.md §2.3).
func TestSchemaMatchesModels(t *testing.T) {
	reg := Registry("test")
	for _, typ := range reg.Types() {
		schema := models.Schema(typ.Key)
		model, err := reg.New(typ, typ.Key+"-01")
		if err != nil {
			t.Fatal(err)
		}
		setter, isSetter := model.(models.StateSetter)
		if len(schema) > 0 && !isSetter {
			t.Errorf("%s: схема есть, а SetState нет", typ.Key)
			continue
		}
		for _, f := range schema {
			if f.Path == "-" {
				continue // значения в состоянии нет (действие)
			}
			path := f.Path
			if path == "" {
				path = f.Key
			}
			v, ok := valueAt(model.State(), path)
			if !ok {
				t.Errorf("%s.%s: нет значения в состоянии по пути %q", typ.Key, f.Key, path)
				continue
			}
			if err := setter.SetState(map[string]any{f.Key: jsonish(v)}); err != nil {
				t.Errorf("%s.%s = %v: модель не принимает своё же значение: %v", typ.Key, f.Key, v, err)
			}
			if f.Kind == "enum" && len(f.Options) == 0 {
				t.Errorf("%s.%s: перечисление без вариантов", typ.Key, f.Key)
			}
		}
	}
}

// jsonish — значение после JSON, как его видят веб-интерфейс и автотесты: состояние уходит
// в GET …/devices и возвращается в PUT …/state (числа — float64, структуры — map).
func jsonish(v any) any {
	b, err := json.Marshal(v)
	if err != nil {
		return v
	}
	var out any
	if err := json.Unmarshal(b, &out); err != nil {
		return v
	}
	return out
}

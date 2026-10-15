package input

import (
	"encoding/json"
	"fmt"
	"strings"
)

// Библиотеки тестовых данных (docs/models/input.md): свои штрихкоды, коды маркировки и карты
// с подписями — для быстрого выбора в карточке. Настройка устройства: хранится в реестре.

const maxLibrary = 50

// Barcode — штрихкод или код маркировки библиотеки сканера.
type Barcode struct {
	Title   string `json:"title"`
	Barcode string `json:"barcode"`
}

// Card — карта библиотеки считывателя: данные дорожек.
type Card struct {
	Title  string `json:"title"`
	Track1 string `json:"track1,omitempty"`
	Track2 string `json:"track2,omitempty"`
	Track3 string `json:"track3,omitempty"`
}

// parseLibrary разбирает массив записей из JSON-значения состояния и проверяет подписи.
func parseLibrary[T any](v any, title func(T) string, check func(T) error) ([]T, error) {
	b, err := json.Marshal(v)
	if err != nil {
		return nil, fmt.Errorf("library: %v", err)
	}
	var list []T
	if err := json.Unmarshal(b, &list); err != nil {
		return nil, fmt.Errorf("library: ожидается массив записей: %v", err)
	}
	if len(list) > maxLibrary {
		return nil, fmt.Errorf("library: не больше %d записей", maxLibrary)
	}
	seen := map[string]bool{}
	for i, x := range list {
		t := strings.TrimSpace(title(x))
		if t == "" || len([]rune(t)) > 60 {
			return nil, fmt.Errorf("library[%d]: подпись от 1 до 60 символов", i)
		}
		if seen[t] {
			return nil, fmt.Errorf("library: подпись %q повторяется", t)
		}
		seen[t] = true
		if err := check(x); err != nil {
			return nil, fmt.Errorf("library[%d]: %v", i, err)
		}
	}
	if list == nil {
		list = []T{}
	}
	return list, nil
}

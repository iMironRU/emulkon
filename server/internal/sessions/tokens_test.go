package sessions

import (
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestWorkstationTokens(t *testing.T) {
	now := time.Date(2026, 10, 9, 12, 0, 0, 0, time.UTC)
	path := filepath.Join(t.TempDir(), "registry.json")
	m := registryManager(t, path, &now) // открытый режим

	// Регистрация до первого подключения: место заводится, токен виден один раз.
	tok, err := m.IssueWorkstationToken("KASSA-3")
	if err != nil || !strings.HasPrefix(tok.Token, tokenPrefix) || !strings.HasSuffix(tok.Token, tok.Hint) {
		t.Fatalf("выдача: %+v %v", tok, err)
	}
	b, _ := os.ReadFile(path)
	if strings.Contains(string(b), tok.Token) || !strings.Contains(string(b), tokenHash(tok.Token)) {
		t.Fatal("на диске — только SHA-256 токена")
	}
	ws := m.Workstations()
	if len(ws) != 1 || ws[0].Token == nil || ws[0].Token.Hint != tok.Hint {
		t.Fatalf("сведения о токене: %+v", ws)
	}

	// Токен сам называет место: имя компьютера (workstation_auto) или пустое — не мешают.
	for _, req := range []OpenRequest{
		{WorkstationID: "DESKTOP-X", WorkstationAuto: true},
		{WorkstationID: ""},
		{WorkstationID: "KASSA-3"},
	} {
		req.EquipmentType, req.DeviceToken = "KKT", tok.Token
		s, d, _, err := m.Open(req)
		if err != nil || s.WorkstationID != "KASSA-3" || d.WorkstationID != "KASSA-3" || s.Auth != AuthWorkstation {
			t.Fatalf("%+v: s=%+v err=%v", req, s, err)
		}
	}
	// Явно указано другое место — ошибка настройки.
	if _, _, _, err := m.Open(OpenRequest{WorkstationID: "KASSA-4", EquipmentType: "KKT", DeviceToken: tok.Token}); !errors.Is(err, ErrBadToken) {
		t.Fatalf("чужое место с токеном: %v", err)
	}
	// Защищённое место без токена или с неверным — отказ, хотя сервер в открытом режиме.
	for _, token := range []string{"", "emk_ws_wrong"} {
		if _, _, _, err := m.Open(OpenRequest{WorkstationID: "KASSA-3", EquipmentType: "KKT", DeviceToken: token}); !errors.Is(err, ErrBadToken) {
			t.Fatalf("место с токеном, токен %q: %v", token, err)
		}
	}
	// Остальные места — по-прежнему открытый режим.
	if s, _, _, err := m.Open(OpenRequest{WorkstationID: "PC", EquipmentType: "KKT"}); err != nil || s.Auth != AuthOpen {
		t.Fatalf("открытый режим: %v", err)
	}

	// Перевыпуск: старый токен не действует. Реестр переживает перезапуск.
	tok2, _ := m.IssueWorkstationToken("KASSA-3")
	m2 := registryManager(t, path, &now)
	if _, _, _, err := m2.Open(OpenRequest{EquipmentType: "KKT", DeviceToken: tok.Token}); !errors.Is(err, ErrBadRequest) {
		t.Fatalf("старый токен после перевыпуска: %v", err)
	}
	if s, _, _, err := m2.Open(OpenRequest{EquipmentType: "KKT", DeviceToken: tok2.Token}); err != nil || s.WorkstationID != "KASSA-3" {
		t.Fatalf("новый токен после перезапуска: %v", err)
	}
	if m2.WorkstationTokens() != 1 {
		t.Fatal("WorkstationTokens")
	}

	// Отзыв: место снова открыто.
	if err := m2.RevokeWorkstationToken("KASSA-3"); err != nil {
		t.Fatal(err)
	}
	if _, _, _, err := m2.Open(OpenRequest{WorkstationID: "KASSA-3", EquipmentType: "KKT"}); err != nil {
		t.Fatalf("после отзыва: %v", err)
	}
	if err := m2.RevokeWorkstationToken("NOPE"); !errors.Is(err, ErrNotFound) {
		t.Fatalf("отзыв у неизвестного места: %v", err)
	}
}

// С общим токеном сервера: токен рабочего места тоже пускает — так закрывают сервер целиком,
// раздав только токены мест.
func TestWorkstationTokenWithServerToken(t *testing.T) {
	now := time.Now()
	m := newManager("secret", &now)
	tok, _ := m.IssueWorkstationToken("KASSA-1")
	if s, _, _, err := m.Open(OpenRequest{EquipmentType: "KKT", DeviceToken: tok.Token}); err != nil || s.Auth != AuthWorkstation {
		t.Fatalf("токен места: %v", err)
	}
	if s, _, _, err := m.Open(OpenRequest{WorkstationID: "PC", EquipmentType: "KKT", DeviceToken: "secret"}); err != nil || s.Auth != AuthServer {
		t.Fatalf("общий токен: %v", err)
	}
	if _, _, _, err := m.Open(OpenRequest{WorkstationID: "KASSA-1", EquipmentType: "KKT", DeviceToken: "secret"}); !errors.Is(err, ErrBadToken) {
		t.Fatalf("общий токен не открывает место со своим токеном: %v", err)
	}
}

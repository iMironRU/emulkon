package sessions

// Токены рабочих мест (ARCHITECTURE §10, docs/protocol.md §1.1): токен выдаётся в UI или Control
// API, показывается один раз, на диске хранится только SHA-256. Токен сам называет рабочее место:
// драйверу достаточно его и адреса сервера.

import (
	"crypto/rand"
	"crypto/subtle"
	"encoding/hex"
	"fmt"
	"time"

	"emulkon/internal/secret"
)

// tokenPrefix отличает токен рабочего места от общего EMULKON_DEVICE_TOKEN.
const tokenPrefix = "emk_ws_"

// Способы допуска сессии — в журнал (session.open, поле auth).
const (
	AuthWorkstation = "workstation" // токен рабочего места
	AuthServer      = "server"      // общий EMULKON_DEVICE_TOKEN
	AuthOpen        = "open"        // открытый режим
)

// TokenInfo — сведения о выданном токене (сам токен не хранится и не возвращается повторно).
type TokenInfo struct {
	IssuedAt time.Time `json:"issued_at"`
	Hint     string    `json:"hint"` // последние 4 символа — узнать токен в настройках драйвера
}

// IssuedToken — ответ на выдачу: токен виден один раз.
type IssuedToken struct {
	WorkstationID string    `json:"workstation_id"`
	Token         string    `json:"token"`
	IssuedAt      time.Time `json:"issued_at"`
	Hint          string    `json:"hint"`
}

func tokenHash(token string) string { return secret.Hash(token) }

// IssueWorkstationToken выдаёт рабочему месту новый токен; прежний перестаёт действовать.
// Неизвестное рабочее место заводится (регистрация до первого подключения).
func (m *Manager) IssueWorkstationToken(id string) (IssuedToken, error) {
	if err := validateID(id); err != nil {
		return IssuedToken{}, fmt.Errorf("%w: workstation_id: %v", ErrBadRequest, err)
	}
	b := make([]byte, 24)
	if _, err := rand.Read(b); err != nil {
		return IssuedToken{}, fmt.Errorf("выдать токен: %w", err)
	}
	token := tokenPrefix + hex.EncodeToString(b)
	now := m.cfg.Now().UTC()
	m.mu.Lock()
	w, ok := m.workstations[id]
	if !ok {
		w = &Workstation{ID: id}
		m.workstations[id] = w
	}
	w.TokenHash, w.Token = tokenHash(token), &TokenInfo{IssuedAt: now, Hint: token[len(token)-4:]}
	m.mu.Unlock()
	m.persist()
	return IssuedToken{WorkstationID: id, Token: token, IssuedAt: now, Hint: token[len(token)-4:]}, nil
}

// RevokeWorkstationToken отзывает токен рабочего места: место снова допускается общим токеном
// сервера или в открытом режиме. Открытые сессии не закрываются.
func (m *Manager) RevokeWorkstationToken(id string) error {
	m.mu.Lock()
	w, ok := m.workstations[id]
	if ok {
		w.TokenHash, w.Token = "", nil
	}
	m.mu.Unlock()
	if !ok {
		return fmt.Errorf("%w: рабочее место %s", ErrNotFound, id)
	}
	m.persist()
	return nil
}

// authorize — допуск при открытии сессии и итоговое рабочее место (вызывается под m.mu):
//  1. токен рабочего места — место из токена; явно указанное другое место — отказ;
//  2. месту выдан токен — без него не пускаем;
//  3. общий токен сервера, если задан;
//  4. иначе открытый режим.
func (m *Manager) authorize(req OpenRequest) (workstation, auth string, err error) {
	if req.DeviceToken != "" {
		h := tokenHash(req.DeviceToken)
		for id, w := range m.workstations {
			if w.TokenHash != "" && subtle.ConstantTimeCompare([]byte(h), []byte(w.TokenHash)) == 1 {
				if req.WorkstationID != "" && req.WorkstationID != id && !req.WorkstationAuto {
					return "", "", fmt.Errorf("%w: токен выдан рабочему месту %s, а в параметрах драйвера — %s",
						ErrBadToken, id, req.WorkstationID)
				}
				return id, AuthWorkstation, nil
			}
		}
	}
	if err := validateID(req.WorkstationID); err != nil {
		return "", "", fmt.Errorf("%w: workstation_id: %v", ErrBadRequest, err)
	}
	if w := m.workstations[req.WorkstationID]; w != nil && w.TokenHash != "" {
		return "", "", fmt.Errorf("%w: рабочему месту %s выдан токен — укажите его в параметре «Токен устройства»",
			ErrBadToken, req.WorkstationID)
	}
	if !m.OpenMode() {
		if !m.cfg.DeviceToken.Match(req.DeviceToken) {
			return "", "", ErrBadToken
		}
		return req.WorkstationID, AuthServer, nil
	}
	return req.WorkstationID, AuthOpen, nil
}

// WorkstationTokens — сколько рабочих мест защищено токеном (для /control/v1/info).
func (m *Manager) WorkstationTokens() int {
	m.mu.Lock()
	defer m.mu.Unlock()
	n := 0
	for _, w := range m.workstations {
		if w.TokenHash != "" {
			n++
		}
	}
	return n
}

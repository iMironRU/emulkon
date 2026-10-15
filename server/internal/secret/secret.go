// Package secret — токены доступа к серверу (Control API, общий токен устройств), заданные открыто
// (переменные окружения, Docker) или хешем SHA-256 (реестр Windows, docs/local-install.md §2.1):
// сам токен показывается один раз при выдаче и нигде не хранится.
package secret

import (
	"crypto/sha256"
	"crypto/subtle"
	"encoding/hex"
	"fmt"
	"strings"
)

// Token — ожидаемый токен. Нулевое значение — токен не задан (открытый режим).
type Token struct {
	hash string // SHA-256 в hex, нижний регистр
}

// Plain — токен, заданный открыто.
func Plain(token string) Token {
	if token == "" {
		return Token{}
	}
	return Token{hash: Hash(token)}
}

// FromHash — токен, заданный хешем SHA-256 (64 шестнадцатеричных символа).
func FromHash(hash string) (Token, error) {
	h := strings.ToLower(strings.TrimSpace(hash))
	if h == "" {
		return Token{}, nil
	}
	if len(h) != sha256.Size*2 {
		return Token{}, fmt.Errorf("хеш токена: ожидается SHA-256 (64 шестнадцатеричных символа), длина %d", len(h))
	}
	if _, err := hex.DecodeString(h); err != nil {
		return Token{}, fmt.Errorf("хеш токена: %w", err)
	}
	return Token{hash: h}, nil
}

// Hash — SHA-256 токена в hex: так токены хранятся на диске и в реестре.
func Hash(token string) string {
	sum := sha256.Sum256([]byte(token))
	return hex.EncodeToString(sum[:])
}

// Empty — токен не задан.
func (t Token) Empty() bool { return t.hash == "" }

// Match — предъявленный токен совпадает с ожидаемым (сравнение за постоянное время).
// Незаданный токен ничему не соответствует: открытый режим проверяется через Empty.
func (t Token) Match(presented string) bool {
	if t.hash == "" || presented == "" {
		return false
	}
	return subtle.ConstantTimeCompare([]byte(Hash(presented)), []byte(t.hash)) == 1
}

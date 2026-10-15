package secret

import (
	"strings"
	"testing"
)

func TestToken(t *testing.T) {
	if !(Token{}).Empty() || !Plain("").Empty() {
		t.Fatal("пустой токен должен быть Empty")
	}
	if (Token{}).Match("") || (Token{}).Match("x") {
		t.Fatal("незаданный токен ничему не соответствует")
	}
	p := Plain("s3cret")
	if p.Empty() || !p.Match("s3cret") || p.Match("S3cret") || p.Match("") {
		t.Fatal("открытый токен")
	}
	h, err := FromHash(strings.ToUpper(Hash("s3cret")))
	if err != nil {
		t.Fatal(err)
	}
	if !h.Match("s3cret") || h.Match("other") {
		t.Fatal("токен по хешу")
	}
	if e, err := FromHash("  "); err != nil || !e.Empty() {
		t.Fatalf("пустой хеш: %v %v", e, err)
	}
	for _, bad := range []string{"abc", strings.Repeat("z", 64)} {
		if _, err := FromHash(bad); err == nil {
			t.Fatalf("хеш %q должен быть ошибкой", bad)
		}
	}
}

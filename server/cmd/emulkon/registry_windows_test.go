//go:build windows

package main

import "testing"

// Чтение настоящего HKLM: свой ключ без прав администратора не создать — берём ключ Windows.
func TestReadKeyHKLM(t *testing.T) {
	v := readKey(`SOFTWARE\Microsoft\Windows NT\CurrentVersion`, []string{"ProductName", "CurrentMajorVersionNumber", "НетТакого"})
	if v["ProductName"] == "" || v["CurrentMajorVersionNumber"] == "" {
		t.Fatalf("REG_SZ и REG_DWORD: %v", v)
	}
	if _, ok := v["НетТакого"]; ok {
		t.Fatal("отсутствующее значение")
	}
	if readKey(`SOFTWARE\Emulkon-нет-такого-ключа`, []string{"Addr"}) != nil {
		t.Fatal("отсутствующий ключ")
	}
	if got := expandEnv("%SystemRoot%"); got == "%SystemRoot%" || got == "" {
		t.Fatalf("REG_EXPAND_SZ: %q", got)
	}
}

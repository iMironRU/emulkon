//go:build windows

package main

import (
	"encoding/binary"
	"strconv"
	"syscall"
	"unsafe"
)

// Ключи настроек (docs/local-install.md §2.1). Политика перекрывает обычные значения.
const (
	regKeyServer = `SOFTWARE\Emulkon\Server`
	regKeyPolicy = `SOFTWARE\Policies\Emulkon`
)

// readRegistry читает известные значения из HKLM; отсутствующие ключи и значения — пусто.
func readRegistry() registryValues {
	out := registryValues{}
	names := []string{regAddr, regNetwork, regDataDir, regDownloadsDir, regLogLevel, regControlHash, regDeviceHash,
		regUpdateMode, regUpdateURL}
	for _, key := range []string{regKeyServer, regKeyPolicy} {
		for name, v := range readKey(key, names) {
			out[name] = v
		}
	}
	return out
}

func readKey(path string, names []string) map[string]string {
	var h syscall.Handle
	p, _ := syscall.UTF16PtrFromString(path)
	if syscall.RegOpenKeyEx(syscall.HKEY_LOCAL_MACHINE, p, 0, syscall.KEY_READ, &h) != nil {
		return nil
	}
	defer syscall.RegCloseKey(h)
	out := map[string]string{}
	for _, name := range names {
		if v, ok := queryValue(h, name); ok {
			out[name] = v
		}
	}
	return out
}

// queryValue — REG_SZ/REG_EXPAND_SZ как строка, REG_DWORD как десятичное число.
func queryValue(h syscall.Handle, name string) (string, bool) {
	n, _ := syscall.UTF16PtrFromString(name)
	var typ, size uint32
	if syscall.RegQueryValueEx(h, n, nil, &typ, nil, &size) != nil || size == 0 {
		return "", false
	}
	buf := make([]byte, size)
	if syscall.RegQueryValueEx(h, n, nil, &typ, &buf[0], &size) != nil {
		return "", false
	}
	switch typ {
	case syscall.REG_SZ, syscall.REG_EXPAND_SZ:
		u := unsafe.Slice((*uint16)(unsafe.Pointer(&buf[0])), size/2)
		s := syscall.UTF16ToString(u)
		if typ == syscall.REG_EXPAND_SZ {
			s = expandEnv(s)
		}
		return s, true
	case syscall.REG_DWORD:
		if size < 4 {
			return "", false
		}
		return strconv.FormatUint(uint64(binary.LittleEndian.Uint32(buf)), 10), true
	}
	return "", false
}

func expandEnv(s string) string {
	src, err := syscall.UTF16PtrFromString(s)
	if err != nil {
		return s
	}
	n, _, _ := procExpandEnvironmentStringsW.Call(uintptr(unsafe.Pointer(src)), 0, 0)
	if n == 0 {
		return s
	}
	buf := make([]uint16, n)
	if r, _, _ := procExpandEnvironmentStringsW.Call(uintptr(unsafe.Pointer(src)), uintptr(unsafe.Pointer(&buf[0])), n); r == 0 {
		return s
	}
	return syscall.UTF16ToString(buf)
}

var procExpandEnvironmentStringsW = syscall.NewLazyDLL("kernel32.dll").NewProc("ExpandEnvironmentStringsW")

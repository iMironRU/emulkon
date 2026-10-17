package main

import (
	"bytes"
	"compress/flate"
	"encoding/binary"
	"os"
	"path/filepath"
	"testing"
	"unicode/utf16"
)

// tinyQcow2 — образ qcow2 v3 с кластером 512 байт и диском в 4 кластера: 0 — обычный, 1 — сжатый,
// 2 — не распределён, 3 — помечен нулевым.
func tinyQcow2(t *testing.T, plain, packed []byte) string {
	t.Helper()
	const cs = 512
	be := binary.BigEndian
	f := make([]byte, 6*cs)
	copy(f, []byte{'Q', 'F', 'I', 0xfb})
	be.PutUint32(f[4:], 3)
	be.PutUint32(f[20:], 9)      // cluster_bits
	be.PutUint64(f[24:], 4*cs)   // размер диска
	be.PutUint32(f[36:], 1)      // L1 из одной записи
	be.PutUint64(f[40:], 1*cs)   // L1 — кластер 1
	be.PutUint32(f[100:], 104)   // длина заголовка
	be.PutUint64(f[cs:], 2*cs)   // L1[0] → L2 в кластере 2
	be.PutUint64(f[2*cs:], 3*cs) // L2[0] → данные в кластере 3
	copy(f[3*cs:], plain)
	var buf bytes.Buffer
	w, _ := flate.NewWriter(&buf, flate.BestCompression)
	w.Write(packed)
	w.Close()
	comp := buf.Bytes()
	off := uint64(4 * cs)
	copy(f[off:], comp)
	x := uint(62 - (9 - 8))
	sectors := uint64((len(comp)+511)/512) - 1
	be.PutUint64(f[2*cs+8:], 1<<62|sectors<<x|off) // L2[1] — сжатый
	be.PutUint64(f[2*cs+24:], 1)                   // L2[3] — нулевой
	p := filepath.Join(t.TempDir(), "tiny.qcow2")
	if err := os.WriteFile(p, f, 0o644); err != nil {
		t.Fatal(err)
	}
	return p
}

func TestConvertVHD(t *testing.T) {
	plain := bytes.Repeat([]byte("ЭМУЛЯТОР"), 512/len("ЭМУЛЯТОР")+1)[:512]
	packed := bytes.Repeat([]byte{0xAB}, 512)
	in := tinyQcow2(t, plain, packed)
	out := filepath.Join(t.TempDir(), "disk.vhd")
	if err := convertVHD(in, out); err != nil {
		t.Fatal(err)
	}
	got, _ := os.ReadFile(out)
	if len(got) != 4*512+512 {
		t.Fatalf("размер VHD %d", len(got))
	}
	want := append(append(append([]byte{}, plain...), packed...), make([]byte, 1024)...)
	if !bytes.Equal(got[:2048], want) {
		t.Fatal("данные диска не совпали с qcow2")
	}
	ft := got[2048:]
	if string(ft[:8]) != "conectix" || binary.BigEndian.Uint32(ft[60:]) != 2 || binary.BigEndian.Uint64(ft[48:]) != 2048 {
		t.Fatal("подвал VHD: не фиксированный диск нужного размера")
	}
	var sum uint32
	for i, b := range ft {
		if i < 64 || i >= 68 {
			sum += uint32(b)
		}
	}
	if ^sum != binary.BigEndian.Uint32(ft[64:]) {
		t.Fatal("контрольная сумма подвала VHD")
	}
}

func TestQcow2Rejects(t *testing.T) {
	p := filepath.Join(t.TempDir(), "x.img")
	os.WriteFile(p, make([]byte, 4096), 0o644) // сырой диск, не qcow2
	if err := convertVHD(p, p+".vhd"); err == nil {
		t.Fatal("не qcow2 — должна быть ошибка")
	}
}

func TestCHS(t *testing.T) {
	// 3,5 ГБ облачного образа и предел формата.
	if c, h, s := chs(3758096384); c == 0 || h != 16 || s != 255 && s != 63 {
		t.Fatalf("chs 3.5 ГБ: %d/%d/%d", c, h, s)
	}
	if c, h, s := chs(1 << 50); c != 65535 || h != 16 || s != 255 {
		t.Fatalf("chs предел: %d/%d/%d", c, h, s)
	}
}

func TestSeedISO(t *testing.T) {
	out := filepath.Join(t.TempDir(), "seed.iso")
	user := []byte("#cloud-config\nhostname: emulkon-linux\n")
	meta := bytes.Repeat([]byte("x"), 3000) // больше сектора
	if err := writeSeed(out, "CIDATA", map[string][]byte{"user-data": user, "meta-data": meta}); err != nil {
		t.Fatal(err)
	}
	iso, _ := os.ReadFile(out)
	if len(iso)%sector != 0 {
		t.Fatal("ISO не кратен сектору")
	}
	pvd, svd := iso[16*sector:], iso[17*sector:]
	if pvd[0] != 1 || string(pvd[1:6]) != "CD001" || string(bytes.TrimRight(pvd[40:72], " ")) != "CIDATA" {
		t.Fatal("основной дескриптор: тип, сигнатура или метка")
	}
	if svd[0] != 2 || string(svd[88:91]) != "%/E" {
		t.Fatal("дескриптор Joliet")
	}
	if iso[18*sector] != 255 {
		t.Fatal("нет завершающего дескриптора")
	}
	if int(binary.LittleEndian.Uint32(pvd[80:]))*sector != len(iso) {
		t.Fatal("размер тома в дескрипторе")
	}
	// Корень Joliet: «.», «..», meta-data, user-data — с данными по своим секторам.
	root := iso[binary.LittleEndian.Uint32(svd[156+2:])*sector:]
	names := map[string][]byte{}
	for p := 0; p < sector && root[p] != 0; p += int(root[p]) {
		r := root[p:]
		idLen := int(r[32])
		if idLen == 1 && (r[33] == 0 || r[33] == 1) {
			continue
		}
		u := make([]uint16, idLen/2)
		for i := range u {
			u[i] = binary.BigEndian.Uint16(r[33+2*i:])
		}
		lba, size := binary.LittleEndian.Uint32(r[2:]), binary.LittleEndian.Uint32(r[10:])
		names[string(utf16.Decode(u))] = iso[int(lba)*sector : int(lba)*sector+int(size)]
	}
	if !bytes.Equal(names["user-data"], user) || !bytes.Equal(names["meta-data"], meta) || len(names) != 2 {
		t.Fatalf("файлы Joliet: %v", len(names))
	}
	if isoName("user-data") != "USER_DAT.;1" {
		t.Fatal(isoName("user-data"))
	}
}

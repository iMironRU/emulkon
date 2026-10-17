package main

// Команда seed: ISO с меткой CIDATA для cloud-init (источник NoCloud) — user-data и meta-data. ISO, а не диск FAT:
// его не нужно монтировать в Windows (права администратора), Hyper-V подключает его как DVD.
//
// ISO 9660 с расширением Joliet: Linux берёт имена из Joliet, поэтому файлы видны как «user-data» и «meta-data»
// (в основном каталоге ISO 9660 дефис недопустим). Только корневой каталог, файлы целиком.

import (
	"encoding/binary"
	"fmt"
	"os"
	"sort"
	"time"
	"unicode/utf16"
)

const sector = 2048

type seedFile struct {
	name string // имя в Joliet: user-data
	iso  string // имя ISO 9660 (уровень 1): USER_DAT.;1
	data []byte
	lba  uint32
}

func both32(b []byte, v uint32) {
	binary.LittleEndian.PutUint32(b, v)
	binary.BigEndian.PutUint32(b[4:], v)
}

func both16(b []byte, v uint16) {
	binary.LittleEndian.PutUint16(b, v)
	binary.BigEndian.PutUint16(b[2:], v)
}

func ucs2(s string) []byte {
	u := utf16.Encode([]rune(s))
	b := make([]byte, 2*len(u))
	for i, c := range u {
		binary.BigEndian.PutUint16(b[2*i:], c)
	}
	return b
}

// pad — строка поля дескриптора: ASCII с пробелами или UCS-2 с пробелами (Joliet).
func pad(dst []byte, s string, joliet bool) {
	if joliet {
		for i := 0; i+1 < len(dst); i += 2 {
			dst[i], dst[i+1] = 0, ' '
		}
		copy(dst, ucs2(s))
		return
	}
	for i := range dst {
		dst[i] = ' '
	}
	copy(dst, s)
}

func recDate(t time.Time) []byte {
	return []byte{byte(t.Year() - 1900), byte(t.Month()), byte(t.Day()), byte(t.Hour()), byte(t.Minute()), byte(t.Second()), 0}
}

func volDate(t time.Time) []byte {
	b := []byte(t.Format("20060102150405") + "00")
	return append(b, 0)
}

// dirRecord — запись каталога ISO 9660; id — идентификатор как есть (байты).
func dirRecord(id []byte, lba, size uint32, dir bool, t time.Time) []byte {
	n := 33 + len(id)
	if len(id)%2 == 0 {
		n++
	}
	r := make([]byte, n)
	r[0] = byte(n)
	both32(r[2:], lba)
	both32(r[10:], size)
	copy(r[18:], recDate(t))
	if dir {
		r[25] = 2
	}
	both16(r[28:], 1)
	r[32] = byte(len(id))
	copy(r[33:], id)
	return r
}

// rootDir — сектор корневого каталога: «.», «..» и файлы (по возрастанию идентификатора).
func rootDir(lba uint32, files []*seedFile, joliet bool, t time.Time) []byte {
	b := make([]byte, 0, sector)
	b = append(b, dirRecord([]byte{0}, lba, sector, true, t)...)
	b = append(b, dirRecord([]byte{1}, lba, sector, true, t)...)
	for _, f := range files {
		id := []byte(f.iso)
		if joliet {
			id = ucs2(f.name)
		}
		b = append(b, dirRecord(id, f.lba, uint32(len(f.data)), false, t)...)
	}
	if len(b) > sector {
		panic("корневой каталог не помещается в сектор")
	}
	return append(b, make([]byte, sector-len(b))...)
}

func pathTable(rootLBA uint32, bigEndian bool) []byte {
	p := make([]byte, sector)
	p[0] = 1 // длина идентификатора корня
	if bigEndian {
		binary.BigEndian.PutUint32(p[2:], rootLBA)
		binary.BigEndian.PutUint16(p[6:], 1)
	} else {
		binary.LittleEndian.PutUint32(p[2:], rootLBA)
		binary.LittleEndian.PutUint16(p[6:], 1)
	}
	return p
}

func descriptor(kind byte, label string, total, lPath, mPath, rootLBA uint32, joliet bool, t time.Time) []byte {
	d := make([]byte, sector)
	d[0] = kind
	copy(d[1:], "CD001")
	d[6] = 1
	pad(d[8:40], "", joliet)
	pad(d[40:72], label, joliet)
	both32(d[80:], total)
	if joliet {
		copy(d[88:], "%/E") // UCS-2, уровень 3
	}
	both16(d[120:], 1)
	both16(d[124:], 1)
	both16(d[128:], sector)
	both32(d[132:], 10) // таблица путей: одна запись корня
	binary.LittleEndian.PutUint32(d[140:], lPath)
	binary.BigEndian.PutUint32(d[148:], mPath)
	copy(d[156:], dirRecord([]byte{0}, rootLBA, sector, true, t))
	for _, r := range [][2]int{{190, 318}, {318, 446}, {446, 574}, {574, 702}, {702, 739}, {739, 776}, {776, 813}} {
		pad(d[r[0]:r[1]], "", joliet)
	}
	copy(d[813:], volDate(t))
	copy(d[830:], volDate(t))
	copy(d[847:], "0000000000000000")
	copy(d[864:], "0000000000000000")
	d[881] = 1
	return d
}

// writeSeed пишет ISO: метка label, файлы name → данные.
func writeSeed(out, label string, files map[string][]byte) error {
	var list []*seedFile
	for name, data := range files {
		iso := isoName(name)
		list = append(list, &seedFile{name: name, iso: iso, data: data})
	}
	sort.Slice(list, func(i, j int) bool { return list[i].name < list[j].name })
	// 0–15 — системная область; 16 PVD, 17 Joliet, 18 конец; 19–20 и 21–22 — таблицы путей; 23, 24 — корни.
	const pvdRoot, jolietRoot = 23, 24
	next := uint32(25)
	for _, f := range list {
		f.lba = next
		next += uint32((len(f.data) + sector - 1) / sector)
		if len(f.data) == 0 {
			next++
		}
	}
	t := time.Now().UTC()
	img := make([]byte, 0, int(next)*sector)
	img = append(img, make([]byte, 16*sector)...)
	img = append(img, descriptor(1, label, next, 19, 20, pvdRoot, false, t)...)
	img = append(img, descriptor(2, label, next, 21, 22, jolietRoot, true, t)...)
	term := make([]byte, sector)
	term[0] = 255
	copy(term[1:], "CD001")
	term[6] = 1
	img = append(img, term...)
	img = append(img, pathTable(pvdRoot, false)...)
	img = append(img, pathTable(pvdRoot, true)...)
	img = append(img, pathTable(jolietRoot, false)...)
	img = append(img, pathTable(jolietRoot, true)...)
	img = append(img, rootDir(pvdRoot, list, false, t)...)
	img = append(img, rootDir(jolietRoot, list, true, t)...)
	for _, f := range list {
		img = append(img, f.data...)
		if rest := len(img) % sector; rest != 0 || len(f.data) == 0 {
			img = append(img, make([]byte, sector-rest)...)
		}
	}
	if len(img) != int(next)*sector {
		return fmt.Errorf("размер ISO %d, ожидалось %d", len(img), int(next)*sector)
	}
	return os.WriteFile(out, img, 0o644)
}

// isoName — имя уровня 1: 8 символов A–Z, 0–9, «_», расширение пустое, версия 1 (user-data → USER_DAT.;1).
func isoName(name string) string {
	b := []byte{}
	for _, c := range []byte(name) {
		switch {
		case c >= 'a' && c <= 'z':
			b = append(b, c-'a'+'A')
		case c >= 'A' && c <= 'Z', c >= '0' && c <= '9':
			b = append(b, c)
		default:
			b = append(b, '_')
		}
		if len(b) == 8 {
			break
		}
	}
	return string(b) + ".;1"
}

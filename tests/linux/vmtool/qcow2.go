package main

// Команда vhd: диск qcow2 (облачный образ Ubuntu) в фиксированный VHD для Hyper-V — Hyper-V qcow2 не читает,
// а qemu-img на Windows нет. Дальше new-vm.ps1 переводит VHD в VHDX (Convert-VHD).
//
// Понимает qcow2 версий 2 и 3 без файла-основы и шифрования, со сжатыми кластерами zlib (так собраны облачные
// образы Ubuntu). Расширенные L2 (подкластеры) и сжатие zstd — отказ с понятной ошибкой.

import (
	"bufio"
	"bytes"
	"compress/flate"
	"crypto/rand"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"os"
	"time"
)

const (
	l1OffsetMask = 0x00fffffffffffe00
	l2OffsetMask = 0x00fffffffffffe00
	l2Compressed = 1 << 62
	l2Zero       = 1 // v3: кластер читается нулями
)

type qcow2 struct {
	f           *os.File
	clusterBits uint32
	clusterSize int64
	size        int64
	l1          []uint64
}

func openQcow2(path string) (*qcow2, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	img, err := readHeader(f)
	if err != nil {
		f.Close()
		return nil, err
	}
	return img, nil
}

func readHeader(f *os.File) (*qcow2, error) {
	var h [112]byte
	if _, err := io.ReadFull(f, h[:]); err != nil {
		return nil, fmt.Errorf("заголовок qcow2: %w", err)
	}
	be := binary.BigEndian
	if !bytes.Equal(h[0:4], []byte{'Q', 'F', 'I', 0xfb}) {
		return nil, errors.New("не qcow2 (нет сигнатуры QFI)")
	}
	version := be.Uint32(h[4:])
	if version != 2 && version != 3 {
		return nil, fmt.Errorf("qcow2 версии %d не поддерживается", version)
	}
	if be.Uint64(h[8:]) != 0 {
		return nil, errors.New("образ с файлом-основой (backing file) не поддерживается")
	}
	if be.Uint32(h[32:]) != 0 {
		return nil, errors.New("зашифрованный образ не поддерживается")
	}
	if version == 3 {
		incompat := be.Uint64(h[72:])
		if incompat&(1<<4) != 0 {
			return nil, errors.New("расширенные L2 (подкластеры) не поддерживаются")
		}
		if incompat&(1<<3) != 0 && be.Uint32(h[100:]) > 104 && h[104] != 0 {
			return nil, errors.New("сжатие zstd не поддерживается — только zlib")
		}
	}
	img := &qcow2{f: f, clusterBits: be.Uint32(h[20:]), size: int64(be.Uint64(h[24:]))}
	if img.clusterBits < 9 || img.clusterBits > 21 {
		return nil, fmt.Errorf("размер кластера 2^%d вне допустимого", img.clusterBits)
	}
	img.clusterSize = 1 << img.clusterBits
	l1Size := be.Uint32(h[36:])
	raw := make([]byte, 8*int(l1Size))
	if _, err := f.ReadAt(raw, int64(be.Uint64(h[40:]))); err != nil {
		return nil, fmt.Errorf("таблица L1: %w", err)
	}
	img.l1 = make([]uint64, l1Size)
	for i := range img.l1 {
		img.l1[i] = be.Uint64(raw[8*i:])
	}
	return img, nil
}

// cluster читает виртуальный кластер n в buf (длиной clusterSize); нераспределённый — нули.
func (img *qcow2) cluster(n int64, l2cache map[uint64][]uint64, buf []byte) error {
	clear(buf)
	l2Entries := img.clusterSize / 8
	l1i := n / l2Entries
	if l1i >= int64(len(img.l1)) {
		return nil
	}
	l2off := img.l1[l1i] & l1OffsetMask
	if l2off == 0 {
		return nil
	}
	l2, ok := l2cache[l2off]
	if !ok {
		raw := make([]byte, img.clusterSize)
		if _, err := img.f.ReadAt(raw, int64(l2off)); err != nil {
			return fmt.Errorf("таблица L2 по смещению %d: %w", l2off, err)
		}
		l2 = make([]uint64, l2Entries)
		for i := range l2 {
			l2[i] = binary.BigEndian.Uint64(raw[8*i:])
		}
		clear(l2cache) // кластеры идут подряд: хватает одной таблицы L2
		l2cache[l2off] = l2
	}
	e := l2[n%l2Entries]
	if e&l2Compressed != 0 {
		x := 62 - (img.clusterBits - 8)
		off := int64(e & (1<<x - 1))
		sectors := int64((e>>x)&(1<<(62-x)-1)) + 1
		comp := make([]byte, sectors*512-(off&511))
		if _, err := img.f.ReadAt(comp, off); err != nil && !errors.Is(err, io.EOF) {
			return fmt.Errorf("сжатый кластер %d: %w", n, err)
		}
		r := flate.NewReader(bytes.NewReader(comp))
		defer r.Close()
		if _, err := io.ReadFull(r, buf); err != nil {
			return fmt.Errorf("распаковать кластер %d: %w", n, err)
		}
		return nil
	}
	if e&l2Zero != 0 {
		return nil
	}
	if off := e & l2OffsetMask; off != 0 {
		if _, err := img.f.ReadAt(buf, int64(off)); err != nil {
			return fmt.Errorf("кластер %d: %w", n, err)
		}
	}
	return nil
}

// vhdFooter — подвал фиксированного VHD (спецификация Microsoft VHD, «Hard Disk Footer Format»).
func vhdFooter(size int64) []byte {
	f := make([]byte, 512)
	be := binary.BigEndian
	copy(f[0:], "conectix")
	be.PutUint32(f[8:], 2)           // features: reserved
	be.PutUint32(f[12:], 0x00010000) // версия формата
	be.PutUint64(f[16:], ^uint64(0)) // data offset: у фиксированного диска нет
	be.PutUint32(f[24:], uint32(time.Since(time.Date(2000, 1, 1, 0, 0, 0, 0, time.UTC)).Seconds()))
	copy(f[28:], "emvt")
	be.PutUint32(f[32:], 0x00010000)
	copy(f[36:], "Wi2k")
	be.PutUint64(f[40:], uint64(size))
	be.PutUint64(f[48:], uint64(size))
	c, h, s := chs(size)
	be.PutUint16(f[56:], c)
	f[58], f[59] = h, s
	be.PutUint32(f[60:], 2) // фиксированный диск
	rand.Read(f[68:84])
	var sum uint32
	for i, b := range f {
		if i < 64 || i >= 68 {
			sum += uint32(b)
		}
	}
	be.PutUint32(f[64:], ^sum)
	return f
}

// chs — геометрия диска по алгоритму из спецификации VHD.
func chs(size int64) (uint16, uint8, uint8) {
	total := size / 512
	if total > 65535*16*255 {
		total = 65535 * 16 * 255
	}
	var spt, heads, cth int64
	if total >= 65535*16*63 {
		spt, heads = 255, 16
		cth = total / spt
	} else {
		spt = 17
		cth = total / spt
		heads = (cth + 1023) / 1024
		if heads < 4 {
			heads = 4
		}
		if cth >= heads*1024 || heads > 16 {
			spt, heads = 31, 16
			cth = total / spt
		}
		if cth >= heads*1024 {
			spt, heads = 63, 16
			cth = total / spt
		}
	}
	return uint16(cth / heads), uint8(heads), uint8(spt)
}

// convertVHD пишет qcow2 из in фиксированным VHD в out.
func convertVHD(in, out string) error {
	img, err := openQcow2(in)
	if err != nil {
		return fmt.Errorf("открыть %s: %w", in, err)
	}
	defer img.f.Close()
	if img.size%512 != 0 {
		return fmt.Errorf("размер диска %d не кратен 512", img.size)
	}
	o, err := os.Create(out)
	if err != nil {
		return err
	}
	w := bufio.NewWriterSize(o, 4<<20)
	buf := make([]byte, img.clusterSize)
	cache := map[uint64][]uint64{}
	clusters := (img.size + img.clusterSize - 1) / img.clusterSize
	for n := int64(0); n < clusters; n++ {
		if err := img.cluster(n, cache, buf); err != nil {
			o.Close()
			return err
		}
		chunk := buf
		if rest := img.size - n*img.clusterSize; rest < img.clusterSize {
			chunk = buf[:rest]
		}
		if _, err := w.Write(chunk); err != nil {
			o.Close()
			return fmt.Errorf("записать %s: %w", out, err)
		}
	}
	if _, err := w.Write(vhdFooter(img.size)); err != nil {
		o.Close()
		return err
	}
	if err := w.Flush(); err != nil {
		o.Close()
		return err
	}
	return o.Close()
}

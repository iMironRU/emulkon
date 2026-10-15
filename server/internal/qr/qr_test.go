package qr

import (
	"bytes"
	"strings"
	"testing"
)

// decode читает матрицу обратно: формат (BCH), маска, данные змейкой, блоки и коды
// Рида — Соломона, режим «байты». Проверяет согласованность кодировщика.
func decode(t *testing.T, c *Code) []byte {
	t.Helper()
	n := c.Size
	m := c.Modules
	// Формат из первой копии.
	read := func(x, y int) int {
		if m[y][x] {
			return 1
		}
		return 0
	}
	var bits int
	for i := 0; i <= 5; i++ {
		bits |= read(8, i) << i
	}
	bits |= read(8, 7)<<6 | read(8, 8)<<7 | read(7, 8)<<8
	for i := 9; i < 15; i++ {
		bits |= read(14-i, 8) << i
	}
	var second int
	for i := 0; i < 8; i++ {
		second |= read(n-1-i, 8) << i
	}
	for i := 8; i < 15; i++ {
		second |= read(8, n-15+i) << i
	}
	if bits != second {
		t.Fatalf("две копии формата расходятся: %015b %015b", bits, second)
	}
	raw := bits ^ 0x5412
	data := raw >> 10
	rem := data
	for i := 0; i < 10; i++ {
		rem = rem<<1 ^ (rem>>9)*0x537
	}
	if rem&0x3FF != raw&0x3FF {
		t.Fatalf("формат не проходит BCH: %015b", raw)
	}
	if data>>3 != 0 {
		t.Fatalf("уровень коррекции не M: %02b", data>>3)
	}
	mask := data & 7
	if n != 17+4*c.Version || !m[n-8][8] {
		t.Fatalf("размер или тёмный модуль")
	}

	// Служебные модули — те же, что рисует кодировщик для этой версии.
	fn := newCode(c.Version)
	fn.drawFunctionPatterns()
	var stream []byte
	var cur byte
	k := 0
	for right := n - 1; right >= 1; right -= 2 {
		if right == 6 {
			right = 5
		}
		for vert := 0; vert < n; vert++ {
			for j := 0; j < 2; j++ {
				x := right - j
				y := vert
				if (right+1)&2 == 0 {
					y = n - 1 - vert
				}
				if fn.fn[y][x] {
					continue
				}
				v := m[y][x] != maskBit(mask, x, y)
				cur <<= 1
				if v {
					cur |= 1
				}
				k++
				if k%8 == 0 {
					stream = append(stream, cur)
					cur = 0
				}
			}
		}
	}
	// Разбор перемежения и проверка кодов коррекции.
	spec := specM[c.Version]
	var lens []int
	for _, g := range spec.groups {
		for i := 0; i < g[0]; i++ {
			lens = append(lens, g[1])
		}
	}
	blocks := make([][]byte, len(lens))
	pos := 0
	for i := 0; ; i++ {
		added := false
		for b, l := range lens {
			if i < l {
				blocks[b] = append(blocks[b], stream[pos])
				pos++
				added = true
			}
		}
		if !added {
			break
		}
	}
	ecc := make([][]byte, len(lens))
	for i := 0; i < spec.ec; i++ {
		for b := range lens {
			ecc[b] = append(ecc[b], stream[pos])
			pos++
		}
	}
	gen := rsGenerator(spec.ec)
	var all []byte
	for b := range blocks {
		if !bytes.Equal(rsRemainder(blocks[b], gen), ecc[b]) {
			t.Fatalf("блок %d: коды Рида — Соломона не сходятся", b)
		}
		all = append(all, blocks[b]...)
	}
	// Режим 0100 и длина.
	if all[0]>>4 != 4 {
		t.Fatalf("режим не «байты»: %x", all[0]>>4)
	}
	bitAt := func(i int) int { return int(all[i/8]>>(7-i%8)) & 1 }
	num := func(from, n int) int {
		v := 0
		for i := 0; i < n; i++ {
			v = v<<1 | bitAt(from+i)
		}
		return v
	}
	countBits := 8
	if c.Version >= 10 {
		countBits = 16
	}
	length := num(4, countBits)
	out := make([]byte, length)
	for i := range out {
		out[i] = byte(num(4+countBits+8*i, 8))
	}
	return out
}

func TestRoundTrip(t *testing.T) {
	cases := []string{
		"",
		"HELLO",
		"t=20261009T1530&s=1179.80&fn=9999078900004312&i=27&fp=2057366264&n=1", // QR чека ФНС
		strings.Repeat("Эмулкон ", 12),                                         // кириллица UTF-8, версия 7+ (информация о версии)
		strings.Repeat("x", 213),                                               // предел версии 10
	}
	for _, s := range cases {
		c, err := Encode([]byte(s))
		if err != nil {
			t.Fatalf("%q: %v", s, err)
		}
		if got := decode(t, c); string(got) != s {
			t.Fatalf("версия %d маска %d: прочитано %q, ждали %q", c.Version, c.Mask, got, s)
		}
		if len(c.Rows()) != c.Size || len(c.Rows()[0]) != c.Size {
			t.Fatal("Rows")
		}
	}
	if _, err := Encode([]byte(strings.Repeat("x", 214))); err != ErrTooLong {
		t.Fatalf("слишком длинные данные: %v", err)
	}
	// Чек ФНС — версия 5 (37×37): помещается и на узкой ленте.
	c, _ := Encode([]byte(cases[2]))
	if c.Version != 5 || c.Size != 37 {
		t.Fatalf("версия QR чека: %d (%d)", c.Version, c.Size)
	}
}

// Известный эталон: версия 1-M, «01234567» в байтовом режиме — проверяем поисковые узоры и
// тайминг (рисунок не зависит от данных).
func TestFixedPatterns(t *testing.T) {
	c, _ := Encode([]byte("01234567"))
	m := c.Modules
	finder := []string{"1111111", "1000001", "1011101", "1011101", "1011101", "1000001", "1111111"}
	for _, origin := range [][2]int{{0, 0}, {c.Size - 7, 0}, {0, c.Size - 7}} {
		for y, row := range finder {
			for x, ch := range row {
				if m[origin[1]+y][origin[0]+x] != (ch == '1') {
					t.Fatalf("поисковый узор в %v", origin)
				}
			}
		}
	}
	for i := 8; i < c.Size-8; i++ {
		if m[6][i] != (i%2 == 0) || m[i][6] != (i%2 == 0) {
			t.Fatalf("тайминг в %d", i)
		}
	}
}

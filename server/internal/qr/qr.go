// Package qr — кодировщик QR-кода (ISO/IEC 18004) без внешних зависимостей: байтовый режим,
// уровень коррекции M, версии 1–10 (до 213 байт). Нужен для QR-кода чека ФНС на ленте ККТ
// (docs/models/kkt.md): веб-интерфейс и трей рисуют готовую матрицу.
package qr

import (
	"errors"
	"strings"
)

// Code — матрица модулей: true — тёмный. Квадрат Size×Size, без поля (рисующий добавляет 4 модуля).
type Code struct {
	Size    int
	Version int
	Mask    int
	Modules [][]bool
}

// Rows — строки «1010…» для JSON: компактно и просто рисовать.
func (c *Code) Rows() []string {
	out := make([]string, c.Size)
	for y, row := range c.Modules {
		var b strings.Builder
		for _, m := range row {
			if m {
				b.WriteByte('1')
			} else {
				b.WriteByte('0')
			}
		}
		out[y] = b.String()
	}
	return out
}

var ErrTooLong = errors.New("qr: данных больше 213 байт")

// block — блоки коррекции уровня M: EC-кодов на блок, затем (блоков, данных в блоке) по группам.
type blockSpec struct {
	ec     int
	groups [][2]int
}

var specM = [11]blockSpec{
	{},
	{10, [][2]int{{1, 16}}},
	{16, [][2]int{{1, 28}}},
	{26, [][2]int{{1, 44}}},
	{18, [][2]int{{2, 32}}},
	{24, [][2]int{{2, 43}}},
	{16, [][2]int{{4, 27}}},
	{18, [][2]int{{4, 31}}},
	{22, [][2]int{{2, 38}, {2, 39}}},
	{22, [][2]int{{3, 36}, {2, 37}}},
	{26, [][2]int{{4, 43}, {1, 44}}},
}

var alignment = [11][]int{nil, {}, {6, 18}, {6, 22}, {6, 26}, {6, 30}, {6, 34}, {6, 22, 38}, {6, 24, 42}, {6, 26, 46}, {6, 28, 50}}

func (s blockSpec) dataCodewords() int {
	n := 0
	for _, g := range s.groups {
		n += g[0] * g[1]
	}
	return n
}

// Encode кодирует данные в QR-код наименьшей подходящей версии с лучшей маской.
func Encode(data []byte) (*Code, error) {
	version := 0
	for v := 1; v <= 10; v++ {
		countBits := 8
		if v >= 10 {
			countBits = 16
		}
		if 4+countBits+8*len(data) <= 8*specM[v].dataCodewords() {
			version = v
			break
		}
	}
	if version == 0 {
		return nil, ErrTooLong
	}
	codewords := addECC(dataCodewords(data, version), version)

	best := (*Code)(nil)
	bestPenalty := 0
	for mask := 0; mask < 8; mask++ {
		c := newCode(version)
		c.drawFunctionPatterns()
		c.drawCodewords(codewords)
		c.applyMask(mask)
		c.drawFormatBits(mask)
		if p := c.penalty(); best == nil || p < bestPenalty {
			best, bestPenalty = c.Code(), p
			best.Mask = mask
		}
	}
	return best, nil
}

// dataCodewords — режим «байты», длина, данные, терминатор и заполнение до ёмкости версии.
func dataCodewords(data []byte, version int) []byte {
	var bits []bool
	put := func(v, n int) {
		for i := n - 1; i >= 0; i-- {
			bits = append(bits, (v>>i)&1 == 1)
		}
	}
	put(0b0100, 4)
	if version >= 10 {
		put(len(data), 16)
	} else {
		put(len(data), 8)
	}
	for _, b := range data {
		put(int(b), 8)
	}
	capacity := 8 * specM[version].dataCodewords()
	for i := 0; i < 4 && len(bits) < capacity; i++ {
		bits = append(bits, false)
	}
	for len(bits)%8 != 0 {
		bits = append(bits, false)
	}
	out := make([]byte, 0, capacity/8)
	for i := 0; i < len(bits); i += 8 {
		var b byte
		for j := 0; j < 8; j++ {
			if bits[i+j] {
				b |= 1 << (7 - j)
			}
		}
		out = append(out, b)
	}
	for pad := byte(0xEC); len(out) < capacity/8; pad ^= 0xEC ^ 0x11 {
		out = append(out, pad)
	}
	return out
}

// addECC делит данные на блоки, добавляет коды Рида — Соломона и перемежает.
func addECC(data []byte, version int) []byte {
	spec := specM[version]
	gen := rsGenerator(spec.ec)
	var blocks, ecc [][]byte
	pos := 0
	for _, g := range spec.groups {
		for i := 0; i < g[0]; i++ {
			b := data[pos : pos+g[1]]
			pos += g[1]
			blocks = append(blocks, b)
			ecc = append(ecc, rsRemainder(b, gen))
		}
	}
	var out []byte
	for i := 0; ; i++ {
		added := false
		for _, b := range blocks {
			if i < len(b) {
				out = append(out, b[i])
				added = true
			}
		}
		if !added {
			break
		}
	}
	for i := 0; i < spec.ec; i++ {
		for _, e := range ecc {
			out = append(out, e[i])
		}
	}
	return out
}

// --- поле GF(256), многочлен 0x11D ---

func gfMul(x, y byte) byte {
	var z byte
	for i := 7; i >= 0; i-- {
		hi := z >> 7
		z = z<<1 ^ hi*0x1D
		z ^= (y >> uint(i) & 1) * x
	}
	return z
}

func rsGenerator(degree int) []byte {
	g := make([]byte, degree)
	g[degree-1] = 1
	root := byte(1)
	for i := 0; i < degree; i++ {
		for j := range g {
			g[j] = gfMul(g[j], root)
			if j+1 < len(g) {
				g[j] ^= g[j+1]
			}
		}
		root = gfMul(root, 2)
	}
	return g
}

func rsRemainder(data, gen []byte) []byte {
	r := make([]byte, len(gen))
	for _, b := range data {
		f := b ^ r[0]
		copy(r, r[1:])
		r[len(r)-1] = 0
		for i, g := range gen {
			r[i] ^= gfMul(g, f)
		}
	}
	return r
}

// --- матрица ---

type builder struct {
	size, version int
	mod, fn       [][]bool // модули и признак «служебный» (не данные)
}

func newCode(version int) *builder {
	size := 17 + 4*version
	b := &builder{size: size, version: version}
	b.mod, b.fn = make([][]bool, size), make([][]bool, size)
	for i := range b.mod {
		b.mod[i], b.fn[i] = make([]bool, size), make([]bool, size)
	}
	return b
}

func (b *builder) Code() *Code {
	c := &Code{Size: b.size, Version: b.version, Modules: make([][]bool, b.size)}
	for y := range b.mod {
		c.Modules[y] = append([]bool(nil), b.mod[y]...)
	}
	return c
}

func (b *builder) set(x, y int, dark bool) {
	b.mod[y][x] = dark
	b.fn[y][x] = true
}

func (b *builder) drawFunctionPatterns() {
	for i := 0; i < b.size; i++ { // тайминг
		b.set(6, i, i%2 == 0)
		b.set(i, 6, i%2 == 0)
	}
	b.finder(3, 3)
	b.finder(b.size-4, 3)
	b.finder(3, b.size-4)
	pos := alignment[b.version]
	n := len(pos)
	for i := 0; i < n; i++ {
		for j := 0; j < n; j++ {
			if i == 0 && j == 0 || i == 0 && j == n-1 || i == n-1 && j == 0 {
				continue // на месте поисковых узоров
			}
			for dy := -2; dy <= 2; dy++ {
				for dx := -2; dx <= 2; dx++ {
					d := max(abs(dx), abs(dy))
					b.set(pos[i]+dx, pos[j]+dy, d != 1)
				}
			}
		}
	}
	b.drawFormatBits(0) // резерв области формата; настоящие биты — после маски
	if b.version >= 7 {
		rem := b.version
		for i := 0; i < 12; i++ {
			rem = rem<<1 ^ (rem>>11)*0x1F25
		}
		bits := b.version<<12 | rem
		for i := 0; i < 18; i++ {
			dark := bits>>i&1 == 1
			x, y := b.size-11+i%3, i/3
			b.set(x, y, dark)
			b.set(y, x, dark)
		}
	}
}

func (b *builder) finder(cx, cy int) {
	for dy := -4; dy <= 4; dy++ {
		for dx := -4; dx <= 4; dx++ {
			x, y := cx+dx, cy+dy
			if x < 0 || y < 0 || x >= b.size || y >= b.size {
				continue
			}
			d := max(abs(dx), abs(dy))
			b.set(x, y, d != 2 && d != 4)
		}
	}
}

// drawFormatBits — уровень M (биты 00) и маска, BCH(15,5), две копии.
func (b *builder) drawFormatBits(mask int) {
	data := 0<<3 | mask
	rem := data
	for i := 0; i < 10; i++ {
		rem = rem<<1 ^ (rem>>9)*0x537
	}
	bits := (data<<10 | rem) ^ 0x5412
	bit := func(i int) bool { return bits>>i&1 == 1 }
	for i := 0; i <= 5; i++ {
		b.set(8, i, bit(i))
	}
	b.set(8, 7, bit(6))
	b.set(8, 8, bit(7))
	b.set(7, 8, bit(8))
	for i := 9; i < 15; i++ {
		b.set(14-i, 8, bit(i))
	}
	for i := 0; i < 8; i++ {
		b.set(b.size-1-i, 8, bit(i))
	}
	for i := 8; i < 15; i++ {
		b.set(8, b.size-15+i, bit(i))
	}
	b.set(8, b.size-8, true) // тёмный модуль
}

// drawCodewords — данные змейкой снизу справа, парами столбцов, минуя столбец тайминга.
func (b *builder) drawCodewords(data []byte) {
	i := 0
	for right := b.size - 1; right >= 1; right -= 2 {
		if right == 6 {
			right = 5
		}
		for vert := 0; vert < b.size; vert++ {
			for j := 0; j < 2; j++ {
				x := right - j
				upward := (right+1)&2 == 0
				y := vert
				if upward {
					y = b.size - 1 - vert
				}
				if !b.fn[y][x] && i < len(data)*8 {
					b.mod[y][x] = data[i>>3]>>(7-i&7)&1 == 1
					i++
				}
			}
		}
	}
}

func maskBit(mask, x, y int) bool {
	switch mask {
	case 0:
		return (x+y)%2 == 0
	case 1:
		return y%2 == 0
	case 2:
		return x%3 == 0
	case 3:
		return (x+y)%3 == 0
	case 4:
		return (x/3+y/2)%2 == 0
	case 5:
		return x*y%2+x*y%3 == 0
	case 6:
		return (x*y%2+x*y%3)%2 == 0
	default:
		return ((x+y)%2+x*y%3)%2 == 0
	}
}

func (b *builder) applyMask(mask int) {
	for y := 0; y < b.size; y++ {
		for x := 0; x < b.size; x++ {
			if !b.fn[y][x] && maskBit(mask, x, y) {
				b.mod[y][x] = !b.mod[y][x]
			}
		}
	}
}

// penalty — штраф маски (ISO/IEC 18004, 7.8.3): серии, блоки 2×2, «поисковые» узоры, баланс.
func (b *builder) penalty() int {
	n := b.size
	p := 0
	at := func(x, y int, horiz bool) bool {
		if horiz {
			return b.mod[y][x]
		}
		return b.mod[x][y]
	}
	for _, horiz := range []bool{true, false} {
		for y := 0; y < n; y++ {
			run := 1
			for x := 1; x <= n; x++ {
				if x < n && at(x, y, horiz) == at(x-1, y, horiz) {
					run++
					continue
				}
				if run >= 5 {
					p += 3 + run - 5
				}
				run = 1
			}
			for x := 0; x+11 <= n; x++ {
				var s [11]bool
				for k := range s {
					s[k] = at(x+k, y, horiz)
				}
				core := s[0] && !s[1] && s[2] && s[3] && s[4] && !s[5] && s[6]
				core2 := s[4] && !s[5] && s[6] && s[7] && s[8] && !s[9] && s[10]
				if core && !s[7] && !s[8] && !s[9] && !s[10] || core2 && !s[0] && !s[1] && !s[2] && !s[3] {
					p += 40
				}
			}
		}
	}
	dark := 0
	for y := 0; y < n; y++ {
		for x := 0; x < n; x++ {
			if b.mod[y][x] {
				dark++
			}
			if x+1 < n && y+1 < n {
				c := b.mod[y][x]
				if b.mod[y][x+1] == c && b.mod[y+1][x] == c && b.mod[y+1][x+1] == c {
					p += 3
				}
			}
		}
	}
	total := n * n
	k := (abs(dark*20-total*10) + total - 1) / total
	return p + max(0, k-1)*10
}

func abs(v int) int {
	if v < 0 {
		return -v
	}
	return v
}

package input

import (
	"context"
	"fmt"
	"strings"
	"time"

	"emulkon/internal/models"
)

// Track — настройка дорожки: передаётся ли, префикс и суффикс (коды символов, 0 — нет).
// По умолчанию — как в настройке считывателя БПО: только дорожка 2, суффикс 13 (CR).
type Track struct {
	Use    bool `json:"use"`
	Prefix int  `json:"prefix"`
	Suffix int  `json:"suffix"`
}

func defaultTracks() [3]Track {
	return [3]Track{{}, {Use: true, Suffix: 13}, {}}
}

// Карты для кнопок панели управления — только тестовые данные (дисконтные, не банковские).
var cardPresets = map[string][3]string{
	"discount": {"B7770000000017^ЭМУЛЯТОР/ДИСКОНТ^2912", "7770000000017=2912", ""},
	"employee": {"B7790000000013^ЭМУЛЯТОР/КАССИР^2912", "7790000000013=2912", ""},
}

type swipeRecord struct {
	Tracks [3]string `json:"tracks"`
	Card   string    `json:"card,omitempty"`
	Event  string    `json:"event,omitempty"` // пусто — только клавиатура
	Data   string    `json:"data"`
	At     time.Time `json:"at"`
	// Keyboard — ввести с клавиатуры на рабочем месте (режим keyboard или both).
	Keyboard *models.KeyboardInput `json:"keyboard,omitempty"`
}

func (r swipeRecord) KeyboardInput() *models.KeyboardInput { return r.Keyboard }

// CardReader — эмулятор считывателя магнитных карт: карту «проводят» в панели управления.
type CardReader struct {
	base
	tracks [3]Track
	swipes int
	last   []swipeRecord
	lib    []Card // библиотека: свои карты
}

// CardReaderFactory — фабрика для реестра.
func CardReaderFactory(version string) models.Factory {
	return func(_ models.EquipmentType, _ string) models.Model { return NewCardReader(Options{Version: version}) }
}

func NewCardReader(opts Options) *CardReader {
	opts.defaults()
	m := &CardReader{base: base{opts: opts, title: "считывателя карт"}}
	m.reset()
	return m
}

func (m *CardReader) reset() {
	m.resetInput("none") // суффикс дорожки (13 — Enter) уже в данных
	m.tracks = defaultTracks()
	m.lib = []Card{}
	m.swipes = 0
	m.last = nil
}

func (m *CardReader) Reset() {
	m.mu.Lock()
	m.reset()
	m.mu.Unlock()
}

func (m *CardReader) Description() models.Description {
	return m.base.Description("ЭМУЛЯТОР: считыватель карт (Эмулкон)",
		"Виртуальный считыватель магнитных карт Эмулкон: карты проводят в панели управления.")
}

func (m *CardReader) Handle(_ context.Context, method string, params []any) models.Result {
	return m.handle(method, params)
}

// Act — swipe: {"card": "discount"} или {"track1": "…", "track2": "…", "track3": "…"}.
func (m *CardReader) Act(name string, args map[string]any) (any, error) {
	if name != "swipe" {
		return nil, models.ErrUnknownAction
	}
	var tracks [3]string
	card, err := strArg(args, "card")
	if err != nil {
		return nil, err
	}
	if card != "" {
		p, ok := cardPresets[card]
		if !ok {
			// Карта из библиотеки — по подписи.
			m.mu.Lock()
			for _, c := range m.lib {
				if c.Title == card {
					p, ok = [3]string{c.Track1, c.Track2, c.Track3}, true
				}
			}
			m.mu.Unlock()
		}
		if !ok {
			return nil, fmt.Errorf("card: discount, employee или подпись карты из библиотеки")
		}
		tracks = p
	} else {
		for i := range tracks {
			if tracks[i], err = strArg(args, fmt.Sprintf("track%d", i+1)); err != nil {
				return nil, err
			}
		}
	}
	m.mu.Lock()
	var data strings.Builder
	for i, t := range m.tracks {
		if !t.Use || tracks[i] == "" {
			continue
		}
		if t.Prefix > 0 {
			data.WriteRune(rune(t.Prefix))
		}
		data.WriteString(tracks[i])
		if t.Suffix > 0 {
			data.WriteRune(rune(t.Suffix))
		}
	}
	if data.Len() == 0 {
		m.mu.Unlock()
		return nil, fmt.Errorf("нет данных ни на одной из передаваемых дорожек")
	}
	e := m.event("TracksData", data.String())
	emit, kb := m.deliver(e, data.String())
	rec := swipeRecord{Tracks: tracks, Card: card, Data: data.String(), At: m.opts.Now(), Keyboard: kb}
	if emit {
		rec.Event = e.Name
	}
	m.swipes++
	m.last = append([]swipeRecord{rec}, m.last...)
	if len(m.last) > 20 {
		m.last = m.last[:20]
	}
	m.mu.Unlock()
	if emit {
		m.emit(e)
	}
	return rec, nil
}

func (m *CardReader) State() map[string]any {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.inputState(map[string]any{
		"model": "card_reader", "event_format": m.format, "tracks": m.tracks,
		"swipes": m.swipes, "last": append([]swipeRecord{}, m.last...), "library": append([]Card{}, m.lib...),
	})
}

// SetState — event_format, tracks: [{use, prefix, suffix}] × 3 (docs/models/input.md).
func (m *CardReader) SetState(patch map[string]any) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	format, tracks, mode, suffix, delay, lib := m.format, m.tracks, m.mode, m.suffix, m.delayMS, m.lib
	for k, v := range patch {
		var err error
		if ok, e := m.setInputKey(k, v, &mode, &suffix, &delay); ok {
			err = e
		} else {
			switch k {
			case "event_format":
				format, err = setFormat(v)
			case "tracks":
				tracks, err = parseTracks(v)
			case "library":
				lib, err = parseLibrary(v, func(c Card) string { return c.Title }, func(c Card) error {
					if c.Track1 == "" && c.Track2 == "" && c.Track3 == "" {
						return fmt.Errorf("нужна хотя бы одна дорожка")
					}
					return nil
				})
			default:
				err = fmt.Errorf("неизвестный ключ %q; допустимы: event_format, tracks, input_mode, keyboard_suffix, keyboard_delay_ms, library", k)
			}
		}
		if err != nil {
			return err
		}
	}
	m.format, m.tracks, m.mode, m.suffix, m.delayMS, m.lib = format, tracks, mode, suffix, delay, lib
	return nil
}

func parseTracks(v any) ([3]Track, error) {
	var out [3]Track
	list, ok := v.([]any)
	if !ok || len(list) != 3 {
		return out, fmt.Errorf("tracks: массив из трёх дорожек {use, prefix, suffix}")
	}
	for i, x := range list {
		o, ok := x.(map[string]any)
		if !ok {
			return out, fmt.Errorf("tracks[%d]: объект {use, prefix, suffix}", i)
		}
		use, _ := o["use"].(bool)
		out[i].Use = use
		for _, f := range []struct {
			key string
			dst *int
		}{{"prefix", &out[i].Prefix}, {"suffix", &out[i].Suffix}} {
			if o[f.key] == nil {
				continue
			}
			n, err := numArg(o[f.key])
			if err != nil || n < 0 || n > 255 || n != float64(int(n)) {
				return out, fmt.Errorf("tracks[%d].%s: код символа 0–255", i, f.key)
			}
			*f.dst = int(n)
		}
	}
	return out, nil
}

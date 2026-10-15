package models

import "strings"

// Вывод документов на рабочем месте (ARCHITECTURE §4.3, docs/protocol.md §2.3): показать
// документ на экране кассы и напечатать на принтер Windows решает сервер по настройке
// устройства, трей выполняет.

// Output — настройка вывода документов устройства.
type Output struct {
	Screen      bool     `json:"screen"`       // показывать на экране кассы
	Printer     bool     `json:"printer"`      // печатать на принтер Windows
	PrinterName string   `json:"printer_name"` // пусто — принтер по умолчанию
	Kinds       []string `json:"kinds"`        // группы документов для печати (OutputKind.ID)
}

// OutputDecision — что сделать с одним документом (payload device.print → output).
type OutputDecision struct {
	Screen  bool   `json:"screen"`
	Print   bool   `json:"print"`
	Printer string `json:"printer"`
}

// OutputKind — группа видов документов для выбора «что печатать».
type OutputKind struct {
	ID    string   `json:"id"`
	Title string   `json:"title"`
	Kinds []string `json:"kinds"` // PrintedDocument.Kind
}

var outputKinds = []OutputKind{
	{"checks", "Чеки", []string{"check"}},
	{"corrections", "Чеки коррекции", []string{"correction"}},
	{"copies", "Копии чеков", []string{"copy"}},
	{"reports", "Отчёты: смена, X-отчёт, расчёты, ФН", []string{"open_shift", "close_shift", "x_report", "settlements", "fn_report"}},
	{"nonfiscal", "Нефискальные: текст, внесение, выплата", []string{"text", "cash_in", "cash_out"}},
	{"slips", "Слипы эквайринга и сверка итогов", []string{"slip", "settlement"}},
	{"labels", "Этикетки", []string{"label"}},
}

// outputByType — какие группы бывают у класса; класса нет — устройство ничего не печатает.
var outputByType = map[string][]string{
	"kkt":             {"checks", "corrections", "copies", "reports", "nonfiscal"},
	"receipt_printer": {"nonfiscal"},
	"acquiring":       {"slips"},
	"label_scales":    {"labels"},
	"label_printer":   {"labels"},
}

// OutputKinds — группы документов класса; nil — класс ничего не печатает.
func OutputKinds(typeKey string) []OutputKind {
	var out []OutputKind
	for _, id := range outputByType[typeKey] {
		for _, k := range outputKinds {
			if k.ID == id {
				out = append(out, k)
			}
		}
	}
	return out
}

// DefaultOutput — вывод нового устройства: на экран — да, на принтер — нет. Если печать
// включат: у ККТ — чеки, коррекции и копии (как раньше в трее), у остальных — все их группы.
func DefaultOutput(typeKey string) *Output {
	kinds := OutputKinds(typeKey)
	if kinds == nil {
		return nil
	}
	o := &Output{Screen: true}
	for _, k := range kinds {
		if typeKey == "kkt" && (k.ID == "reports" || k.ID == "nonfiscal") {
			continue
		}
		o.Kinds = append(o.Kinds, k.ID)
	}
	return o
}

// Decide — что сделать с документом вида kind.
func (o *Output) Decide(kind string) OutputDecision {
	if o == nil {
		return OutputDecision{Screen: true}
	}
	d := OutputDecision{Screen: o.Screen, Printer: o.PrinterName}
	if o.Printer {
		for _, id := range o.Kinds {
			for _, k := range outputKinds {
				if k.ID == id && contains(k.Kinds, kind) {
					d.Print = true
				}
			}
		}
	}
	return d
}

// Normalize проверяет настройку для класса: только его группы, имя принтера без управляющих символов.
func (o *Output) Normalize(typeKey string) (string, bool) {
	allowed := OutputKinds(typeKey)
	if allowed == nil {
		return "класс " + typeKey + " ничего не печатает", false
	}
	if strings.ContainsFunc(o.PrinterName, func(r rune) bool { return r < 0x20 }) || len(o.PrinterName) > 256 {
		return "printer_name: до 256 символов без управляющих", false
	}
	seen := map[string]bool{}
	var kinds []string
	for _, id := range o.Kinds {
		ok := false
		for _, k := range allowed {
			ok = ok || k.ID == id
		}
		if !ok {
			return "kinds: у класса " + typeKey + " нет группы " + id, false
		}
		if !seen[id] {
			seen[id] = true
			kinds = append(kinds, id)
		}
	}
	o.Kinds = kinds
	return "", true
}

func contains(list []string, s string) bool {
	for _, v := range list {
		if v == s {
			return true
		}
	}
	return false
}

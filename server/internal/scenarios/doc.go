// Package scenarios — движок сценариев и внедрения сбоев (ARCHITECTURE §7).
//
// Правило: match (устройство, класс, рабочее место, метод, параметр, номер вызова, каждый N-й,
// вероятность) → action (error, delay, drop, late, set_state, emit_event). Правила хранятся
// в JSON (data/scenarios.json), включаются на лету, каждое срабатывание пишется в журнал
// (applied_rules). Правило вычисляется один раз на первое выполнение команды: повтор с тем же
// request_id получает сохранённый ответ (идемпотентность, docs/protocol.md §1.2).
package scenarios

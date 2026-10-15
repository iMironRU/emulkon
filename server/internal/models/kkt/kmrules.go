package kkt

import (
	"encoding/json"
	"fmt"
	"strings"
)

// KMRule — ответ ОИСМ по коду маркировки (docs/models/kkt.md, «Маркировка»): код содержит
// Contains — ответ Result. Правила проверяются по порядку; ни одно не подошло — km_result.
type KMRule struct {
	Title    string `json:"title,omitempty"`
	Contains string `json:"contains"`
	Result   string `json:"result"`
}

func validKMResult(s string) bool {
	return s == kmValid || s == kmInvalid || s == kmSuspended || s == kmUnavailable
}

// kmRuleFor — ответ ОИСМ по правилу для кода маркировки (раскодированного из Base64); пусто —
// правила нет, ответ — текущий km_result.
func (st *state) kmRuleFor(code string) string {
	for _, r := range st.kmRules {
		if strings.Contains(code, r.Contains) {
			return r.Result
		}
	}
	return ""
}

// kmResultOf — ответ ОИСМ на запрос: по правилу кода или текущий km_result (меняется на лету).
func (st *state) kmResultOf(req *kmRequest) string {
	if req.Result != "" {
		return req.Result
	}
	return st.kmResult
}

func parseKMRules(v any) ([]KMRule, error) {
	b, err := json.Marshal(v)
	if err != nil {
		return nil, fmt.Errorf("km_rules: %v", err)
	}
	var rules []KMRule
	if err := json.Unmarshal(b, &rules); err != nil {
		return nil, fmt.Errorf("km_rules: массив {title, contains, result}: %v", err)
	}
	if len(rules) > 50 {
		return nil, fmt.Errorf("km_rules: не больше 50 правил")
	}
	for i, r := range rules {
		if r.Contains == "" || len(r.Contains) > 200 {
			return nil, fmt.Errorf("km_rules[%d]: contains — от 1 до 200 символов", i)
		}
		if !validKMResult(r.Result) {
			return nil, fmt.Errorf("km_rules[%d]: result — %s, %s, %s или %s", i, kmValid, kmInvalid, kmSuspended, kmUnavailable)
		}
	}
	if rules == nil {
		rules = []KMRule{}
	}
	return rules, nil
}

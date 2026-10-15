package acquiring

// Эквайринг старой схемы (требования до 4.3, БПО УТ 10.3 — ПодключаемоеОборудованиеУниверсальныйДрайверКлиент,
// docs/bpo-requirements/versions.md): операции с картой — позиционными параметрами, без XML.
// Модель переводит их в операции 4.3+ (Pay, ReturnPayment…) и раскладывает ответ по старым позициям,
// так что поведение (экран, отказы, сценарии, пакет дня) общее.

import (
	"context"
	"fmt"
	"html"

	"emulkon/internal/models"
	"emulkon/internal/models/kkt"
)

// legacyMethods — старое имя (английское, как в БПО: AuthorizeSales…) → операция 4.3+.
var legacyMethods = map[string]string{
	"AuthorizeSales":        "Pay",
	"AuthorizeRefund":       "ReturnPayment",
	"AuthorizeVoid":         "CancelPayment",
	"AuthorizePreSales":     "Authorisation",
	"AuthorizeVoidPreSales": "CancelAuthorisation",
	"AuthorizeCompletion":   "AuthConfirmation",
}

// legacyOperation — Метод(0 DeviceID, 1 CardNumber in/out, 2 Amount, 3 ReceiptNumber in/out,
// 4 RRNCode in/out, 5 AuthorizationCode in/out, 6 Slip out).
func (m *Model) legacyOperation(ctx context.Context, method string, params []any) models.Result {
	a := models.Args(params)
	out := models.CopyParams(params)
	if err := a.Need(7); err != nil {
		te := fail(ErrBadParams, "%v", err)
		return models.Result{Return: false, ErrorCode: te.code, ErrorText: te.Error()}
	}
	amount, err := a.Float(2)
	if err != nil {
		te := fail(ErrBadParams, "сумма (Amount): %v", err)
		return models.Result{Return: false, ErrorCode: te.code, ErrorText: te.Error()}
	}
	card, _ := a.String(1)
	receipt, _ := a.String(3)
	rrn, _ := a.String(4)
	auth, _ := a.String(5)
	sum := fmt.Sprintf("%.2f", amount)
	in := fmt.Sprintf(`<?xml version="1.0" encoding="UTF-8"?><OperationParameters MerchantNumber="0" UseBiometrics="0"`+
		` AuthorizationType="0" Amount="%s" AmountCash="0" AmountOriginalTransaction="0" ElectronicCertificateAmount="0"`+
		` ElectronicCertificateDiscount="0" OwnFundsAmount="%s" CardNumber="%s" ReceiptNumber="%s" RRNCode="%s"`+
		` AuthorizationCode="%s"/>`, sum, sum, html.EscapeString(card), html.EscapeString(receipt),
		html.EscapeString(rrn), html.EscapeString(auth))

	r := m.Handle(ctx, legacyMethods[method], []any{params[0], in, "", ""})
	if len(r.Params) > 3 {
		out[6] = r.Params[3] // слип и при отказе
	}
	if ok, _ := r.Return.(bool); !ok {
		return models.Result{Return: false, Params: out, ErrorCode: r.ErrorCode, ErrorText: r.ErrorText}
	}
	var res OperationOut
	if s, ok := r.Params[2].(string); ok {
		if err := kkt.Unmarshal(s, &res); err != nil {
			te := fail(ErrBadParams, "ответ операции: %v", err)
			return models.Result{Return: false, Params: out, ErrorCode: te.code, ErrorText: te.Error()}
		}
	}
	out[1], out[3], out[4], out[5] = res.CardNumber, res.ReceiptNumber, res.RRNCode, res.AuthorizationCode
	return models.Result{Return: true, Params: out}
}

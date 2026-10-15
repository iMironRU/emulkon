// Package acquiring — модель эквайрингового терминала (раздел 3.9 требований к драйверам).
//
// XML-структуры обмена с БПО — docs/bpo-requirements/pos-terminal.md. Значения (булево,
// числа, даты) пишутся по тем же правилам, что у ККТ, поэтому типы взяты из пакета kkt.
package acquiring

import (
	"encoding/xml"

	"emulkon/internal/models/kkt"
)

// TerminalParameters — ПараметрыТерминала (out). БПО сравнивает флаги через ВРег(…) = "TRUE".
type TerminalParameters struct {
	XMLName                               xml.Name `xml:"TerminalParameters"`
	TerminalID                            string   `xml:"TerminalID,attr"`
	MerchantID                            string   `xml:"MerchantID,attr"`
	AcquiringBankIdentifier               string   `xml:"AcquiringBankIdentifier,attr"`
	PrintSlipOnTerminal                   kkt.Bool `xml:"PrintSlipOnTerminal,attr"`
	ShortSlip                             kkt.Bool `xml:"ShortSlip,attr"`
	CashWithdrawal                        kkt.Bool `xml:"CashWithdrawal,attr"`
	ElectronicCertificates                kkt.Bool `xml:"ElectronicCertificates,attr"`
	ElectronicCertificateDiscount         kkt.Bool `xml:"ElectronicCertificateDiscount,attr"`
	PartialCancellation                   kkt.Bool `xml:"PartialCancellation,attr"`
	ConsumerPresentedQR                   kkt.Bool `xml:"ConsumerPresentedQR,attr"`
	ListCardTransactions                  kkt.Bool `xml:"ListCardTransactions,attr"`
	ReturnElectronicCertificateByBasketID kkt.Bool `xml:"ReturnElectronicCertificateByBasketID,attr"`
	PurchaseWithEnrollment                kkt.Bool `xml:"PurchaseWithEnrollment,attr"`
	OperationStatus                       kkt.Bool `xml:"OperationStatus,attr"`
}

// OperationIn — входные OperationParameters, как их собирает БПО
// (ОборудованиеПлатежныеСистемыКлиентСервер.ПолучитьXMLПакетДляОперации). Корень не проверяется.
type OperationIn struct {
	RequestID                     string     `xml:"RequestID,attr"`
	MerchantNumber                kkt.Number `xml:"MerchantNumber,attr"`
	ConsumerPresentedQR           string     `xml:"ConsumerPresentedQR,attr"`
	UseBiometrics                 kkt.Number `xml:"UseBiometrics,attr"`
	AuthorizationType             kkt.Number `xml:"AuthorizationType,attr"`
	Amount                        kkt.Number `xml:"Amount,attr"`
	AmountCash                    kkt.Number `xml:"AmountCash,attr"`
	AmountOriginalTransaction     kkt.Number `xml:"AmountOriginalTransaction,attr"`
	CardNumber                    string     `xml:"CardNumber,attr"`
	CardNumberHash                string     `xml:"CardNumberHash,attr"`
	BasketID                      string     `xml:"BasketID,attr"`
	ElectronicCertificateAmount   kkt.Number `xml:"ElectronicCertificateAmount,attr"`
	ElectronicCertificateDiscount kkt.Number `xml:"ElectronicCertificateDiscount,attr"`
	OwnFundsAmount                kkt.Number `xml:"OwnFundsAmount,attr"`
	ReceiptNumber                 string     `xml:"ReceiptNumber,attr"`
	RRNCode                       string     `xml:"RRNCode,attr"`
	AuthorizationCode             string     `xml:"AuthorizationCode,attr"`
}

// OperationOut — выходные OperationParameters. OperationStatus и ResultCode пишутся
// всегда: БПО читает их через Число() без проверки.
type OperationOut struct {
	XMLName                 xml.Name     `xml:"OperationParameters"`
	RequestID               string       `xml:"RequestID,attr,omitempty"`
	MerchantNumber          int          `xml:"MerchantNumber,attr"`
	Amount                  kkt.Number   `xml:"Amount,attr"`
	AmountCash              kkt.Number   `xml:"AmountCash,attr"`
	OperationStatus         int          `xml:"OperationStatus,attr"`
	AuthorizationType       int          `xml:"AuthorizationType,attr"`
	CardNumber              string       `xml:"CardNumber,attr"`
	CardNumberHash          string       `xml:"CardNumberHash,attr,omitempty"`
	ReceiptNumber           string       `xml:"ReceiptNumber,attr"`
	RRNCode                 string       `xml:"RRNCode,attr"`
	AuthorizationCode       string       `xml:"AuthorizationCode,attr"`
	Date                    kkt.DateTime `xml:"Date,attr"`
	PaymentSystemIdentifier string       `xml:"PaymentSystemIdentifier,attr,omitempty"`
	IssuerBankIdentifier    string       `xml:"IssuerBankIdentifier,attr,omitempty"`
	BIKAcquiring            string       `xml:"BIKAcquiring,attr,omitempty"`
	BIKPay                  string       `xml:"BIKPay,attr,omitempty"`
	UUID                    string       `xml:"UUID,attr,omitempty"`
	QRTransactionIdentifier string       `xml:"QRTransactionIdentifier,attr,omitempty"`
	ResultCode              int          `xml:"ResultCode,attr"`
}

// TableOperations — список операций дня (GetOperationByCards).
type TableOperations struct {
	XMLName xml.Name          `xml:"Table"`
	Records []OperationRecord `xml:"Record"`
}

type OperationRecord struct {
	MerchantNumber    int          `xml:"MerchantNumber,attr"`
	CardNumber        string       `xml:"CardNumber,attr"`
	CardNumberHash    string       `xml:"CardNumberHash,attr,omitempty"`
	Amount            kkt.Number   `xml:"Amount,attr"`
	AmountCash        kkt.Number   `xml:"AmountCash,attr"`
	TypeOperation     string       `xml:"TypeOperation,attr"`
	AuthorizationCode string       `xml:"AuthorizationCode,attr"`
	RRNCode           string       `xml:"RRNCode,attr"`
	DateTime          kkt.DateTime `xml:"DateTime,attr"`
}

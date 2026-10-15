package kkt

import (
	"encoding/xml"
	"fmt"
	"strings"
)

// Marshal сериализует структуру в XML-строку с объявлением UTF-8 — так, как её ждёт БПО.
func Marshal(v any) (string, error) {
	b, err := xml.Marshal(v)
	if err != nil {
		return "", fmt.Errorf("сформировать XML: %w", err)
	}
	return xml.Header + string(b), nil
}

// Unmarshal разбирает XML-строку от БПО. Пустая строка — ошибка: структура обязательна.
func Unmarshal(s string, v any) error {
	if strings.TrimSpace(s) == "" {
		return fmt.Errorf("пустой XML-пакет")
	}
	if err := xml.Unmarshal([]byte(s), v); err != nil {
		return fmt.Errorf("разобрать XML-пакет: %w", err)
	}
	return nil
}

// --- регистрация ККТ ---

// Registration — регистрационные данные ККТ: атрибуты TableParametersKKT
// и основа ParametersFiscal. Включены и атрибуты, которые БПО читает и пишет
// сверх требований: KKTModelID, IsOnlineOnly.
type Registration struct {
	KKTNumber          string   `xml:"KKTNumber,attr,omitempty"`
	KKTSerialNumber    string   `xml:"KKTSerialNumber,attr,omitempty"`
	KKTModelID         string   `xml:"KKTModelID,attr,omitempty"`
	FirmwareVersion    string   `xml:"FirmwareVersion,attr,omitempty"`
	Fiscal             Bool     `xml:"Fiscal,attr"`
	SupportFNPiot      Bool     `xml:"SupportFNPiot,attr"`
	FFDVersionFN       string   `xml:"FFDVersionFN,attr,omitempty"`
	FFDVersionKKT      string   `xml:"FFDVersionKKT,attr,omitempty"`
	FNSerialNumber     string   `xml:"FNSerialNumber,attr,omitempty"`
	DocumentNumber     string   `xml:"DocumentNumber,attr,omitempty"`
	DateTime           DateTime `xml:"DateTime,attr"`
	CompanyName        string   `xml:"CompanyName,attr,omitempty"`
	INN                string   `xml:"INN,attr,omitempty"`
	SaleAddress        string   `xml:"SaleAddress,attr,omitempty"`
	SaleLocation       string   `xml:"SaleLocation,attr,omitempty"`
	TaxationSystems    string   `xml:"TaxationSystems,attr,omitempty"` // коды через «,»
	IsOffline          Bool     `xml:"IsOffline,attr"`
	IsEncrypted        Bool     `xml:"IsEncrypted,attr"`
	IsService          Bool     `xml:"IsService,attr"`
	IsExcisable        Bool     `xml:"IsExcisable,attr"`
	IsGambling         Bool     `xml:"IsGambling,attr"`
	IsLottery          Bool     `xml:"IsLottery,attr"`
	BSOSing            Bool     `xml:"BSOSing,attr"`
	IsOnline           Bool     `xml:"IsOnline,attr"`
	IsOnlineOnly       Bool     `xml:"IsOnlineOnly,attr"`
	IsAutomaticPrinter Bool     `xml:"IsAutomaticPrinter,attr"`
	IsAutomatic        Bool     `xml:"IsAutomatic,attr"`
	IsMarking          Bool     `xml:"IsMarking,attr"`
	IsPawnshop         Bool     `xml:"IsPawnshop,attr"`
	IsAssurance        Bool     `xml:"IsAssurance,attr"`
	IsVendingMachine   Bool     `xml:"IsVendingMachine,attr"`
	IsCateringServices Bool     `xml:"IsCateringServices,attr"`
	IsWholesaleTrade   Bool     `xml:"IsWholesaleTrade,attr"`
	AgentTypes         string   `xml:"AgentTypes,attr,omitempty"` // коды через «,»
	AutomaticNumber    string   `xml:"AutomaticNumber,attr,omitempty"`
	OFDCompany         string   `xml:"OFDCompany,attr,omitempty"`
	OFDCompanyINN      string   `xml:"OFDCompanyINN,attr,omitempty"`
	FNSURL             string   `xml:"FNSURL,attr,omitempty"`
	SenderEmail        string   `xml:"SenderEmail,attr,omitempty"`
}

// TableParametersKKT — параметры ККТ (GetDataKKT, out). Атрибуты на корне.
type TableParametersKKT struct {
	XMLName xml.Name `xml:"TableParametersKKT"`
	Registration
}

// ParametersFiscal — параметры регистрации ФН (OperationFN, in). Атрибуты на корне.
type ParametersFiscal struct {
	XMLName xml.Name `xml:"ParametersFiscal"`
	Registration
	CashierName            string `xml:"CashierName,attr"`
	CashierINN             string `xml:"CashierINN,attr"`
	FFDVersion             string `xml:"FFDVersion,attr"`
	RegistrationLabelCodes string `xml:"RegistrationLabelCodes,attr"`
	RegistrationReasonCode string `xml:"RegistrationReasonCode,attr"` // ревизии < 3004
}

// --- смена ---

// InputParameters — параметры операции (OpenShift, CloseShift, PrintXReport, …; in).
type InputParameters struct {
	XMLName    xml.Name        `xml:"InputParameters"`
	Parameters OperationParams `xml:"Parameters"`
}

// OperationParams — атрибуты InputParameters/Parameters.
type OperationParams struct {
	CashierName         string `xml:"CashierName,attr"`
	CashierINN          string `xml:"CashierINN,attr"`
	SaleAddress         string `xml:"SaleAddress,attr"`
	SaleLocation        string `xml:"SaleLocation,attr"`
	PrintRequired       *Bool  `xml:"PrintRequired,attr"`       // нет — по умолчанию печатать
	TimeSynchronization Bool   `xml:"TimeSynchronization,attr"` // OpenShift: синхронизировать время ККТ с ПК
}

// ShiftStates — ShiftState в OutputParameters.
const (
	ShiftClosed  = 1
	ShiftOpen    = 2
	ShiftExpired = 3
)

// OutputParameters — состояние смены (OpenShift, CloseShift, GetCurrentStatus,
// ReportCurrentStatusOfSettlements; out).
type OutputParameters struct {
	XMLName    xml.Name    `xml:"OutputParameters"`
	Parameters ShiftStatus `xml:"Parameters"`
}

// ShiftStatus — атрибуты OutputParameters/Parameters. Необязательные поля —
// указатели: nil не пишется, пустых числовых атрибутов не бывает.
type ShiftStatus struct {
	ShiftNumber                  *int     `xml:"ShiftNumber,attr,omitempty"`
	CheckNumber                  *int     `xml:"CheckNumber,attr,omitempty"`
	ShiftClosingCheckNumber      *int     `xml:"ShiftClosingCheckNumber,attr,omitempty"`
	DateTime                     DateTime `xml:"DateTime,attr"`
	ShiftState                   int      `xml:"ShiftState,attr"` // всегда: БПО делает Число() без проверки
	DocumentsCounter             *int     `xml:"DocumentsCounter,attr,omitempty"`
	CashBalance                  *Number  `xml:"CashBalance,attr,omitempty"`
	BacklogDocumentsCounter      *int     `xml:"BacklogDocumentsCounter,attr,omitempty"`
	BacklogDocumentFirstNumber   *int     `xml:"BacklogDocumentFirstNumber,attr,omitempty"`
	BacklogDocumentFirstDateTime DateTime `xml:"BacklogDocumentFirstDateTime,attr"`
	FNError                      *Bool    `xml:"FNError,attr,omitempty"`
	FNOverflow                   *Bool    `xml:"FNOverflow,attr,omitempty"`
	FNFail                       *Bool    `xml:"FNFail,attr,omitempty"`
	FNValidityDate               DateTime `xml:"FNValidityDate,attr"`

	// Счётчики по типам операций (CloseShift). Вложение дочерними элементами —
	// наше прочтение требований, БПО их не читает (docs/bpo-requirements/kkt-xml.md).
	CountersOperationType1 *OperationCounters `xml:"CountersOperationType1,omitempty"`
	CountersOperationType2 *OperationCounters `xml:"CountersOperationType2,omitempty"`
	CountersOperationType3 *OperationCounters `xml:"CountersOperationType3,omitempty"`
	CountersOperationType4 *OperationCounters `xml:"CountersOperationType4,omitempty"`
}

// OperationCounters — счётчики операций одного типа.
type OperationCounters struct {
	CheckCount                  int    `xml:"CheckCount,attr"`
	TotalChecksAmount           Number `xml:"TotalChecksAmount,attr"`
	CorrectionCheckCount        int    `xml:"CorrectionCheckCount,attr"`
	TotalCorrectionChecksAmount Number `xml:"TotalCorrectionChecksAmount,attr"`
}

// --- чек ---

// Типы операций чека (OperationType, ФФД табл. 25).
const (
	OpIncome        = 1 // приход
	OpIncomeReturn  = 2 // возврат прихода
	OpExpense       = 3 // расход
	OpExpenseReturn = 4 // возврат расхода
)

// CheckPackage — чек или чек коррекции (ProcessCheck, ProcessCorrectionCheck; in).
// Только разбор: формирует пакет 1С.
type CheckPackage struct {
	XMLName               xml.Name            `xml:"CheckPackage"`
	Parameters            CheckParameters     `xml:"Parameters"`
	Positions             Positions           `xml:"Positions"`
	Payments              Payments            `xml:"Payments"`
	ElectronicPaymentInfo []ElectronicPayment `xml:"ElectronicPaymentInfo>ElectronicPayment"`
}

// CheckParameters — CheckPackage/Parameters.
type CheckParameters struct {
	CashierName                   string `xml:"CashierName,attr"`
	CashierINN                    string `xml:"CashierINN,attr"`
	OperationType                 int    `xml:"OperationType,attr"`
	TaxationSystem                int    `xml:"TaxationSystem,attr"`
	CustomerEmail                 string `xml:"CustomerEmail,attr"`
	CustomerPhone                 string `xml:"CustomerPhone,attr"`
	SenderEmail                   string `xml:"SenderEmail,attr"`
	SaleAddress                   string `xml:"SaleAddress,attr"`
	SaleLocation                  string `xml:"SaleLocation,attr"`
	AutomatNumber                 string `xml:"AutomatNumber,attr"`
	AgentType                     *int   `xml:"AgentType,attr"`
	AdditionalAttribute           string `xml:"AdditionalAttribute,attr"`
	OperationOnline               Bool   `xml:"OperationOnline,attr"`
	TimeZone                      *int   `xml:"TimeZone,attr"`
	GroupingPositionsWhenPrinting Bool   `xml:"GroupingPositionsWhenPrinting,attr"`

	CorrectionData       *CorrectionData       `xml:"CorrectionData"`
	CustomerDetail       *CustomerDetail       `xml:"CustomerDetail"`
	AgentData            *AgentData            `xml:"AgentData"`
	VendorData           *VendorData           `xml:"VendorData"`
	UserAttribute        *UserAttribute        `xml:"UserAttribute"`
	OperationalAttribute *OperationalAttribute `xml:"OperationalAttribute"`
	IndustryAttribute    *IndustryAttribute    `xml:"IndustryAttribute"`
}

// CorrectionData — данные коррекции (только чек коррекции).
// В требованиях имя «Datе» с кириллической «е» — опечатка; БПО пишет Date.
type CorrectionData struct {
	Type        int      `xml:"Type,attr"` // 0 — самостоятельно, 1 — по предписанию
	Date        DateTime `xml:"Date,attr"`
	Number      string   `xml:"Number,attr"`
	Description string   `xml:"Description,attr"`
}

// CustomerDetail — сведения о покупателе.
type CustomerDetail struct {
	Info             string `xml:"Info,attr"`
	INN              string `xml:"INN,attr"`
	DateOfBirth      string `xml:"DateOfBirth,attr"` // DD.MM.YYYY
	Citizenship      string `xml:"Citizenship,attr"`
	DocumentTypeCode string `xml:"DocumentTypeCode,attr"`
	DocumentData     string `xml:"DocumentData,attr"`
	Address          string `xml:"Address,attr"`
}

// AgentData — данные агента. Пустой элемент — данных нет.
type AgentData struct {
	AgentOperation          string `xml:"AgentOperation,attr"`
	AgentPhone              string `xml:"AgentPhone,attr"`
	PaymentProcessorPhone   string `xml:"PaymentProcessorPhone,attr"`
	AcquirerOperatorPhone   string `xml:"AcquirerOperatorPhone,attr"`
	AcquirerOperatorName    string `xml:"AcquirerOperatorName,attr"`
	AcquirerOperatorAddress string `xml:"AcquirerOperatorAddress,attr"`
	AcquirerOperatorINN     string `xml:"AcquirerOperatorINN,attr"`
}

// VendorData — данные поставщика. Пустой элемент — данных нет.
type VendorData struct {
	VendorPhone string `xml:"VendorPhone,attr"`
	VendorName  string `xml:"VendorName,attr"`
	VendorINN   string `xml:"VendorINN,attr"`
}

// UserAttribute — дополнительный реквизит пользователя.
type UserAttribute struct {
	Name  string `xml:"Name,attr"`
	Value string `xml:"Value,attr"`
}

// OperationalAttribute — операционный реквизит чека.
type OperationalAttribute struct {
	DateTime      DateTime `xml:"DateTime,attr"`
	OperationID   string   `xml:"OperationID,attr"`
	OperationData string   `xml:"OperationData,attr"`
}

// IndustryAttribute — отраслевой реквизит.
type IndustryAttribute struct {
	IdentifierFOIV string `xml:"IdentifierFOIV,attr"`
	DocumentDate   string `xml:"DocumentDate,attr"` // DD.MM.YYYY
	DocumentNumber string `xml:"DocumentNumber,attr"`
	AttributeValue string `xml:"AttributeValue,attr"`
}

// Position — элемент Positions: ровно одно поле не nil. Порядок позиций — порядок печати.
type Position struct {
	Fiscal  *FiscalString
	Text    *TextString
	Barcode *Barcode
}

// Positions — CheckPackage/Positions и Document/Positions с сохранением порядка
// разнотипных элементов (FiscalString, TextString, Barcode).
type Positions struct {
	Items []Position
}

func (p *Positions) UnmarshalXML(d *xml.Decoder, start xml.StartElement) error {
	for {
		tok, err := d.Token()
		if err != nil {
			return err
		}
		switch t := tok.(type) {
		case xml.StartElement:
			var pos Position
			switch t.Name.Local {
			case "FiscalString":
				pos.Fiscal = new(FiscalString)
				err = d.DecodeElement(pos.Fiscal, &t)
			case "TextString":
				pos.Text = new(TextString)
				err = d.DecodeElement(pos.Text, &t)
			case "Barcode":
				pos.Barcode = new(Barcode)
				err = d.DecodeElement(pos.Barcode, &t)
			default:
				err = d.Skip() // неизвестный элемент — игнорируем
				if err == nil {
					continue
				}
			}
			if err != nil {
				return fmt.Errorf("позиция %d (%s): %w", len(p.Items)+1, t.Name.Local, err)
			}
			p.Items = append(p.Items, pos)
		case xml.EndElement:
			return nil
		}
	}
}

func (p Positions) MarshalXML(e *xml.Encoder, start xml.StartElement) error {
	if err := e.EncodeToken(start); err != nil {
		return err
	}
	for _, pos := range p.Items {
		var err error
		switch {
		case pos.Fiscal != nil:
			err = e.EncodeElement(pos.Fiscal, xml.StartElement{Name: xml.Name{Local: "FiscalString"}})
		case pos.Text != nil:
			err = e.EncodeElement(pos.Text, xml.StartElement{Name: xml.Name{Local: "TextString"}})
		case pos.Barcode != nil:
			err = e.EncodeElement(pos.Barcode, xml.StartElement{Name: xml.Name{Local: "Barcode"}})
		}
		if err != nil {
			return err
		}
	}
	return e.EncodeToken(start.End())
}

// FiscalString — фискальная строка (предмет расчёта).
type FiscalString struct {
	Name                string  `xml:"Name,attr"`
	Quantity            Number  `xml:"Quantity,attr"`
	PriceWithDiscount   Number  `xml:"PriceWithDiscount,attr"`
	AmountWithDiscount  Number  `xml:"AmountWithDiscount,attr"`
	DiscountAmount      Number  `xml:"DiscountAmount,attr"`
	Department          *int    `xml:"Department,attr"`
	VATRate             string  `xml:"VATRate,attr"`
	VATAmount           *Number `xml:"VATAmount,attr"`
	PaymentMethod       *int    `xml:"PaymentMethod,attr"`
	CalculationSubject  *int    `xml:"CalculationSubject,attr"`
	CalculationAgent    *int    `xml:"CalculationAgent,attr"`
	MeasureOfQuantity   string  `xml:"MeasureOfQuantity,attr"` // код ФФД табл. 114
	MarkingCode         string  `xml:"MarkingCode,attr"`       // Base64
	CountryOfOrigin     string  `xml:"CountryOfOrigin,attr"`
	CustomsDeclaration  string  `xml:"CustomsDeclaration,attr"`
	AdditionalAttribute string  `xml:"AdditionalAttribute,attr"`
	ExciseAmount        *Number `xml:"ExciseAmount,attr"`

	AgentData          *AgentData          `xml:"AgentData"`
	VendorData         *VendorData         `xml:"VendorData"`
	FractionalQuantity *FractionalQuantity `xml:"FractionalQuantity"`
	GoodCodeData       *GoodCodeData       `xml:"GoodCodeData"`
	IndustryAttribute  *IndustryAttribute  `xml:"IndustryAttribute"`
}

// FractionalQuantity — дробное количество маркированного товара.
type FractionalQuantity struct {
	Numerator   int `xml:"Numerator,attr"`
	Denominator int `xml:"Denominator,attr"`
}

// GoodCodeData — код товара; все значения в Base64, кроме MI (КИЗ мехового изделия).
type GoodCodeData struct {
	NotIdentified string `xml:"NotIdentified,attr"`
	EAN8          string `xml:"EAN8,attr"`
	EAN13         string `xml:"EAN13,attr"`
	ITF14         string `xml:"ITF14,attr"`
	GS10          string `xml:"GS1.0,attr"`
	GS1M          string `xml:"GS1.M,attr"`
	KMK           string `xml:"KMK,attr"`
	MI            string `xml:"MI,attr"`
	EGAIS20       string `xml:"EGAIS20,attr"`
	EGAIS30       string `xml:"EGAIS30,attr"`
	F1            string `xml:"F1,attr"`
	F2            string `xml:"F2,attr"`
	F3            string `xml:"F3,attr"`
	F4            string `xml:"F4,attr"`
	F5            string `xml:"F5,attr"`
	F6            string `xml:"F6,attr"`
}

// TextString — нефискальная текстовая строка.
type TextString struct {
	Text string `xml:"Text,attr"`
}

// Barcode — штрихкод для печати. Type: EAN8, EAN13, CODE39, QR.
type Barcode struct {
	Type        string `xml:"Type,attr"`
	ValueBase64 string `xml:"ValueBase64,attr"`
	// Старые ревизии драйвера (docs/bpo-requirements/printers.md): 3000–3002 — Value,
	// до 3000 — BarcodeType и Barcode. Эмулятор объявляет 4008, БПО их не шлёт.
	Value       string `xml:"Value,attr,omitempty"`
	LegacyType  string `xml:"BarcodeType,attr,omitempty"`
	LegacyValue string `xml:"Barcode,attr,omitempty"`
}

// BarcodeTypes — типы штрихкодов из требований.
var BarcodeTypes = []string{"EAN8", "EAN13", "CODE39", "QR"}

// Payments — суммы оплат по видам.
type Payments struct {
	Cash              Number `xml:"Cash,attr"`
	ElectronicPayment Number `xml:"ElectronicPayment,attr"`
	PrePayment        Number `xml:"PrePayment,attr"`
	PostPayment       Number `xml:"PostPayment,attr"`
	Barter            Number `xml:"Barter,attr"`
}

// Total — сумма всех видов оплат, до копеек.
func (p Payments) Total() float64 {
	return Kopecks(float64(p.Cash + p.ElectronicPayment + p.PrePayment + p.PostPayment + p.Barter))
}

// ElectronicPayment — сведения об одной безналичной оплате (теги 1082, 1236–1238).
type ElectronicPayment struct {
	Amount                Number `xml:"Amount,attr"`
	PaymentMethod         int    `xml:"PaymentMethod,attr"` // 1 — НПС, 2 — МПС, 3 — СБП, 6 — прочее
	Identifiers           string `xml:"Identifiers,attr"`
	AdditionalInformation string `xml:"AdditionalInformation,attr"`
}

// FiscalStrings — фискальные строки чека по порядку.
func (c *CheckPackage) FiscalStrings() []*FiscalString {
	var out []*FiscalString
	for _, p := range c.Positions.Items {
		if p.Fiscal != nil {
			out = append(out, p.Fiscal)
		}
	}
	return out
}

// Total — сумма чека: итог фискальных строк с учётом скидок, до копеек.
func (c *CheckPackage) Total() float64 {
	var sum float64
	for _, f := range c.FiscalStrings() {
		sum += float64(f.AmountWithDiscount)
	}
	return Kopecks(sum)
}

// DocumentOutputParameters — результат фискализации чека (ProcessCheck,
// ProcessCorrectionCheck; out).
type DocumentOutputParameters struct {
	XMLName    xml.Name       `xml:"DocumentOutputParameters"`
	Parameters DocumentResult `xml:"Parameters"`
}

// DocumentResult — атрибуты DocumentOutputParameters/Parameters.
type DocumentResult struct {
	ShiftNumber             int      `xml:"ShiftNumber,attr"`
	CheckNumber             int      `xml:"CheckNumber,attr"`
	ShiftClosingCheckNumber int      `xml:"ShiftClosingCheckNumber,attr"`
	AddressSiteInspections  string   `xml:"AddressSiteInspections,attr"`
	FiscalSign              string   `xml:"FiscalSign,attr"`
	DateTime                DateTime `xml:"DateTime,attr"`
	MTNumber                *int     `xml:"MTNumber,attr,omitempty"`
	PrintError              *Bool    `xml:"PrintError,attr,omitempty"`
}

// --- нефискальный документ ---

// DocumentPackage — текстовый документ (PrintTextDocument, in). Корень — Document.
type DocumentPackage struct {
	XMLName   xml.Name  `xml:"Document"`
	Positions Positions `xml:"Positions"`
}

// --- маркировка ---

// RequestKM — запрос на проверку КМ (RequestKM, in). Атрибуты на корне.
type RequestKM struct {
	XMLName            xml.Name            `xml:"RequestKM"`
	GUID               string              `xml:"GUID,attr"`
	WaitForResult      *Bool               `xml:"WaitForResult,attr"`
	NotSendToServer    Bool                `xml:"NotSendToServer,attr"`
	MarkingCode        string              `xml:"MarkingCode,attr"` // Base64
	PlannedStatus      int                 `xml:"PlannedStatus,attr"`
	Quantity           *Number             `xml:"Quantity,attr"`
	MeasureOfQuantity  string              `xml:"MeasureOfQuantity,attr"`
	FractionalQuantity *FractionalQuantity `xml:"FractionalQuantity"`
}

// RequestKMResult — результат локальной проверки КМ фискальным накопителем (RequestKM, out).
type RequestKMResult struct {
	XMLName        xml.Name `xml:"RequestKMResult"`
	Checking       Bool     `xml:"Checking,attr"`
	CheckingResult Bool     `xml:"CheckingResult,attr"`
}

// ProcessingKMResult — ответ ОИСМ о КМ (GetProcessingKMResult, out).
type ProcessingKMResult struct {
	XMLName    xml.Name `xml:"ProcessingKMResult"`
	GUID       string   `xml:"GUID,attr"`
	Result     Bool     `xml:"Result,attr"`
	ResultCode int      `xml:"ResultCode,attr"` // тег 2106, битовая маска
	StatusInfo *int     `xml:"StatusInfo,attr,omitempty"`
	HandleCode int      `xml:"HandleCode,attr"`
}

// RequestStatus — значения RequestStatus метода GetProcessingKMResult.
const (
	KMResultReady       = 0 // результат получен
	KMResultPending     = 1 // ещё не получен
	KMResultUnavailable = 2 // не может быть получен
)

package kkt

import (
	"encoding/base64"
	"encoding/xml"
	"strings"
	"testing"
	"time"
)

// Пакет чека в том виде, в каком его пишет БПО 3.3.4.30 при ревизии 4008
// (ОборудованиеЧекопечатающиеУстройства.СформироватьXMLПакетДляФискализацияЧека):
// булево из XMLСтрока, пустые AgentData/VendorData, ElectronicPaymentInfo.
// Плюс неизвестные атрибут и элемент — их надо игнорировать.
const bpoCheckPackage = `<?xml version="1.0" encoding="UTF-8"?>
<CheckPackage>
<Parameters CashierName="Иванов И.И., кассир" CashierINN="500100732259" OperationType="1" TaxationSystem="0"
  SaleAddress="г. Москва, ул. Тестовая, 1" SaleLocation="Касса 1" GroupingPositionsWhenPrinting="false"
  CustomerEmail="" CustomerPhone="+79990000000" TimeZone="2" CustomerInfo="устаревший атрибут">
 <AgentData/>
 <VendorData/>
 <OperationalAttribute DateTime="2026-10-08T12:00:00" OperationID="0" OperationData="DATA"/>
</Parameters>
<Positions>
 <TextString Text="ТЕСТОВЫЙ ЧЕК — ЭМУЛЯТОР"/>
 <FiscalString Name="Молоко 3,2%" Quantity="2" PriceWithDiscount="89.9" AmountWithDiscount="179.8"
   DiscountAmount="0" Department="1" VATRate="10" VATAmount="16.35" PaymentMethod="4" CalculationSubject="33"
   MeasureOfQuantity="0" MarkingCode="MDEwNDYwNzQyODc5Mzk1NjIxNWQ=">
  <AgentData/>
  <VendorData/>
  <FractionalQuantity Numerator="1" Denominator="2"/>
  <GoodCodeData GS1.M="MDEwNDYwNzQyODc5Mzk1Ng==" EAN13="NDYwNzQyODc5Mzk1Ng=="/>
  <IndustryAttribute IdentifierFOIV="030" DocumentDate="21.11.2023" DocumentNumber="1944" AttributeValue="UUID=x&amp;Time=1"/>
 </FiscalString>
 <Unknown Foo="bar"/>
 <FiscalString Name="Пакет" Quantity="1" PriceWithDiscount="5" AmountWithDiscount="5" DiscountAmount="0"
   VATRate="none" PaymentMethod="4" CalculationSubject="1"/>
 <Barcode Type="QR" ValueBase64="aHR0cHM6Ly9leGFtcGxlLnRlc3Q="/>
</Positions>
<Payments Cash="100" ElectronicPayment="84.8" PrePayment="0" PostPayment="0" Barter="0"/>
<ElectronicPaymentInfo>
 <ElectronicPayment Amount="84.8" PaymentMethod="1" Identifiers="date=2026-10-08T12:00:00Z&amp;psid=22&amp;rrn=000000000001" AdditionalInformation="pc=1&amp;tips=false"/>
</ElectronicPaymentInfo>
</CheckPackage>`

func TestParseBPOCheckPackage(t *testing.T) {
	var c CheckPackage
	if err := Unmarshal(bpoCheckPackage, &c); err != nil {
		t.Fatal(err)
	}
	p := c.Parameters
	if p.CashierName != "Иванов И.И., кассир" || p.OperationType != OpIncome || p.TaxationSystem != 0 {
		t.Fatalf("параметры чека: %+v", p)
	}
	if p.TimeZone == nil || *p.TimeZone != 2 || p.AgentType != nil {
		t.Fatalf("необязательные числа: TimeZone=%v AgentType=%v", p.TimeZone, p.AgentType)
	}
	if p.AgentData == nil || *p.AgentData != (AgentData{}) {
		t.Fatalf("пустой AgentData должен разбираться в пустую структуру: %+v", p.AgentData)
	}
	if p.OperationalAttribute == nil || p.OperationalAttribute.DateTime.Hour() != 12 {
		t.Fatalf("операционный реквизит: %+v", p.OperationalAttribute)
	}

	// Порядок позиций сохраняется, неизвестный элемент пропущен.
	kinds := ""
	for _, it := range c.Positions.Items {
		switch {
		case it.Fiscal != nil:
			kinds += "F"
		case it.Text != nil:
			kinds += "T"
		case it.Barcode != nil:
			kinds += "B"
		}
	}
	if kinds != "TFFB" {
		t.Fatalf("порядок позиций: %s", kinds)
	}

	f := c.FiscalStrings()[0]
	if f.Name != "Молоко 3,2%" || f.Quantity != 2 || f.AmountWithDiscount != 179.8 || f.VATRate != "10" {
		t.Fatalf("фискальная строка: %+v", f)
	}
	if f.VATAmount == nil || *f.VATAmount != 16.35 || f.CalculationSubject == nil || *f.CalculationSubject != 33 {
		t.Fatalf("НДС/предмет расчёта: %+v", f)
	}
	if f.FractionalQuantity == nil || f.FractionalQuantity.Denominator != 2 {
		t.Fatalf("дробное количество: %+v", f.FractionalQuantity)
	}
	if f.GoodCodeData == nil || f.GoodCodeData.GS1M == "" || f.GoodCodeData.EAN13 == "" {
		t.Fatalf("код товара (атрибуты с точкой в имени): %+v", f.GoodCodeData)
	}
	if km, err := base64.StdEncoding.DecodeString(f.MarkingCode); err != nil || !strings.HasPrefix(string(km), "0104607") {
		t.Fatalf("КМ в Base64: %q %v", km, err)
	}
	if f.IndustryAttribute == nil || f.IndustryAttribute.AttributeValue != "UUID=x&Time=1" {
		t.Fatalf("отраслевой реквизит: %+v", f.IndustryAttribute)
	}
	if v := c.FiscalStrings()[1]; v.VATAmount != nil || v.VATRate != "none" {
		t.Fatalf("строка без НДС: %+v", v)
	}

	if c.Total() != 184.8 || c.Payments.Total() != 184.8 {
		t.Fatalf("сумма чека %v, оплаты %v", c.Total(), c.Payments.Total())
	}
	if len(c.ElectronicPaymentInfo) != 1 || c.ElectronicPaymentInfo[0].PaymentMethod != 1 ||
		!strings.Contains(c.ElectronicPaymentInfo[0].Identifiers, "psid=22") {
		t.Fatalf("сведения о безналичной оплате: %+v", c.ElectronicPaymentInfo)
	}
}

func TestBoolAttr(t *testing.T) {
	cases := map[string]bool{"true": true, "True": true, "TRUE": true, "1": true, "false": false, "False": false, "0": false, "": false}
	for in, want := range cases {
		var r RequestKMResult
		if err := Unmarshal(`<RequestKMResult Checking="`+in+`" CheckingResult="true"/>`, &r); err != nil {
			t.Fatalf("%q: %v", in, err)
		}
		if bool(r.Checking) != want {
			t.Fatalf("%q → %v, want %v", in, r.Checking, want)
		}
	}
	var r RequestKMResult
	if err := Unmarshal(`<RequestKMResult Checking="да"/>`, &r); err == nil {
		t.Fatal("нераспознанное булево должно быть ошибкой")
	}
}

// Выход: нет пустых числовых атрибутов, ShiftState всегда, даты без часового пояса,
// числа без экспоненты, булево true/false.
func TestMarshalOutputParameters(t *testing.T) {
	opened := time.Date(2026, 10, 8, 9, 0, 0, 0, time.Local)
	out := OutputParameters{Parameters: ShiftStatus{
		ShiftNumber:             IntPtr(12),
		DateTime:                DateTime{opened},
		ShiftState:              ShiftOpen,
		CashBalance:             NumberPtr(1000000),
		BacklogDocumentsCounter: IntPtr(0),
		FNError:                 BoolPtr(false),
		FNValidityDate:          DateTime{time.Date(2027, 10, 8, 0, 0, 0, 0, time.Local)},
	}}
	s, err := Marshal(out)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.HasPrefix(s, `<?xml version="1.0" encoding="UTF-8"?>`) {
		t.Fatalf("нет объявления XML: %s", s)
	}
	for _, want := range []string{
		`<OutputParameters><Parameters `,
		`ShiftNumber="12"`, `ShiftState="2"`, `DateTime="2026-10-08T09:00:00"`,
		`CashBalance="1000000"`, `BacklogDocumentsCounter="0"`, `FNError="false"`,
		`FNValidityDate="2027-10-08T00:00:00"`,
	} {
		if !strings.Contains(s, want) {
			t.Errorf("нет %s в %s", want, s)
		}
	}
	for _, absent := range []string{`=""`, `CheckNumber`, `FNOverflow`, `BacklogDocumentFirstDateTime`, `CountersOperationType`, `e+`} {
		if strings.Contains(s, absent) {
			t.Errorf("лишнее %s в %s", absent, s)
		}
	}

	// ShiftState пишется и нулевым — модель обязана его задать, но атрибут не пропадёт.
	s, _ = Marshal(OutputParameters{})
	if !strings.Contains(s, `ShiftState="0"`) {
		t.Fatalf("ShiftState должен присутствовать всегда: %s", s)
	}
}

func TestDocumentOutputParametersRoundTrip(t *testing.T) {
	in := DocumentOutputParameters{Parameters: DocumentResult{
		ShiftNumber: 2, CheckNumber: 5, ShiftClosingCheckNumber: 1,
		AddressSiteInspections: "www.nalog.gov.ru", FiscalSign: "2721313672",
		DateTime: DateTime{time.Date(2026, 10, 8, 12, 59, 0, 0, time.Local)},
	}}
	s, err := Marshal(in)
	if err != nil {
		t.Fatal(err)
	}
	if strings.Contains(s, "PrintError") || strings.Contains(s, "MTNumber") {
		t.Fatalf("необязательные атрибуты без значения: %s", s)
	}
	var back DocumentOutputParameters
	if err := Unmarshal(s, &back); err != nil {
		t.Fatal(err)
	}
	back.XMLName = xml.Name{}
	if back.Parameters.FiscalSign != "2721313672" || !back.Parameters.DateTime.Equal(in.Parameters.DateTime.Time) || back.Parameters.CheckNumber != 5 {
		t.Fatalf("после разбора: %+v", back)
	}

	in.Parameters.PrintError = BoolPtr(true)
	s, _ = Marshal(in)
	if !strings.Contains(s, `PrintError="true"`) {
		t.Fatalf("PrintError: %s", s)
	}
}

func TestTableParametersKKT(t *testing.T) {
	s, err := Marshal(TableParametersKKT{Registration: Registration{
		KKTNumber: "0000000001000001", KKTSerialNumber: "EMU0000001", KKTModelID: "EMULKON",
		Fiscal: true, FFDVersionFN: "1.2", FFDVersionKKT: "1.2", FNSerialNumber: "9999078900000001",
		TaxationSystems: "0,1", IsMarking: true, IsAutomatic: false,
	}})
	if err != nil {
		t.Fatal(err)
	}
	for _, want := range []string{`<TableParametersKKT `, `Fiscal="true"`, `IsMarking="true"`, `IsAutomatic="false"`, `TaxationSystems="0,1"`, `KKTModelID="EMULKON"`} {
		if !strings.Contains(s, want) {
			t.Errorf("нет %s в %s", want, s)
		}
	}
	if strings.Contains(s, `=""`) || strings.Contains(s, "<Parameters") {
		t.Fatalf("атрибуты должны быть на корне и без пустых значений: %s", s)
	}
}

func TestParseInputsLeniently(t *testing.T) {
	// InputParameters без кассира (БПО пишет CashierName только если он заполнен).
	var in InputParameters
	if err := Unmarshal(`<?xml version="1.0" encoding="UTF-8"?><InputParameters><Parameters PrintRequired="false" TimeSynchronization="true"/></InputParameters>`, &in); err != nil {
		t.Fatal(err)
	}
	if in.Parameters.CashierName != "" || in.Parameters.PrintRequired == nil || *in.Parameters.PrintRequired || !in.Parameters.TimeSynchronization {
		t.Fatalf("InputParameters: %+v", in.Parameters)
	}

	// ParametersFiscal так, как пишет БПО: без FFDVersion, с IsOnlineOnly и неизвестным атрибутом.
	var pf ParametersFiscal
	if err := Unmarshal(`<ParametersFiscal CashierName="Администратор" CashierINN="" RegistrationLabelCodes="1,2"
		KKTNumber="0000000001000001" CompanyName="ООО &quot;Тест&quot;" INN="7700000000" TaxationSystems="0"
		IsOnlineOnly="false" IsMarking="true" Foo="bar"/>`, &pf); err != nil {
		t.Fatal(err)
	}
	if pf.CashierName != "Администратор" || pf.CompanyName != `ООО "Тест"` || !bool(pf.IsMarking) || pf.FFDVersion != "" {
		t.Fatalf("ParametersFiscal: %+v", pf)
	}

	// RequestKM в форме БПО (МенеджерОборудованияМаркировкаСлужебныйКлиентСервер:20).
	var rq RequestKM
	if err := Unmarshal(`<RequestKM GUID="193DDFC8-15DF-4B14-AE84-1D5B741BC0AC" WaitForResult="True" NotSendToServer="False"
		MarkingCode="MDEwNDYw" PlannedStatus="1" Quantity="0.5" MeasureOfQuantity="0"><FractionalQuantity Numerator="1" Denominator="2"/></RequestKM>`, &rq); err != nil {
		t.Fatal(err)
	}
	if rq.WaitForResult == nil || !bool(*rq.WaitForResult) || bool(rq.NotSendToServer) || rq.PlannedStatus != 1 ||
		rq.Quantity == nil || *rq.Quantity != 0.5 || rq.FractionalQuantity == nil {
		t.Fatalf("RequestKM: %+v", rq)
	}

	// Нефискальный документ: корень Document.
	var doc DocumentPackage
	if err := Unmarshal(`<Document><Positions><TextString Text="Строка 1"/><Barcode Type="EAN13" ValueBase64="NDYwNzQyODc5Mzk1Ng=="/><TextString Text="Строка 2"/></Positions></Document>`, &doc); err != nil {
		t.Fatal(err)
	}
	if len(doc.Positions.Items) != 3 || doc.Positions.Items[1].Barcode == nil || doc.Positions.Items[2].Text.Text != "Строка 2" {
		t.Fatalf("DocumentPackage: %+v", doc.Positions.Items)
	}

	if err := Unmarshal("  ", &doc); err == nil {
		t.Fatal("пустой пакет должен быть ошибкой")
	}
	if err := Unmarshal(`<CheckPackage><Parameters OperationType="abc"/></CheckPackage>`, &CheckPackage{}); err == nil {
		t.Fatal("нечисловой OperationType должен быть ошибкой")
	}
}

func TestProcessingKMResultOmitsEmptyNumbers(t *testing.T) {
	s, _ := Marshal(ProcessingKMResult{GUID: "g", Result: true, ResultCode: 15, HandleCode: 0})
	if strings.Contains(s, "StatusInfo") || !strings.Contains(s, `ResultCode="15"`) || !strings.Contains(s, `HandleCode="0"`) {
		t.Fatalf("ProcessingKMResult: %s", s)
	}
}

// Сериализация позиций сохраняет порядок — для тестовых пакетов и отладки.
func TestPositionsMarshalOrder(t *testing.T) {
	doc := DocumentPackage{Positions: Positions{Items: []Position{
		{Text: &TextString{Text: "A"}}, {Barcode: &Barcode{Type: "QR", ValueBase64: "eA=="}}, {Text: &TextString{Text: "B"}},
	}}}
	s, err := Marshal(doc)
	if err != nil {
		t.Fatal(err)
	}
	want := `<Document><Positions><TextString Text="A"></TextString><Barcode Type="QR" ValueBase64="eA=="></Barcode><TextString Text="B"></TextString></Positions></Document>`
	if !strings.HasSuffix(s, want) {
		t.Fatalf("порядок позиций:\n%s\nwant suffix\n%s", s, want)
	}
}

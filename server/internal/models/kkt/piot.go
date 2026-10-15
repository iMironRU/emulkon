package kkt

// ТС ПИоТ — требования к драйверам 5.0, раздел ККТ (docs/models/kkt.md, «ТС ПИоТ»): ККТ передаёт
// данные ТС ПИоТ через ФН — аутентификацию (PiotSetChallenge, PiotCheckChallenge) и защищённый
// контейнер (PiotSetDKMsg, PiotGetDKMsg). Остальное делает компонента ТС ПИоТ из БПО.
//
// Криптографии ФН у эмулятора нет: ответы — детерминированные тестовые данные с меткой
// EMULKON в заголовке, ГИС МТ их не примет. Проверяются признак поддержки (piot), регистрация
// и форма входных данных — этого хватает, чтобы пройти цепочку вызовов 1С и её ошибки.

import (
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"

	"emulkon/internal/models"
)

const (
	piotHeaderLen  = 30   // заголовок сеансового уровня t_hdr
	piotRequestLen = 269  // dk_msg1, дополненное нулями
	piotHashLen    = 32   // хэш dk_msg
	piotMaxPart    = 1283 // расшифрованное dk_msg1
	piotMaxSID     = 88   // X-FN-SID
)

// piotHeader — t_hdr эмулятора: метка вместо настоящего заголовка ФН.
func piotHeader() []byte {
	h := make([]byte, piotHeaderLen)
	copy(h, "EMULKON PIOT TEST")
	return h
}

func b64(b []byte) string { return base64.StdEncoding.EncodeToString(b) }

// piotData — Base64 входного параметра с проверкой длины в байтах (max 0 — без верхней границы).
func piotData(a models.Args, i int, name string, min, max int) ([]byte, error) {
	s, err := a.String(i)
	if err != nil {
		return nil, err
	}
	b, err := base64.StdEncoding.DecodeString(s)
	if err != nil {
		return nil, fail(ErrPiotData, "%s: не Base64", name)
	}
	if len(b) < min || (max > 0 && len(b) > max) {
		return nil, fail(ErrPiotData, "%s: %d байт, ожидается %d–%d", name, len(b), min, max)
	}
	return b, nil
}

// requirePiot — ККТ зарегистрирована и её ФН работает с ТС ПИоТ.
func (m *Model) requirePiot() error {
	if err := m.requireFiscal(); err != nil {
		return err
	}
	if !m.st.piot {
		return failCode(ErrPiotNotSupported)
	}
	return nil
}

// PiotSetChallenge(0 DeviceID, 1 IncomingData — Rsp_get_chlg 16–32 байт, 2 OutputData out — Req_chk_chlg).
func (m *Model) piotSetChallenge(a models.Args, out []any) error {
	if err := a.Need(3); err != nil {
		return err
	}
	if err := m.requirePiot(); err != nil {
		return err
	}
	in, err := piotData(a, 1, "IncomingData", 16, 32)
	if err != nil {
		return err
	}
	sum := sha256.Sum256(append([]byte("challenge:"), in...))
	out[2] = b64(append(piotHeader(), sum[:]...))
	return nil
}

// PiotCheckChallenge(0 DeviceID, 1 IncomingData — Rsp_chk_chlg, 2 OutputData out — X-FN-SID до 88 символов).
func (m *Model) piotCheckChallenge(a models.Args, out []any) error {
	if err := a.Need(3); err != nil {
		return err
	}
	if err := m.requirePiot(); err != nil {
		return err
	}
	in, err := piotData(a, 1, "IncomingData", 1, 0)
	if err != nil {
		return err
	}
	sum := sha256.Sum256(in)
	sid := "EMULKON-FN-SID-" + hex.EncodeToString(sum[:])
	if len(sid) > piotMaxSID {
		sid = sid[:piotMaxSID]
	}
	out[2] = sid
	return nil
}

// PiotSetDKMsg(0 DeviceID, 1 RequestPart — dk_msg1 269 байт, 2 Hash — 32 байта, 3 BinaryStructure out).
// Контейнер эмулятора — t_hdr + dk_msg1 + хэш: PiotGetDKMsg разбирает его обратно.
func (m *Model) piotSetDKMsg(a models.Args, out []any) error {
	if err := a.Need(4); err != nil {
		return err
	}
	if err := m.requirePiot(); err != nil {
		return err
	}
	part, err := piotData(a, 1, "RequestPart", piotRequestLen, piotRequestLen)
	if err != nil {
		return err
	}
	hash, err := piotData(a, 2, "Hash", piotHashLen, piotHashLen)
	if err != nil {
		return err
	}
	msg := append(piotHeader(), part...)
	out[3] = b64(append(msg, hash...))
	return nil
}

// PiotGetDKMsg(0 DeviceID, 1 BinaryStructure — dk_ctrl с t_hdr, 2 RequestPart out — до 1283 байт,
// 3 ArrayNumbers out — rand_ar 16–32 байта, 4 Hash out — 32 байта).
func (m *Model) piotGetDKMsg(a models.Args, out []any) error {
	if err := a.Need(5); err != nil {
		return err
	}
	if err := m.requirePiot(); err != nil {
		return err
	}
	ctrl, err := piotData(a, 1, "BinaryStructure", piotHeaderLen+piotHashLen, piotHeaderLen+piotMaxPart+piotHashLen)
	if err != nil {
		return err
	}
	body := ctrl[piotHeaderLen:]
	part, hash := body[:len(body)-piotHashLen], body[len(body)-piotHashLen:]
	random := sha256.Sum256(body)
	out[2] = b64(part)
	out[3] = b64(random[:16])
	out[4] = b64(hash)
	return nil
}

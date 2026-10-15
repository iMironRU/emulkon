package downloads

import (
	"archive/zip"
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"regexp"
	"strings"

	"emulkon/internal/models"
)

// Пакет драйвера под один тип оборудования: тот же архив, что общий, но в INFO.XML один тип.
// БПО заводит запись справочника «Драйверы оборудования» на каждый тип из INFO.XML, и не
// всегда нужны сразу все. Собирается на лету из общего пакета той же версии — сборке и
// установщику ничего не нужно знать о типах.

var (
	reTypePackage = regexp.MustCompile(`^EmulkonDriver_(\d+\.\d+\.\d+)_([a-z_]+)\.zip$`)
	reInfoType    = regexp.MustCompile(`type="[^"]*"`)
)

// TypePackageName — имя пакета драйвера под тип key.
func TypePackageName(version, key string) string {
	return fmt.Sprintf("EmulkonDriver_%s_%s.zip", version, key)
}

type typePackage struct {
	data []byte
	sum  string
}

// typePackage собирает пакет под тип из общего пакета general (кэш — по хешу общего).
func (h *Handler) typePackage(general File, key string) (typePackage, error) {
	t, ok := h.typeByKey(key)
	if !ok || !driverTypes[key] {
		return typePackage{}, fmt.Errorf("тип %q не объявлен в пакете драйвера", key)
	}
	cacheKey := general.SHA256 + "/" + key
	h.mu.Lock()
	p, ok := h.typePkgs[cacheKey]
	h.mu.Unlock()
	if ok {
		return p, nil
	}
	zr, err := zip.OpenReader(h.path(general.Name))
	if err != nil {
		return typePackage{}, fmt.Errorf("открыть %s: %w", general.Name, err)
	}
	defer zr.Close()
	var buf bytes.Buffer
	zw := zip.NewWriter(&buf)
	for _, f := range zr.File {
		rc, err := f.Open()
		if err != nil {
			return typePackage{}, fmt.Errorf("%s/%s: %w", general.Name, f.Name, err)
		}
		data, err := io.ReadAll(rc)
		rc.Close()
		if err != nil {
			return typePackage{}, fmt.Errorf("%s/%s: %w", general.Name, f.Name, err)
		}
		if strings.EqualFold(f.Name, "INFO.XML") {
			data = reInfoType.ReplaceAll(data, []byte(`type="`+t.BPOName()+`"`))
		}
		w, err := zw.CreateHeader(&zip.FileHeader{Name: f.Name, Method: zip.Deflate, Modified: f.Modified})
		if err != nil {
			return typePackage{}, err
		}
		if _, err := w.Write(data); err != nil {
			return typePackage{}, err
		}
	}
	if err := zw.Close(); err != nil {
		return typePackage{}, err
	}
	sum := sha256.Sum256(buf.Bytes())
	p = typePackage{data: buf.Bytes(), sum: hex.EncodeToString(sum[:])}
	h.mu.Lock()
	h.typePkgs[cacheKey] = p
	h.mu.Unlock()
	return p, nil
}

func (h *Handler) typeByKey(key string) (models.EquipmentType, bool) {
	for _, t := range h.cfg.Types {
		if t.Key == key {
			return t, true
		}
	}
	return models.EquipmentType{}, false
}

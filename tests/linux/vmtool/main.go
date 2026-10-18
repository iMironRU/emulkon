// vmtool — подготовка ВМ Hyper-V из облачного образа Ubuntu без прав администратора и без qemu-img
// (tests/linux/new-vm.ps1):
//
//	go run ./tests/linux/vmtool vhd  <образ.img> <диск.vhd>                 qcow2 → фиксированный VHD
//	go run ./tests/linux/vmtool seed <seed.iso> <user-data> <meta-data>     ISO CIDATA для cloud-init (NoCloud)
//	go run ./tests/linux/vmtool iso  <out.iso> <метка> <файл>…              файлы в корень ISO (ответы Windows)
package main

import (
	"fmt"
	"os"
	"path/filepath"
)

func main() {
	if err := run(os.Args[1:]); err != nil {
		fmt.Fprintln(os.Stderr, "vmtool:", err)
		os.Exit(1)
	}
}

func run(args []string) error {
	switch {
	case len(args) == 3 && args[0] == "vhd":
		return convertVHD(args[1], args[2])
	case len(args) == 4 && args[0] == "seed":
		user, err := os.ReadFile(args[2])
		if err != nil {
			return err
		}
		meta, err := os.ReadFile(args[3])
		if err != nil {
			return err
		}
		return writeSeed(args[1], "CIDATA", map[string][]byte{"user-data": user, "meta-data": meta})
	case len(args) >= 4 && args[0] == "iso":
		// Произвольные файлы в корень ISO — например, autounattend.xml для установки Windows (tests/windows).
		files := map[string][]byte{}
		for _, p := range args[3:] {
			data, err := os.ReadFile(p)
			if err != nil {
				return err
			}
			files[filepath.Base(p)] = data
		}
		return writeSeed(args[1], args[2], files)
	}
	fmt.Fprintln(os.Stderr, "использование: vmtool vhd <образ.img> <диск.vhd> | vmtool seed <seed.iso> <user-data> <meta-data> | vmtool iso <out.iso> <метка> <файл>…")
	os.Exit(2)
	return nil
}

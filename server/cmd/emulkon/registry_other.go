//go:build !windows

package main

// readRegistry — реестра нет: настройки только из ключей и окружения.
func readRegistry() registryValues { return registryValues{} }

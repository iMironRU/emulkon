//go:build !windows

package main

import (
	"context"
	"errors"
)

func runAsService(func(ctx context.Context, ready func()) error) error {
	return errors.New("ключ -service — только для службы Windows")
}

// eventLog — журнала событий Windows нет: записи только в лог.
type eventLog struct{}

func openEventLog() *eventLog                     { return nil }
func (l *eventLog) Info(id uint32, msg string)    {}
func (l *eventLog) Warning(id uint32, msg string) {}
func (l *eventLog) Error(id uint32, msg string)   {}
func (l *eventLog) Close()                        {}

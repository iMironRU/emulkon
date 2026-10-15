//go:build windows

package main

// Служба Windows (docs/local-install.md §2.2) и журнал событий — на системных вызовах advapi32, без внешних
// зависимостей. Служба регистрируется установщиком с ключом -service; вне SCM этот ключ — ошибка.

import (
	"context"
	"errors"
	"fmt"
	"sync"
	"syscall"
	"unsafe"
)

const serviceName = "Emulkon"

var (
	advapi32                         = syscall.NewLazyDLL("advapi32.dll")
	procStartServiceCtrlDispatcherW  = advapi32.NewProc("StartServiceCtrlDispatcherW")
	procRegisterServiceCtrlHandlerEx = advapi32.NewProc("RegisterServiceCtrlHandlerExW")
	procSetServiceStatus             = advapi32.NewProc("SetServiceStatus")
	procRegisterEventSourceW         = advapi32.NewProc("RegisterEventSourceW")
	procReportEventW                 = advapi32.NewProc("ReportEventW")
	procDeregisterEventSource        = advapi32.NewProc("DeregisterEventSource")
)

const (
	serviceWin32OwnProcess = 0x10

	stateStopped      = 1
	stateStartPending = 2
	stateStopPending  = 3
	stateRunning      = 4

	acceptStop     = 0x1
	acceptShutdown = 0x4

	controlStop        = 1
	controlInterrogate = 4
	controlShutdown    = 5

	errServiceSpecific             = 1066 // ERROR_SERVICE_SPECIFIC_ERROR
	errFailedServiceControllerConn = 1063 // ERROR_FAILED_SERVICE_CONTROLLER_CONNECT
)

type serviceStatus struct {
	ServiceType             uint32
	CurrentState            uint32
	ControlsAccepted        uint32
	Win32ExitCode           uint32
	ServiceSpecificExitCode uint32
	CheckPoint              uint32
	WaitHint                uint32
}

type serviceTableEntry struct {
	name *uint16
	proc uintptr
}

// svc — состояние единственной службы процесса (обратные вызовы SCM не несут контекста Go).
var svc struct {
	mu     sync.Mutex
	handle uintptr
	status serviceStatus
	cancel context.CancelFunc
	run    func(ctx context.Context, ready func()) error
	err    error
}

func setStatus(state, accepted, waitHint uint32, exitErr error) {
	svc.mu.Lock()
	defer svc.mu.Unlock()
	s := &svc.status
	s.ServiceType, s.CurrentState, s.ControlsAccepted, s.WaitHint = serviceWin32OwnProcess, state, accepted, waitHint
	s.Win32ExitCode, s.ServiceSpecificExitCode = 0, 0
	if exitErr != nil {
		s.Win32ExitCode, s.ServiceSpecificExitCode = errServiceSpecific, 1
	}
	if state == stateStartPending || state == stateStopPending {
		s.CheckPoint++
	} else {
		s.CheckPoint = 0
	}
	if svc.handle != 0 {
		procSetServiceStatus.Call(svc.handle, uintptr(unsafe.Pointer(s)))
	}
}

func serviceHandler(control, _, _, _ uintptr) uintptr {
	switch uint32(control) {
	case controlStop, controlShutdown:
		setStatus(stateStopPending, 0, 15000, nil)
		svc.mu.Lock()
		cancel := svc.cancel
		svc.mu.Unlock()
		if cancel != nil {
			cancel()
		}
	case controlInterrogate:
		svc.mu.Lock()
		s := svc.status
		h := svc.handle
		svc.mu.Unlock()
		procSetServiceStatus.Call(h, uintptr(unsafe.Pointer(&s)))
	}
	return 0
}

func serviceMain(_, _ uintptr) uintptr {
	name, _ := syscall.UTF16PtrFromString(serviceName)
	h, _, _ := procRegisterServiceCtrlHandlerEx.Call(uintptr(unsafe.Pointer(name)), syscall.NewCallback(serviceHandler), 0)
	if h == 0 {
		svc.err = errors.New("RegisterServiceCtrlHandlerEx: не удалось")
		return 0
	}
	svc.mu.Lock()
	svc.handle = h
	svc.mu.Unlock()
	setStatus(stateStartPending, 0, 30000, nil)

	ctx, cancel := context.WithCancel(context.Background())
	svc.mu.Lock()
	svc.cancel = cancel
	svc.mu.Unlock()
	err := svc.run(ctx, func() { setStatus(stateRunning, acceptStop|acceptShutdown, 0, nil) })
	cancel()
	svc.err = err
	setStatus(stateStopped, 0, 0, err)
	return 0
}

// runAsService отдаёт процесс SCM: run работает, пока служба не остановлена; ready — сервер принимает соединения.
func runAsService(run func(ctx context.Context, ready func()) error) error {
	svc.run = run
	name, _ := syscall.UTF16PtrFromString(serviceName)
	table := []serviceTableEntry{{name: name, proc: syscall.NewCallback(serviceMain)}, {}}
	r, _, e := procStartServiceCtrlDispatcherW.Call(uintptr(unsafe.Pointer(&table[0])))
	if r == 0 {
		if errno, ok := e.(syscall.Errno); ok && errno == errFailedServiceControllerConn {
			return errors.New("ключ -service — только для запуска службой Windows (её регистрирует установщик)")
		}
		return fmt.Errorf("StartServiceCtrlDispatcher: %w", e)
	}
	return svc.err
}

// Журнал событий Windows: источник Emulkon в журнале «Приложение». Текст сообщений показывается, если установщик
// зарегистрировал источник (EventMessageFile); иначе событие всё равно пишется.
const (
	eventTypeError   = 0x0001
	eventTypeWarning = 0x0002
	eventTypeInfo    = 0x0004
)

type eventLog struct{ h uintptr }

func openEventLog() *eventLog {
	name, _ := syscall.UTF16PtrFromString(serviceName)
	h, _, _ := procRegisterEventSourceW.Call(0, uintptr(unsafe.Pointer(name)))
	return &eventLog{h: h}
}

func (l *eventLog) report(typ uint16, id uint32, msg string) {
	if l == nil || l.h == 0 {
		return
	}
	s, err := syscall.UTF16PtrFromString(msg)
	if err != nil {
		return
	}
	strs := []*uint16{s}
	procReportEventW.Call(l.h, uintptr(typ), 0, uintptr(id), 0, 1, 0, uintptr(unsafe.Pointer(&strs[0])), 0)
}

func (l *eventLog) Info(id uint32, msg string)    { l.report(eventTypeInfo, id, msg) }
func (l *eventLog) Warning(id uint32, msg string) { l.report(eventTypeWarning, id, msg) }
func (l *eventLog) Error(id uint32, msg string)   { l.report(eventTypeError, id, msg) }

func (l *eventLog) Close() {
	if l != nil && l.h != 0 {
		procDeregisterEventSource.Call(l.h)
		l.h = 0
	}
}

// Package logging provides the small leveled logger used by the Go server.
package logging

import (
	"fmt"
	"io"
	"os"
	"strings"
	"sync"
	"time"
)

// Level is a log severity.
type Level int

const (
	Debug Level = iota
	Info
	Warning
	Error
)

// ParseLevel parses a configuration log-level name.
func ParseLevel(text string) (Level, bool) {
	switch strings.ToLower(text) {
	case "debug":
		return Debug, true
	case "info":
		return Info, true
	case "warn", "warning":
		return Warning, true
	case "error":
		return Error, true
	default:
		return Info, false
	}
}

func (l Level) String() string {
	switch l {
	case Debug:
		return "DEBUG"
	case Info:
		return "INFO"
	case Warning:
		return "WARN"
	case Error:
		return "ERROR"
	default:
		return "?"
	}
}

// Logger writes timestamped lines to one or more sinks.
type Logger struct {
	mu    sync.Mutex
	level Level
	out   io.Writer
	file  *os.File
}

// New creates a logger. A nil out discards output.
func New(level Level, out io.Writer) *Logger {
	return &Logger{level: level, out: out}
}

// OpenFile appends log output to path. The file is closed by Close.
func (l *Logger) OpenFile(path string) error {
	file, err := os.OpenFile(path, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0o644)
	if err != nil {
		return err
	}

	l.mu.Lock()
	old := l.file
	l.file = file
	l.mu.Unlock()

	if old != nil {
		_ = old.Close()
	}
	return nil
}

// SetLevel changes the minimum level.
func (l *Logger) SetLevel(level Level) {
	l.mu.Lock()
	l.level = level
	l.mu.Unlock()
}

// Level returns the current minimum level.
func (l *Logger) Level() Level {
	l.mu.Lock()
	defer l.mu.Unlock()
	return l.level
}

func (l *Logger) Debug(message string)   { l.write(Debug, message) }
func (l *Logger) Info(message string)    { l.write(Info, message) }
func (l *Logger) Warning(message string) { l.write(Warning, message) }
func (l *Logger) Error(message string)   { l.write(Error, message) }

func (l *Logger) write(level Level, message string) {
	l.mu.Lock()
	defer l.mu.Unlock()

	if level < l.level {
		return
	}

	line := fmt.Sprintf("%s [%s] %s\n",
		time.Now().Format("2006-01-02 15:04:05.000"), level.String(), message)
	if l.out != nil {
		_, _ = io.WriteString(l.out, line)
	}
	if l.file != nil {
		_, _ = io.WriteString(l.file, line)
	}
}

// Close closes the optional log file.
func (l *Logger) Close() error {
	l.mu.Lock()
	file := l.file
	l.file = nil
	l.mu.Unlock()

	if file != nil {
		return file.Close()
	}
	return nil
}

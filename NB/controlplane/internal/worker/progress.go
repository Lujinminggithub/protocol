package worker

import (
	"bytes"
	"context"
	"io"
	"strings"
	"sync"
	"time"
)

const maxProgressMessageRunes = 500

type progressWriter struct {
	destination io.Writer
	emit        func(string)
	interval    time.Duration
	limit       int

	mu       sync.Mutex
	buffer   bytes.Buffer
	pending  string
	last     string
	lastEmit time.Time
	emitted  int
}

func newProgressWriter(destination io.Writer, interval time.Duration, limit int, emit func(string)) *progressWriter {
	return &progressWriter{destination: destination, interval: interval, limit: limit, emit: emit}
}

func progressMessage(value string) string {
	value = strings.TrimSpace(value)
	if value == "" || strings.Contains(value, "PRIVATE KEY") {
		return ""
	}
	value = sensitiveErrorValue.ReplaceAllString(value, "$1$2[REDACTED]")
	runes := []rune(value)
	if len(runes) > maxProgressMessageRunes {
		value = string(runes[:maxProgressMessageRunes]) + "..."
	}
	return value
}

func (w *progressWriter) Write(data []byte) (int, error) {
	written, err := w.destination.Write(data)
	if written == 0 {
		return written, err
	}
	w.mu.Lock()
	defer w.mu.Unlock()
	_, _ = w.buffer.Write(data[:written])
	for {
		line, readErr := w.buffer.ReadString('\n')
		if readErr != nil {
			_, _ = w.buffer.WriteString(line)
			break
		}
		w.accept(strings.TrimSuffix(line, "\r\n"), false)
	}
	return written, err
}

func (w *progressWriter) accept(line string, force bool) {
	message := progressMessage(line)
	if message == "" || message == w.last {
		return
	}
	w.pending = message
	if force || w.lastEmit.IsZero() || time.Since(w.lastEmit) >= w.interval {
		w.publish()
	}
}

func (w *progressWriter) publish() {
	if w.pending == "" || w.emitted >= w.limit {
		return
	}
	w.emit(w.pending)
	w.last, w.pending = w.pending, ""
	w.lastEmit = time.Now()
	w.emitted++
}

func (w *progressWriter) Flush() {
	w.mu.Lock()
	defer w.mu.Unlock()
	if w.buffer.Len() > 0 {
		w.accept(w.buffer.String(), true)
		w.buffer.Reset()
	}
	w.publish()
}

func emitProgress(ctx context.Context, sequence *int, stage, message string) {
	_ = emitOperationEvent(ctx, OperationEvent{Sequence: *sequence, Stage: stage, Status: "running", Message: message})
	*sequence++
}

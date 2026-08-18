package webapp

import (
	"sync"

	"nb-controlplane/internal/central"
)

type snapshotHub struct {
	mu          sync.Mutex
	subscribers map[string]map[chan central.Snapshot]struct{}
}

func newSnapshotHub() *snapshotHub {
	return &snapshotHub{subscribers: map[string]map[chan central.Snapshot]struct{}{}}
}

func (h *snapshotHub) subscribe(lineID string) (<-chan central.Snapshot, func()) {
	channel := make(chan central.Snapshot, 1)
	h.mu.Lock()
	if h.subscribers[lineID] == nil {
		h.subscribers[lineID] = map[chan central.Snapshot]struct{}{}
	}
	h.subscribers[lineID][channel] = struct{}{}
	h.mu.Unlock()
	return channel, func() {
		h.mu.Lock()
		delete(h.subscribers[lineID], channel)
		if len(h.subscribers[lineID]) == 0 {
			delete(h.subscribers, lineID)
		}
		h.mu.Unlock()
	}
}

func (h *snapshotHub) publish(snapshot central.Snapshot) {
	h.mu.Lock()
	defer h.mu.Unlock()
	for subscriber := range h.subscribers[snapshot.LineID] {
		select {
		case subscriber <- snapshot:
		default:
			select {
			case <-subscriber:
			default:
			}
			select {
			case subscriber <- snapshot:
			default:
			}
		}
	}
}

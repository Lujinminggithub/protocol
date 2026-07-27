package nbproto

import (
	"context"
	"errors"
	"sync"
	"sync/atomic"
	"time"
)

// FrameSender is the authenticated, ordered NB channel supplied by an Xray
// outbound. SendFrame must copy frame before returning if it retains the data.
type FrameSender interface {
	SendFrame(context.Context, []byte) error
}

// UDPTransport adapts Xray UDP datagrams to NBUD v1 frames. It emits every
// fragment immediately; it does not wait for another datagram or a full window.
type UDPTransport struct {
	sender    FrameSender
	sessionID uint32
	sequence  atomic.Uint32
	receiver  *UDPReassembler
	receiveMu sync.Mutex
}

func NewUDPTransport(sender FrameSender, sessionID uint32, reassemblyTimeout time.Duration) (*UDPTransport, error) {
	if sender == nil || sessionID == 0 || reassemblyTimeout <= 0 {
		return nil, errors.New("invalid UDP transport configuration")
	}
	transport := &UDPTransport{sender: sender, sessionID: sessionID,
		receiver: NewUDPReassembler(uint64(reassemblyTimeout.Microseconds()))}
	transport.sequence.Store(0)
	return transport, nil
}

func (t *UDPTransport) SendDatagram(ctx context.Context, route string, payload []byte) error {
	if err := ctx.Err(); err != nil {
		return err
	}
	sequence := t.sequence.Add(1)
	if sequence == 0 {
		sequence = t.sequence.Add(1)
	}
	frames, err := FragmentUDP(UDPTypeClientToExit, t.sessionID, sequence, route, payload)
	if err != nil {
		return err
	}
	for _, frame := range frames {
		if err = t.sender.SendFrame(ctx, frame); err != nil {
			return err
		}
	}
	return nil
}

// AcceptFrame returns one complete exit-to-client datagram when available.
func (t *UDPTransport) AcceptFrame(frame []byte, receivedAt time.Time) ([]byte, string, bool, error) {
	fragment, err := DecodeUDPFragment(frame)
	if err != nil {
		return nil, "", false, err
	}
	if fragment.Type != UDPTypeExitToClient || fragment.SessionID != t.sessionID {
		return nil, "", false, errors.New("UDP frame does not belong to this session")
	}
	t.receiveMu.Lock()
	defer t.receiveMu.Unlock()
	return t.receiver.Feed(frame, uint64(receivedAt.UnixMicro()))
}

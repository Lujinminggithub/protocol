package nbproto

import (
	"bytes"
	"context"
	"errors"
	"testing"
	"time"
)

type recordingSender struct {
	frames [][]byte
	errAt  int
}

func (s *recordingSender) SendFrame(_ context.Context, frame []byte) error {
	if s.errAt > 0 && len(s.frames)+1 == s.errAt {
		return errors.New("channel closed")
	}
	s.frames = append(s.frames, append([]byte(nil), frame...))
	return nil
}

func TestUDPTransportSendsLargeDatagramImmediately(t *testing.T) {
	sender := &recordingSender{}
	transport, err := NewUDPTransport(sender, 77, 5*time.Second)
	if err != nil {
		t.Fatal(err)
	}
	payload := make([]byte, UDPMaxPayload)
	for i := range payload {
		payload[i] = byte(i * 17)
	}
	if err = transport.SendDatagram(context.Background(), "dns.example:53", payload); err != nil {
		t.Fatal(err)
	}
	if len(sender.frames) != UDPMaxFragments {
		t.Fatalf("frames=%d want=%d", len(sender.frames), UDPMaxFragments)
	}
	for index, frame := range sender.frames {
		fragment, decodeErr := DecodeUDPFragment(frame)
		if decodeErr != nil || fragment.Sequence != 1 || int(fragment.FragmentIndex) != index {
			t.Fatalf("frame %d sequence=%d index=%d err=%v", index, fragment.Sequence, fragment.FragmentIndex, decodeErr)
		}
	}
}

func TestUDPTransportReassemblesExitFramesOutOfOrder(t *testing.T) {
	transport, err := NewUDPTransport(&recordingSender{}, 88, 5*time.Second)
	if err != nil {
		t.Fatal(err)
	}
	payload := bytes.Repeat([]byte("large-udp"), 800)
	frames, err := FragmentUDP(UDPTypeExitToClient, 88, 4, "client:9000", payload)
	if err != nil {
		t.Fatal(err)
	}
	var got []byte
	for i := len(frames) - 1; i >= 0; i-- {
		data, route, complete, acceptErr := transport.AcceptFrame(frames[i], time.UnixMicro(int64(i+1)))
		if acceptErr != nil {
			t.Fatal(acceptErr)
		}
		if complete {
			if route != "client:9000" {
				t.Fatalf("route=%q", route)
			}
			got = data
		}
	}
	if !bytes.Equal(got, payload) {
		t.Fatal("reassembled payload mismatch")
	}
}

func TestUDPTransportRejectsWrongSessionAndStopsOnSendFailure(t *testing.T) {
	sender := &recordingSender{errAt: 2}
	transport, err := NewUDPTransport(sender, 99, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if err = transport.SendDatagram(context.Background(), "target:1", make([]byte, 1001)); err == nil || len(sender.frames) != 1 {
		t.Fatalf("send failure err=%v frames=%d", err, len(sender.frames))
	}
	frames, err := FragmentUDP(UDPTypeExitToClient, 100, 1, "client:1", []byte("x"))
	if err != nil {
		t.Fatal(err)
	}
	if _, _, _, err = transport.AcceptFrame(frames[0], time.Now()); err == nil {
		t.Fatal("wrong session accepted")
	}
}

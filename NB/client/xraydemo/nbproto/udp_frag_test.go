package nbproto

import (
	"bytes"
	"encoding/binary"
	"testing"
)

func TestUDPFragmentRoundTripOutOfOrder(t *testing.T) {
	payload := make([]byte, UDPMaxPayload)
	for i := range payload {
		payload[i] = byte(i * 31)
	}
	frames, err := FragmentUDP(UDPTypeClientToExit, 7, 42, "udp.example:443", payload)
	if err != nil || len(frames) != 66 {
		t.Fatalf("fragment: count=%d err=%v", len(frames), err)
	}
	r := NewUDPReassembler(5_000_000)
	order := make([]int, 0, len(frames)+1)
	for i := len(frames) - 1; i >= 0; i-- {
		order = append(order, i)
	}
	order = append(order, 3) // A duplicate must not advance completion.
	var got []byte
	var route string
	for step, index := range order {
		data, target, complete, feedErr := r.Feed(frames[index], uint64(step+1))
		if feedErr != nil {
			t.Fatal(feedErr)
		}
		if complete {
			got, route = data, target
		}
	}
	if route != "udp.example:443" || !bytes.Equal(got, payload) {
		t.Fatal("reassembled datagram mismatch")
	}
}

func TestUDPFragmentRejectsMalformedAndInconsistent(t *testing.T) {
	if _, err := FragmentUDP(UDPTypeClientToExit, 1, 1, "route", make([]byte, UDPMaxPayload+1)); err == nil {
		t.Fatal("oversize payload accepted")
	}
	frames, err := FragmentUDP(UDPTypeClientToExit, 1, 9, "route", make([]byte, 1001))
	if err != nil {
		t.Fatal(err)
	}
	broken := append([]byte(nil), frames[0]...)
	binary.BigEndian.PutUint16(broken[18:20], 3)
	if _, err := DecodeUDPFragment(broken); err == nil {
		t.Fatal("inconsistent fragment count accepted")
	}
	r := NewUDPReassembler(100)
	if _, _, _, err = r.Feed(frames[0], 1); err != nil {
		t.Fatal(err)
	}
	changed := append([]byte(nil), frames[1]...)
	changed[UDPHeaderSize] = 'R'
	if _, _, _, err = r.Feed(changed, 2); err == nil {
		t.Fatal("route change accepted")
	}
}

func TestUDPReassemblyExpiresIncompleteDatagram(t *testing.T) {
	frames, err := FragmentUDP(UDPTypeClientToExit, 1, 10, "route", make([]byte, 1001))
	if err != nil {
		t.Fatal(err)
	}
	r := NewUDPReassembler(10)
	if _, _, complete, err := r.Feed(frames[0], 1); err != nil || complete {
		t.Fatalf("first fragment: complete=%v err=%v", complete, err)
	}
	if _, _, complete, err := r.Feed(frames[1], 12); err != nil || complete {
		t.Fatalf("expired fragment: complete=%v err=%v", complete, err)
	}
}

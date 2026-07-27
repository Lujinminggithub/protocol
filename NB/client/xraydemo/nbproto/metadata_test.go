package nbproto

import (
	"encoding/hex"
	"reflect"
	"testing"
)

const golden = "4e424d3201010030000000030206020001020304050607081112131415161718" +
	"000000c80000002a00290010000b0000483a6d6964646c652e6578616d706c653a343434332c" +
	"483a657869742e6578616d706c653a343434336c6976652e6578616d706c653a34343374696b746f6b2e6c697665"

func sample() Metadata {
	return Metadata{Flags: FlagAllowFEC | FlagAllowMultipath, TrafficClass: ClassRealtime,
		Priority: 6, PathPreference: PathLowJitter, SessionID: 0x0102030405060708,
		FlowID: 0x1112131415161718, DeadlineMS: 200, PolicyID: 42,
		Route: "H:middle.example:4443,H:exit.example:4443", Target: "live.example:443",
		BusinessTag: "tiktok.live"}
}

func TestGoldenVector(t *testing.T) {
	wire, err := Encode(sample())
	if err != nil {
		t.Fatal(err)
	}
	if got := hex.EncodeToString(wire); got != golden {
		t.Fatalf("golden mismatch\nwant %s\n got %s", golden, got)
	}
	decoded, err := Decode(wire)
	if err != nil || !reflect.DeepEqual(decoded, sample()) {
		t.Fatalf("roundtrip mismatch: %#v %v", decoded, err)
	}
	for length := range len(wire) {
		if _, err := Decode(wire[:length]); err == nil {
			t.Fatalf("accepted truncation at %d", length)
		}
	}
}

func TestInvalidCombinations(t *testing.T) {
	tests := []Metadata{sample(), sample(), sample(), sample()}
	tests[0].SessionID = 0
	tests[1].DeadlineMS = 0
	tests[2].TrafficClass = ClassReliable
	tests[3].BusinessTag = "invalid tag"
	for i, metadata := range tests {
		if _, err := Encode(metadata); err == nil {
			t.Fatalf("case %d accepted", i)
		}
	}
}

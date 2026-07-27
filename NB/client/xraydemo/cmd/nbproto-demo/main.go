package main

import (
	"bufio"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"net"
	"os"
	"time"

	"nb-xraydemo/nbproto"
)

const maxFrame = 1024

func sample() nbproto.Metadata {
	return nbproto.Metadata{Flags: nbproto.FlagAllowFEC | nbproto.FlagAllowMultipath,
		TrafficClass: nbproto.ClassRealtime, Priority: 6, PathPreference: nbproto.PathLowJitter,
		SessionID: 0x0102030405060708, FlowID: 0x1112131415161718, DeadlineMS: 200,
		PolicyID: 42, Route: "H:middle.example:4443,H:exit.example:4443",
		Target: "live.example:443", BusinessTag: "tiktok.live"}
}

func writeFrame(w io.Writer, payload []byte) error {
	if len(payload) == 0 || len(payload) > maxFrame {
		return errors.New("invalid demo frame length")
	}
	var header [4]byte
	binary.BigEndian.PutUint32(header[:], uint32(len(payload)))
	if _, err := w.Write(header[:]); err != nil {
		return err
	}
	_, err := w.Write(payload)
	return err
}

func readFrame(r io.Reader) ([]byte, error) {
	var header [4]byte
	if _, err := io.ReadFull(r, header[:]); err != nil {
		return nil, err
	}
	length := binary.BigEndian.Uint32(header[:])
	if length == 0 || length > maxFrame {
		return nil, errors.New("invalid demo frame length")
	}
	payload := make([]byte, length)
	_, err := io.ReadFull(r, payload)
	return payload, err
}

func printJSON(metadata nbproto.Metadata) error {
	output, err := json.MarshalIndent(metadata, "", "  ")
	if err == nil {
		fmt.Println(string(output))
	}
	return err
}

func serve(address string) error {
	listener, err := net.Listen("tcp", address)
	if err != nil {
		return err
	}
	defer listener.Close()
	fmt.Printf("demo server listening on %s\n", listener.Addr())
	connection, err := listener.Accept()
	if err != nil {
		return err
	}
	defer connection.Close()
	payload, err := readFrame(bufio.NewReader(connection))
	if err != nil {
		return err
	}
	metadata, err := nbproto.Decode(payload)
	if err != nil {
		return err
	}
	if err := printJSON(metadata); err != nil {
		return err
	}
	return writeFrame(connection, payload)
}

func send(address string) error {
	payload, err := nbproto.Encode(sample())
	if err != nil {
		return err
	}
	connection, err := net.DialTimeout("tcp", address, 5*time.Second)
	if err != nil {
		return err
	}
	defer connection.Close()
	if err := writeFrame(connection, payload); err != nil {
		return err
	}
	reply, err := readFrame(bufio.NewReader(connection))
	if err != nil {
		return err
	}
	metadata, err := nbproto.Decode(reply)
	if err != nil {
		return err
	}
	return printJSON(metadata)
}

func run() error {
	if len(os.Args) < 2 {
		return errors.New("usage: nbproto-demo encode|decode|serve|send")
	}
	switch os.Args[1] {
	case "encode":
		payload, err := nbproto.Encode(sample())
		if err == nil {
			fmt.Println(hex.EncodeToString(payload))
		}
		return err
	case "decode":
		flags := flag.NewFlagSet("decode", flag.ContinueOnError)
		value := flags.String("hex", "", "hex-encoded metadata")
		if err := flags.Parse(os.Args[2:]); err != nil {
			return err
		}
		payload, err := hex.DecodeString(*value)
		if err != nil {
			return err
		}
		metadata, err := nbproto.Decode(payload)
		if err != nil {
			return err
		}
		return printJSON(metadata)
	case "serve", "send":
		flags := flag.NewFlagSet(os.Args[1], flag.ContinueOnError)
		address := flags.String("addr", "127.0.0.1:19090", "demo TCP address")
		if err := flags.Parse(os.Args[2:]); err != nil {
			return err
		}
		if os.Args[1] == "serve" {
			return serve(*address)
		}
		return send(*address)
	default:
		return errors.New("unknown command")
	}
}

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

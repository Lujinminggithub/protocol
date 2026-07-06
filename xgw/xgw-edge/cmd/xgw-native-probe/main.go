package main

import (
	"context"
	"flag"
	"fmt"
	"io"
	"os"
	"time"

	"github.com/local/xgw-edge/internal/config"
	"github.com/local/xgw-edge/internal/native"
	"go.uber.org/zap"
)

func main() {
	var configPath string
	var target string
	var payload string
	var readBytes int
	var timeout time.Duration
	var udp bool
	flag.StringVar(&configPath, "config", "", "client config file")
	flag.StringVar(&target, "target", "", "target host:port")
	flag.StringVar(&payload, "payload", "", "payload to write after TCP open")
	flag.IntVar(&readBytes, "read-bytes", 4096, "maximum bytes to read after payload write")
	flag.DurationVar(&timeout, "timeout", 10*time.Second, "probe timeout")
	flag.BoolVar(&udp, "udp", false, "send payload as native UDP datagram instead of TCP stream")
	flag.Parse()

	if target == "" {
		fmt.Fprintln(os.Stderr, "xgw-native-probe: -target is required")
		os.Exit(2)
	}
	cfg, err := config.LoadClient(configPath)
	if err != nil {
		fmt.Fprintf(os.Stderr, "load config: %v\n", err)
		os.Exit(1)
	}
	logger, err := zap.NewDevelopment()
	if err != nil {
		fmt.Fprintf(os.Stderr, "logger: %v\n", err)
		os.Exit(1)
	}
	defer logger.Sync()

	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()

	client := native.NewClient(cfg, logger.Sugar())
	session, err := client.Connect(ctx)
	if err != nil {
		fmt.Fprintf(os.Stderr, "connect: %v\n", err)
		os.Exit(1)
	}
	defer session.Close()
	fmt.Printf("native.auth.ok session=%s route=%s cc=%s\n", session.Resp.SessionID, session.Resp.SelectedRoute.Name, session.Resp.SelectedCongestion)

	if udp {
		if payload == "" {
			payload = "ping"
		}
		if err := session.SendDatagramTo(ctx, target, []byte(payload)); err != nil {
			fmt.Fprintf(os.Stderr, "udp send: %v\n", err)
			os.Exit(1)
		}
		fmt.Printf("native.udp.write.ok target=%s bytes=%d\n", target, len(payload))
		dg, err := session.ReceiveDatagramFrom(ctx)
		if err != nil {
			fmt.Fprintf(os.Stderr, "udp read: %v\n", err)
			os.Exit(1)
		}
		fmt.Printf("native.udp.read.ok target=%s bytes=%d\n", dg.Target, len(dg.Payload))
		if len(dg.Payload) > 0 {
			_, _ = os.Stdout.Write(dg.Payload)
			fmt.Println()
		}
		return
	}

	conn, err := session.DialTCP(ctx, target)
	if err != nil {
		fmt.Fprintf(os.Stderr, "tcp open: %v\n", err)
		os.Exit(1)
	}
	defer conn.Close()
	fmt.Printf("native.tcp.open.ok target=%s\n", target)

	if payload != "" {
		if _, err := conn.Write([]byte(payload)); err != nil {
			fmt.Fprintf(os.Stderr, "write payload: %v\n", err)
			os.Exit(1)
		}
		fmt.Printf("native.tcp.write.ok bytes=%d\n", len(payload))
	}
	if readBytes > 0 {
		_ = conn.SetReadDeadline(time.Now().Add(timeout / 2))
		buf := make([]byte, readBytes)
		n, err := conn.Read(buf)
		if err != nil && err != io.EOF {
			fmt.Printf("native.tcp.read.err err=%v\n", err)
			return
		}
		fmt.Printf("native.tcp.read.ok bytes=%d\n", n)
		if n > 0 {
			_, _ = os.Stdout.Write(buf[:n])
			fmt.Println()
		}
	}
}

package main

import (
	"flag"
	"fmt"
	"net"
	"os"
	"time"

	hyclient "github.com/apernet/hysteria/core/v2/client"
)

func main() {
	var (
		serverAddr string
		targetAddr string
		token      string
		sni        string
		payload    string
		timeout    time.Duration
		insecure   bool
	)

	flag.StringVar(&serverAddr, "server", "", "HY2 server udp address, e.g. 127.0.0.1:1023")
	flag.StringVar(&targetAddr, "target", "", "target udp address forwarded by the HY2 front")
	flag.StringVar(&token, "token", "edge-secret", "auth token")
	flag.StringVar(&sni, "sni", "www.example.com", "tls server name")
	flag.StringVar(&payload, "payload", "ping", "payload to send")
	flag.DurationVar(&timeout, "timeout", 12*time.Second, "receive timeout")
	flag.BoolVar(&insecure, "insecure", true, "skip certificate verification")
	flag.Parse()

	if serverAddr == "" || targetAddr == "" {
		fmt.Fprintln(os.Stderr, "server and target are required")
		os.Exit(2)
	}

	raddr, err := net.ResolveUDPAddr("udp", serverAddr)
	if err != nil {
		fmt.Fprintf(os.Stderr, "resolve server: %v\n", err)
		os.Exit(1)
	}

	cli, _, err := hyclient.NewClient(&hyclient.Config{
		ServerAddr: raddr,
		Auth:       token,
		TLSConfig: hyclient.TLSConfig{
			ServerName:         sni,
			InsecureSkipVerify: insecure,
		},
		QUICConfig: hyclient.QUICConfig{
			MaxIdleTimeout:  30 * time.Second,
			KeepAlivePeriod: 10 * time.Second,
		},
	})
	if err != nil {
		fmt.Fprintf(os.Stderr, "connect: %v\n", err)
		os.Exit(1)
	}
	defer cli.Close()

	udpConn, err := cli.UDP()
	if err != nil {
		fmt.Fprintf(os.Stderr, "udp session: %v\n", err)
		os.Exit(1)
	}
	defer udpConn.Close()

	if err := udpConn.Send([]byte(payload), targetAddr); err != nil {
		fmt.Fprintf(os.Stderr, "udp send: %v\n", err)
		os.Exit(1)
	}

	type result struct {
		data []byte
		addr string
		err  error
	}
	done := make(chan result, 1)
	go func() {
		data, addr, err := udpConn.Receive()
		done <- result{data: data, addr: addr, err: err}
	}()

	select {
	case res := <-done:
		if res.err != nil {
			fmt.Fprintf(os.Stderr, "udp receive: %v\n", res.err)
			os.Exit(1)
		}
		fmt.Printf("addr=%s\n", res.addr)
		fmt.Printf("data=%s\n", string(res.data))
	case <-time.After(timeout):
		fmt.Fprintln(os.Stderr, "udp receive timeout")
		os.Exit(1)
	}
}

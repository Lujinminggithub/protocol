package main

import (
	"context"
	"crypto/rand"
	"crypto/rsa"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/pem"
	"flag"
	"fmt"
	"io"
	"log"
	"math/big"
	"net"
	"time"

	quic "github.com/quic-go/quic-go"
)

func main() {
	var listenAddr string
	flag.StringVar(&listenAddr, "listen", ":9443", "listen udp address")
	flag.Parse()

	tlsConf, err := buildTLSConfig()
	if err != nil {
		log.Fatalf("tls config: %v", err)
	}
	listener, err := quic.ListenAddr(listenAddr, tlsConf, &quic.Config{
		EnableDatagrams:     true,
		HandshakeIdleTimeout: 10 * time.Second,
		MaxIdleTimeout:      60 * time.Second,
		KeepAlivePeriod:     10 * time.Second,
		MaxIncomingStreams:  1024,
	})
	if err != nil {
		log.Fatalf("listen: %v", err)
	}
	defer listener.Close()

	log.Printf("quic-echo.listen addr=%s", listenAddr)
	for {
		conn, err := listener.Accept(context.Background())
		if err != nil {
			log.Fatalf("accept conn: %v", err)
		}
		go handleConn(conn)
	}
}

func handleConn(conn *quic.Conn) {
	log.Printf("quic-echo.conn.accept remote=%s", conn.RemoteAddr())
	go handleDatagrams(conn)
	for {
		stream, err := conn.AcceptStream(context.Background())
		if err != nil {
			log.Printf("quic-echo.conn.close remote=%s err=%v", conn.RemoteAddr(), err)
			return
		}
		go func() {
			defer stream.Close()
			if _, err := io.Copy(stream, stream); err != nil {
				log.Printf("quic-echo.stream.close remote=%s id=%d err=%v", conn.RemoteAddr(), stream.StreamID(), err)
			}
		}()
	}
}

func handleDatagrams(conn *quic.Conn) {
	for {
		msg, err := conn.ReceiveDatagram(context.Background())
		if err != nil {
			log.Printf("quic-echo.datagram.close remote=%s err=%v", conn.RemoteAddr(), err)
			return
		}
		if err := conn.SendDatagram(msg); err != nil {
			log.Printf("quic-echo.datagram.send_fail remote=%s err=%v", conn.RemoteAddr(), err)
			return
		}
	}
}

func buildTLSConfig() (*tls.Config, error) {
	key, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		return nil, err
	}
	template := &x509.Certificate{
		SerialNumber: big.NewInt(time.Now().UnixNano()),
		Subject: pkix.Name{
			CommonName: "xgw-quic-echo",
		},
		NotBefore:             time.Now().Add(-time.Hour),
		NotAfter:              time.Now().Add(24 * time.Hour),
		KeyUsage:              x509.KeyUsageDigitalSignature | x509.KeyUsageKeyEncipherment,
		ExtKeyUsage:           []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
		BasicConstraintsValid: true,
		DNSNames:              []string{"xgw-quic-echo", "localhost"},
		IPAddresses:           []net.IP{net.ParseIP("127.0.0.1")},
	}
	der, err := x509.CreateCertificate(rand.Reader, template, template, &key.PublicKey, key)
	if err != nil {
		return nil, err
	}
	certPEM := pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: der})
	keyPEM := pem.EncodeToMemory(&pem.Block{Type: "RSA PRIVATE KEY", Bytes: x509.MarshalPKCS1PrivateKey(key)})
	cert, err := tls.X509KeyPair(certPEM, keyPEM)
	if err != nil {
		return nil, fmt.Errorf("x509 key pair: %w", err)
	}
	return &tls.Config{
		Certificates: []tls.Certificate{cert},
		NextProtos:   []string{"xgw-quic-echo"},
		MinVersion:   tls.VersionTLS13,
	}, nil
}

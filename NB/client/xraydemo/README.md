# NB protocol demo for Xray integration

This module is a reusable protocol codec and loopback sender/receiver, not a mobile app and not a production transport.
An Xray-core outbound can map its routing context with `nbproto.FromXrayFlow`, encode the metadata, then send it only after the authenticated NB QUIC session is established.

The `nbproto` package also contains bounded `NBUD v1` fragmentation/reassembly for UDP datagrams up to 65,507 bytes. These fragments must be sent immediately over the authenticated NB transport; they are not standalone public UDP packets.

```sh
go test ./...
go run ./cmd/nbproto-demo encode
go run ./cmd/nbproto-demo serve -addr 127.0.0.1:19090
go run ./cmd/nbproto-demo send -addr 127.0.0.1:19090
```

The demo TCP length prefix is only for codec interoperability testing. It must not be exposed as a public SOCKS service or used in place of NB authentication and encryption.

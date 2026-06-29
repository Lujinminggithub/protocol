package compat

import (
	"net/http"
	"strconv"
)

const (
	URLHost = "hysteria"
	URLPath = "/auth"

	RequestHeaderAuth        = "Hysteria-Auth"
	ResponseHeaderUDPEnabled = "Hysteria-UDP"
	CommonHeaderCCRX         = "Hysteria-CC-RX"
	CommonHeaderPadding      = "Hysteria-Padding"
	StatusAuthOK             = 233
)

type AuthRequest struct {
	Auth string
	Rx   uint64
}

type AuthResponse struct {
	UDPEnabled bool
	Rx         uint64
	RxAuto     bool
}

func AuthRequestFromHeader(h http.Header) AuthRequest {
	rx, _ := strconv.ParseUint(h.Get(CommonHeaderCCRX), 10, 64)
	return AuthRequest{
		Auth: h.Get(RequestHeaderAuth),
		Rx:   rx,
	}
}

func AuthResponseToHeader(h http.Header, resp AuthResponse) {
	h.Set(ResponseHeaderUDPEnabled, strconv.FormatBool(resp.UDPEnabled))
	if resp.RxAuto {
		h.Set(CommonHeaderCCRX, "auto")
	} else {
		h.Set(CommonHeaderCCRX, strconv.FormatUint(resp.Rx, 10))
	}
	h.Set(CommonHeaderPadding, "ok")
}

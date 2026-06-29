package masq

import (
	"net/http"
	"net/http/httputil"
	"net/url"

	"github.com/local/xgw-edge/internal/config"
)

type Handler struct {
	cfg   config.MasqueradeConfig
	proxy *httputil.ReverseProxy
}

func New(cfg config.MasqueradeConfig) (*Handler, error) {
	h := &Handler{cfg: cfg}
	if cfg.UpstreamURL != "" {
		upstream, err := url.Parse(cfg.UpstreamURL)
		if err != nil {
			return nil, err
		}
		h.proxy = httputil.NewSingleHostReverseProxy(upstream)
	}
	return h, nil
}

func (h *Handler) ServeUnauthorized(w http.ResponseWriter, r *http.Request) {
	if h.cfg.UnauthorizedPassth && h.proxy != nil {
		h.proxy.ServeHTTP(w, r)
		return
	}
	if h.cfg.FakeServerHeader != "" {
		w.Header().Set("Server", h.cfg.FakeServerHeader)
	}
	if h.cfg.FakePoweredBy != "" {
		w.Header().Set("X-Powered-By", h.cfg.FakePoweredBy)
	}
	status := h.cfg.UnauthStatusCode
	if status == 0 {
		status = http.StatusOK
	}
	w.WriteHeader(status)
	_, _ = w.Write([]byte("<html><body><h1>Welcome</h1></body></html>"))
}

func (h *Handler) ServeFallback(w http.ResponseWriter, r *http.Request) {
	if h.proxy != nil {
		h.proxy.ServeHTTP(w, r)
		return
	}
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write([]byte("<html><body><h1>edge ok</h1></body></html>"))
}

package edge

import (
	"bytes"
	"context"
	"encoding/json"
	"net/http"
	"time"

	"github.com/local/xgw-edge/internal/control"
	"go.uber.org/zap"
)

func StartClientKeepalive(ctx context.Context,
	logger *zap.SugaredLogger,
	httpClient *http.Client,
	serverURL string,
	host string,
	path string,
	sessionID string,
	interval time.Duration) {
	if httpClient == nil || serverURL == "" || interval <= 0 {
		return
	}
	go func() {
		ticker := time.NewTicker(interval)
		defer ticker.Stop()
		for {
			select {
			case <-ctx.Done():
				return
			case <-ticker.C:
				payload, _ := json.Marshal(control.Keepalive{
					SessionID: sessionID,
				})
				req, err := http.NewRequestWithContext(ctx, http.MethodPost, serverURL+path+"/keepalive", bytes.NewReader(payload))
				if err != nil {
					logger.Warnf("keepalive build failed: %v", err)
					continue
				}
				req.Host = host
				req.Header.Set("Content-Type", "application/json")
				resp, err := httpClient.Do(req)
				if err != nil {
					logger.Warnf("keepalive send failed: %v", err)
					continue
				}
				_ = resp.Body.Close()
			}
		}
	}()
}

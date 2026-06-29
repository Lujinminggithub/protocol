package native

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"github.com/local/xgw-edge/internal/control"
)

// backendRouteController is the narrow control boundary from Go edge/control
// plane into the C data plane. Go decides the route; C owns prewarm/switch/drain.
type backendRouteController struct {
	path string
}

func newBackendRouteController(path string) *backendRouteController {
	path = strings.TrimSpace(path)
	if path == "" {
		return nil
	}
	return &backendRouteController{path: path}
}

func (c *backendRouteController) Prewarm(route control.RouteCandidate) error {
	line := routeControlLineID(route)
	if line == "" {
		return nil
	}
	return c.write("PREWARM_LINE", line)
}

func (c *backendRouteController) SetActive(route control.RouteCandidate) error {
	line := routeControlLineID(route)
	if line == "" {
		return nil
	}
	return c.write("SET_ACTIVE_LINE", line)
}

func (c *backendRouteController) Drain(route control.RouteCandidate) error {
	line := routeControlLineID(route)
	if line == "" {
		return nil
	}
	return c.write("DRAIN_LINE", line)
}

func (c *backendRouteController) write(op string, line string) error {
	if c == nil || c.path == "" {
		return nil
	}
	if err := os.MkdirAll(filepath.Dir(c.path), 0o755); err != nil {
		return err
	}
	tmp := c.path + ".tmp"
	if err := os.WriteFile(tmp, []byte(fmt.Sprintf("%s %s\n", op, line)), 0o644); err != nil {
		return err
	}
	return os.Rename(tmp, c.path)
}

func routeControlLineID(route control.RouteCandidate) string {
	if route.LineID != "" {
		return route.LineID
	}
	return route.Name
}

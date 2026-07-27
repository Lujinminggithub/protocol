package app

import (
	"bufio"
	"context"
	"errors"
	"fmt"
	"os"
	"strconv"
	"strings"

	"nb-controlplane/internal/store"
)

func dataLines(path string) ([]string, error) {
	if path == "" {
		return nil, nil
	}
	file, err := os.Open(path)
	if errors.Is(err, os.ErrNotExist) {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	defer file.Close()
	var lines []string
	scanner := bufio.NewScanner(file)
	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())
		if line != "" && !strings.HasPrefix(line, "#") {
			lines = append(lines, line)
		}
	}
	return lines, scanner.Err()
}

func parsePositive(value, field string) (int64, error) {
	parsed, err := strconv.ParseInt(value, 10, 64)
	if err != nil || parsed < 0 {
		return 0, fmt.Errorf("invalid %s", field)
	}
	return parsed, nil
}

func (a *App) Bootstrap(ctx context.Context) error {
	tenantLines, err := dataLines(a.cfg.TenantsFile)
	if err != nil {
		return err
	}
	authLines, err := dataLines(a.cfg.UsersFile)
	if err != nil {
		return err
	}
	credentials := make(map[string]store.Credential, len(authLines))
	for _, line := range authLines {
		parts := strings.Split(line, ":")
		if len(parts) != 4 || !safeID.MatchString(parts[0]) || len(parts[2]) != 32 || len(parts[3]) != 64 {
			return fmt.Errorf("invalid credential record for %q", parts[0])
		}
		iterations, parseErr := strconv.Atoi(parts[1])
		if parseErr != nil || iterations < 100000 || iterations > 1000000 {
			return fmt.Errorf("invalid credential iterations for %s", parts[0])
		}
		credentials[parts[0]] = store.Credential{ID: "imported-" + parts[0], Iterations: iterations, Salt: parts[2], Hash: parts[3]}
	}
	users := make([]store.User, 0, len(tenantLines))
	active := make(map[string]bool, len(tenantLines))
	for _, line := range tenantLines {
		parts := strings.Fields(line)
		if len(parts) != 6 || parts[0] != "tenant" || !safeID.MatchString(parts[1]) {
			return fmt.Errorf("invalid tenant record %q", line)
		}
		values := make([]int64, 4)
		for index := range values {
			values[index], err = parsePositive(parts[index+2], parts[1])
			if err != nil {
				return err
			}
		}
		users = append(users, store.User{ID: parts[1], Username: parts[1], Status: "active", Plan: "legacy",
			Route: "legacy", MaxTCP: values[0], MaxUDP: values[1], RateKbps: values[2], QuotaMB: values[3]})
		active[parts[1]] = true
	}
	for username := range credentials {
		if !active[username] {
			users = append(users, store.User{ID: username, Username: username, Status: "disabled",
				Plan: "legacy", Route: "legacy"})
		}
	}
	_, err = a.store.BootstrapUsers(ctx, users, credentials)
	return err
}

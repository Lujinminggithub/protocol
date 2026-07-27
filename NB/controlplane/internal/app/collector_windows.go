//go:build windows

package app

import "context"

func (a *App) RunCollector(ctx context.Context) { <-ctx.Done() }

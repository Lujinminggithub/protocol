package central

import (
	"context"
	"database/sql"
	"errors"
	"strings"
)

var ErrOperationActive = errors.New("执行中的任务不能清理")

var terminalOperationStatuses = map[string]bool{
	"succeeded":   true,
	"failed":      true,
	"cancelled":   true,
	"rolled_back": true,
}

func operationPlaceholders(count int) string {
	return strings.TrimSuffix(strings.Repeat("?,", count), ",")
}

func (s *Store) DeleteOperations(ctx context.Context, ids []string) (int64, error) {
	unique := make([]string, 0, len(ids))
	seen := make(map[string]bool, len(ids))
	for _, id := range ids {
		id = strings.TrimSpace(id)
		if id != "" && !seen[id] {
			seen[id] = true
			unique = append(unique, id)
		}
	}
	if len(unique) == 0 {
		return 0, errors.New("至少选择一个任务")
	}
	args := make([]any, len(unique))
	for index, id := range unique {
		args[index] = id
	}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return 0, err
	}
	defer tx.Rollback()
	rows, err := tx.QueryContext(ctx, `SELECT id,status FROM operations
	 WHERE NOT EXISTS (SELECT 1 FROM operation_cleanups cleanup WHERE cleanup.operation_id=operations.id)
	 AND id IN (`+operationPlaceholders(len(unique))+`)`, args...)
	if err != nil {
		return 0, err
	}
	found := 0
	for rows.Next() {
		var id, status string
		if err = rows.Scan(&id, &status); err != nil {
			_ = rows.Close()
			return 0, err
		}
		found++
		if !terminalOperationStatuses[status] {
			_ = rows.Close()
			return 0, ErrOperationActive
		}
	}
	if err = rows.Close(); err != nil {
		return 0, err
	}
	if found != len(unique) {
		return 0, sql.ErrNoRows
	}
	stamp := now()
	for _, id := range unique {
		if _, err = tx.ExecContext(ctx, `INSERT INTO operation_cleanups(operation_id,cleaned_at) VALUES(?,?)`, id, stamp); err != nil {
			return 0, errors.New("任务清理发生并发冲突")
		}
	}
	if err = tx.Commit(); err != nil {
		return 0, err
	}
	return int64(len(unique)), nil
}

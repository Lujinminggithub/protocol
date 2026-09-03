package central

import (
	"context"
	"time"
)

const (
	successOperationDetailRetention = 24 * time.Hour
	failureOperationDetailRetention = 30 * 24 * time.Hour
	rawEventRetention               = 7 * 24 * time.Hour
)

// PruneControlHistory removes verbose, reproducible detail while retaining the
// durable operation summary and final result used for audit and reconciliation.
func (s *Store) PruneControlHistory(ctx context.Context, nowAt time.Time) error {
	unlock := s.lockWrite()
	defer unlock()
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	if _, err = tx.ExecContext(ctx, `DELETE FROM operation_events WHERE operation_id IN (
 SELECT id FROM operations WHERE
  (status='succeeded' AND updated_at<?) OR
  (status IN ('failed','cancelled','rolled_back') AND updated_at<?))`,
		nowAt.Add(-successOperationDetailRetention).UTC().Format(time.RFC3339Nano),
		nowAt.Add(-failureOperationDetailRetention).UTC().Format(time.RFC3339Nano)); err != nil {
		return err
	}
	if _, err = tx.ExecContext(ctx, `DELETE FROM raw_events WHERE received_at<?`,
		nowAt.Add(-rawEventRetention).UTC().Format(time.RFC3339Nano)); err != nil {
		return err
	}
	if _, err = tx.ExecContext(ctx, `DELETE FROM user_sessions WHERE expires_at<?`, nowAt.UTC().Format(time.RFC3339Nano)); err != nil {
		return err
	}
	return tx.Commit()
}

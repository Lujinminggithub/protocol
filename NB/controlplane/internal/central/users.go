package central

import (
	"context"
	"database/sql"
	"errors"
	"time"
)

var ErrAuthentication = errors.New("authentication failed")

type UserSession struct {
	UserID             int64
	Username           string
	PasswordHash       []byte
	MustChangePassword bool
}

func (s *Store) EnsureInitialUser(ctx context.Context, username string, passwordHash []byte) error {
	stamp := now()
	query := s.controlSQL(`INSERT OR IGNORE INTO users
 (username,password_hash,must_change_password,status,created_at,updated_at) VALUES(?,?,1,'active',?,?)`,
		`INSERT IGNORE INTO users (username,password_hash,must_change_password,status,created_at,updated_at)
 VALUES(?,?,TRUE,'active',?,?)`)
	_, err := s.db.ExecContext(ctx, query, username, passwordHash, stamp, stamp)
	return err
}

func (s *Store) UserForLogin(ctx context.Context, username string) (UserSession, error) {
	var user UserSession
	var status string
	err := s.db.QueryRowContext(ctx, `SELECT id,username,password_hash,must_change_password,status
 FROM users WHERE username=?`, username).Scan(&user.UserID, &user.Username, &user.PasswordHash, &user.MustChangePassword, &status)
	if err != nil || status != "active" {
		return UserSession{}, ErrAuthentication
	}
	return user, nil
}

func (s *Store) CreateUserSession(ctx context.Context, userID int64, tokenHash []byte, expiresAt time.Time) error {
	stamp := now()
	query := s.controlSQL(`INSERT OR REPLACE INTO user_sessions
 (token_hash,user_id,expires_at,created_at,last_seen_at) VALUES(?,?,?,?,?)`,
		`INSERT INTO user_sessions (token_hash,user_id,expires_at,created_at,last_seen_at) VALUES(?,?,?,?,?)
 ON DUPLICATE KEY UPDATE user_id=VALUES(user_id),expires_at=VALUES(expires_at),last_seen_at=VALUES(last_seen_at)`)
	_, err := s.db.ExecContext(ctx, query, tokenHash, userID, expiresAt.UTC().Format(time.RFC3339Nano), stamp, stamp)
	return err
}

func (s *Store) UserBySession(ctx context.Context, tokenHash []byte, at time.Time) (UserSession, error) {
	var user UserSession
	var status string
	err := s.db.QueryRowContext(ctx, `SELECT users.id,users.username,users.password_hash,
 users.must_change_password,users.status FROM user_sessions
 JOIN users ON users.id=user_sessions.user_id
 WHERE user_sessions.token_hash=? AND user_sessions.expires_at>?`, tokenHash,
		at.UTC().Format(time.RFC3339Nano)).Scan(&user.UserID, &user.Username, &user.PasswordHash, &user.MustChangePassword, &status)
	if err != nil || status != "active" {
		return UserSession{}, ErrAuthentication
	}
	return user, nil
}

func (s *Store) ChangeUserPassword(ctx context.Context, userID int64, currentTokenHash, passwordHash []byte) error {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	result, err := tx.ExecContext(ctx, `UPDATE users SET password_hash=?,must_change_password=0,updated_at=? WHERE id=?`,
		passwordHash, now(), userID)
	if err != nil {
		return err
	}
	if changed, _ := result.RowsAffected(); changed != 1 {
		return sql.ErrNoRows
	}
	if _, err = tx.ExecContext(ctx, `DELETE FROM user_sessions WHERE user_id=? AND token_hash<>?`, userID, currentTokenHash); err != nil {
		return err
	}
	return tx.Commit()
}

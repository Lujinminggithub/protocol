package central

import (
	"context"
	"strings"
)

func (s *Store) controlSQL(sqliteQuery, mysqlQuery string) string {
	if s.dialect == "mysql" {
		return mysqlQuery
	}
	return sqliteQuery
}

func (s *Store) linesTable() string {
	if s.dialect == "mysql" {
		return "`lines`"
	}
	return "lines"
}

func (s *Store) migrateMySQL(ctx context.Context) error {
	_, err := s.db.ExecContext(ctx, `
CREATE TABLE IF NOT EXISTS `+"`lines`"+` (
 id VARCHAR(191) PRIMARY KEY, name VARCHAR(100) NOT NULL, status VARCHAR(32) NOT NULL,
 environment VARCHAR(32) NOT NULL DEFAULT 'production',
 entry_region VARCHAR(100) NOT NULL, exit_region VARCHAR(100) NOT NULL, provider VARCHAR(100) NOT NULL,
 capacity_mbps BIGINT NOT NULL, active_deployment VARCHAR(191) NOT NULL DEFAULT '',
 profile VARCHAR(191) NOT NULL DEFAULT '', secret_ref VARCHAR(191) NOT NULL DEFAULT '',
 created_at VARCHAR(40) NOT NULL, updated_at VARCHAR(40) NOT NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
CREATE TABLE IF NOT EXISTS incidents (
 id VARCHAR(191) PRIMARY KEY, line_id VARCHAR(191) NOT NULL, severity VARCHAR(32) NOT NULL,
 status VARCHAR(32) NOT NULL, kind VARCHAR(100) NOT NULL, message TEXT NOT NULL,
 observed_at VARCHAR(40) NOT NULL, payload MEDIUMBLOB NOT NULL,
 INDEX incidents_line(line_id,status,observed_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
CREATE TABLE IF NOT EXISTS operations (
 id VARCHAR(191) PRIMARY KEY, line_id VARCHAR(191) NOT NULL, kind VARCHAR(64) NOT NULL,
 status VARCHAR(32) NOT NULL, requested_by VARCHAR(191) NOT NULL,
 idempotency_key VARCHAR(191) NOT NULL UNIQUE, request MEDIUMBLOB NOT NULL, result MEDIUMBLOB NULL,
 created_at VARCHAR(40) NOT NULL, updated_at VARCHAR(40) NOT NULL,
 INDEX operations_ready(line_id,status,created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
CREATE TABLE IF NOT EXISTS operation_cleanups (
 operation_id VARCHAR(191) PRIMARY KEY, cleaned_at VARCHAR(40) NOT NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
CREATE TABLE IF NOT EXISTS executors (
 worker_id VARCHAR(191) PRIMARY KEY, status VARCHAR(32) NOT NULL, version VARCHAR(100) NOT NULL,
 capabilities MEDIUMBLOB NOT NULL, observed_at VARCHAR(40) NOT NULL, updated_at VARCHAR(40) NOT NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
CREATE TABLE IF NOT EXISTS devices (
 id VARCHAR(191) PRIMARY KEY, name VARCHAR(100) NOT NULL, status VARCHAR(32) NOT NULL,
 host VARCHAR(253) NOT NULL, ssh_port INT NOT NULL, ssh_user VARCHAR(100) NOT NULL,
 ssh_host_key TEXT NOT NULL, ssh_host_key_type VARCHAR(100) NOT NULL DEFAULT '',
 ssh_host_key_sha256 VARCHAR(191) NOT NULL DEFAULT '', ssh_host_key_status VARCHAR(32) NOT NULL DEFAULT 'pending',
 ssh_host_key_confirmed_at VARCHAR(40) NOT NULL DEFAULT '', private_ip VARCHAR(64) NOT NULL DEFAULT '',
 region VARCHAR(100) NOT NULL DEFAULT '', provider VARCHAR(100) NOT NULL DEFAULT '',
 os VARCHAR(100) NOT NULL DEFAULT '', arch VARCHAR(100) NOT NULL DEFAULT '', secret_ref VARCHAR(191) NOT NULL DEFAULT '',
 labels MEDIUMBLOB NOT NULL, last_health VARCHAR(32) NOT NULL DEFAULT 'unknown', last_seen_at VARCHAR(40) NOT NULL DEFAULT '',
 created_at VARCHAR(40) NOT NULL, updated_at VARCHAR(40) NOT NULL, environment VARCHAR(32) NOT NULL DEFAULT 'production',
 INDEX devices_status(status,region,name)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
CREATE TABLE IF NOT EXISTS topology_layouts (
 device_id VARCHAR(191) PRIMARY KEY, x DOUBLE NOT NULL, y DOUBLE NOT NULL, z DOUBLE NOT NULL,
 updated_by VARCHAR(191) NOT NULL, updated_at VARCHAR(40) NOT NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
CREATE TABLE IF NOT EXISTS line_specs (
 line_id VARCHAR(191) PRIMARY KEY, resource_group VARCHAR(191) NOT NULL, instance_id VARCHAR(191) NOT NULL,
 bandwidth_mbps BIGINT NOT NULL, upstream_mbps BIGINT NOT NULL, downstream_mbps BIGINT NOT NULL,
 socks_port INT NOT NULL, udp_port_min INT NOT NULL, udp_port_max INT NOT NULL,
 relay_port INT NOT NULL, exit_port INT NOT NULL, exit_bind_ip VARCHAR(64) NOT NULL DEFAULT '',
 dns_servers MEDIUMBLOB NOT NULL, whitelist MEDIUMBLOB NOT NULL, build_mode VARCHAR(32) NOT NULL,
 artifact_ref TEXT NOT NULL, source_ref TEXT NOT NULL, srs_ref TEXT NOT NULL, jump_policy VARCHAR(64) NOT NULL,
 created_at VARCHAR(40) NOT NULL, updated_at VARCHAR(40) NOT NULL, environment VARCHAR(32) NOT NULL DEFAULT 'production'
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
CREATE TABLE IF NOT EXISTS line_port_allocation (
 line_id VARCHAR(191) PRIMARY KEY, socks_port_auto BOOLEAN NOT NULL DEFAULT FALSE,
 relay_port_auto BOOLEAN NOT NULL DEFAULT FALSE, exit_port_auto BOOLEAN NOT NULL DEFAULT FALSE,
 udp_ports_auto BOOLEAN NOT NULL DEFAULT FALSE, updated_at VARCHAR(40) NOT NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
CREATE TABLE IF NOT EXISTS runtime_port_claims (
 worker_id VARCHAR(191) NOT NULL, device_id VARCHAR(191) NOT NULL, role VARCHAR(32) NOT NULL,
 resource_kind VARCHAR(32) NOT NULL, instance_id VARCHAR(191) NOT NULL,
 port_start INT NOT NULL, port_end INT NOT NULL, observed_at VARCHAR(40) NOT NULL,
 expires_at VARCHAR(40) NOT NULL, source VARCHAR(32) NOT NULL,
 PRIMARY KEY(worker_id,device_id,role,resource_kind,instance_id,port_start,port_end),
 INDEX runtime_port_claims_lookup(device_id,role,resource_kind,port_start,port_end)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
CREATE TABLE IF NOT EXISTS line_nodes (
 line_id VARCHAR(191) NOT NULL, device_id VARCHAR(191) NOT NULL, role VARCHAR(32) NOT NULL,
 ordinal INT NOT NULL, next_hop_device_id VARCHAR(191) NOT NULL DEFAULT '',
 jump_candidates MEDIUMBLOB NOT NULL, config MEDIUMBLOB NOT NULL,
 PRIMARY KEY(line_id,role,ordinal), INDEX line_nodes_device(device_id,line_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
CREATE TABLE IF NOT EXISTS network_links (
 id VARCHAR(191) PRIMARY KEY,
 from_device_id VARCHAR(191) NOT NULL, from_role VARCHAR(32) NOT NULL,
 to_device_id VARCHAR(191) NOT NULL, to_role VARCHAR(32) NOT NULL,
 forward_capacity_mbps BIGINT NOT NULL, reverse_capacity_mbps BIGINT NOT NULL,
 billing_mode VARCHAR(32) NOT NULL, environment VARCHAR(32) NOT NULL, status VARCHAR(32) NOT NULL,
 created_at VARCHAR(40) NOT NULL, updated_at VARCHAR(40) NOT NULL,
 UNIQUE KEY network_links_identity(from_device_id,from_role,to_device_id,to_role),
 INDEX network_links_endpoints(from_device_id,from_role,to_device_id,to_role)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
CREATE TABLE IF NOT EXISTS line_capacity_reservations (
 line_id VARCHAR(191) NOT NULL, link_id VARCHAR(191) NOT NULL,
 forward_mbps BIGINT NOT NULL, reverse_mbps BIGINT NOT NULL,
 state VARCHAR(32) NOT NULL, operation_id VARCHAR(191) NOT NULL,
 created_at VARCHAR(40) NOT NULL, updated_at VARCHAR(40) NOT NULL,
 PRIMARY KEY(line_id,link_id),
 INDEX line_capacity_reservations_link(link_id,state,line_id),
 INDEX line_capacity_reservations_operation(operation_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
CREATE TABLE IF NOT EXISTS line_qualifications (
 operation_id VARCHAR(191) PRIMARY KEY, line_id VARCHAR(191) NOT NULL, deployment_id VARCHAR(191) NOT NULL,
 target_upstream_mbps DOUBLE NOT NULL, target_downstream_mbps DOUBLE NOT NULL,
 achieved_upstream_mbps DOUBLE NOT NULL, achieved_downstream_mbps DOUBLE NOT NULL,
 duration_seconds INT NOT NULL, required_ratio DOUBLE NOT NULL,
 status VARCHAR(32) NOT NULL, reasons MEDIUMBLOB NOT NULL, evidence MEDIUMBLOB NOT NULL,
 created_at VARCHAR(40) NOT NULL,
 INDEX line_qualifications_latest(line_id,created_at,operation_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
CREATE TABLE IF NOT EXISTS operation_events (
 id BIGINT AUTO_INCREMENT PRIMARY KEY, operation_id VARCHAR(191) NOT NULL, sequence INT NOT NULL,
 stage VARCHAR(64) NOT NULL, status VARCHAR(32) NOT NULL, message TEXT NOT NULL,
 parameters MEDIUMBLOB NOT NULL, created_at VARCHAR(40) NOT NULL,
 UNIQUE KEY operation_events_unique(operation_id,sequence), INDEX operation_events_order(operation_id,sequence)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
CREATE TABLE IF NOT EXISTS raw_events (
 id BIGINT AUTO_INCREMENT PRIMARY KEY, path VARCHAR(191) NOT NULL,
 idempotency_key VARCHAR(191) NOT NULL UNIQUE, payload MEDIUMBLOB NOT NULL, received_at VARCHAR(40) NOT NULL,
 INDEX raw_events_path(path,received_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
CREATE TABLE IF NOT EXISTS line_deletion_audit (
 id BIGINT AUTO_INCREMENT PRIMARY KEY, line_id VARCHAR(191) NOT NULL, line_name VARCHAR(100) NOT NULL,
 requested_by VARCHAR(191) NOT NULL, reason TEXT NOT NULL, snapshot MEDIUMBLOB NOT NULL,
 deleted_at VARCHAR(40) NOT NULL, INDEX line_deletion_audit_line(line_id,deleted_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
CREATE TABLE IF NOT EXISTS line_deletion_requests (
 line_id VARCHAR(191) PRIMARY KEY, operation_id VARCHAR(191) NOT NULL UNIQUE,
 requested_by VARCHAR(191) NOT NULL, reason TEXT NOT NULL, previous_status VARCHAR(32) NOT NULL,
 created_at VARCHAR(40) NOT NULL, INDEX line_deletion_requests_operation(operation_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
CREATE TABLE IF NOT EXISTS line_deletion_completions (
 operation_id VARCHAR(191) PRIMARY KEY, line_id VARCHAR(191) NOT NULL,
 completed_at VARCHAR(40) NOT NULL, INDEX line_deletion_completions_line(line_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
CREATE TABLE IF NOT EXISTS transport_generations (
 line_id VARCHAR(191) PRIMARY KEY, current_generation BIGINT UNSIGNED NOT NULL, updated_at VARCHAR(40) NOT NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
CREATE TABLE IF NOT EXISTS users (
 id BIGINT AUTO_INCREMENT PRIMARY KEY, username VARCHAR(100) NOT NULL UNIQUE,
 password_hash VARBINARY(255) NOT NULL, must_change_password BOOLEAN NOT NULL DEFAULT TRUE,
 status VARCHAR(32) NOT NULL DEFAULT 'active', created_at VARCHAR(40) NOT NULL, updated_at VARCHAR(40) NOT NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
CREATE TABLE IF NOT EXISTS user_sessions (
 token_hash BINARY(32) PRIMARY KEY, user_id BIGINT NOT NULL, expires_at VARCHAR(40) NOT NULL,
 created_at VARCHAR(40) NOT NULL, last_seen_at VARCHAR(40) NOT NULL, INDEX user_sessions_user(user_id,expires_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
`)
	if err != nil {
		return err
	}
	for _, statement := range []string{
		`ALTER TABLE ` + "`lines`" + ` ADD COLUMN environment VARCHAR(32) NOT NULL DEFAULT 'production'`,
		`ALTER TABLE devices ADD COLUMN environment VARCHAR(32) NOT NULL DEFAULT 'production'`,
		`ALTER TABLE line_specs ADD COLUMN environment VARCHAR(32) NOT NULL DEFAULT 'production'`,
	} {
		if _, alterErr := s.db.ExecContext(ctx, statement); alterErr != nil && !strings.Contains(strings.ToLower(alterErr.Error()), "duplicate") {
			return alterErr
		}
	}
	_, err = s.db.ExecContext(ctx, `UPDATE devices SET environment='production' WHERE environment IS NULL OR environment=''`)
	if err != nil {
		return err
	}
	_, err = s.db.ExecContext(ctx, "UPDATE `lines` SET environment='production' WHERE environment IS NULL OR environment=''")
	if err != nil {
		return err
	}
	_, err = s.db.ExecContext(ctx, `UPDATE line_specs SET environment='production' WHERE environment IS NULL OR environment=''`)
	return err
}

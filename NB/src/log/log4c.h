/*
 * log4c.h - Public API header for log4c (Log for C)
 *
 * A cross-platform, thread-safe logging library with:
 *   - 4 log levels (DEBUG, INFO, WARN, ERROR)
 *   - JSON configuration file support (log4c.json)
 *   - Hot reload of log level and runtime-configurable settings
 *   - Automatic log file rotation at configurable size
 *   - Directory cleanup when total size exceeds configurable threshold
 *   - DEBUG logs include source filename, line number, and function name
 *
 * Usage:
 *   #include "log4c.h"
 *
 *   log4c_init("myapp");
 *   log4c_debug("debug message");
 *   log4c_info("info message");
 *   log4c_warn("warn message");
 *   log4c_error("error message");
 *   log4c_shutdown();
 *
 * Configuration (log4c.json in executable directory):
 *   {
 *     "log_dir": "custom/logs/path",
 *     "log_filename": "myapp.log",
 *     "initial_level": "DEBUG",
 *     "rotate_size_mb": 50,
 *     "cleanup_size_mb": 2000,
 *     "cleanup_every_n": 20,
 *     "hot_reload_interval_ms": 2000,
 *     "level_file_name": "level.txt",
 *     "include_source_info": true,
 *     "timestamp_format": "%Y-%m-%dT%H:%M:%S.%03dZ"
 *   }
 *
 * Build:
 *   gcc -std=c11 -pthread -o myapp myapp.c log4c.c
 *   cl /std:c11 /W4 myapp.c log4c.c
 */

#ifndef LOG4C_H
#define LOG4C_H

#ifdef __cplusplus
extern "C" {
#endif

/* ── Log Levels ─────────────────────────────────────────────── */
#define LOG4C_DEBUG 0
#define LOG4C_INFO  1
#define LOG4C_WARN  2
#define LOG4C_ERROR 3

/* ── Public API ─────────────────────────────────────────────── */

/**
 * Initialize the logging system.
 * Creates the logs/ directory alongside the executable (or uses config).
 * Loads log4c.json config if present.
 * Starts the hot-reload poller thread.
 * Must be called before any logging function.
 *
 * @param process_name  The executable name (e.g. "myapp"). Used as prefix
 *                      for log filenames: myapp.log, myapp_20260627_153000.log
 *                      Log timestamps are emitted in UTC with a trailing "Z".
 */
void log4c_init(const char* process_name);

/**
 * Shut down the logging system.
 * Flushes pending writes, closes the log file, joins the hot-reload thread.
 * Must be called at program exit.
 */
void log4c_shutdown(void);

/**
 * Log a DEBUG message.
 * Includes source file basename, line number, and function name in output.
 */
void log4c_debug(const char* fmt, ...);

/**
 * Log an INFO message.
 */
void log4c_info(const char* fmt, ...);

/**
 * Log a WARN message.
 */
void log4c_warn(const char* fmt, ...);

/**
 * Log an ERROR message.
 */
void log4c_error(const char* fmt, ...);

/**
 * Low-level log function. Callers should use the convenience functions above.
 *
 * @param level    One of LOG4C_DEBUG, LOG4C_INFO, LOG4C_WARN, LOG4C_ERROR
 * @param file     Source filename (typically __FILE__)
 * @param line     Source line number (typically __LINE__)
 * @param func     Function name (typically __func__)
 * @param fmt      printf-style format string
 */
void log4c_log(int level, const char* file, int line, const char* func,
               const char* fmt, ...);

/**
 * Get the current log level.
 * @return Current level (LOG4C_DEBUG, LOG4C_INFO, LOG4C_WARN, LOG4C_ERROR)
 */
int log4c_get_level(void);

/**
 * Set the log level directly (bypasses file-based hot reload temporarily).
 * @param level  One of LOG4C_DEBUG, LOG4C_INFO, LOG4C_WARN, LOG4C_ERROR
 */
void log4c_set_level(int level);

/**
 * Load and apply a JSON configuration file.
 * The config file is expected at {exe_dir}/log4c.json.
 *
 * This function is called automatically by log4c_init(). It can also be
 * called manually to load a config from a custom path.
 *
 * @param config_path  Absolute path to the JSON config file, or NULL to
 *                     use the default location ({exe_dir}/log4c.json).
 * @return             0 on success, -1 on error (file not found is not an error).
 */
int log4c_load_config(const char* config_path);

/**
 * Apply runtime-hot-reloadable settings from a parsed config snippet.
 * Called internally during hot-reload when config values change.
 * Only applies settings that are safe to change at runtime (level, rotate_size,
 * cleanup_size, cleanup_every, hot_reload_interval, include_source_info).
 *
 * @param config_path  Path to the config file that was read (used for hot-reload).
 * @return             0 on success, -1 on error.
 */
int log4c_apply_config(const char* config_path);

#ifdef __cplusplus
}
#endif

#endif /* LOG4C_H */

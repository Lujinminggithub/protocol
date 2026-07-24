/*
 * log4c.c - Implementation for log4c (Log for C)
 *
 * Thread-safe, cross-platform logging with JSON configuration,
 * hot-reload, file rotation, and automatic directory cleanup.
 *
 * Compile:
 *   Linux:  gcc -std=c11 -pthread -O2 -o log4c.o -c log4c.c
 *   Windows: cl /std:c11 /W4 log4c.c
 */

#define LOG4C_IMPLEMENTATION 1

/* 暴露 POSIX 接口(readlink/usleep 等);需在任何标准头之前定义。 */
#ifndef _WIN32
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200809L
#  endif
#endif

#include "log4c.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>
#include <ctype.h>
#include <sys/stat.h>

/* ── Platform Detection ─────────────────────────────────────── */
#if defined(_WIN32) || defined(_WIN64)
  #define LOG4C_WIN 1
  #define LOG4C_POSIX 0
#else
  #define LOG4C_WIN 0
  #define LOG4C_POSIX 1
#endif

/* ── Platform Headers ───────────────────────────────────────── */
#if LOG4C_WIN
  #include <windows.h>
  #include <direct.h>
  #include <io.h>
#else
  #include <pthread.h>
  #include <dirent.h>
  #include <unistd.h>
  #include <libgen.h>
  #include <sys/time.h>
#endif

/* ── Atomics ────────────────────────────────────────────────── */
#include <stdatomic.h>

/* ── Constants ──────────────────────────────────────────────── */
#define LOG4C_MAX_PATH_LEN  1024
#define LOG4C_NAME_LEN      64
#define LOG4C_LINE_BUF      8192

/* ── JSON Config Parser ─────────────────────────────────────── */
/* Minimal flat-key-value JSON parser. Handles strings,
 * integers, booleans, comments (// and block), and whitespace. */

#define LOG4C_CFG_MAX_KEYS  16

typedef enum {
    CFG_NONE,
    CFG_STRING,
    CFG_INT,
    CFG_BOOL
} cfg_type_t;

typedef struct {
    char key[64];
    cfg_type_t type;
    union {
        char str_val[LOG4C_MAX_PATH_LEN];
        int  int_val;
        int  bool_val;
    } val;
} log4c_cfg_entry_t;

typedef struct {
    log4c_cfg_entry_t entries[LOG4C_CFG_MAX_KEYS];
    int count;
} log4c_config_t;

/* Skip whitespace and comments */
static const char* log4c_cfg_skip(const char* p)
{
    while (*p) {
        if (*p == '/' && *(p+1) == '/') {
            while (*p && *p != '\n') p++;
        } else if (*p == '/' && *(p+1) == '*') {
            p += 2;
            while (*p && !(*p == '*' && *(p+1) == '/')) p++;
            if (*p) p += 2;
        } else if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
            p++;
        } else {
            break;
        }
    }
    return p;
}

/* Parse a JSON string value, advance p past closing quote. Returns new position. */
static const char* log4c_cfg_parse_str(const char* p, char* out, size_t out_size)
{
    if (*p != '"') { if (out) out[0] = '\0'; return p; }
    p++; /* skip opening quote */
    size_t i = 0;
    while (*p && *p != '"' && i < out_size - 1) {
        if (*p == '\\' && *(p+1)) {
            p++;
            switch (*p) {
                case '"':  out[i++] = '"'; break;
                case '\\': out[i++] = '\\'; break;
                case '/':  out[i++] = '/'; break;
                case 'n':  out[i++] = '\n'; break;
                case 't':  out[i++] = '\t'; break;
                case 'r':  out[i++] = '\r'; break;
                default:   out[i++] = *p; break;
            }
        } else {
            out[i++] = *p;
        }
        p++;
    }
    out[i] = '\0';
    if (*p == '"') p++;
    return p;
}

/* Parse a JSON config from text into the config struct. Returns 0 on success. */
static int log4c_cfg_parse(const char* json, log4c_config_t* cfg)
{
    cfg->count = 0;
    const char* p = log4c_cfg_skip(json);

    if (*p != '{') return -1;
    p++; /* skip '{' */

    while (*p) {
        p = log4c_cfg_skip(p);
        if (*p == '}') break;
        if (*p == ',') { p++; continue; }

        /* Parse key */
        char key[64] = {0};
        if (*p != '"') return -1;
        p = log4c_cfg_parse_str(p, key, sizeof(key));

        p = log4c_cfg_skip(p);
        if (*p != ':') return -1;
        p++;
        p = log4c_cfg_skip(p);

        /* Parse value */
        if (cfg->count >= LOG4C_CFG_MAX_KEYS) return -1;
        log4c_cfg_entry_t* entry = &cfg->entries[cfg->count++];
        snprintf(entry->key, sizeof(entry->key), "%s", key);

        if (*p == '"') {
            entry->type = CFG_STRING;
            p = log4c_cfg_parse_str(p, entry->val.str_val, sizeof(entry->val.str_val));
        } else if (*p == 't') {
            entry->type = CFG_BOOL;
            entry->val.bool_val = 1;
            p += 4; /* "true" */
        } else if (*p == 'f') {
            entry->type = CFG_BOOL;
            entry->val.bool_val = 0;
            p += 5; /* "false" */
        } else if (isdigit((unsigned char)*p) || *p == '-') {
            entry->type = CFG_INT;
            entry->val.int_val = atoi(p);
            while (*p && !isspace((unsigned char)*p) && *p != ',' && *p != '}') p++;
        } else {
            p++; /* skip unknown token */
            continue;
        }

        p = log4c_cfg_skip(p);
    }
    return 0;
}

/* Look up a config value by key. Returns entry pointer or NULL. */
static const log4c_cfg_entry_t* log4c_cfg_find(const log4c_config_t* cfg, const char* key)
{
    for (int i = 0; i < cfg->count; i++) {
        if (strcmp(cfg->entries[i].key, key) == 0)
            return &cfg->entries[i];
    }
    return NULL;
}

/* Free config resources (none needed for flat storage, but for consistency) */
static void log4c_cfg_free(log4c_config_t* cfg)
{
    cfg->count = 0;
}

/* ── Runtime Config State ───────────────────────────────────── */

/* Configurable thresholds (updated at runtime via hot-reload) */
typedef struct {
    unsigned long long  rotate_size;       /* bytes */
    unsigned long long  cleanup_size;      /* bytes */
    int                 cleanup_every_n;
    int                 hot_reload_interval_ms;
    int                 include_source_info;
    char                timestamp_format[64];
} log4c_runtime_cfg_t;

static log4c_runtime_cfg_t g_runtime_cfg = {
    .rotate_size        = 10ULL * 1024 * 1024,   /* 10 MB */
    .cleanup_size       = 500ULL * 1024 * 1024,  /* 500 MB */
    .cleanup_every_n    = 10,
    .hot_reload_interval_ms = 1000,
    .include_source_info = 1,
    .timestamp_format   = "%Y-%m-%dT%H:%M:%S.%03dZ"
};

/* ── Internal State ─────────────────────────────────────────── */
typedef struct {
    _Atomic int         level;           /* current log level (atomic) */
    char                process_name[LOG4C_NAME_LEN];
    char                log_dir[LOG4C_MAX_PATH_LEN];
    char                log_file_path[LOG4C_MAX_PATH_LEN];
    char                log_filename[LOG4C_NAME_LEN];  /* configurable filename stem */
    FILE*               fp;              /* current log file handle */
    unsigned long long  file_size;       /* current file size in bytes */
    int                 write_count;     /* writes since last cleanup check */
    _Atomic int         shutdown;        /* graceful shutdown flag */
    char                level_file_name[64];  /* hot-reload config file name */

#if LOG4C_WIN
    CRITICAL_SECTION    mtx;
    HANDLE              reload_thread;
#else
    pthread_mutex_t     mtx;
    pthread_t           reload_thread;
#endif
} log4c_state_t;

/* Global singleton state - zero-initialized at startup */
static log4c_state_t g_state;

/* ── Helpers: Platform-Independent ──────────────────────────── */

/* Get basename from a full path */
static const char* log4c_basename(const char* path)
{
    if (!path || !*path) return path;
    const char* last = path;
    for (; *path; ++path) {
        if (*path == '/' || *path == '\\') last = path + 1;
    }
    return last;
}

/* Get millisecond component of current time */
static int log4c_current_ms(void)
{
#if LOG4C_WIN
    return (int)(GetTickCount64() % 1000);
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int)(ts.tv_nsec / 1000000L);
#endif
}

/* Convert current wall-clock time to UTC broken-down time */
static void log4c_gmtime_utc(time_t now, struct tm* out_tm)
{
#if LOG4C_WIN
    gmtime_s(out_tm, &now);
#else
    gmtime_r(&now, out_tm);
#endif
}

/* Format current timestamp into buffer using UTC time, independent of machine local timezone. */
static void log4c_timestamp(char* buf, size_t buflen)
{
    time_t now = time(NULL);
    struct tm tm_buf;
    log4c_gmtime_utc(now, &tm_buf);

    int ms = log4c_current_ms();
    snprintf(buf, buflen, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
             tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
             tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, ms);
}

/* Map level enum to string */
static const char* log4c_level_str(int level)
{
    switch (level) {
        case LOG4C_DEBUG: return "DEBUG";
        case LOG4C_INFO:  return "INFO";
        case LOG4C_WARN:  return "WARN";
        case LOG4C_ERROR: return "ERROR";
        default:          return "UNKNOWN";
    }
}

/* Parse level string from file content */
static int log4c_parse_level(const char* s)
{
    if (!s) return LOG4C_INFO;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') ++s;
    if (strncmp(s, "DEBUG", 5) == 0 && !isalnum((unsigned char)s[5])) return LOG4C_DEBUG;
    if (strncmp(s, "INFO", 4) == 0  && !isalnum((unsigned char)s[4]))  return LOG4C_INFO;
    if (strncmp(s, "WARN", 4) == 0  && !isalnum((unsigned char)s[4]))  return LOG4C_WARN;
    if (strncmp(s, "ERROR", 5) == 0 && !isalnum((unsigned char)s[5])) return LOG4C_ERROR;
    return LOG4C_INFO;
}

/* ── Helpers: Directory Creation ────────────────────────────── */

static int log4c_mkdir(const char* path)
{
#if LOG4C_WIN
    return _mkdir(path);
#else
    return mkdir(path, 0755);
#endif
}

/* ── Helpers: Executable Directory Resolution ───────────────── */

static void log4c_get_exe_dir(char* buf, size_t buflen)
{
#if LOG4C_WIN
    HMODULE hMod = GetModuleHandleW(NULL);
    if (!hMod) { strcpy(buf, "."); return; }

    wchar_t wpath[MAX_PATH];
    if (!GetModuleFileNameW(hMod, wpath, MAX_PATH)) { strcpy(buf, "."); return; }

    /* Convert wide-char path to UTF-8 */
    int utf8_len = WideCharToMultiByte(CP_UTF8, 0, wpath, -1, NULL, 0, NULL, NULL);
    if (utf8_len <= 0) { strcpy(buf, "."); return; }

    char* full = (char*)malloc((size_t)utf8_len + 16);
    if (!full) { strcpy(buf, "."); return; }
    WideCharToMultiByte(CP_UTF8, 0, wpath, -1, full, utf8_len, NULL, NULL);

    /* Strip executable name to get directory */
    char* last_sep = strrchr(full, '\\');
    if (last_sep) *last_sep = '\0';
    strncpy(buf, full, buflen - 1);
    buf[buflen - 1] = '\0';
    free(full);
#else
    char tmp[LOG4C_MAX_PATH_LEN];
    ssize_t len = readlink("/proc/self/exe", tmp, sizeof(tmp) - 1);
    if (len > 0) {
        tmp[len] = '\0';
        char* last_sep = strrchr(tmp, '/');
        if (last_sep) *last_sep = '\0';
        size_t copy_len=strlen(tmp);if(copy_len>=buflen)copy_len=buflen-1;
        memcpy(buf,tmp,copy_len);buf[copy_len]='\0';
    } else {
        strcpy(buf, ".");
    }
#endif
}

/* ── Helpers: File Operations ───────────────────────────────── */

static unsigned long long log4c_get_file_size(const char* path)
{
#if LOG4C_WIN
    /* Convert UTF-8 path to wide-char for _wstat64 */
    wchar_t wpath[LOG4C_MAX_PATH_LEN];
    int len = MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, (int)sizeof(wpath)/sizeof(wchar_t));
    if (len > 0) {
        struct __stat64 st;
        if (_wstat64(wpath, &st) == 0)
            return (unsigned long long)st.st_size;
    }
    /* Fallback: try stat() directly (works for ASCII paths) */
    {
        struct stat st2;
        return (stat(path, &st2) == 0) ? (unsigned long long)st2.st_size : 0;
    }
#else
    struct stat st;
    return (stat(path, &st) == 0) ? (unsigned long long)st.st_size : 0;
#endif
}

/* ── Log File Rotation ─────────────────────────────────────── */

static void log4c_rotate_file(void)
{
    if (!g_state.fp || !g_state.log_file_path[0]) return;

    /* Flush and close */
    fflush(g_state.fp);
    fclose(g_state.fp);
    g_state.fp = NULL;

    /* Build rotated filename with UTC timestamp so rotated files match log line time basis. */
    char ts_buf[32];
    time_t now = time(NULL);
    struct tm tm_buf;
    log4c_gmtime_utc(now, &tm_buf);

    snprintf(ts_buf, sizeof(ts_buf), "%04d%02d%02d_%02d%02d%02d",
             tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
             tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);

    char rotated_name[LOG4C_MAX_PATH_LEN];
    int rn = snprintf(rotated_name, sizeof(rotated_name), "%s/%s_%s.log",
                      g_state.log_dir, g_state.process_name, ts_buf);
    if (rn < 0 || (size_t) rn >= sizeof(rotated_name)) {
        rotated_name[sizeof(rotated_name) - 1] = '\0';
    }

    /* Rename current log to rotated name */
#if LOG4C_WIN
    /* Windows: RemoveFile + MoveFile for reliable rename */
    wchar_t w_src[LOG4C_MAX_PATH_LEN], w_dst[MAX_PATH];
    int len = MultiByteToWideChar(CP_UTF8, 0, g_state.log_file_path, -1, w_src, sizeof(w_src)/sizeof(wchar_t));
    (void)len;
    len = MultiByteToWideChar(CP_UTF8, 0, rotated_name, -1, w_dst, sizeof(w_dst)/sizeof(wchar_t));
    (void)len;
    /* Try to remove destination if it exists */
    DeleteFileW(w_dst);
    MoveFileW(w_src, w_dst);
#else
    unlink(rotated_name);
    rename(g_state.log_file_path, rotated_name);
#endif

    /* Reset file size tracking */
    g_state.file_size = 0;

    /* Open new log file */
    g_state.fp = fopen(g_state.log_file_path, "a");
}

/* ── Directory Cleanup ──────────────────────────────────────── */

typedef struct {
    char path[LOG4C_MAX_PATH_LEN];
    time_t mtime;
    unsigned long long size;
} log4c_file_entry_t;

static void log4c_cleanup_directory(void)
{
    if (g_state.log_dir[0] == '\0') return;

    log4c_file_entry_t* files = NULL;
    int count = 0;
    int capacity = 32;
    files = (log4c_file_entry_t*)calloc((size_t)capacity, sizeof(log4c_file_entry_t));
    if (!files) return;

#if LOG4C_WIN
    /* Windows directory scan using FindFirstFile/FindNextFile */
    char pattern[LOG4C_MAX_PATH_LEN];
    int pn = snprintf(pattern, sizeof(pattern), "%s\\%s*.log",
                      g_state.log_dir, g_state.process_name);
    if (pn < 0 || (size_t) pn >= sizeof(pattern)) {
        pattern[sizeof(pattern) - 1] = '\0';
    }

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const char* name = fd.cFileName;

            /* Skip if not matching our process name pattern */
            if (strncmp(name, g_state.process_name, strlen(g_state.process_name)) != 0)
                continue;

            /* Must end with .log */
            const char* dotlog = strstr(name, ".log");
            if (!dotlog || dotlog[4] != '\0')
                continue;

            /* Skip the current log file (no underscore after process name) */
            char current_name[LOG4C_NAME_LEN + 8];
            snprintf(current_name, sizeof(current_name), "%s.log", g_state.process_name);
            if (strcmp(name, current_name) == 0)
                continue;

            /* Build full path */
            char fpath[LOG4C_MAX_PATH_LEN];
            int fpn = snprintf(fpath, sizeof(fpath), "%s\\%s", g_state.log_dir, name);
            if (fpn < 0 || (size_t) fpn >= sizeof(fpath)) {
                fpath[sizeof(fpath) - 1] = '\0';
            }

            /* Get file info */
            struct __stat64 st;
            if (_stat64(fpath, &st) != 0)
                continue;

            /* Expand array if needed */
            if (count >= capacity) {
                capacity *= 2;
                log4c_file_entry_t* tmp = (log4c_file_entry_t*)realloc(
                    files, (size_t)capacity * sizeof(log4c_file_entry_t));
                if (!tmp) { free(files); return; }
                files = tmp;
            }

            size_t path_len = strlen(fpath);
            if (path_len >= sizeof(files[count].path)) path_len = sizeof(files[count].path) - 1;
            memcpy(files[count].path, fpath, path_len);
            files[count].path[path_len] = '\0';
            files[count].mtime = st.st_mtime;
            files[count].size = (unsigned long long)st.st_size;
            count++;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    /* POSIX directory scan using opendir/readdir */
    DIR* dir = opendir(g_state.log_dir);
    if (!dir) { free(files); return; }

    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
        const char* name = ent->d_name;

        /* Skip if not matching our process name pattern */
        if (strncmp(name, g_state.process_name, strlen(g_state.process_name)) != 0)
            continue;

        /* Must end with .log */
        const char* dotlog = strstr(name, ".log");
        if (!dotlog || dotlog[4] != '\0')
            continue;

        /* Skip the current log file */
        char current_name[LOG4C_NAME_LEN * 2];
        snprintf(current_name, sizeof(current_name), "%.*s.log",
                 (int) (LOG4C_NAME_LEN - 1), g_state.process_name);
        if (strcmp(name, current_name) == 0)
            continue;

        /* Build full path */
        char fpath[LOG4C_MAX_PATH_LEN * 2];
        snprintf(fpath, sizeof(fpath), "%s/%s", g_state.log_dir, name);

        /* Get file info */
        struct stat st;
        if (stat(fpath, &st) != 0)
            continue;

        /* Expand array if needed */
        if (count >= capacity) {
            capacity *= 2;
            log4c_file_entry_t* tmp = (log4c_file_entry_t*)realloc(
                files, (size_t)capacity * sizeof(log4c_file_entry_t));
            if (!tmp) { free(files); closedir(dir); return; }
            files = tmp;
        }

        size_t path_len = strlen(fpath);
        if (path_len >= sizeof(files[count].path)) path_len = sizeof(files[count].path) - 1;
        memcpy(files[count].path, fpath, path_len);
        files[count].path[path_len] = '\0';
        files[count].mtime = st.st_mtime;
        files[count].size = (unsigned long long)st.st_size;
        count++;
    }
    closedir(dir);
#endif

    /* Sort by mtime ascending (oldest first) using insertion sort */
    for (int i = 1; i < count; i++) {
        log4c_file_entry_t key = files[i];
        int j = i - 1;
        while (j >= 0 && files[j].mtime > key.mtime) {
            files[j + 1] = files[j];
            j--;
        }
        files[j + 1] = key;
    }

    /* Calculate total size */
    unsigned long long total = 0;
    for (int i = 0; i < count; i++) total += files[i].size;

    /* Delete oldest files until under limit */
    int idx = 0;
    while (total > g_runtime_cfg.cleanup_size && idx < count) {
#if LOG4C_WIN
        DeleteFileA(files[idx].path);
#else
        unlink(files[idx].path);
#endif
        total -= files[idx].size;
        idx++;
    }

    free(files);
}

/* ── Ensure Log File Is Open ────────────────────────────────── */

static void log4c_ensure_open(void)
{
    if (g_state.fp) return;

    /* Build log directory path alongside executable */
    if (g_state.log_dir[0] == '\0') {
        char exe_dir[LOG4C_MAX_PATH_LEN];
        log4c_get_exe_dir(exe_dir, sizeof(exe_dir));

        /* Use configured log_dir if set, otherwise default to {exe_dir}/logs */
        if (g_state.log_filename[0] != '\0') {
            /* log_filename was set via config, meaning log_dir is also set */
            /* log_dir was pre-populated by config loader */
        } else {
            int dn = snprintf(g_state.log_dir, sizeof(g_state.log_dir), "%s/logs", exe_dir);
            if (dn < 0 || (size_t) dn >= sizeof(g_state.log_dir)) {
                g_state.log_dir[sizeof(g_state.log_dir) - 1] = '\0';
            }
        }
        log4c_mkdir(g_state.log_dir);
    }

    /* Build log file path */
    if (g_state.log_filename[0]) {
        /* Custom filename from config */
        int fn = snprintf(g_state.log_file_path, sizeof(g_state.log_file_path),
                          "%s/%s", g_state.log_dir, g_state.log_filename);
        if (fn < 0 || (size_t) fn >= sizeof(g_state.log_file_path)) {
            g_state.log_file_path[sizeof(g_state.log_file_path) - 1] = '\0';
        }
    } else {
        /* Default: {process_name}.log */
        int fn = snprintf(g_state.log_file_path, sizeof(g_state.log_file_path),
                          "%s/%s.log", g_state.log_dir, g_state.process_name);
        if (fn < 0 || (size_t) fn >= sizeof(g_state.log_file_path)) {
            g_state.log_file_path[sizeof(g_state.log_file_path) - 1] = '\0';
        }
    }

    g_state.fp = fopen(g_state.log_file_path, "a");
    if (g_state.fp) {
        g_state.file_size = log4c_get_file_size(g_state.log_file_path);
    }
}

/* ── Core: Write a Single Log Line ──────────────────────────── */

static void log4c_write_line(int level, const char* file, int line,
                              const char* func, const char* fmt, va_list ap)
{
    /* Atomically check log level threshold (before locking) */
    int current = atomic_load(&g_state.level);
    if (level < current) return;

#if LOG4C_WIN
    EnterCriticalSection(&g_state.mtx);
#else
    pthread_mutex_lock(&g_state.mtx);
#endif

    /* Ensure log file is open (inside mutex for thread safety) */
    log4c_ensure_open();
    if (!g_state.fp) {
#if LOG4C_WIN
        LeaveCriticalSection(&g_state.mtx);
#else
        pthread_mutex_unlock(&g_state.mtx);
#endif
        return;
    }

    /* Check if rotation is needed */
    if (g_state.file_size >= g_runtime_cfg.rotate_size) {
        log4c_rotate_file();
        if (!g_state.fp) {
#if LOG4C_WIN
            LeaveCriticalSection(&g_state.mtx);
#else
            pthread_mutex_unlock(&g_state.mtx);
#endif
            return;
        }
    }

    /* Format the log line */
    char line_buf[LOG4C_LINE_BUF];
    char ts_buf[64];
    log4c_timestamp(ts_buf, sizeof(ts_buf));
    const char* level_name = log4c_level_str(level);

    /* Source info inclusion is configurable */
    int include_src = g_runtime_cfg.include_source_info;

    if (level == LOG4C_DEBUG && include_src) {
        const char* bn = log4c_basename(file);
        snprintf(line_buf, sizeof(line_buf),
                 "[%s] [%s] [%s:%d %s] ",
                 ts_buf, level_name, bn, line, func);
    } else {
        snprintf(line_buf, sizeof(line_buf),
                 "[%s] [%s] ", ts_buf, level_name);
    }

    /* Append formatted message */
    va_list ap2;
    va_copy(ap2, ap);
    int remaining = (int)sizeof(line_buf) - (int)strlen(line_buf) - 2;
    if (remaining < 0) remaining = 0;
    vsnprintf(line_buf + strlen(line_buf), (size_t)remaining, fmt, ap2);
    va_end(ap2);

    /* Add newline if present */
    size_t len = strlen(line_buf);
    if (len < sizeof(line_buf) - 2) {
        line_buf[len] = '\n';
        line_buf[len + 1] = '\0';
    }

    /* Write to file */
    unsigned long long written = (unsigned long long)strlen(line_buf);
    fwrite(line_buf, 1, written, g_state.fp);
    fflush(g_state.fp);

    /* Update tracked file size */
    g_state.file_size += written;

    /* Check for periodic cleanup */
    g_state.write_count++;
    if (g_state.write_count >= g_runtime_cfg.cleanup_every_n) {
        g_state.write_count = 0;

#if LOG4C_WIN
        LeaveCriticalSection(&g_state.mtx);
#else
        pthread_mutex_unlock(&g_state.mtx);
#endif

        log4c_cleanup_directory();

        /* Re-open log file in case cleanup deleted it */
        log4c_ensure_open();
        return;
    }

#if LOG4C_WIN
    LeaveCriticalSection(&g_state.mtx);
#else
    pthread_mutex_unlock(&g_state.mtx);
#endif
}

/* ── Hot-Reload Thread ──────────────────────────────────────── */

static void log4c_apply_parsed_config(const log4c_config_t* cfg)
{
    /* Apply level */
    const log4c_cfg_entry_t* e;
    e = log4c_cfg_find(cfg, "initial_level");
    if (e && e->type == CFG_STRING) {
        int new_level = log4c_parse_level(e->val.str_val);
        int cur = atomic_load(&g_state.level);
        if (new_level != cur) {
            atomic_store(&g_state.level, new_level);
        }
    }

    /* Apply rotate_size_mb */
    e = log4c_cfg_find(cfg, "rotate_size_mb");
    if (e && e->type == CFG_INT && e->val.int_val > 0) {
        g_runtime_cfg.rotate_size = (unsigned long long)e->val.int_val * 1024ULL * 1024ULL;
    }

    /* Apply cleanup_size_mb */
    e = log4c_cfg_find(cfg, "cleanup_size_mb");
    if (e && e->type == CFG_INT && e->val.int_val > 0) {
        g_runtime_cfg.cleanup_size = (unsigned long long)e->val.int_val * 1024ULL * 1024ULL;
    }

    /* Apply cleanup_every_n */
    e = log4c_cfg_find(cfg, "cleanup_every_n");
    if (e && e->type == CFG_INT && e->val.int_val > 0) {
        g_runtime_cfg.cleanup_every_n = e->val.int_val;
    }

    /* Apply include_source_info */
    e = log4c_cfg_find(cfg, "include_source_info");
    if (e && e->type == CFG_BOOL) {
        g_runtime_cfg.include_source_info = e->val.bool_val;
    }
}

#if LOG4C_WIN
static DWORD WINAPI log4c_hot_reload_thread(LPVOID param)
{
    (void)param;
    while (!atomic_load_explicit(&g_state.shutdown, memory_order_acquire)) {
        Sleep(g_runtime_cfg.hot_reload_interval_ms);

        char level_path[LOG4C_MAX_PATH_LEN];
        int lpn = snprintf(level_path, sizeof(level_path), "%s\\%s",
                           g_state.log_dir, g_state.level_file_name);
        if (lpn < 0 || (size_t) lpn >= sizeof(level_path)) {
            level_path[sizeof(level_path) - 1] = '\0';
        }

        FILE* fp = fopen(level_path, "r");
        if (fp) {
            char buf[512];
            if (fgets(buf, (int)sizeof(buf), fp)) {
                /* Try parsing as full JSON config first */
                log4c_config_t cfg;
                if (log4c_cfg_parse(buf, &cfg) == 0 && cfg.count > 0) {
                    log4c_apply_parsed_config(&cfg);
                    log4c_cfg_free(&cfg);
                } else {
                    /* Fall back to plain level name */
                    int new_level = log4c_parse_level(buf);
                    int current = atomic_load(&g_state.level);
                    if (new_level != current) {
                        atomic_store(&g_state.level, new_level);
                    }
                }
            }
            fclose(fp);
        }
    }
    return 0;
}
#else
static void* log4c_hot_reload_thread(void* param)
{
    (void)param;
    while (!atomic_load_explicit(&g_state.shutdown, memory_order_acquire)) {
        /* usleep 在 POSIX.1-2008 已移除,用 nanosleep(标准 <time.h>,无 feature macro 依赖)。 */
        {
            int ms = g_runtime_cfg.hot_reload_interval_ms;
            struct timespec ts;
            ts.tv_sec = ms / 1000;
            ts.tv_nsec = (long) (ms % 1000) * 1000000L;
            nanosleep(&ts, NULL);
        }

        char level_path[LOG4C_MAX_PATH_LEN * 2];
        snprintf(level_path, sizeof(level_path), "%s/%s",
                 g_state.log_dir, g_state.level_file_name);

        FILE* fp = fopen(level_path, "r");
        if (fp) {
            char buf[512];
            if (fgets(buf, sizeof(buf), fp)) {
                /* Try parsing as full JSON config first */
                log4c_config_t cfg;
                if (log4c_cfg_parse(buf, &cfg) == 0 && cfg.count > 0) {
                    log4c_apply_parsed_config(&cfg);
                    log4c_cfg_free(&cfg);
                } else {
                    /* Fall back to plain level name */
                    int new_level = log4c_parse_level(buf);
                    int current = atomic_load(&g_state.level);
                    if (new_level != current) {
                        atomic_store(&g_state.level, new_level);
                    }
                }
            }
            fclose(fp);
        }
    }
    return NULL;
}
#endif

/* ── Config Loading ─────────────────────────────────────────── */

/* Read entire file into a null-terminated buffer. Returns NULL on failure. */
static char* log4c_read_file(const char* path)
{
    FILE* fp = fopen(path, "r");
    if (!fp) return NULL;

    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (sz <= 0 || sz > 65536) { fclose(fp); return NULL; }

    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return NULL; }

    size_t nread = fread(buf, 1, (size_t)sz, fp);
    buf[nread] = '\0';
    fclose(fp);
    return buf;
}

/* Load config from a known path into g_state and g_runtime_cfg */
static int log4c_do_load_config(const char* config_path)
{
    char* content = log4c_read_file(config_path);
    if (!content) return -1;  /* File not found or unreadable -- not an error */

    log4c_config_t cfg;
    if (log4c_cfg_parse(content, &cfg) != 0) {
        free(content);
        return -1;
    }

    /* Apply initial_level to set the starting log level */
    const log4c_cfg_entry_t* e;
    e = log4c_cfg_find(&cfg, "initial_level");
    if (e && e->type == CFG_STRING) {
        int new_level = log4c_parse_level(e->val.str_val);
        atomic_store(&g_state.level, new_level);
    }

    /* Apply rotate_size_mb */
    e = log4c_cfg_find(&cfg, "rotate_size_mb");
    if (e && e->type == CFG_INT && e->val.int_val > 0) {
        g_runtime_cfg.rotate_size = (unsigned long long)e->val.int_val * 1024ULL * 1024ULL;
    }

    /* Apply cleanup_size_mb */
    e = log4c_cfg_find(&cfg, "cleanup_size_mb");
    if (e && e->type == CFG_INT && e->val.int_val > 0) {
        g_runtime_cfg.cleanup_size = (unsigned long long)e->val.int_val * 1024ULL * 1024ULL;
    }

    /* Apply cleanup_every_n */
    e = log4c_cfg_find(&cfg, "cleanup_every_n");
    if (e && e->type == CFG_INT && e->val.int_val > 0) {
        g_runtime_cfg.cleanup_every_n = e->val.int_val;
    }

    /* Apply hot_reload_interval_ms */
    e = log4c_cfg_find(&cfg, "hot_reload_interval_ms");
    if (e && e->type == CFG_INT && e->val.int_val > 0) {
        g_runtime_cfg.hot_reload_interval_ms = e->val.int_val;
    }

    /* Apply level_file_name */
    e = log4c_cfg_find(&cfg, "level_file_name");
    if (e && e->type == CFG_STRING) {
        strncpy(g_state.level_file_name, e->val.str_val, sizeof(g_state.level_file_name) - 1);
        g_state.level_file_name[sizeof(g_state.level_file_name) - 1] = '\0';
    }

    /* Apply include_source_info */
    e = log4c_cfg_find(&cfg, "include_source_info");
    if (e && e->type == CFG_BOOL) {
        g_runtime_cfg.include_source_info = e->val.bool_val;
    }

    /* Apply log_dir (only effective if log_dir not yet set) */
    e = log4c_cfg_find(&cfg, "log_dir");
    if (e && e->type == CFG_STRING && e->val.str_val[0]) {
        strncpy(g_state.log_dir, e->val.str_val, sizeof(g_state.log_dir) - 1);
        g_state.log_dir[sizeof(g_state.log_dir) - 1] = '\0';
    }

    /* Apply log_filename (custom filename stem, only effective at init time) */
    e = log4c_cfg_find(&cfg, "log_filename");
    if (e && e->type == CFG_STRING && e->val.str_val[0]) {
        strncpy(g_state.log_filename, e->val.str_val, sizeof(g_state.log_filename) - 1);
        g_state.log_filename[sizeof(g_state.log_filename) - 1] = '\0';
    }

    log4c_cfg_free(&cfg);
    free(content);
    return 0;
}

/* ── Public API ─────────────────────────────────────────────── */

void log4c_init(const char* process_name)
{
    /* Guard against double initialization */
    static _Atomic int inited = 0;
    if (atomic_exchange(&inited, 1)) return; /* already initialized */
    if (!process_name) return;

    /* Zero-initialize the global state */
    memset(&g_state, 0, sizeof(g_state));
    atomic_store(&g_state.level, LOG4C_DEBUG);
    strncpy(g_state.process_name, process_name, LOG4C_NAME_LEN - 1);
    g_state.process_name[LOG4C_NAME_LEN - 1] = '\0';
    atomic_store(&g_state.shutdown, 0);
    strncpy(g_state.level_file_name, "level.txt", sizeof(g_state.level_file_name) - 1);
    g_state.level_file_name[sizeof(g_state.level_file_name) - 1] = '\0';

    /* Attempt to load config from default location */
    char exe_dir[LOG4C_MAX_PATH_LEN];
    log4c_get_exe_dir(exe_dir, sizeof(exe_dir));
    char config_path[LOG4C_MAX_PATH_LEN];
    int cn = snprintf(config_path, sizeof(config_path), "%s/cfg/log4c.json", exe_dir);
    if (cn < 0 || (size_t) cn >= sizeof(config_path)) {
        config_path[sizeof(config_path) - 1] = '\0';
    }
    log4c_do_load_config(config_path);

#if LOG4C_WIN
    InitializeCriticalSection(&g_state.mtx);
    g_state.reload_thread = CreateThread(NULL, 0,
                                          log4c_hot_reload_thread, NULL, 0, NULL);
#else
    pthread_mutex_init(&g_state.mtx, NULL);
    pthread_create(&g_state.reload_thread, NULL, log4c_hot_reload_thread, NULL);
#endif
}

void log4c_shutdown(void)
{
    /* Signal the hot-reload thread to stop */
    atomic_store_explicit(&g_state.shutdown, 1, memory_order_release);

#if LOG4C_WIN
    if (g_state.reload_thread && g_state.reload_thread != INVALID_HANDLE_VALUE) {
        WaitForSingleObject(g_state.reload_thread, 3000);
        CloseHandle(g_state.reload_thread);
    }
    DeleteCriticalSection(&g_state.mtx);
#else
    /* The hot-reload thread sleeps and checks shutdown atomically.
     * pthread_join will return within the interval after the flag is set. */
    if (g_state.reload_thread) {
        pthread_join(g_state.reload_thread, NULL);
    }
    pthread_mutex_destroy(&g_state.mtx);
#endif

    /* Flush and close the log file */
    if (g_state.fp) {
        fflush(g_state.fp);
        fclose(g_state.fp);
        g_state.fp = NULL;
    }
}

void log4c_debug(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log4c_write_line(LOG4C_DEBUG, __FILE__, __LINE__, __func__, fmt, ap);
    va_end(ap);
}

void log4c_info(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log4c_write_line(LOG4C_INFO, __FILE__, __LINE__, __func__, fmt, ap);
    va_end(ap);
}

void log4c_warn(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log4c_write_line(LOG4C_WARN, __FILE__, __LINE__, __func__, fmt, ap);
    va_end(ap);
}

void log4c_error(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log4c_write_line(LOG4C_ERROR, __FILE__, __LINE__, __func__, fmt, ap);
    va_end(ap);
}

void log4c_log(int level, const char* file, int line, const char* func,
               const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log4c_write_line(level, file, line, func, fmt, ap);
    va_end(ap);
}

int log4c_get_level(void)
{
    return atomic_load(&g_state.level);
}

void log4c_set_level(int level)
{
    if (level >= LOG4C_DEBUG && level <= LOG4C_ERROR) {
        atomic_store(&g_state.level, level);
    }
}

int log4c_load_config(const char* config_path)
{
    char default_path[LOG4C_MAX_PATH_LEN];
    if (!config_path) {
        /* Default: find log4c.json next to executable */
        char exe_dir[LOG4C_MAX_PATH_LEN];
        int n;
        log4c_get_exe_dir(exe_dir, sizeof(exe_dir));
        n = snprintf(default_path, sizeof(default_path), "%s/log4c.json", exe_dir);
        if (n < 0 || (size_t) n >= sizeof(default_path)) {
            default_path[sizeof(default_path) - 1] = '\0';
        }
        config_path = default_path;
    }
    return log4c_do_load_config(config_path);
}

int log4c_apply_config(const char* config_path)
{
    (void)config_path;  /* config_path is informational for hot-reload */

    /* Read the config file */
    const char* path = config_path;
    if (!path) {
        char exe_dir[LOG4C_MAX_PATH_LEN / 2];
        char buf[LOG4C_MAX_PATH_LEN];
        log4c_get_exe_dir(exe_dir, sizeof(exe_dir));
        /* exe_dir(<=512) + "/log4c.json"(11) 必 < buf(1024),截断在数学上不可能 */
        snprintf(buf, sizeof(buf), "%s/log4c.json", exe_dir);
        path = buf;
    }

    char* content = log4c_read_file(path);
    if (!content) return -1;

    log4c_config_t cfg;
    if (log4c_cfg_parse(content, &cfg) != 0) {
        free(content);
        return -1;
    }

    /* Apply only runtime-hot-reloadable fields */
    const log4c_cfg_entry_t* e;

    /* Apply initial_level (log level) */
    e = log4c_cfg_find(&cfg, "initial_level");
    if (e && e->type == CFG_STRING) {
        int new_level = log4c_parse_level(e->val.str_val);
        int cur = atomic_load(&g_state.level);
        if (new_level != cur) {
            atomic_store(&g_state.level, new_level);
        }
    }

    /* Apply rotate_size_mb */
    e = log4c_cfg_find(&cfg, "rotate_size_mb");
    if (e && e->type == CFG_INT && e->val.int_val > 0) {
        g_runtime_cfg.rotate_size = (unsigned long long)e->val.int_val * 1024ULL * 1024ULL;
    }

    /* Apply cleanup_size_mb */
    e = log4c_cfg_find(&cfg, "cleanup_size_mb");
    if (e && e->type == CFG_INT && e->val.int_val > 0) {
        g_runtime_cfg.cleanup_size = (unsigned long long)e->val.int_val * 1024ULL * 1024ULL;
    }

    /* Apply cleanup_every_n */
    e = log4c_cfg_find(&cfg, "cleanup_every_n");
    if (e && e->type == CFG_INT && e->val.int_val > 0) {
        g_runtime_cfg.cleanup_every_n = e->val.int_val;
    }

    /* Apply hot_reload_interval_ms */
    e = log4c_cfg_find(&cfg, "hot_reload_interval_ms");
    if (e && e->type == CFG_INT && e->val.int_val > 0) {
        g_runtime_cfg.hot_reload_interval_ms = e->val.int_val;
    }

    /* Apply include_source_info */
    e = log4c_cfg_find(&cfg, "include_source_info");
    if (e && e->type == CFG_BOOL) {
        g_runtime_cfg.include_source_info = e->val.bool_val;
    }

    log4c_cfg_free(&cfg);
    free(content);
    return 0;
}

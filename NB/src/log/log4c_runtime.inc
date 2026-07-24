
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

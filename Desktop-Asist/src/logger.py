"""
应用日志 - 落盘的滚动日志文件

供 main.py 的致命异常兜底、以及 Hermes 面板"日志查看"读取使用。
"""

import logging
import logging.handlers
import os

from src.paths import LOG_DIR, LOG_FILE

_logger = None


def get_logger() -> logging.Logger:
    """获取全局应用日志器(单例)。"""
    global _logger
    if _logger is not None:
        return _logger

    os.makedirs(LOG_DIR, exist_ok=True)

    logger = logging.getLogger("aisprite")
    logger.setLevel(logging.INFO)

    if not logger.handlers:
        handler = logging.handlers.RotatingFileHandler(
            LOG_FILE, maxBytes=2 * 1024 * 1024, backupCount=3, encoding="utf-8"
        )
        handler.setFormatter(
            logging.Formatter("%(asctime)s [%(levelname)s] %(name)s: %(message)s")
        )
        logger.addHandler(handler)

    _logger = logger
    return logger

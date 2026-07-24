"""
配置管理 - 多 Provider 设置与 Token 持久化

支持多个 AI 供应商(DeepSeek / Agnes AI)各自独立的 API Key/模型等设置，
用 active_provider 标记 AI助手聊天面板(以及 AIGC 文本类任务)当前使用哪一个。
API Key 使用 Windows DPAPI 透明加解密存储。
"""

import copy
import json
import os

from src.paths import CONFIG_FILE, ensure_user_data_dir
from src import crypto_utils

# 2026-07-24 起停用的旧模型名 -> 新模型名 迁移映射
_LEGACY_MODEL_MIGRATION = {
    "deepseek-chat": "deepseek-v4-flash",
    "deepseek-reasoner": "deepseek-v4-flash",
}

# 各 Provider 的默认设置
_PROVIDER_DEFAULTS = {
    "deepseek": {
        "api_base_url": "https://api.deepseek.com/v1",
        "api_key": "",
        "api_key_encrypted": False,
        "model": "deepseek-v4-flash",
        "thinking_enabled": True,  # 深度思考模式(展示推理过程)
        "reasoning_effort": "high",  # "high" | "max"
    },
    "agnes": {
        "api_base_url": "https://apihub.agnes-ai.com/v1",
        "api_key": "",
        "api_key_encrypted": False,
        "model": "agnes-2.0-flash",
    },
}

# 各"工具"(Function Calling里需要独立Key的第三方服务)的默认设置。
# 与 _PROVIDER_DEFAULTS 分开维护：这些不是对话后端，没有 model/thinking 等语义，
# 混进 providers 会污染 build_provider() 的语义边界。
_TOOL_DEFAULTS = {
    "tavily": {
        "api_key": "",
        "api_key_encrypted": False,
    },
    "qweather": {
        # 和风天气新版控制台采用"项目专属域名"，没有固定公共Host，
        # 需要用户从控制台复制自己的 API Host(如 https://xxxxx.qweatherapi.com)
        "api_host": "",
        "api_key": "",
        "api_key_encrypted": False,
    },
}


class Config:
    """本地配置文件管理器。"""

    DEFAULTS = {
        "active_provider": "deepseek",  # AI助手聊天面板 / AIGC文本类任务 使用的 provider
        "providers": copy.deepcopy(_PROVIDER_DEFAULTS),
        "tools": copy.deepcopy(_TOOL_DEFAULTS),
        "video_pipeline": {
            "default_voice": "zh-CN-XiaoxiaoNeural",
            "default_scene_count": 6,  # 0 = 自动
            "narration_mode": "voice_subtitles",
            "enable_narration": True,
            "burn_subtitles": True,
            "max_concurrent_scenes": 3,
            "enable_multimodal_planning": True,
            "prompt_debug_mode": True,
        },
        "temperature": 0.7,
        "max_tokens": 2048,
        "cleanup_mode": "safe",  # safe: 只清 tmp, aggressive: 全部
    }

    def __init__(self):
        self._data = copy.deepcopy(self.DEFAULTS)
        self._load()

    def _load(self):
        ensure_user_data_dir()
        """从本地文件加载配置，并做透明迁移(旧扁平结构/旧模型名/明文Key)。"""
        migrated = False
        saved = {}

        if os.path.exists(CONFIG_FILE):
            try:
                with open(CONFIG_FILE, "r", encoding="utf-8") as f:
                    saved = json.load(f) or {}
            except (json.JSONDecodeError, IOError):
                saved = {}

        if saved:
            # 迁移0: 旧版扁平配置(单一 provider = deepseek) -> 新的 providers 结构
            if "providers" not in saved and "api_key" in saved:
                legacy = dict(_PROVIDER_DEFAULTS["deepseek"])
                legacy.update(
                    {
                        k: saved[k]
                        for k in (
                            "api_base_url",
                            "api_key",
                            "api_key_encrypted",
                            "model",
                            "thinking_enabled",
                            "reasoning_effort",
                        )
                        if k in saved
                    }
                )
                saved = {
                    "active_provider": "deepseek",
                    "providers": {"deepseek": legacy},
                    "temperature": saved.get("temperature", 0.7),
                    "max_tokens": saved.get("max_tokens", 2048),
                    "cleanup_mode": saved.get("cleanup_mode", "safe"),
                }
                migrated = True

            # 合并: providers/tools 需要逐个条目深度合并，不能整体覆盖
            for key, value in saved.items():
                if key == "providers" and isinstance(value, dict):
                    for pid, pconf in value.items():
                        base = dict(_PROVIDER_DEFAULTS.get(pid, {}))
                        base.update(pconf)
                        self._data.setdefault("providers", {})[pid] = base
                elif key == "tools" and isinstance(value, dict):
                    for tid, tconf in value.items():
                        base = dict(_TOOL_DEFAULTS.get(tid, {}))
                        base.update(tconf)
                        self._data.setdefault("tools", {})[tid] = base
                elif key == "video_pipeline" and isinstance(value, dict):
                    self._data.setdefault("video_pipeline", {}).update(value)
                else:
                    self._data[key] = value

        # 迁移1: 旧模型名(2026-07-24停用)自动映射到新模型
        ds = self._data.get("providers", {}).get("deepseek", {})
        if ds.get("model") in _LEGACY_MODEL_MIGRATION:
            ds["model"] = _LEGACY_MODEL_MIGRATION[ds["model"]]
            migrated = True

        # 迁移2: 明文 api_key -> DPAPI 密文(逐个 provider 检查)
        for pid, pconf in self._data.get("providers", {}).items():
            if pconf.get("api_key") and not pconf.get("api_key_encrypted"):
                pconf["api_key"] = crypto_utils.encrypt(pconf["api_key"])
                pconf["api_key_encrypted"] = True
                migrated = True

        # 迁移3: 工具Key同样做明文 -> DPAPI 密文迁移
        for tid, tconf in self._data.get("tools", {}).items():
            if tconf.get("api_key") and not tconf.get("api_key_encrypted"):
                tconf["api_key"] = crypto_utils.encrypt(tconf["api_key"])
                tconf["api_key_encrypted"] = True
                migrated = True

        if migrated:
            self.save()

    def save(self):
        """保存配置到本地文件。"""
        ensure_user_data_dir()
        with open(CONFIG_FILE, "w", encoding="utf-8") as f:
            json.dump(self._data, f, ensure_ascii=False, indent=2)

    def get(self, key, default=None):
        return self._data.get(key, default)

    def set(self, key, value):
        self._data[key] = value

    # ===== 多 Provider 设置 =====

    def get_provider_settings(self, provider_id: str) -> dict:
        """返回指定 provider 的设置(api_key 已解密为明文)。"""
        pconf = self._data.get("providers", {}).get(provider_id)
        if pconf is None:
            pconf = dict(_PROVIDER_DEFAULTS.get(provider_id, {}))
        result = dict(pconf)
        raw_key = result.get("api_key", "")
        if result.get("api_key_encrypted"):
            result["api_key"] = crypto_utils.decrypt(raw_key)
        return result

    def set_provider_settings(self, provider_id: str, **kwargs):
        """更新指定 provider 的设置；kwargs 里的 api_key 会自动加密存储。"""
        providers = self._data.setdefault("providers", {})
        pconf = providers.setdefault(provider_id, dict(_PROVIDER_DEFAULTS.get(provider_id, {})))
        if "api_key" in kwargs:
            pconf["api_key"] = crypto_utils.encrypt(kwargs.pop("api_key"))
            pconf["api_key_encrypted"] = True
        pconf.update(kwargs)
        self.save()

    def has_api_key(self, provider_id: str = None) -> bool:
        """检查指定(或当前 active)provider 是否已配置 API Key。"""
        pid = provider_id or self.active_provider
        return bool(self.get_provider_settings(pid).get("api_key", "").strip())

    # ===== 工具(Function Calling里需要独立Key的第三方服务)设置 =====

    def get_tool_settings(self, tool_id: str) -> dict:
        """返回指定工具的设置(api_key 已解密为明文)。"""
        tconf = self._data.get("tools", {}).get(tool_id)
        if tconf is None:
            tconf = dict(_TOOL_DEFAULTS.get(tool_id, {}))
        result = dict(tconf)
        raw_key = result.get("api_key", "")
        if result.get("api_key_encrypted"):
            result["api_key"] = crypto_utils.decrypt(raw_key)
        return result

    def set_tool_settings(self, tool_id: str, **kwargs):
        """更新指定工具的设置；kwargs 里的 api_key 会自动加密存储。"""
        tools = self._data.setdefault("tools", {})
        tconf = tools.setdefault(tool_id, dict(_TOOL_DEFAULTS.get(tool_id, {})))
        if "api_key" in kwargs:
            tconf["api_key"] = crypto_utils.encrypt(kwargs.pop("api_key"))
            tconf["api_key_encrypted"] = True
        tconf.update(kwargs)
        self.save()

    def has_tool_key(self, tool_id: str) -> bool:
        """检查指定工具是否已配置 API Key。"""
        return bool(self.get_tool_settings(tool_id).get("api_key", "").strip())

    # ===== 剧本成片流水线设置 =====

    def get_video_pipeline_settings(self) -> dict:
        base = dict(self.DEFAULTS["video_pipeline"])
        base.update(self._data.get("video_pipeline", {}))
        return base

    def set_video_pipeline_settings(self, **kwargs):
        vp = self._data.setdefault("video_pipeline", dict(self.DEFAULTS["video_pipeline"]))
        vp.update(kwargs)
        self.save()

    @property
    def active_provider(self):
        return self._data.get("active_provider", self.DEFAULTS["active_provider"])

    @active_provider.setter
    def active_provider(self, value):
        self._data["active_provider"] = value
        self.save()

    # ===== 通用设置(跨 provider 共享) =====

    @property
    def temperature(self):
        return self._data.get("temperature", self.DEFAULTS["temperature"])

    @temperature.setter
    def temperature(self, value):
        self._data["temperature"] = value
        self.save()

    @property
    def max_tokens(self):
        return self._data.get("max_tokens", self.DEFAULTS["max_tokens"])

    @max_tokens.setter
    def max_tokens(self, value):
        self._data["max_tokens"] = value
        self.save()

    @property
    def cleanup_mode(self):
        return self._data.get("cleanup_mode", self.DEFAULTS["cleanup_mode"])

    @cleanup_mode.setter
    def cleanup_mode(self, value):
        self._data["cleanup_mode"] = value
        self.save()

    def reset(self):
        """重置为默认配置。"""
        self._data = copy.deepcopy(self.DEFAULTS)
        self.save()


# 全局单例
_config_instance = None


def get_config():
    """获取全局配置单例。"""
    global _config_instance
    if _config_instance is None:
        _config_instance = Config()
    return _config_instance

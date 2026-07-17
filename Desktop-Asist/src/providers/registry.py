"""
Provider 注册表 - 为后续扩展多AI供应商预留统一入口。
"""

from src.providers.deepseek import DeepSeekProvider
from src.providers.agnes import AgnesProvider

PROVIDERS = {
    "deepseek": DeepSeekProvider,
    "agnes": AgnesProvider,
}


def get_provider_class(provider_id: str):
    """按 provider_id 查找对应的 Provider 类，找不到则回退到 DeepSeek。"""
    return PROVIDERS.get(provider_id, DeepSeekProvider)


def build_provider(provider_id: str, cfg):
    """按配置构建指定 provider 的实例，供各面板复用。"""
    settings = cfg.get_provider_settings(provider_id)
    cls = get_provider_class(provider_id)
    kwargs = dict(
        base_url=settings.get("api_base_url", ""),
        api_key=settings.get("api_key", ""),
        model=settings.get("model", ""),
        temperature=cfg.temperature,
        max_tokens=cfg.max_tokens,
    )
    if provider_id == "deepseek":
        kwargs["thinking_enabled"] = settings.get("thinking_enabled", True)
        kwargs["reasoning_effort"] = settings.get("reasoning_effort", "high")
    return cls(**kwargs)


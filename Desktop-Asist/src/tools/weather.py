"""
天气查询工具

地理编码统一用 Open-Meteo(免费，无需Key)把城市名转成经纬度；
若用户配置了和风天气(QWeather) Key，优先用它查实时天气(国内城市数据
明显更准，已实测徐州同一坐标 Open-Meteo 报27℃、百度报36℃，偏差近10度)；
未配置或调用失败时，自动静默回退到 Open-Meteo 的天气预报接口。
"""

import json

import requests

from src.config import get_config

_GEOCODING_URL = "https://geocoding-api.open-meteo.com/v1/search"
_FORECAST_URL = "https://api.open-meteo.com/v1/forecast"

# WMO 天气代码 -> 中文描述(节选常见值)
_WEATHER_CODE_MAP = {
    0: "晴朗", 1: "大致晴朗", 2: "局部多云", 3: "阴天",
    45: "雾", 48: "雾凇",
    51: "小毛毛雨", 53: "中等毛毛雨", 55: "大毛毛雨",
    56: "冻毛毛雨", 57: "强冻毛毛雨",
    61: "小雨", 63: "中雨", 65: "大雨",
    66: "冻雨", 67: "强冻雨",
    71: "小雪", 73: "中雪", 75: "大雪", 77: "雪粒",
    80: "小阵雨", 81: "中阵雨", 82: "强阵雨",
    85: "小阵雪", 86: "大阵雪",
    95: "雷暴", 96: "雷暴伴小冰雹", 99: "雷暴伴大冰雹",
}


def _describe_weather_code(code) -> str:
    try:
        return _WEATHER_CODE_MAP.get(int(code), f"未知天气代码({code})")
    except (TypeError, ValueError):
        return "未知"


def _geocode(city: str):
    """城市名 -> (纬度, 经度, 解析出的地名)，失败抛异常。"""
    resp = requests.get(
        _GEOCODING_URL,
        params={"name": city, "count": 1, "language": "zh", "format": "json"},
        timeout=10,
    )
    resp.raise_for_status()
    results = resp.json().get("results") or []
    if not results:
        raise ValueError(f"未找到城市: {city}")
    loc = results[0]
    return loc.get("latitude"), loc.get("longitude"), loc.get("name", city)


def _query_qweather(api_host: str, api_key: str, lat, lon, resolved_name: str) -> dict:
    """查和风天气实时天气(优先数据源，国内城市精度更高)。"""
    host = api_host.strip().rstrip("/")
    if "://" not in host:
        # 控制台复制出来的 Host 通常是裸域名(如 abcxyz.qweatherapi.com)，
        # 不强制要求用户自己拼协议头，自动补全更符合直觉、更不容易踩坑
        host = f"https://{host}"
    url = f"{host}/v7/weather/now"
    resp = requests.get(
        url,
        params={"location": f"{lon},{lat}"},
        headers={"X-QW-Api-Key": api_key},
        timeout=10,
    )
    resp.raise_for_status()
    data = resp.json()
    if str(data.get("code")) != "200":
        raise ValueError(f"和风天气返回错误码: {data.get('code')}")
    now = data.get("now", {})
    return {
        "city": resolved_name,
        "temperature_c": _to_float(now.get("temp")),
        "feels_like_c": _to_float(now.get("feelsLike")),
        "humidity_percent": _to_float(now.get("humidity")),
        "weather": now.get("text"),
        "wind_speed_kmh": _to_float(now.get("windSpeed")),
        "wind_direction": now.get("windDir"),
        "observation_time": data.get("updateTime"),
        "source": "和风天气",
    }


def _to_float(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return v


def _query_open_meteo(lat, lon, resolved_name: str) -> dict:
    """免Key兜底数据源：网格模型预测，国内城市可能与本地气象台实测有明显偏差。"""
    resp = requests.get(
        _FORECAST_URL,
        params={
            "latitude": lat,
            "longitude": lon,
            "current": "temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m",
            "timezone": "auto",
        },
        timeout=10,
    )
    resp.raise_for_status()
    current = resp.json().get("current", {})
    return {
        "city": resolved_name,
        "temperature_c": current.get("temperature_2m"),
        "humidity_percent": current.get("relative_humidity_2m"),
        "weather": _describe_weather_code(current.get("weather_code")),
        "wind_speed_kmh": current.get("wind_speed_10m"),
        "observation_time": current.get("time"),
        "source": "Open-Meteo(网格模型预测，国内城市可能与实测有偏差)",
    }


def run(args: dict) -> str:
    city = str(args.get("city", "")).strip()
    if not city:
        return json.dumps({"error": "city 不能为空"}, ensure_ascii=False)

    try:
        lat, lon, resolved_name = _geocode(city)
    except requests.exceptions.Timeout:
        return json.dumps({"error": "天气查询超时，请稍后再试。"}, ensure_ascii=False)
    except requests.exceptions.RequestException as e:
        return json.dumps({"error": f"天气查询失败: {e}"}, ensure_ascii=False)
    except ValueError as e:
        return json.dumps({"error": str(e)}, ensure_ascii=False)

    cfg = get_config()
    qw = cfg.get_tool_settings("qweather")
    if qw.get("api_key") and qw.get("api_host"):
        try:
            result = _query_qweather(qw["api_host"], qw["api_key"], lat, lon, resolved_name)
            return json.dumps(result, ensure_ascii=False)
        except Exception as e:
            # 和风天气调用失败(Key失效/超额/Host格式错误/网络问题等)时回退到免Key
            # 数据源，但不能完全静默——记日志，否则用户永远不知道自己的配置
            # 到底有没有生效、生效失败的真实原因是什么(之前踩过这个坑)。
            from src.logger import get_logger

            get_logger().warning(f"和风天气查询失败，回退到Open-Meteo: {e}")

    try:
        result = _query_open_meteo(lat, lon, resolved_name)
        return json.dumps(result, ensure_ascii=False)
    except requests.exceptions.Timeout:
        return json.dumps({"error": "天气查询超时，请稍后再试。"}, ensure_ascii=False)
    except requests.exceptions.RequestException as e:
        return json.dumps({"error": f"天气查询失败: {e}"}, ensure_ascii=False)

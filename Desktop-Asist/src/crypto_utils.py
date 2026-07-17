"""
加密工具 - 使用 Windows DPAPI 加密本地保存的敏感信息(如 API Key)

DPAPI (Data Protection API) 绑定当前 Windows 用户账户，
同一用户下次启动可正常解密；换用户/换机器则解密失败(视为未配置，需重新输入)。
"""

import base64

try:
    import win32crypt

    _HAS_DPAPI = True
except ImportError:
    _HAS_DPAPI = False

# 应用私有熵值，增加一层区分度(不是真正的密钥，仅防止与其他应用的 DPAPI 数据混淆)
_ENTROPY = b"AiSprite-config-v1"


def encrypt(plaintext: str) -> str:
    """加密字符串，返回 base64 编码的密文。

    未安装 pywin32 时原样返回明文(降级兼容，非 Windows 环境仍可运行)。
    """
    if not plaintext or not _HAS_DPAPI:
        return plaintext
    try:
        blob = win32crypt.CryptProtectData(
            plaintext.encode("utf-8"), "AiSprite API Key", _ENTROPY, None, None, 0
        )
        return base64.b64encode(blob).decode("ascii")
    except Exception:
        return plaintext


def decrypt(ciphertext_b64: str) -> str:
    """解密 base64 编码的密文，返回原始字符串。

    解密失败(换机器/换用户/数据损坏)时返回空串，视为未配置，强制用户重新输入，安全优先。
    """
    if not ciphertext_b64 or not _HAS_DPAPI:
        return ciphertext_b64
    try:
        raw = base64.b64decode(ciphertext_b64)
        _, plain = win32crypt.CryptUnprotectData(raw, _ENTROPY, None, None, 0)
        return plain.decode("utf-8")
    except Exception:
        return ""

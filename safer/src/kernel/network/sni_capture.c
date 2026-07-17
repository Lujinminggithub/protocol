/*
 * sni_capture.c - HTTPS SNI 捕获实现
 * 从 TLS ClientHello 中解析 SNI 域名
 */

#include "sni_capture.h"
#include "../common/shared_events.h"

/*++
 * SnipCaptureParseClientHello
 *
 * 解析 TLS ClientHello 数据包，提取 SNI 域名
 *
 * TLS ClientHello 结构:
 *   ContentType (1 byte) = 0x16 (Handshake)
 *   Version (2 bytes)
 *   Length (2 bytes)
 *   HandshakeType (1 byte) = 0x01 (ClientHello)
 *   Length (3 bytes)
 *   ClientVersion (2 bytes)
 *   ClientRandom (32 bytes)
 *   SessionIdLength (1 byte)
 *   SessionId (variable)
 *   CipherSuitesLength (2 bytes)
 *   CipherSuites (variable)
 *   CompressionMethodsLength (1 byte)
 *   CompressionMethods (variable)
 *   ExtensionsLength (2 bytes)
 *   Extensions (variable) <-- SNI 在这里，每个 ServerName 条目为
 *                              NameType(1B) + Length(2B) + Data
 *
 * --*/
NTSTATUS
SnipCaptureParseClientHello(
    _In_reads_(packetLength) const UCHAR *packetData,
    _In_ ULONG packetLength,
    _Out_ PSNI_CAPTURE_RESULT Result
    )
{
    if (packetData == NULL || Result == NULL || packetLength < 49) {
        return STATUS_INVALID_BUFFER_SIZE;
    }

    RtlZeroMemory(Result, sizeof(SNI_CAPTURE_RESULT));
    KeQuerySystemTime(&Result->Timestamp);

    // 跳过 TLS 记录头 (5 bytes)
    const UCHAR *ptr = packetData + 5;
    ULONG remaining = packetLength - 5;

    // 检查 HandshakeType = 0x01 (ClientHello)，跳过握手头 (Type 1B + Length 3B)
    if (remaining < 4 || ptr[0] != 0x01) {
        return STATUS_UNSUCCESSFUL;
    }
    ptr += 4;
    remaining -= 4;

    // ClientVersion (2 bytes) + ClientRandom (32 bytes)
    if (remaining < 34) return STATUS_UNSUCCESSFUL;
    ptr += 34;
    remaining -= 34;

    // SessionId: 1 字节长度前缀 + 变长数据
    if (remaining < 1) return STATUS_UNSUCCESSFUL;
    UCHAR sessionIdLen = ptr[0];
    ptr += 1;
    remaining -= 1;
    if (remaining < sessionIdLen) return STATUS_UNSUCCESSFUL;
    ptr += sessionIdLen;
    remaining -= sessionIdLen;

    // CipherSuites: 2 字节长度前缀 + 变长数据
    if (remaining < 2) return STATUS_UNSUCCESSFUL;
    USHORT cipherSuitesLen = (USHORT)((ptr[0] << 8) | ptr[1]);
    ptr += 2;
    remaining -= 2;
    if (remaining < cipherSuitesLen) return STATUS_UNSUCCESSFUL;
    ptr += cipherSuitesLen;
    remaining -= cipherSuitesLen;

    // CompressionMethods: 1 字节长度前缀 + 变长数据
    if (remaining < 1) return STATUS_UNSUCCESSFUL;
    UCHAR compressionLen = ptr[0];
    ptr += 1;
    remaining -= 1;
    if (remaining < compressionLen) return STATUS_UNSUCCESSFUL;
    ptr += compressionLen;
    remaining -= compressionLen;

    // Extensions: 2 字节总长度 + 扩展列表
    if (remaining < 2) return STATUS_UNSUCCESSFUL;
    USHORT extensionsLen = (USHORT)((ptr[0] << 8) | ptr[1]);
    ptr += 2;
    remaining -= 2;
    if (extensionsLen > remaining) return STATUS_UNSUCCESSFUL;
    remaining = extensionsLen;  // 只在 Extensions 区间内解析，避免读到扩展之外

    // 解析 Extensions
    while (remaining >= 4) {
        USHORT extType = (USHORT)((ptr[0] << 8) | ptr[1]);
        ULONG extLen = (ULONG)((ptr[2] << 8) | ptr[3]);
        ptr += 4;
        remaining -= 4;

        if (extLen > remaining) break;

        // Extension Type 0 = Server Name Indication (SNI)
        if (extType == 0) {
            // 解析 SNI 列表：2 字节列表总长 + 若干 ServerName 条目
            if (extLen >= 2) {
                USHORT sniListLen = (USHORT)((ptr[0] << 8) | ptr[1]);
                const UCHAR *sniPtr = ptr + 2;
                ULONG sniRemaining = min((ULONG)sniListLen, extLen - 2);

                // ServerName 条目: NameType(1B) + Length(2B) + Data
                if (sniRemaining >= 3) {
                    UCHAR sniType = sniPtr[0];
                    USHORT sniNameLen = (USHORT)((sniPtr[1] << 8) | sniPtr[2]);
                    sniPtr += 3;
                    sniRemaining -= 3;

                    // SNI Type 0 = host_name
                    if (sniType == 0 && sniNameLen <= 254 && sniNameLen <= sniRemaining) {
                        for (USHORT i = 0; i < sniNameLen; i++) {
                            Result->DomainName[i] = sniPtr[i];
                        }
                        Result->DomainName[sniNameLen] = L'\0';
                    }
                }
            }
            break;
        }

        ptr += extLen;
        remaining -= extLen;
    }

    return Result->DomainName[0] ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

/*++
 * SnipCaptureInitialize
 *
 * 初始化 SNI 捕获模块
 *
 * --*/
NTSTATUS
SnipCaptureInitialize(VOID)
{
    DLP_LOG(DLP_DEBUG_INFO, "SNI capture initialized");
    return STATUS_SUCCESS;
}

/*++
 * SnipCaptureCleanup
 *
 * 清理 SNI 捕获模块
 *
 * --*/
VOID
SnipCaptureCleanup(VOID)
{
    DLP_LOG(DLP_DEBUG_INFO, "SNI capture cleaned up");
}

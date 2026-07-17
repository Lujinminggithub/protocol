/*
 * network_monitor.cpp - Network monitor implementation
 * Uses GetIfEntry2 + GetExtendedTcpTable
 */

#include "network_monitor.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <vector>
#pragma comment(lib, "iphlpapi.lib")

/*
 * GetNetworkInfo
 *
 * Get network interface info and TCP/UDP connection status
 */
Napi::Value GetNetworkInfo(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    // GetIfTable2Ex allocates the table itself (unlike the classic
    // GetIfTable/GetExtendedTcpTable two-call sizing idiom) — the caller
    // must release it with FreeMibTable.
    PMIB_IF_TABLE2 pIfTable = nullptr;
    DWORD result = GetIfTable2Ex(MibIfTableRaw, &pIfTable);
    if (result != NO_ERROR || pIfTable == nullptr) {
        Napi::Object resultObj = Napi::Object::New(env);
        resultObj.Set("interfaces", Napi::Array::New(env, 0));
        resultObj.Set("tcpConnections", Napi::Number::New(env, 0));
        return resultObj;
    }

    ULONG tcpTableSize = 0;
    GetExtendedTcpTable(nullptr, &tcpTableSize, FALSE, AF_INET,
                        TCP_TABLE_OWNER_PID_ALL, 0);

    std::vector<BYTE> tcpBuffer(tcpTableSize);
    PMIB_TCPTABLE_OWNER_PID pTcpTable =
        reinterpret_cast<MIB_TCPTABLE_OWNER_PID*>(tcpBuffer.data());

    DWORD tcpResult = GetExtendedTcpTable(pTcpTable, &tcpTableSize, FALSE, AF_INET,
                                          TCP_TABLE_OWNER_PID_ALL, 0);

    int tcpConnectionCount = 0;
    if (tcpResult == NO_ERROR) {
        tcpConnectionCount = pTcpTable->dwNumEntries;
    }

    Napi::Object resultObj = Napi::Object::New(env);
    Napi::Array interfaces = Napi::Array::New(env, pIfTable->NumEntries);

    for (ULONG i = 0; i < pIfTable->NumEntries; i++) {
        const MIB_IF_ROW2& ifRow = pIfTable->Table[i];

        Napi::Object iface = Napi::Object::New(env);
        iface.Set("index", Napi::Number::New(env, ifRow.InterfaceIndex));
        iface.Set("name", Napi::String::New(env,
            reinterpret_cast<const char16_t*>(ifRow.Alias)));
        iface.Set("description", Napi::String::New(env,
            reinterpret_cast<const char16_t*>(ifRow.Description)));
        iface.Set("type", Napi::Number::New(env, ifRow.Type));
        iface.Set("speed", Napi::Number::New(env, (double)ifRow.TransmitLinkSpeed));
        iface.Set("bytesReceived", Napi::Number::New(env, (double)ifRow.InOctets));
        iface.Set("bytesSent", Napi::Number::New(env, (double)ifRow.OutOctets));
        iface.Set("packetsReceived", Napi::Number::New(env, (double)ifRow.InUcastPkts));
        iface.Set("packetsSent", Napi::Number::New(env, (double)ifRow.OutUcastPkts));
        iface.Set("status", Napi::String::New(env,
            ifRow.OperStatus == IfOperStatusUp ? "up" : "down"));

        interfaces.Set(i, iface);
    }

    resultObj.Set("interfaces", interfaces);
    resultObj.Set("tcpConnections", Napi::Number::New(env, tcpConnectionCount));

    FreeMibTable(pIfTable);
    return resultObj;
}

/*
 * InitNetworkAddon
 *
 * Initialize network monitor module
 */
Napi::Object InitNetworkAddon(Napi::Env env, Napi::Object exports) {
    exports.Set("getNetworkInfo", Napi::Function::New(env, GetNetworkInfo));
    return exports;
}

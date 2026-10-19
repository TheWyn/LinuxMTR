// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Wyn

#pragma once

// Options for one trace. Defaults match WinMTR where it has an equivalent.
struct TraceOptions
{
    enum class Protocol { Icmp, Udp, Tcp };
    enum class IpVersion { Auto, V4, V6 };

    double intervalSec = 1.0;     // time between round starts (WinMTR default)
    int pingSizeBytes = 64;       // probe payload (WinMTR default); UDP sends >= 8, TCP none
    double timeoutSec = 3.0;      // reply timeout of each probe (WinMTR uses a fixed 5.0)
    int maxHops = 30;             // largest TTL to probe (WinMTR MAX_HOPS)
    bool resolveNames = true;     // reverse DNS for hop addresses (WinMTR DEFAULT_DNS)
    Protocol protocol = Protocol::Icmp;   // Icmp falls back to Udp without ping sockets
    IpVersion ipVersion = IpVersion::Auto;
    int tcpPort = 80;             // TCP connect() probe port (mtr --tcp default)
    int maxRounds = 0;            // stop probing after N rounds; 0 = unlimited
};

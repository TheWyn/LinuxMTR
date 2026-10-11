// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Wyn

#pragma once

#include "Prober.h"

namespace linmtr::core {

// Unprivileged ICMP echo probes over ping sockets (SOCK_DGRAM + IPPROTO_ICMP[V6]).
// The kernel fills in the echo id and checksum, and it delivers only the echo
// replies to this socket. Received data starts at the ICMP header (no IP header).
class IcmpProber final : public Prober
{
public:
    IcmpProber(const QHostAddress &target, int payloadBytes);

protected:
    int socketProtocol() const override;
    void buildPacket(int ttl, quint16 seq, std::vector<quint8> &packet) const override;
    quint16 destinationPort(int ttl) const override;
    void handleData(const QHostAddress &from, quint16 fromPort,
                    const quint8 *data, std::size_t len, const EventSink &sink) override;
    void handleError(const ErrQueueInfo &info,
                     const quint8 *data, std::size_t len, const EventSink &sink) override;
};

} // namespace linmtr::core

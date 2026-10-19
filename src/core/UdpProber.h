// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Wyn

#pragma once

#include "Prober.h"

namespace linmtr::core {

// Unprivileged UDP probes (tracepath method). Every answer arrives through the
// IP_RECVERR error queue; a port unreachable means that the probe reached the
// target. A quoted payload tag gives the seq. Without it, the destination port
// (one per TTL) and the socket (one per round slot) identify the probe
// (matchesByPort(), DESIGN.md §3.1).
class UdpProber final : public Prober
{
public:
    UdpProber(const QHostAddress &target, int payloadBytes, int socketCount);

    bool matchesByPort() const override { return true; }

protected:
    int socketProtocol() const override;
    int configureSocket(int fd) override;
    void buildPacket(int ttl, quint16 seq, std::vector<quint8> &packet) const override;
    quint16 destinationPort(int ttl) const override;
    void handleData(const QHostAddress &from, quint16 fromPort,
                    const quint8 *data, std::size_t len, const EventSink &sink) override;
    void handleError(const ErrQueueInfo &info,
                     const quint8 *data, std::size_t len, const EventSink &sink) override;
};

} // namespace linmtr::core

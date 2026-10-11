// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Wyn

#pragma once

#include "Prober.h"

namespace linmtr::core {

// Unprivileged UDP probes (tracepath method). Every answer arrives through the
// IP_RECVERR error queue. An ICMP port unreachable means that the probe reached
// the target host.
//
// Routers can quote only the first 8 bytes after the IP header (the UDP header).
// Thus the destination port (one port per TTL) and the socket that the answer
// arrived on identify the pending probe. The session spreads rounds over
// socketCount sockets, each with its own ephemeral source port. It never puts a
// second probe of the same TTL in flight on one socket (matchesByPort()), so
// this match is exact. When the quote includes the payload tag, handleError()
// uses the exact sequence number.
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

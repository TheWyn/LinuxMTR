// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Wyn

#include "UdpProber.h"

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace linmtr::core {

namespace {

constexpr std::size_t kMinPayload = 8;   // bytes; holds the 5-byte payload tag
// Payload tag: [0..1] seq (network order), [2..3] magic "LM", [4] TTL, rest spaces.
constexpr std::size_t kTagLen = 5;
constexpr quint8 kMagic0 = 0x4c;   // 'L'
constexpr quint8 kMagic1 = 0x4d;   // 'M'
constexpr quint8 kPayloadByte = 0x20;   // ASCII space

int ttlFromPort(int port)
{
    const int ttl = port - kUdpBasePort + 1;
    return (ttl >= 1 && ttl <= kMaxTtl) ? ttl : 0;
}

} // namespace

UdpProber::UdpProber(const QHostAddress &target, int payloadBytes, int socketCount)
    : Prober(target, payloadBytes, socketCount)
{
}

int UdpProber::socketProtocol() const
{
    return 0;
}

int UdpProber::configureSocket(int fd)
{
    // Bind an ephemeral local port: the kernel routes each ICMP error to the
    // socket that owns the quoted source port, which identifies the round slot.
    int rc = 0;
    if (isIPv6()) {
        sockaddr_in6 any{};
        any.sin6_family = AF_INET6;
        any.sin6_addr = in6addr_any;
        any.sin6_port = 0;
        rc = ::bind(fd, reinterpret_cast<const sockaddr *>(&any), sizeof(any));
    } else {
        sockaddr_in any{};
        any.sin_family = AF_INET;
        any.sin_addr.s_addr = htonl(INADDR_ANY);
        any.sin_port = 0;
        rc = ::bind(fd, reinterpret_cast<const sockaddr *>(&any), sizeof(any));
    }
    return rc == 0 ? 0 : errno;
}

void UdpProber::buildPacket(int ttl, quint16 seq, std::vector<quint8> &packet) const
{
    const std::size_t size = std::max(kMinPayload, static_cast<std::size_t>(payloadBytes()));
    packet.assign(size, kPayloadByte);
    packet[0] = static_cast<quint8>(seq >> 8);
    packet[1] = static_cast<quint8>(seq & 0xff);
    packet[2] = kMagic0;
    packet[3] = kMagic1;
    packet[4] = static_cast<quint8>(ttl & 0xff);
}

quint16 UdpProber::destinationPort(int ttl) const
{
    return static_cast<quint16>(kUdpBasePort + ttl - 1);
}

void UdpProber::handleData(const QHostAddress &from, quint16 fromPort,
                           const quint8 *, std::size_t, const EventSink &sink)
{
    // A UDP answer from the target itself (a service on the probe port) also
    // proves that the probe reached the target. The source port identifies the TTL.
    if (!sameAddress(from, target()))
        return;
    const int ttl = ttlFromPort(fromPort);
    if (ttl == 0)
        return;

    ProbeEvent ev;
    ev.kind = ProbeEvent::Kind::Reply;
    ev.ttl = ttl;
    ev.responder = from;
    ev.reachedTarget = true;
    sink(ev);
}

void UdpProber::handleError(const ErrQueueInfo &info,
                            const quint8 *data, std::size_t len, const EventSink &sink)
{
    // msg_name holds the original destination; its port encodes the TTL of the probe.
    if (!info.hasDestination || !sameAddress(info.destination, target()))
        return;
    const int ttl = ttlFromPort(info.destinationPort);
    if (ttl == 0)
        return;

    ProbeEvent ev;
    ev.ttl = ttl;
    // The returned data is the UDP payload of the probe, as much as the router quoted.
    if (len >= kTagLen && data[2] == kMagic0 && data[3] == kMagic1
        && data[4] == static_cast<quint8>(ttl & 0xff)) {
        ev.hasSeq = true;
        ev.seq = static_cast<quint16>((quint16(data[0]) << 8) | quint16(data[1]));
    }

    if (info.origin == ErrQueueInfo::Origin::Local) {
        // Mark only an exact match as lost: a port-only match can hit a
        // different, still valid probe of the same TTL.
        if (!ev.hasSeq)
            return;
        ev.kind = ProbeEvent::Kind::Lost;
        sink(ev);
        return;
    }

    const IcmpVerdict verdict = classifyIcmp(info.origin, info.icmpType, info.icmpCode);
    switch (verdict.kind) {
    case IcmpVerdict::Kind::Ignore:
        return;
    case IcmpVerdict::Kind::TimeExceeded:
        ev.kind = ProbeEvent::Kind::Reply;
        ev.responder = info.offender;
        break;
    case IcmpVerdict::Kind::Unreachable:
        ev.responder = info.offender;
        // Only the host where the probe ends sends a port unreachable, often from
        // another of its addresses (anycast, multi-homed), so the offender can differ.
        // The same rule is in traceroute, tracepath and mtr.
        if (verdict.portUnreachable) {
            ev.kind = ProbeEvent::Kind::Reply;
            ev.reachedTarget = true;
        } else {
            ev.kind = ProbeEvent::Kind::Error;
            ev.errorText = verdict.text;
        }
        break;
    }
    sink(ev);
}

} // namespace linmtr::core

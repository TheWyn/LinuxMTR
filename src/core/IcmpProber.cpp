// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Wyn

#include "IcmpProber.h"

namespace linmtr::core {

namespace {

constexpr std::size_t kIcmpHeaderLen = 8;   // bytes
constexpr quint8 kIcmpEchoReply = 0;        // RFC 792
constexpr quint8 kIcmpEchoRequest = 8;      // RFC 792
constexpr quint8 kIcmp6EchoRequest = 128;   // RFC 4443
constexpr quint8 kIcmp6EchoReply = 129;     // RFC 4443
constexpr quint8 kPayloadByte = 0x20;   // WinMTR fills the echo payload with spaces

quint16 readSeq(const quint8 *icmpHeader)
{
    return static_cast<quint16>((quint16(icmpHeader[6]) << 8) | quint16(icmpHeader[7]));
}

} // namespace

IcmpProber::IcmpProber(const QHostAddress &target, int payloadBytes)
    : Prober(target, payloadBytes, 1)
{
}

int IcmpProber::socketProtocol() const
{
    return isIPv6() ? static_cast<int>(IPPROTO_ICMPV6) : static_cast<int>(IPPROTO_ICMP);
}

void IcmpProber::buildPacket(int, quint16 seq, std::vector<quint8> &packet) const
{
    packet.assign(kIcmpHeaderLen + static_cast<std::size_t>(payloadBytes()), kPayloadByte);
    packet[0] = isIPv6() ? kIcmp6EchoRequest : kIcmpEchoRequest;
    packet[1] = 0;                        // code
    packet[2] = packet[3] = 0;            // checksum: the kernel fills it in
    packet[4] = packet[5] = 0;            // identifier: the kernel uses the socket's port
    packet[6] = static_cast<quint8>(seq >> 8);
    packet[7] = static_cast<quint8>(seq & 0xff);
}

quint16 IcmpProber::destinationPort(int) const
{
    return 0;
}

void IcmpProber::handleData(const QHostAddress &from, quint16,
                            const quint8 *data, std::size_t len, const EventSink &sink)
{
    if (len < kIcmpHeaderLen || from.isNull())
        return;
    if (data[0] != (isIPv6() ? kIcmp6EchoReply : kIcmpEchoReply))
        return;

    ProbeEvent ev;
    ev.kind = ProbeEvent::Kind::Reply;
    ev.hasSeq = true;
    ev.seq = readSeq(data);
    ev.responder = from;
    ev.reachedTarget = true;   // only the destination host answers an echo request
    sink(ev);
}

void IcmpProber::handleError(const ErrQueueInfo &info,
                             const quint8 *data, std::size_t len, const EventSink &sink)
{
    // The returned data starts at the original ICMP header of the probe.
    if (len < kIcmpHeaderLen)
        return;
    if (data[0] != (isIPv6() ? kIcmp6EchoRequest : kIcmpEchoRequest) || data[1] != 0)
        return;
    if (info.hasDestination && !sameAddress(info.destination, target()))
        return;

    ProbeEvent ev;
    ev.hasSeq = true;
    ev.seq = readSeq(data);

    if (info.origin == ErrQueueInfo::Origin::Local) {
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
        ev.kind = ProbeEvent::Kind::Error;
        ev.responder = info.offender;
        ev.errorText = verdict.text;
        break;
    }
    sink(ev);
}

} // namespace linmtr::core

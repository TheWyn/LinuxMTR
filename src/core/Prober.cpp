// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Wyn

#include "Prober.h"

#include <QObject>
#include <QSocketNotifier>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>

#include <net/if.h>
#include <sys/types.h>
#include <unistd.h>

#include <linux/errqueue.h>

namespace linmtr::core {

namespace {

// Most messages that one drain() call reads, so that a flood cannot starve the
// event loop. The notifiers are level-triggered, so the remaining messages
// wake the loop again.
constexpr int kDrainBudget = 1024;

// The ICMP type numbers are local because mixed glibc and kernel headers conflict.
constexpr quint8 kIcmpDestUnreach = 3;         // RFC 792
constexpr quint8 kIcmpTimeExceeded = 11;       // RFC 792
constexpr quint8 kIcmpParameterProblem = 12;   // RFC 792
constexpr quint8 kIcmp6DestUnreach = 1;        // RFC 4443
constexpr quint8 kIcmp6PacketTooBig = 2;       // RFC 4443
constexpr quint8 kIcmp6TimeExceeded = 3;       // RFC 4443
constexpr quint8 kIcmp6ParameterProblem = 4;   // RFC 4443

QString unreachableText(int code)
{
    return QStringLiteral("Destination unreachable (code %1).").arg(code);
}

} // namespace

void UniqueFd::reset(int fd)
{
    if (m_fd >= 0)
        ::close(m_fd);   // Linux always releases the fd, also on EINTR: do not retry.
    m_fd = fd;
}

IcmpVerdict classifyIcmp(ErrQueueInfo::Origin origin, quint8 type, quint8 code)
{
    using K = IcmpVerdict::Kind;
    IcmpVerdict v;

    if (origin == ErrQueueInfo::Origin::Icmp) {
        switch (type) {
        case kIcmpTimeExceeded:
            if (code == 0) {
                v.kind = K::TimeExceeded;
            } else {
                v.kind = K::Unreachable;
                v.text = QStringLiteral("The time to live expired during fragment reassembly.");
            }
            break;
        case kIcmpDestUnreach:
            v.kind = K::Unreachable;
            switch (code) {
            case 0: v.text = QStringLiteral("Destination network unreachable."); break;
            case 1: v.text = QStringLiteral("Destination host unreachable."); break;
            case 2: v.text = QStringLiteral("Destination protocol unreachable."); break;
            case 3:
                v.text = QStringLiteral("Destination port unreachable.");
                v.portUnreachable = true;
                break;
            case 4: v.text = QStringLiteral("Packet was too big."); break;
            case 13:
                v.text = QStringLiteral("Destination host unreachable (administratively prohibited).");
                break;
            default: v.text = unreachableText(code); break;
            }
            break;
        case kIcmpParameterProblem:
            v.kind = K::Unreachable;
            v.text = QStringLiteral("Parameter problem.");
            break;
        default:
            // Redirects (ping sockets queue them) and source quench are advisory:
            // the router still forwarded the probe, so they are not answers.
            break;
        }
    } else if (origin == ErrQueueInfo::Origin::Icmp6) {
        switch (type) {
        case kIcmp6TimeExceeded:
            if (code == 0) {
                v.kind = K::TimeExceeded;
            } else {
                v.kind = K::Unreachable;
                v.text = QStringLiteral("The time to live expired during fragment reassembly.");
            }
            break;
        case kIcmp6DestUnreach:
            v.kind = K::Unreachable;
            switch (code) {
            case 0: v.text = QStringLiteral("Destination network unreachable."); break;
            case 1:
                v.text = QStringLiteral("Destination host unreachable (administratively prohibited).");
                break;
            case 3: v.text = QStringLiteral("Destination host unreachable."); break;
            case 4:
                v.text = QStringLiteral("Destination port unreachable.");
                v.portUnreachable = true;
                break;
            default: v.text = unreachableText(code); break;
            }
            break;
        case kIcmp6PacketTooBig:
            v.kind = K::Unreachable;
            v.text = QStringLiteral("Packet was too big.");
            break;
        case kIcmp6ParameterProblem:
            v.kind = K::Unreachable;
            v.text = QStringLiteral("Parameter problem.");
            break;
        default:
            break;
        }
    }
    return v;
}

bool sameAddress(const QHostAddress &a, const QHostAddress &b)
{
    if (a.isNull() || b.isNull() || a.protocol() != b.protocol())
        return false;
    if (a.protocol() == QHostAddress::IPv4Protocol)
        return a.toIPv4Address() == b.toIPv4Address();
    if (a.protocol() == QHostAddress::IPv6Protocol) {
        const Q_IPV6ADDR x = a.toIPv6Address();
        const Q_IPV6ADDR y = b.toIPv6Address();
        return std::memcmp(x.c, y.c, sizeof(x.c)) == 0;
    }
    return false;
}

QHostAddress normalizeAddress(const QHostAddress &address)
{
    if (address.protocol() != QHostAddress::IPv6Protocol)
        return address;
    const Q_IPV6ADDR a = address.toIPv6Address();
    for (int i = 0; i < 10; ++i) {
        if (a.c[i] != 0)
            return address;
    }
    if (a.c[10] != 0xff || a.c[11] != 0xff)
        return address;
    const quint32 v4 = (quint32(a.c[12]) << 24) | (quint32(a.c[13]) << 16)
                     | (quint32(a.c[14]) << 8) | quint32(a.c[15]);
    return QHostAddress(v4);
}

bool addressFromSockaddr(const sockaddr_storage &ss, socklen_t len,
                         QHostAddress *address, quint16 *port)
{
    if (len < static_cast<socklen_t>(sizeof(sa_family_t)))
        return false;
    if (ss.ss_family == AF_INET && len >= static_cast<socklen_t>(sizeof(sockaddr_in))) {
        sockaddr_in sin;
        std::memcpy(&sin, &ss, sizeof(sin));
        *address = QHostAddress(ntohl(sin.sin_addr.s_addr));
        if (port)
            *port = ntohs(sin.sin_port);
        return true;
    }
    if (ss.ss_family == AF_INET6 && len >= static_cast<socklen_t>(sizeof(sockaddr_in6))) {
        sockaddr_in6 sin6;
        std::memcpy(&sin6, &ss, sizeof(sin6));
        *address = QHostAddress(reinterpret_cast<const quint8 *>(sin6.sin6_addr.s6_addr));
        if (port)
            *port = ntohs(sin6.sin6_port);
        return true;
    }
    return false;
}

Prober::Prober(const QHostAddress &target, int payloadBytes, int socketCount)
    : m_target(target)
    , m_v6(target.protocol() == QHostAddress::IPv6Protocol)
    , m_payloadBytes(std::max(0, payloadBytes))
    , m_socketCount(std::clamp(socketCount, 1, kMaxProberSockets))
{
    if (m_v6) {
        const QString scope = target.scopeId();
        if (!scope.isEmpty()) {
            bool ok = false;
            const uint id = scope.toUInt(&ok);
            m_scopeId = ok ? id : ::if_nametoindex(scope.toLocal8Bit().constData());
        }
    }
}

Prober::~Prober()
{
    closeSockets();
}

void Prober::closeSockets()
{
    detachNotifiers();   // turn off the notifiers before the fd numbers go free
    m_fds.clear();
}

int Prober::configureSocket(int)
{
    return 0;
}

int Prober::open()
{
    closeSockets();

    std::vector<UniqueFd> fds;
    fds.reserve(static_cast<std::size_t>(m_socketCount));
    const int family = m_v6 ? AF_INET6 : AF_INET;
    for (int i = 0; i < m_socketCount; ++i) {
        UniqueFd fd(::socket(family, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, socketProtocol()));
        if (!fd.isValid())
            return errno;

        const int one = 1;
        const int rc = m_v6 ? ::setsockopt(fd.get(), IPPROTO_IPV6, IPV6_RECVERR, &one, sizeof(one))
                            : ::setsockopt(fd.get(), IPPROTO_IP, IP_RECVERR, &one, sizeof(one));
        if (rc != 0)
            return errno;
        // Arrival stamps for the RTT. Without them, the RTT ends at the read (DESIGN.md §3.2).
        ::setsockopt(fd.get(), SOL_SOCKET, SO_TIMESTAMPNS, &one, sizeof(one));

        if (const int err = configureSocket(fd.get()); err != 0)
            return err;
        fds.push_back(std::move(fd));
    }
    m_fds = std::move(fds);
    return 0;
}

bool Prober::makeDestination(quint16 port, sockaddr_storage *ss, socklen_t *len) const
{
    std::memset(ss, 0, sizeof(*ss));
    if (!m_v6) {
        sockaddr_in sin{};
        sin.sin_family = AF_INET;
        sin.sin_port = htons(port);
        sin.sin_addr.s_addr = htonl(m_target.toIPv4Address());
        std::memcpy(ss, &sin, sizeof(sin));
        *len = sizeof(sin);
        return true;
    }
    sockaddr_in6 sin6{};
    sin6.sin6_family = AF_INET6;
    sin6.sin6_port = htons(port);
    const Q_IPV6ADDR a = m_target.toIPv6Address();
    std::memcpy(sin6.sin6_addr.s6_addr, a.c, sizeof(a.c));
    sin6.sin6_scope_id = m_scopeId;
    std::memcpy(ss, &sin6, sizeof(sin6));
    *len = sizeof(sin6);
    return true;
}

int Prober::send(int ttl, quint16 seq, int slot)
{
    if (m_fds.empty())
        return EBADF;
    if (ttl < 1 || ttl > kMaxTtl || slot < 0)
        return EINVAL;
    const int fd = m_fds[static_cast<std::size_t>(slot % static_cast<int>(m_fds.size()))].get();

    // The kernel stamps the TTL at send time and the sends are sequential, so
    // a setsockopt() before each send is race-free.
    const int hops = ttl;
    const int rc = m_v6 ? ::setsockopt(fd, IPPROTO_IPV6, IPV6_UNICAST_HOPS, &hops, sizeof(hops))
                        : ::setsockopt(fd, IPPROTO_IP, IP_TTL, &hops, sizeof(hops));
    if (rc != 0)
        return errno;

    buildPacket(ttl, seq, m_txBuf);
    sockaddr_storage dst;
    socklen_t dstLen = 0;
    if (!makeDestination(destinationPort(ttl), &dst, &dstLen))
        return EAFNOSUPPORT;

    bool retried = false;
    for (int attempt = 0; attempt < 8; ++attempt) {
        const ssize_t sent = ::sendto(fd, m_txBuf.data(), m_txBuf.size(), MSG_DONTWAIT,
                                      reinterpret_cast<const sockaddr *>(&dst), dstLen);
        if (sent >= 0)
            return 0;
        const int err = errno;
        if (err == EINTR)
            continue;
        if (err == EAGAIN || err == EWOULDBLOCK || err == ENOBUFS || err == ENOMEM || retried)
            return err;
        // With IP_RECVERR, each queued ICMP error also sets the pending error of
        // the socket. The next send reports and clears it, and does not send.
        // That error belongs to an earlier probe, so retry once. A real local
        // failure (no route, EPERM) repeats, and the function returns it.
        retried = true;
    }
    return EINTR;
}

namespace {

// Clears the pending socket error. Without errqueue data, that error keeps
// POLLERR set, and the notifiers wake again and again.
void clearPendingSocketError(int fd)
{
    int soError = 0;
    socklen_t len = sizeof(soError);
    ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soError, &len);
}

} // namespace

Prober::ReadResult Prober::readOne(int slot, bool errQueue, const EventSink &sink)
{
    const int fd = m_fds[static_cast<std::size_t>(slot)].get();
    sockaddr_storage name;
    std::memset(&name, 0, sizeof(name));
    alignas(cmsghdr) unsigned char control[512];
    iovec iov{m_rxBuf.data(), m_rxBuf.size()};

    msghdr msg{};
    msg.msg_name = &name;
    msg.msg_namelen = sizeof(name);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);

    const int flags = MSG_DONTWAIT | (errQueue ? MSG_ERRQUEUE : 0);
    const ssize_t n = ::recvmsg(fd, &msg, flags);
    if (n < 0) {
        const int err = errno;
        if (err == EINTR)
            return ReadResult::Consumed;
        if (errQueue || err == EAGAIN || err == EWOULDBLOCK || err == EBADF
            || err == ENOTSOCK || err == EFAULT || err == EINVAL)
            return ReadResult::Empty;
        // Normal queue: this call reports and clears a pending socket error,
        // which each errqueue entry sets. Keep reading.
        return ReadResult::Consumed;
    }

    // Do not trust a length larger than the data that the kernel copied.
    const std::size_t len = std::min(static_cast<std::size_t>(n), m_rxBuf.size());
    const socklen_t nameLen = std::min<socklen_t>(msg.msg_namelen, sizeof(name));
    QHostAddress peer;
    quint16 peerPort = 0;
    const bool hasName = addressFromSockaddr(name, nameLen, &peer, &peerPort);

    const qint64 arrivalNs = decodeArrivalStamp(&msg, sizeof(control));
    const EventSink stampSink = [&sink, arrivalNs](const ProbeEvent &ev) {
        ProbeEvent stamped = ev;
        stamped.arrivalNs = arrivalNs;
        sink(stamped);
    };

    if (!errQueue) {
        if (hasName)
            handleData(normalizeAddress(peer), peerPort, m_rxBuf.data(), len, stampSink);
        return ReadResult::Consumed;
    }

    ErrQueueInfo info;
    if (!decodeExtendedError(&msg, sizeof(control), &info))
        return ReadResult::Consumed;
    info.hasDestination = hasName;
    if (hasName) {
        info.destination = normalizeAddress(peer);
        info.destinationPort = peerPort;
    }
    handleError(info, m_rxBuf.data(), len, stampSink);
    return ReadResult::Consumed;
}

bool decodeExtendedError(msghdr *msg, std::size_t controlCapacity, ErrQueueInfo *info)
{
    const std::size_t controlLen = std::min<std::size_t>(msg->msg_controllen, controlCapacity);
    msg->msg_controllen = controlLen;
    const auto *control = static_cast<const unsigned char *>(msg->msg_control);
    for (cmsghdr *c = CMSG_FIRSTHDR(msg); c != nullptr; c = CMSG_NXTHDR(msg, c)) {
        const bool isErr = (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_RECVERR)
                        || (c->cmsg_level == IPPROTO_IPV6 && c->cmsg_type == IPV6_RECVERR);
        if (!isErr)
            continue;

        const unsigned char *payload = CMSG_DATA(c);
        const std::size_t payloadOffset = static_cast<std::size_t>(payload - control);
        if (c->cmsg_len < CMSG_LEN(0) || payloadOffset > controlLen)
            continue;
        const std::size_t avail = std::min<std::size_t>(c->cmsg_len - CMSG_LEN(0),
                                                        controlLen - payloadOffset);
        if (avail < sizeof(sock_extended_err))
            continue;

        sock_extended_err ee;
        std::memcpy(&ee, payload, sizeof(ee));

        *info = ErrQueueInfo{};
        info->icmpType = ee.ee_type;
        info->icmpCode = ee.ee_code;
        switch (ee.ee_origin) {
        case SO_EE_ORIGIN_LOCAL: info->origin = ErrQueueInfo::Origin::Local; break;
        case SO_EE_ORIGIN_ICMP: info->origin = ErrQueueInfo::Origin::Icmp; break;
        case SO_EE_ORIGIN_ICMP6: info->origin = ErrQueueInfo::Origin::Icmp6; break;
        default: info->origin = ErrQueueInfo::Origin::Other; break;
        }

        // SO_EE_OFFENDER: a sockaddr after the extended error. It can be AF_UNSPEC.
        const std::size_t offenderAvail = avail - sizeof(ee);
        if (offenderAvail >= sizeof(sa_family_t)) {
            sa_family_t family;
            std::memcpy(&family, payload + sizeof(ee), sizeof(family));
            std::size_t need = 0;
            if (family == AF_INET)
                need = sizeof(sockaddr_in);
            else if (family == AF_INET6)
                need = sizeof(sockaddr_in6);
            if (need != 0 && offenderAvail >= need) {
                sockaddr_storage off;
                std::memset(&off, 0, sizeof(off));
                std::memcpy(&off, payload + sizeof(ee), need);
                QHostAddress offender;
                if (addressFromSockaddr(off, static_cast<socklen_t>(need), &offender, nullptr))
                    info->offender = normalizeAddress(offender);
            }
        }
        return true;
    }
    return false;
}

qint64 decodeArrivalStamp(msghdr *msg, std::size_t controlCapacity)
{
    const std::size_t controlLen = std::min<std::size_t>(msg->msg_controllen, controlCapacity);
    msg->msg_controllen = controlLen;
    const auto *control = static_cast<const unsigned char *>(msg->msg_control);
    for (cmsghdr *c = CMSG_FIRSTHDR(msg); c != nullptr; c = CMSG_NXTHDR(msg, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_TIMESTAMPNS)
            continue;
        const std::size_t payloadOffset = static_cast<std::size_t>(CMSG_DATA(c) - control);
        if (c->cmsg_len < CMSG_LEN(sizeof(timespec)) || payloadOffset + sizeof(timespec) > controlLen)
            continue;
        timespec ts;
        std::memcpy(&ts, CMSG_DATA(c), sizeof(ts));
        return static_cast<qint64>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
    }
    return 0;
}

qint64 replyRttNs(const ProbeEvent &ev, qint64 readNs, qint64 wallNs)
{
    qint64 kernelNs = static_cast<qint64>(ev.rttUs) * 1000;
    if (kernelNs == 0 && ev.arrivalNs > 0)
        kernelNs = readNs - (wallNs - ev.arrivalNs);
    // Out of range: no kernel value, or a wall-clock step or suspend after the arrival.
    return kernelNs > 0 && kernelNs <= readNs ? kernelNs : readNs;
}

void Prober::drain(const EventSink &sink)
{
    int budget = kDrainBudget;
    const int count = static_cast<int>(m_fds.size());
    if (count == 0)
        return;
    // Rotate the starting socket so one busy socket cannot starve the others.
    const int first = m_nextDrainSlot;
    m_nextDrainSlot = (m_nextDrainSlot + 1) % count;
    for (int i = 0; i < count; ++i) {
        const int slot = (first + i) % count;
        const EventSink slotSink = [&sink, slot](const ProbeEvent &ev) {
            ProbeEvent tagged = ev;
            tagged.slot = slot;
            sink(tagged);
        };
        bool progress = true;
        while (progress && budget > 0) {
            progress = false;
            while (budget > 0 && readOne(slot, false, slotSink) == ReadResult::Consumed) {
                --budget;
                progress = true;
            }
            while (budget > 0 && readOne(slot, true, slotSink) == ReadResult::Consumed) {
                --budget;
                progress = true;
            }
        }
        clearPendingSocketError(m_fds[static_cast<std::size_t>(slot)].get());
    }
}

void Prober::attachNotifiers(QObject *context, const std::function<void()> &onActivity)
{
    detachNotifiers();
    // The Qt dispatchers wake Read notifiers on POLLERR, which signals errqueue
    // data. Exception notifiers cover POLLPRI.
    for (const UniqueFd &fd : m_fds) {
        for (const QSocketNotifier::Type type : {QSocketNotifier::Read, QSocketNotifier::Exception}) {
            auto *notifier = new QSocketNotifier(fd.get(), type, context);
            QObject::connect(notifier, &QSocketNotifier::activated, notifier,
                             [onActivity] { onActivity(); });
            m_notifiers.append(QPointer<QSocketNotifier>(notifier));
        }
    }
}

void Prober::detachNotifiers()
{
    for (const QPointer<QSocketNotifier> &notifier : std::as_const(m_notifiers)) {
        if (!notifier)
            continue;
        QObject::disconnect(notifier, nullptr, nullptr, nullptr);
        notifier->setEnabled(false);
        notifier->deleteLater();
    }
    m_notifiers.clear();
}

} // namespace linmtr::core

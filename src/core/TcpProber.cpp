// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Wyn

#include "TcpProber.h"

#include <QObject>
#include <QSocketNotifier>

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <utility>
#include <vector>

#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <unistd.h>

namespace linmtr::core {

namespace {

// Most error queue entries that one drain() call reads per probe. A socket stops
// matching ICMP errors when its connect() fails, so one entry is the normal case.
constexpr int kErrQueueBudget = 8;

enum class ErrQueueRead { Empty, Skipped, Entry };

// *arrivalNs gets the kernel arrival stamp of the entry, or 0.
ErrQueueRead readErrQueue(int fd, ErrQueueInfo *info, qint64 *arrivalNs)
{
    // The returned data is the quoted TCP header. The prober does not need it:
    // the socket alone identifies the probe.
    std::array<quint8, 64> data;
    alignas(cmsghdr) unsigned char control[512];
    for (int attempt = 0; attempt < 4; ++attempt) {
        iovec iov{data.data(), data.size()};
        msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);
        if (::recvmsg(fd, &msg, MSG_ERRQUEUE | MSG_DONTWAIT) >= 0) {
            *arrivalNs = decodeArrivalStamp(&msg, sizeof(control));
            return decodeExtendedError(&msg, sizeof(control), info) ? ErrQueueRead::Entry
                                                                    : ErrQueueRead::Skipped;
        }
        if (errno != EINTR)
            return ErrQueueRead::Empty;
    }
    return ErrQueueRead::Empty;
}

// True when the kernel resent the SYN. It resends an unanswered SYN after its
// initial RTO (1 s). A later answer can belong to the resend, and its RTT is
// then wrong by whole seconds (Karn's rule). Nothing answered the probe that the
// session sent, so it counts as lost, as an ICMP or UDP probe does. This applies
// to every reply: handshake, RST, time exceeded and port unreachable.
// *rttUs gets tcpi_rtt: the RTT in microseconds that the kernel measured from the
// SYN to the SYN-ACK, or 0 without a handshake (RST, ICMP answer). A late read does
// not change it. When TCP_INFO fails, *rttUs does not change.
bool synRetransmitted(int fd, quint32 *rttUs)
{
    tcp_info info{};
    socklen_t len = sizeof(info);
    if (::getsockopt(fd, IPPROTO_TCP, TCP_INFO, &info, &len) != 0)
        return false;
    // tcpi_rtt comes before tcpi_total_retrans, so this check covers both.
    if (len < offsetof(tcp_info, tcpi_total_retrans) + sizeof(info.tcpi_total_retrans))
        return false;
    *rttUs = info.tcpi_rtt;
    return info.tcpi_total_retrans > 0;
}

// Reset instead of FIN after a completed handshake: the target sees the probe
// connection end at once, and this host keeps no TIME_WAIT state.
void abortOnClose(int fd)
{
    const linger abortive{1, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_LINGER, &abortive, sizeof(abortive));
}

void markReached(ProbeEvent *ev, const QHostAddress &target)
{
    ev->kind = ProbeEvent::Kind::Reply;
    ev->responder = target;
    ev->reachedTarget = true;
}

void releaseNotifier(QPointer<QSocketNotifier> &notifier)
{
    if (!notifier)
        return;
    QObject::disconnect(notifier, nullptr, nullptr, nullptr);
    notifier->setEnabled(false);
    notifier->deleteLater();
    notifier.clear();
}

} // namespace

TcpProber::TcpProber(const QHostAddress &target, quint16 port)
    : m_target(target)
    , m_v6(target.protocol() == QHostAddress::IPv6Protocol)
    , m_port(port)
{
}

TcpProber::~TcpProber()
{
    closeAll();
}

int TcpProber::open()
{
    closeAll();
    m_open = false;
    if (m_port == 0)
        return EINVAL;

    std::memset(&m_destination, 0, sizeof(m_destination));
    if (m_target.protocol() == QHostAddress::IPv4Protocol) {
        sockaddr_in sin{};
        sin.sin_family = AF_INET;
        sin.sin_port = htons(m_port);
        sin.sin_addr.s_addr = htonl(m_target.toIPv4Address());
        std::memcpy(&m_destination, &sin, sizeof(sin));
        m_destinationLen = sizeof(sin);
    } else if (m_v6) {
        sockaddr_in6 sin6{};
        sin6.sin6_family = AF_INET6;
        sin6.sin6_port = htons(m_port);
        const Q_IPV6ADDR a = m_target.toIPv6Address();
        std::memcpy(sin6.sin6_addr.s6_addr, a.c, sizeof(a.c));
        const QString scope = m_target.scopeId();
        if (!scope.isEmpty()) {
            bool ok = false;
            const uint id = scope.toUInt(&ok);
            sin6.sin6_scope_id = ok ? id : ::if_nametoindex(scope.toLocal8Bit().constData());
        }
        std::memcpy(&m_destination, &sin6, sizeof(sin6));
        m_destinationLen = sizeof(sin6);
    } else {
        return EAFNOSUPPORT;
    }

    // send() makes one socket per probe. Make one now, so that a missing
    // permission stops the trace at once and does not fail every probe. Keep it
    // open: the kernel turns the arrival stamps on with a delay, and off when the
    // last socket that uses them closes. This socket keeps them on for the probes.
    if (const int err = newSocket(&m_stampKeeper); err != 0)
        return err;
    m_open = true;
    return 0;
}

int TcpProber::newSocket(UniqueFd *out) const
{
    UniqueFd fd(::socket(m_v6 ? AF_INET6 : AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!fd.isValid())
        return errno;
    const int one = 1;
    const int rc = m_v6 ? ::setsockopt(fd.get(), IPPROTO_IPV6, IPV6_RECVERR, &one, sizeof(one))
                        : ::setsockopt(fd.get(), IPPROTO_IP, IP_RECVERR, &one, sizeof(one));
    if (rc != 0)
        return errno;
    // Arrival stamps for ICMP answers. Without them, such an answer gets the RTT at the read.
    ::setsockopt(fd.get(), SOL_SOCKET, SO_TIMESTAMPNS, &one, sizeof(one));
    *out = std::move(fd);
    return 0;
}

bool TcpProber::canSend() const
{
    return m_open && m_probes.size() < static_cast<std::size_t>(kMaxInFlight);
}

int TcpProber::send(int ttl, quint16 seq, int slot)
{
    if (!m_open)
        return EBADF;
    if (ttl < 1 || ttl > kMaxTtl || slot < 0)
        return EINVAL;
    if (!canSend())
        return EAGAIN;

    UniqueFd fd;
    if (const int err = newSocket(&fd); err != 0)
        return err;
    const int hops = ttl;
    const int rc = m_v6 ? ::setsockopt(fd.get(), IPPROTO_IPV6, IPV6_UNICAST_HOPS, &hops, sizeof(hops))
                        : ::setsockopt(fd.get(), IPPROTO_IP, IP_TTL, &hops, sizeof(hops));
    if (rc != 0)
        return errno;

    Early early = Early::None;
    if (::connect(fd.get(), reinterpret_cast<const sockaddr *>(&m_destination), m_destinationLen) == 0) {
        early = Early::Connected;
    } else {
        const int err = errno;
        if (err == ECONNREFUSED)
            early = Early::Refused;
        else if (err != EINPROGRESS && err != EINTR)   // EINTR: the connect goes on asynchronously
            return err;
    }

    // The session never reuses the seq of a live probe. Close a stale entry anyway,
    // so that its socket does not leak.
    if (const auto stale = m_probes.find(seq); stale != m_probes.end())
        closeProbe(stale);
    Probe &probe = m_probes[seq];
    probe.fd = std::move(fd);
    probe.ttl = ttl;
    probe.slot = slot;
    probe.early = early;
    watch(probe);
    return 0;
}

bool TcpProber::inspect(Probe &probe, short revents, ProbeEvent *ev)
{
    const int fd = probe.fd.get();
    if (probe.early != Early::None) {
        if (probe.early == Early::Connected)
            abortOnClose(fd);
        markReached(ev, m_target);
        return true;
    }

    // Read the error queue first. Reading the ICMP error also clears SO_ERROR,
    // and the bare SO_ERROR (EHOSTUNREACH) does not tell which router answered.
    for (int i = 0; i < kErrQueueBudget; ++i) {
        ErrQueueInfo info;
        qint64 arrivalNs = 0;
        const ErrQueueRead read = readErrQueue(fd, &info, &arrivalNs);
        if (read == ErrQueueRead::Empty)
            break;
        if (read == ErrQueueRead::Skipped)
            continue;
        if (info.origin == ErrQueueInfo::Origin::Local) {
            ev->kind = ProbeEvent::Kind::Lost;
            return true;
        }
        const IcmpVerdict verdict = classifyIcmp(info.origin, info.icmpType, info.icmpCode);
        if (verdict.kind == IcmpVerdict::Kind::TimeExceeded) {
            ev->kind = ProbeEvent::Kind::Reply;
            ev->responder = info.offender;
            ev->arrivalNs = arrivalNs;
            return true;
        }
        if (verdict.kind == IcmpVerdict::Kind::Unreachable) {
            ev->responder = info.offender;
            ev->arrivalNs = arrivalNs;
            if (verdict.portUnreachable) {
                // A REJECT firewall at or in front of the destination answered
                // for it: target reached, as in UDP mode and mtr --tcp. The
                // offender stays the hop address (it can be the firewall).
                ev->kind = ProbeEvent::Kind::Reply;
                ev->reachedTarget = true;
            } else {
                ev->kind = ProbeEvent::Kind::Error;
                ev->errorText = verdict.text;
            }
            return true;
        }
    }

    if ((revents & (POLLOUT | POLLERR | POLLHUP)) == 0)
        return false;   // handshake still in progress

    int soError = 0;
    socklen_t len = sizeof(soError);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soError, &len) != 0)
        soError = errno;
    if (soError == ECONNREFUSED) {
        markReached(ev, m_target);   // RST: the port is closed, but the target answered
        return true;
    }
    if (soError != 0) {
        ev->kind = ProbeEvent::Kind::Lost;   // failure without an ICMP origin
        return true;
    }
    if ((revents & (POLLOUT | POLLHUP)) == 0)
        return false;   // POLLERR from an ignored error queue entry: still connecting

    // Writable without an error is a completed handshake, unless the error
    // queue read above already consumed the error of a failed connect().
    sockaddr_storage peer;
    socklen_t peerLen = sizeof(peer);
    if (::getpeername(fd, reinterpret_cast<sockaddr *>(&peer), &peerLen) != 0) {
        ev->kind = ProbeEvent::Kind::Lost;
        return true;
    }
    abortOnClose(fd);
    markReached(ev, m_target);
    return true;
}

void TcpProber::drain(const EventSink &sink)
{
    if (m_probes.empty())
        return;

    std::vector<pollfd> fds;
    std::vector<quint16> seqs;
    fds.reserve(m_probes.size());
    seqs.reserve(m_probes.size());
    for (const auto &entry : m_probes) {
        fds.push_back(pollfd{entry.second.fd.get(), POLLOUT, 0});
        seqs.push_back(entry.first);
    }
    // One zero-timeout poll() over all probes in flight finds the finished
    // ones, also the probes that no notifier reported.
    if (::poll(fds.data(), static_cast<nfds_t>(fds.size()), 0) < 0)
        return;   // EINTR or ENOMEM: the next activation or pump tick retries

    for (std::size_t i = 0; i < fds.size(); ++i) {
        const auto it = m_probes.find(seqs[i]);
        if (it == m_probes.end())
            continue;
        if (fds[i].revents == 0 && it->second.early == Early::None)
            continue;
        ProbeEvent ev;
        if (!inspect(it->second, fds[i].revents, &ev))
            continue;
        if (ev.kind == ProbeEvent::Kind::Reply && synRetransmitted(it->second.fd.get(), &ev.rttUs)) {
            ev = ProbeEvent{};
            ev.kind = ProbeEvent::Kind::Lost;
        }
        ev.hasSeq = true;
        ev.seq = seqs[i];
        ev.ttl = it->second.ttl;
        ev.slot = it->second.slot;
        closeProbe(it);
        sink(ev);
    }
}

void TcpProber::attachNotifiers(QObject *context, const std::function<void()> &onActivity)
{
    m_context = context;
    m_onActivity = onActivity;
    for (auto &entry : m_probes) {
        for (QPointer<QSocketNotifier> &notifier : entry.second.notifiers)
            releaseNotifier(notifier);
        watch(entry.second);
    }
}

void TcpProber::watch(Probe &probe)
{
    if (!m_context || !m_onActivity)
        return;
    // Every outcome of a non-blocking connect() makes the socket writable. The
    // Qt glib dispatcher also wakes Write and Exception notifiers on POLLERR
    // (error queue data). drain() polls all probes, so one wake-up is sufficient.
    constexpr QSocketNotifier::Type types[] = {QSocketNotifier::Write, QSocketNotifier::Exception};
    static_assert(std::size(types) == std::tuple_size_v<decltype(probe.notifiers)>);
    for (std::size_t i = 0; i < probe.notifiers.size(); ++i) {
        auto *notifier = new QSocketNotifier(probe.fd.get(), types[i], m_context.data());
        QObject::connect(notifier, &QSocketNotifier::activated, notifier,
                         [onActivity = m_onActivity] { onActivity(); });
        probe.notifiers[i] = notifier;
    }
}

void TcpProber::discard(quint16 seq)
{
    if (const auto it = m_probes.find(seq); it != m_probes.end())
        closeProbe(it);
}

void TcpProber::closeProbe(ProbeMap::iterator it)
{
    // Turn off the notifiers before the fd number goes free for reuse.
    for (QPointer<QSocketNotifier> &notifier : it->second.notifiers)
        releaseNotifier(notifier);
    m_probes.erase(it);   // UniqueFd closes the socket
}

void TcpProber::closeAll()
{
    while (!m_probes.empty())
        closeProbe(m_probes.begin());
}

} // namespace linmtr::core

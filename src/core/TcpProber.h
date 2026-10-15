// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Wyn

#pragma once

#include <QHostAddress>
#include <QPointer>

#include <array>
#include <functional>
#include <unordered_map>

#include <sys/socket.h>

#include "Prober.h"

class QObject;
class QSocketNotifier;

namespace linmtr::core {

// Unprivileged TCP probes (the "tcpconn" method of mtr --tcp and traceroute -T):
// one non-blocking connect() per probe, on its own TTL-limited socket. The socket
// identifies the probe, so answers need no packet matching:
// - completed handshake, or RST from the target (ECONNREFUSED): target reached;
// - ICMP time exceeded or destination unreachable: the IP_RECVERR error queue of
//   the socket reports it, with the address of the router that sent it.
//   Port unreachable (a REJECT firewall) counts as target reached, as in UDP mode.
// An answer that arrives after the kernel resent the SYN counts as lost: it can
// answer the resend, so its RTT is unknown.
// A probe socket stays open until drain() reports its answer, the session
// discards the probe (reply timeout), or the prober goes away. The check socket
// of open() stays open until the prober goes away.
class TcpProber final : public ProbeBackend
{
public:
    // Most probe sockets (one fd each) open at the same time. At this cap,
    // canSend() is false and the session skips the send.
    static constexpr int kMaxInFlight = 256;

    TcpProber(const QHostAddress &target, quint16 port);
    ~TcpProber() override;

    // Checks the target, and checks that a probe socket opens. That socket stays
    // open and keeps the kernel arrival stamps on. send() makes the probe sockets.
    // Returns 0 or an errno value.
    int open() override;
    int socketCount() const override { return 1; }

    // Starts one connect() with the given TTL. Returns 0 or an errno value; on
    // failure no socket stays open.
    int send(int ttl, quint16 seq, int slot) override;

    // Reports and closes every probe whose connect() has finished. Never blocks.
    void drain(const EventSink &sink) override;

    // Every probe socket gets Write and Exception notifiers (children of context)
    // that call onActivity. When the probe closes, the prober disables them and
    // releases them with deleteLater(). Thus a probe can close inside onActivity.
    void attachNotifiers(QObject *context, const std::function<void()> &onActivity) override;

    bool canSend() const override;
    void discard(quint16 seq) override;

private:
    // connect() can finish before it returns; drain() reports that outcome.
    enum class Early { None, Connected, Refused };

    struct Probe
    {
        UniqueFd fd;
        int ttl = 0;
        int slot = 0;
        Early early = Early::None;
        std::array<QPointer<QSocketNotifier>, 2> notifiers;
    };
    using ProbeMap = std::unordered_map<quint16, Probe>;

    int newSocket(UniqueFd *out) const;
    bool inspect(Probe &probe, short revents, ProbeEvent *ev);
    void watch(Probe &probe);
    void closeProbe(ProbeMap::iterator it);
    void closeAll();

    QHostAddress m_target;
    bool m_v6 = false;
    quint16 m_port = 0;
    sockaddr_storage m_destination{};
    socklen_t m_destinationLen = 0;
    bool m_open = false;
    UniqueFd m_stampKeeper;   // the check socket of open()
    QPointer<QObject> m_context;
    std::function<void()> m_onActivity;
    ProbeMap m_probes;
};

} // namespace linmtr::core

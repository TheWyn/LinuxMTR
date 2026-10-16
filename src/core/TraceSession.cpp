// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Wyn

#include "TraceSession.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QHostInfo>
#include <QSet>
#include <QStringList>
#include <QTimer>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <ctime>
#include <map>
#include <memory>
#include <vector>

#include "DnsResolver.h"
#include "IcmpProber.h"
#include "Prober.h"
#include "TcpProber.h"
#include "UdpProber.h"

using linmtr::core::DnsResolver;
using linmtr::core::IcmpProber;
using linmtr::core::ProbeBackend;
using linmtr::core::ProbeEvent;
using linmtr::core::TcpProber;
using linmtr::core::UdpProber;
using linmtr::core::normalizeAddress;
using linmtr::core::replyRttNs;
using linmtr::core::sameAddress;

namespace {

constexpr int kPumpIntervalMs = 20;            // pump timer period
constexpr int kCoalesceMs = 100;               // shortest time between two hopsChanged()
// Largest send delay between two TTLs of a round, in nanoseconds (20 ms). Each
// pump tick sends all due probes, so a shorter delay sends groups (DESIGN.md §3.3).
constexpr qint64 kMaxStaggerNs = 20'000'000;
constexpr qsizetype kMaxPending = 60000;       // keeps the 16-bit seq space from filling
// Largest UDP socket pool. Wider engine-only settings stay exact: sendProbe()
// skips a probe whose socket still has one in flight for its TTL.
constexpr int kMaxUdpSockets = linmtr::core::kMaxProberSockets;

const QString kResolveFailed = QStringLiteral("Unable to resolve hostname.");

TraceOptions sanitized(TraceOptions o)
{
    const auto clampFinite = [](double v, double lo, double hi, double fallback) {
        return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
    };
    o.intervalSec = clampFinite(o.intervalSec, 0.1, 3600.0, 1.0);
    o.timeoutSec = clampFinite(o.timeoutSec, 0.1, 60.0, 3.0);
    o.pingSizeBytes = std::clamp(o.pingSizeBytes, 0, 8192);
    o.maxHops = std::clamp(o.maxHops, 1, linmtr::core::kMaxTtl);
    o.tcpPort = std::clamp(o.tcpPort, 1, 65535);
    o.maxRounds = std::max(0, o.maxRounds);
    return o;
}

// The clock of the kernel arrival stamps (CLOCK_REALTIME), in ns since the Unix epoch.
qint64 wallClockNs()
{
    timespec now{};
    ::clock_gettime(CLOCK_REALTIME, &now);
    return static_cast<qint64>(now.tv_sec) * 1'000'000'000 + now.tv_nsec;
}

linmtr::core::BackendFactory testBackendFactory;   // set only by tests

} // namespace

void linmtr::core::setBackendFactoryForTests(BackendFactory factory)
{
    testBackendFactory = std::move(factory);
}

struct TraceSession::Private
{
    struct Pending
    {
        int ttl = 0;
        int round = 0;
        int slot = 0;          // prober socket that sent the probe
        qint64 sentAtNs = 0;
        qint64 sentAtMs = 0;   // wall clock, for HopSample
    };

    struct Round
    {
        int outstanding = 0;
        int sendOk = 0;
        int sendFailed = 0;
        bool sendingDone = false;
    };

    explicit Private(TraceSession *owner);

    // lifecycle
    void resetForStart(const QString &target, const TraceOptions &options);
    void teardownIo();
    void fail(const QString &message);
    void onForwardLookup(const QHostInfo &info);
    void useTargetAddress(const QHostAddress &address);
    void beginTracing();

    // event handlers
    void onPumpTick();
    void onSocketActivity();
    void finishHandler(int generation);

    // probing
    void drainSocket();
    void onProbeEvent(const ProbeEvent &ev);
    void startRound(qint64 now);
    void sendDue(qint64 now, bool force);
    void sendProbe(int ttl);
    void finishRoundSending();
    void sweepTimeouts(qint64 now);
    QHash<quint16, Pending>::iterator releasePending(QHash<quint16, Pending>::iterator it);
    int *inFlightCount(int slot, int ttl);
    void reportCapacity();
    void checkRounds();
    quint16 allocateSeq();

    // bookkeeping
    int currentMaxTtl() const;
    void updateHopAddress(HopStats &hop, const QHostAddress &address, bool reachedTarget);
    void onHostResolved(const QHostAddress &address, const QString &name);
    void markDirty();
    void flushHops();

    TraceSession *q;

    State state = State::Idle;
    QString target;
    QHostAddress targetAddress;
    TraceOptions options;
    QString lastError;

    std::vector<HopStats> hopStats;   // index = ttl - 1, sized options.maxHops

    std::unique_ptr<ProbeBackend> prober;
    DnsResolver dns;
    int lookupId = -1;

    QTimer pump;
    QTimer coalesce;
    bool dirty = false;
    QElapsedTimer clock;

    QHash<quint16, Pending> pending;
    std::vector<int> inFlight;        // pending probes per (slot, ttl): slot * maxHops + ttl - 1
    std::map<int, Round> rounds;
    quint16 nextSeq = 0;
    int currentRound = 0;
    bool roundActive = false;
    int nextTtl = 1;
    qint64 roundStartNs = 0;
    qint64 staggerNs = 0;
    qint64 nextRoundAtNs = 0;
    qint64 intervalNs = 0;
    qint64 timeoutNs = 0;
    int roundsResolved = 0;    // rounds 1..n fully resolved
    int roundsCompleted = 0;   // rounds announced through roundCompleted()

    QSet<int> reportedErrnos;
    bool capacityReported = false;
    bool socketReuseReported = false;
    int lastSendErrno = 0;
    QStringList queuedInfo;
    QString queuedFatal;

    int generation = 0;        // incremented at each teardown and reset; stale callbacks compare it
    quint64 startSerial = 0;
};

TraceSession::Private::Private(TraceSession *owner)
    : q(owner)
{
    pump.setInterval(kPumpIntervalMs);
    pump.setTimerType(Qt::PreciseTimer);
    QObject::connect(&pump, &QTimer::timeout, q, [this] { onPumpTick(); });

    coalesce.setSingleShot(true);
    coalesce.setInterval(kCoalesceMs);
    QObject::connect(&coalesce, &QTimer::timeout, q, [this] { flushHops(); });

    dns.setCallback([this](const QHostAddress &address, const QString &name) {
        onHostResolved(address, name);
    });
}

void TraceSession::Private::resetForStart(const QString &newTarget, const TraceOptions &newOptions)
{
    target = newTarget;
    options = sanitized(newOptions);
    targetAddress = QHostAddress();
    lastError.clear();

    hopStats.assign(static_cast<std::size_t>(options.maxHops), HopStats{});
    for (std::size_t i = 0; i < hopStats.size(); ++i)
        hopStats[i].ttl = static_cast<int>(i) + 1;

    pending.clear();
    inFlight.clear();
    rounds.clear();
    nextSeq = 0;
    currentRound = 0;
    roundActive = false;
    nextTtl = 1;
    roundStartNs = 0;
    staggerNs = 0;
    nextRoundAtNs = 0;
    intervalNs = static_cast<qint64>(std::llround(options.intervalSec * 1e9));
    timeoutNs = static_cast<qint64>(std::llround(options.timeoutSec * 1e9));
    roundsResolved = 0;
    roundsCompleted = 0;
    reportedErrnos.clear();
    capacityReported = false;
    socketReuseReported = false;
    lastSendErrno = 0;
    queuedInfo.clear();
    queuedFatal.clear();

    ++generation;
    markDirty();
}

void TraceSession::Private::teardownIo()
{
    ++generation;
    if (lookupId >= 0) {
        QHostInfo::abortHostLookup(lookupId);
        lookupId = -1;
    }
    dns.abortAll();
    pump.stop();
    // No hopsChanged() after teardown: an idle session emits nothing. Slots of
    // stateChanged(Idle) read the final hops() themselves.
    coalesce.stop();
    dirty = false;
    prober.reset();   // disables and releases the notifiers, then closes the fds
    pending.clear();
    inFlight.clear();
    rounds.clear();
    roundActive = false;
    queuedInfo.clear();
    queuedFatal.clear();
}

void TraceSession::Private::fail(const QString &message)
{
    const bool wasActive = state != State::Idle;
    teardownIo();
    lastError = message;
    state = State::Idle;
    const int gen = generation;
    if (wasActive) {
        emit q->stateChanged(State::Idle);
        if (gen != generation)
            return;   // a slot restarted the session; the old error is moot
    }
    emit q->errorOccurred(message);
}

void TraceSession::Private::onForwardLookup(const QHostInfo &info)
{
    lookupId = -1;
    if (info.error() != QHostInfo::NoError || info.addresses().isEmpty()) {
        fail(kResolveFailed);
        return;
    }

    QHostAddress first;   // the resolver's preferred address (glibc sorts by RFC 6724)
    QHostAddress v4;
    QHostAddress v6;
    for (const QHostAddress &raw : info.addresses()) {
        const QHostAddress a = normalizeAddress(raw);
        const bool isV4 = a.protocol() == QHostAddress::IPv4Protocol;
        const bool isV6 = a.protocol() == QHostAddress::IPv6Protocol;
        if (!isV4 && !isV6)
            continue;
        if (first.isNull())
            first = a;
        if (isV4 && v4.isNull())
            v4 = a;
        else if (isV6 && v6.isNull())
            v6 = a;
    }

    QHostAddress chosen;
    switch (options.ipVersion) {
    case TraceOptions::IpVersion::Auto:
        chosen = first;
        break;
    case TraceOptions::IpVersion::V4:
        chosen = v4;
        break;
    case TraceOptions::IpVersion::V6:
        chosen = v6;
        break;
    }
    if (chosen.isNull()) {
        if (options.ipVersion == TraceOptions::IpVersion::V4)
            fail(QStringLiteral("Unable to resolve hostname to an IPv4 address."));
        else if (options.ipVersion == TraceOptions::IpVersion::V6)
            fail(QStringLiteral("Unable to resolve hostname to an IPv6 address."));
        else
            fail(kResolveFailed);
        return;
    }
    targetAddress = chosen;
    beginTracing();
}

void TraceSession::Private::useTargetAddress(const QHostAddress &address)
{
    const bool isV4 = address.protocol() == QHostAddress::IPv4Protocol;
    const bool isV6 = address.protocol() == QHostAddress::IPv6Protocol;
    if ((!isV4 && !isV6)
        || (options.ipVersion == TraceOptions::IpVersion::V4 && !isV4)) {
        fail(QStringLiteral("%1 is not an IPv4 address.").arg(address.toString()));
        return;
    }
    if (options.ipVersion == TraceOptions::IpVersion::V6 && !isV6) {
        fail(QStringLiteral("%1 is not an IPv6 address.").arg(address.toString()));
        return;
    }
    targetAddress = address;
    beginTracing();
}

void TraceSession::Private::beginTracing()
{
    QStringList notices;
    std::unique_ptr<ProbeBackend> p;

    if (testBackendFactory) {
        p = testBackendFactory();
    } else if (options.protocol == TraceOptions::Protocol::Tcp) {
        // The user selected TCP, so no other protocol replaces it. A SYN
        // carries no payload: pingSizeBytes does not apply.
        p = std::make_unique<TcpProber>(targetAddress, static_cast<quint16>(options.tcpPort));
        if (const int err = p->open(); err != 0) {
            fail(QStringLiteral("Unable to create TCP socket: %1").arg(qt_error_string(err)));
            return;
        }
    } else if (options.protocol == TraceOptions::Protocol::Icmp) {
        p = std::make_unique<IcmpProber>(targetAddress, options.pingSizeBytes);
        const int err = p->open();
        if (err == EACCES || err == EPERM || err == EPROTONOSUPPORT) {
            p.reset();
            options.protocol = TraceOptions::Protocol::Udp;
            notices << QStringLiteral(
                "ICMP sockets are not permitted for this user (see sysctl "
                "net.ipv4.ping_group_range); using UDP probes instead.");
        } else if (err != 0) {
            fail(QStringLiteral("Unable to create ICMP socket: %1").arg(qt_error_string(err)));
            return;
        }
    }
    if (!p) {
        // One socket per round slot: a TTL uses a socket again only after its
        // earlier probe there timed out. The pool adds one interval of slack for
        // timer jitter. Each socket has its own source port, so an ICMP error that
        // quotes only the UDP header still identifies the round, if it arrives
        // within the reply timeout (DESIGN.md §3.1).
        const int slotCount = static_cast<int>(std::ceil(options.timeoutSec / options.intervalSec)) + 2;
        p = std::make_unique<UdpProber>(targetAddress, options.pingSizeBytes,
                                        std::clamp(slotCount, 1, kMaxUdpSockets));
        if (const int err = p->open(); err != 0) {
            fail(QStringLiteral("Unable to create UDP socket: %1").arg(qt_error_string(err)));
            return;
        }
    }

    prober = std::move(p);
    inFlight.assign(static_cast<std::size_t>(prober->socketCount()) * hopStats.size(), 0);
    prober->attachNotifiers(q, [this] { onSocketActivity(); });
    clock.start();
    nextRoundAtNs = 0;   // first round at once
    queuedInfo += notices;
    pump.start();

    const int gen = generation;
    if (state != State::Tracing) {
        state = State::Tracing;
        emit q->stateChanged(State::Tracing);
        if (gen != generation)
            return;
    }
    onPumpTick();
}

void TraceSession::Private::onPumpTick()
{
    if (state != State::Tracing || !prober)
        return;
    const int gen = generation;

    drainSocket();
    sweepTimeouts(clock.nsecsElapsed());

    // maxRounds > 0: no new round after that many; the sweeps above go on until
    // the last one resolves, so roundCompleted() arrives for every round.
    const qint64 now = clock.nsecsElapsed();
    const bool roundsLeft = options.maxRounds == 0 || currentRound < options.maxRounds;
    if (roundsLeft && now >= nextRoundAtNs && queuedFatal.isEmpty())
        startRound(now);
    if (queuedFatal.isEmpty())
        sendDue(now, false);

    finishHandler(gen);
}

void TraceSession::Private::onSocketActivity()
{
    if (state != State::Tracing || !prober)
        return;
    const int gen = generation;
    drainSocket();
    finishHandler(gen);
}

// Emits the signals that the handler collected. During socket processing, only
// this function emits signals; no loop over sockets or tables emits one. After
// each emission, it checks the generation again: a slot can stop or restart the
// session.
void TraceSession::Private::finishHandler(int gen)
{
    if (gen != generation)
        return;
    while (!queuedInfo.isEmpty()) {
        const QString message = queuedInfo.takeFirst();
        emit q->infoMessage(message);
        if (gen != generation)
            return;
    }
    if (!queuedFatal.isEmpty()) {
        const QString message = queuedFatal;
        fail(message);
        return;
    }
    checkRounds();
    while (roundsCompleted < roundsResolved) {
        ++roundsCompleted;
        emit q->roundCompleted(roundsCompleted);
        if (gen != generation)
            return;
    }
}

void TraceSession::Private::drainSocket()
{
    if (!prober)
        return;
    prober->drain([this](const ProbeEvent &ev) { onProbeEvent(ev); });
}

void TraceSession::Private::onProbeEvent(const ProbeEvent &ev)
{
    auto it = pending.end();
    if (ev.hasSeq) {
        // Exact match only: a seq that already expired is a late answer. Never
        // credit it to a newer probe.
        it = pending.find(ev.seq);
        if (it != pending.end()
            && (it->slot != ev.slot || (ev.ttl > 0 && it->ttl != ev.ttl)))
            it = pending.end();
    } else if (ev.ttl > 0 && ev.kind != ProbeEvent::Kind::Lost) {
        // UDP port matching: the pending probe for that TTL on that socket
        // (sendProbe() keeps it unique; "oldest" is only a tie-break).
        const int *count = inFlightCount(ev.slot, ev.ttl);
        if (count == nullptr || *count == 0)
            return;
        for (auto cur = pending.begin(); cur != pending.end(); ++cur) {
            if (cur->ttl == ev.ttl && cur->slot == ev.slot
                && (it == pending.end() || cur->sentAtNs < it->sentAtNs))
                it = cur;
        }
    }
    if (it == pending.end())
        return;

    const int ttl = it->ttl;
    if (ttl < 1 || ttl > static_cast<int>(hopStats.size()))
        return;
    HopStats &hop = hopStats[static_cast<std::size_t>(ttl - 1)];

    // The kernel values exclude the time that the answer waited for this thread.
    // replyRttNs() needs the wall clock before the monotonic clock.
    const qint64 wallNs = wallClockNs();
    const qint64 readNs = std::max<qint64>(0, clock.nsecsElapsed() - it->sentAtNs);
    const qint64 rttNs = replyRttNs(ev, readNs, wallNs);
    // A result below readNs is a kernel value. A kernel value at or above the
    // timeout shows an answer that arrived late. Ignore it: the sweep expires the
    // probe, as for an answer after the sweep. The RTT at the read can include a
    // wait for this thread, thus it never makes an answer late.
    if (rttNs >= timeoutNs && rttNs < readNs)
        return;

    switch (ev.kind) {
    case ProbeEvent::Kind::Reply: {
        const double rtt = static_cast<double>(rttNs) / 1e6;
        if (hop.received > 0)
            hop.jitterTotalMs += std::abs(rtt - hop.lastMs);
        ++hop.received;
        hop.lastMs = rtt;
        if (hop.received == 1) {
            hop.bestMs = rtt;
            hop.worstMs = rtt;
        } else {
            hop.bestMs = std::min(hop.bestMs, rtt);
            hop.worstMs = std::max(hop.worstMs, rtt);
        }
        hop.totalMs += rtt;
        hop.addSample(it->round, static_cast<float>(rtt), it->sentAtMs);
        hop.errorText.clear();
        updateHopAddress(hop, ev.responder, ev.reachedTarget);
        break;
    }
    case ProbeEvent::Kind::Error:
        // WinMTR: an ICMP error names the hop but is not a reply (counts as loss).
        ++hop.timedOut;
        hop.addSample(it->round, -1.0f, it->sentAtMs);
        hop.errorText = ev.errorText;
        // An ICMP error from a router does not change the hop address, as upstream
        // (SetName() only). A router can send the error for each TTL up to maxHops.
        // With its address on these hops, currentMaxTtl() stops the probes at that
        // router until the trace ends. An error from the target makes this the target hop.
        if (sameAddress(ev.responder, targetAddress))
            updateHopAddress(hop, ev.responder, true);
        break;
    case ProbeEvent::Kind::Lost:
        ++hop.timedOut;
        hop.addSample(it->round, -1.0f, it->sentAtMs);
        break;
    }
    releasePending(it);
    markDirty();
}

// The only place a pending probe leaves the table (besides a full reset).
QHash<quint16, TraceSession::Private::Pending>::iterator
TraceSession::Private::releasePending(QHash<quint16, Pending>::iterator it)
{
    const auto r = rounds.find(it->round);
    if (r != rounds.end() && r->second.outstanding > 0)
        --r->second.outstanding;
    if (int *count = inFlightCount(it->slot, it->ttl); count != nullptr && *count > 0)
        --*count;
    return pending.erase(it);
}

int *TraceSession::Private::inFlightCount(int slot, int ttl)
{
    const int hops = static_cast<int>(hopStats.size());
    if (slot < 0 || ttl < 1 || ttl > hops)
        return nullptr;
    const std::size_t index = static_cast<std::size_t>(slot) * hopStats.size()
                            + static_cast<std::size_t>(ttl - 1);
    return index < inFlight.size() ? &inFlight[index] : nullptr;
}

// Queues one notice per trace for the caps that skip probes (engine and
// backend together). Skipped probes do not count as sent, so loss stays true.
void TraceSession::Private::reportCapacity()
{
    if (capacityReported)
        return;
    capacityReported = true;
    queuedInfo << QStringLiteral(
        "%1 probes are in flight; skipping new probes until earlier ones are "
        "answered or time out.").arg(pending.size());
}

void TraceSession::Private::startRound(qint64 now)
{
    if (roundActive)
        sendDue(now, true);   // event loop stalled: finish the previous round first
    if (!queuedFatal.isEmpty())
        return;

    ++currentRound;
    rounds[currentRound] = Round{};
    roundActive = true;
    nextTtl = 1;
    roundStartNs = now;
    const int maxTtl = std::max(1, currentMaxTtl());
    staggerNs = std::min<qint64>(kMaxStaggerNs, intervalNs / (2 * maxTtl));

    nextRoundAtNs += intervalNs;
    if (nextRoundAtNs <= now)
        nextRoundAtNs = now + intervalNs;   // fell behind: do not burst to catch up
}

void TraceSession::Private::sendDue(qint64 now, bool force)
{
    while (roundActive) {
        if (nextTtl > options.maxHops || nextTtl > currentMaxTtl()) {
            finishRoundSending();
            break;
        }
        const qint64 due = roundStartNs + static_cast<qint64>(nextTtl - 1) * staggerNs;
        if (!force && due > now)
            break;
        sendProbe(nextTtl);
        ++nextTtl;
    }
}

void TraceSession::Private::sendProbe(int ttl)
{
    if (!prober || ttl < 1 || ttl > static_cast<int>(hopStats.size()))
        return;
    // Engine cap (extreme settings: skip, do not reuse a live seq) and backend
    // cap (TCP: sockets in flight). A skipped probe does not count as sent.
    if (pending.size() >= kMaxPending || !prober->canSend()) {
        reportCapacity();
        return;
    }
    const int slot = (currentRound - 1) % prober->socketCount();
    int *slotInFlight = inFlightCount(slot, ttl);
    if (prober->matchesByPort() && slotInFlight != nullptr && *slotInFlight > 0) {
        // An answer that quotes only the UDP header cannot tell a second probe
        // of this TTL on this socket from the first one.
        if (!socketReuseReported) {
            socketReuseReported = true;
            queuedInfo << QStringLiteral(
                "The reply timeout is too long for this interval with UDP probes; "
                "skipping probes that could not be told apart from earlier ones "
                "still in flight.");
        }
        return;
    }
    Round &round = rounds[currentRound];
    HopStats &hop = hopStats[static_cast<std::size_t>(ttl - 1)];

    const quint16 seq = allocateSeq();
    const qint64 sentAt = clock.nsecsElapsed();
    const qint64 sentAtMs = QDateTime::currentMSecsSinceEpoch();
    const int err = prober->send(ttl, seq, slot);
    ++hop.sent;
    if (err == 0) {
        pending.insert(seq, Pending{ttl, currentRound, slot, sentAt, sentAtMs});
        if (slotInFlight != nullptr)
            ++*slotInFlight;
        ++round.outstanding;
        ++round.sendOk;
    } else {
        ++hop.timedOut;
        hop.addSample(currentRound, -1.0f, sentAtMs);
        ++round.sendFailed;
        lastSendErrno = err;
        if (!reportedErrnos.contains(err)) {
            reportedErrnos.insert(err);
            queuedInfo << QStringLiteral("Failed to send probe: %1").arg(qt_error_string(err));
        }
    }
    markDirty();
}

void TraceSession::Private::finishRoundSending()
{
    if (!roundActive)
        return;
    roundActive = false;
    const auto it = rounds.find(currentRound);
    if (it == rounds.end())
        return;
    it->second.sendingDone = true;
    // A round with a successful send ends an outage: a later failure gets a new notice.
    if (it->second.sendOk > 0)
        reportedErrnos.clear();
    if (currentRound == 1 && it->second.sendOk == 0 && it->second.sendFailed > 0
        && queuedFatal.isEmpty()) {
        queuedFatal = QStringLiteral("Unable to send probes: %1").arg(qt_error_string(lastSendErrno));
    }
}

void TraceSession::Private::sweepTimeouts(qint64 now)
{
    bool changed = false;
    for (auto it = pending.begin(); it != pending.end();) {
        if (now - it->sentAtNs < timeoutNs) {
            ++it;
            continue;
        }
        const int ttl = it->ttl;
        if (ttl >= 1 && ttl <= static_cast<int>(hopStats.size())) {
            HopStats &hop = hopStats[static_cast<std::size_t>(ttl - 1)];
            ++hop.timedOut;
            hop.addSample(it->round, -1.0f, it->sentAtMs);
        }
        if (prober)
            prober->discard(it.key());   // TCP: closes the probe's socket
        it = releasePending(it);
        changed = true;
    }
    if (changed)
        markDirty();
}

void TraceSession::Private::checkRounds()
{
    for (;;) {
        const auto it = rounds.find(roundsResolved + 1);
        if (it == rounds.end() || !it->second.sendingDone || it->second.outstanding > 0)
            break;
        rounds.erase(it);
        ++roundsResolved;
    }
}

quint16 TraceSession::Private::allocateSeq()
{
    // Skip sequence numbers still in flight after a 16-bit wrap.
    for (int i = 0; i < 65536; ++i) {
        const quint16 seq = nextSeq++;
        if (!pending.contains(seq))
            return seq;
    }
    return nextSeq++;
}

// WinMTR GetMax(): stop at the first hop that is the target; otherwise trim
// trailing hops while the last two share the same non-null address.
// "Is the target" also covers a destination that answered from another of its
// addresses (isTarget, see ProbeEvent::reachedTarget).
int TraceSession::Private::currentMaxTtl() const
{
    const int n = static_cast<int>(hopStats.size());
    if (n == 0)
        return 0;
    for (int i = 0; i < n; ++i) {
        const HopStats &hop = hopStats[static_cast<std::size_t>(i)];
        if (hop.isTarget || sameAddress(hop.address, targetAddress))
            return i + 1;
    }
    int max = n;
    while (max > 1) {
        const QHostAddress &last = hopStats[static_cast<std::size_t>(max - 1)].address;
        const QHostAddress &prev = hopStats[static_cast<std::size_t>(max - 2)].address;
        if (last.isNull() || !sameAddress(last, prev))
            break;
        --max;
    }
    return max;
}

void TraceSession::Private::updateHopAddress(HopStats &hop, const QHostAddress &address,
                                             bool reachedTarget)
{
    if (!address.isNull() && !sameAddress(hop.address, address)) {
        // New or changed responder (for example a load-balanced path): resolve again.
        hop.address = address;
        hop.hostname.clear();
        if (options.resolveNames) {
            QString name;
            if (dns.resolve(address, &name))
                hop.hostname = name;
        }
    }
    hop.isTarget = reachedTarget || sameAddress(hop.address, targetAddress);
}

void TraceSession::Private::onHostResolved(const QHostAddress &address, const QString &name)
{
    if (!options.resolveNames)
        return;
    bool changed = false;
    for (HopStats &hop : hopStats) {
        if (sameAddress(hop.address, address) && hop.hostname != name) {
            hop.hostname = name;
            changed = true;
        }
    }
    if (changed)
        markDirty();
}

void TraceSession::Private::markDirty()
{
    dirty = true;
    if (!coalesce.isActive())
        coalesce.start();
}

void TraceSession::Private::flushHops()
{
    if (!dirty)
        return;
    dirty = false;
    emit q->hopsChanged();
}

// ---------------------------------------------------------------------------

TraceSession::TraceSession(QObject *parent)
    : QObject(parent)
    , d(new Private(this))
{
}

TraceSession::~TraceSession()
{
    // Silent teardown: no signals from a half-destroyed object.
    d->teardownIo();
    delete d;
    d = nullptr;
}

TraceSession::State TraceSession::state() const
{
    return d->state;
}

QString TraceSession::target() const
{
    return d->target;
}

QHostAddress TraceSession::targetAddress() const
{
    return d->targetAddress;
}

TraceOptions TraceSession::options() const
{
    return d->options;
}

int TraceSession::roundsCompleted() const
{
    return d->roundsCompleted;
}

QString TraceSession::lastError() const
{
    return d->lastError;
}

QList<HopStats> TraceSession::hops() const
{
    QList<HopStats> out;
    const int max = std::min(d->currentMaxTtl(), static_cast<int>(d->hopStats.size()));
    out.reserve(max);
    for (int i = 0; i < max; ++i) {
        const HopStats &hop = d->hopStats[static_cast<std::size_t>(i)];
        if (hop.sent > 0)
            out.append(hop);
    }
    return out;
}

void TraceSession::start(const QString &target, const TraceOptions &options)
{
    const quint64 serial = ++d->startSerial;
    stop();
    if (serial != d->startSerial)
        return;   // a slot connected to stateChanged() already started a newer trace

    d->resetForStart(target, options);
    const int gen = d->generation;

    QString host = target.trimmed();
    if (host.size() > 2 && host.startsWith(u'[') && host.endsWith(u']'))
        host = host.mid(1, host.size() - 2);

    QHostAddress literal;
    if (!host.isEmpty() && literal.setAddress(host)) {
        // A literal address skips DNS. Socket setup runs from the event loop, so
        // that failures reach the caller in the same way as for host names.
        literal = normalizeAddress(literal);
        d->state = State::Tracing;
        emit stateChanged(State::Tracing);
        if (gen != d->generation)
            return;
        QTimer::singleShot(0, this, [this, gen, literal] {
            if (gen == d->generation && d->state == State::Tracing && !d->prober)
                d->useTargetAddress(literal);
        });
        return;
    }

    d->state = State::Resolving;
    emit stateChanged(State::Resolving);
    if (gen != d->generation)
        return;

    if (host.isEmpty()) {
        QTimer::singleShot(0, this, [this, gen] {
            if (gen == d->generation && d->state == State::Resolving)
                d->fail(kResolveFailed);
        });
        return;
    }

    d->lookupId = QHostInfo::lookupHost(host, this, [this, gen](const QHostInfo &info) {
        if (gen == d->generation && d->state == State::Resolving)
            d->onForwardLookup(info);
    });
}

void TraceSession::stop()
{
    const bool wasActive = d->state != State::Idle;
    d->teardownIo();
    if (!wasActive)
        return;
    d->state = State::Idle;
    emit stateChanged(State::Idle);
}

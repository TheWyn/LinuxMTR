// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Wyn

#pragma once

#include <QHostAddress>
#include <QList>
#include <QPointer>
#include <QString>
#include <QtGlobal>

#include <array>
#include <cstddef>
#include <functional>
#include <vector>

#include <netinet/in.h>
#include <sys/socket.h>

class QObject;
class QSocketNotifier;

// Internal probing engine. The GUI does not see this namespace. The
// TraceSession, HopStats and TraceOptions headers are the only API.
namespace linmtr::core {

// Owns one file descriptor and closes it exactly once.
class UniqueFd
{
public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) : m_fd(fd) {}
    ~UniqueFd() { reset(); }

    UniqueFd(const UniqueFd &) = delete;
    UniqueFd &operator=(const UniqueFd &) = delete;
    UniqueFd(UniqueFd &&other) noexcept : m_fd(other.release()) {}
    UniqueFd &operator=(UniqueFd &&other) noexcept
    {
        if (this != &other)
            reset(other.release());
        return *this;
    }

    int get() const { return m_fd; }
    bool isValid() const { return m_fd >= 0; }
    int release()
    {
        const int fd = m_fd;
        m_fd = -1;
        return fd;
    }
    void reset(int fd = -1);

private:
    int m_fd = -1;
};

// Base port of classic traceroute: the UDP probe for TTL n goes to port
// kUdpBasePort + n - 1.
inline constexpr int kUdpBasePort = 33434;
// Largest TTL that a probe can carry, in hops: the limit of the 8-bit IP field.
inline constexpr int kMaxTtl = 255;
// Largest socket pool of one Prober (UDP round slots). 128 covers all GUI and
// CLI option values: ceil(10 s timeout / 0.1 s interval) + 2 = 102.
inline constexpr int kMaxProberSockets = 128;

// One decoded answer from the socket, matched to a pending probe by the session.
struct ProbeEvent
{
    enum class Kind {
        Reply,  // counts as received (echo reply, time exceeded, UDP/TCP target reached)
        Error,  // ICMP error: sets errorText, does not count as received
        Lost    // local error: the probe counts as lost at once
    };

    Kind kind = Kind::Reply;
    bool hasSeq = false;      // true: seq gives an exact match to one probe
    quint16 seq = 0;
    int ttl = 0;              // > 0: TTL the answer belongs to (UDP port matching)
    int slot = 0;             // index of the socket the answer arrived on (set by Prober)
    bool reachedTarget = false; // the probe ended at the destination host (echo reply,
                                // port unreachable, TCP handshake or RST). The answer
                                // can come from another address (anycast, multi-homed
                                // hosts, a REJECT firewall).
    QHostAddress responder;   // null when the kernel reported no offender
    QString errorText;        // Kind::Error only
    qint64 arrivalNs = 0;     // kernel arrival stamp, CLOCK_REALTIME ns since the epoch; 0 = none
    quint32 rttUs = 0;        // TCP handshake: tcpi_rtt, the RTT that the kernel measured,
                              // in microseconds; 0 = none
};

// Decoded MSG_ERRQUEUE entry (sock_extended_err + msg_name).
struct ErrQueueInfo
{
    enum class Origin { Other, Local, Icmp, Icmp6 };

    Origin origin = Origin::Other;
    quint8 icmpType = 0;
    quint8 icmpCode = 0;
    QHostAddress offender;          // null for AF_UNSPEC
    bool hasDestination = false;    // msg_name present
    QHostAddress destination;       // original destination of the probe
    quint16 destinationPort = 0;    // host byte order
};

// How an ICMP error type/code maps onto WinMTR semantics.
struct IcmpVerdict
{
    enum class Kind { Ignore, TimeExceeded, Unreachable };

    Kind kind = Kind::Ignore;
    QString text;                   // WinMTR error string for Kind::Unreachable
    bool portUnreachable = false;
};

IcmpVerdict classifyIcmp(ErrQueueInfo::Origin origin, quint8 type, quint8 code);

// Decodes the IP_RECVERR or IPV6_RECVERR control message of a MSG_ERRQUEUE
// recvmsg() result into origin, ICMP type and code, and offender. On success,
// it resets the destination fields; the caller sets them from msg_name.
// controlCapacity is the real size of msg->msg_control; the function clamps
// msg->msg_controllen to it. Returns false and does not change info when no
// complete extended error is present.
bool decodeExtendedError(msghdr *msg, std::size_t controlCapacity, ErrQueueInfo *info);

// Decodes the SO_TIMESTAMPNS control message of a recvmsg() result: the time
// when the kernel received the packet, in ns since the Unix epoch (CLOCK_REALTIME).
// controlCapacity is the real size of msg->msg_control; the function clamps
// msg->msg_controllen to it. Returns 0 when no complete stamp is present.
qint64 decodeArrivalStamp(msghdr *msg, std::size_t controlCapacity);

// Returns the RTT of a Reply event in ns. The kernel value is ev.rttUs, else the
// stamp converted at the read: readNs - (wallNs - ev.arrivalNs). A kernel value in
// (0, readNs] is the result; else the result is readNs. readNs is the monotonic
// time from the send to the read. wallNs is CLOCK_REALTIME at the read. Read it
// before readNs: a delay between the two reads then makes the result larger.
qint64 replyRttNs(const ProbeEvent &ev, qint64 readNs, qint64 wallNs);

// Address helpers. sameAddress() ignores IPv6 scope ids and is false for null addresses.
bool sameAddress(const QHostAddress &a, const QHostAddress &b);
QHostAddress normalizeAddress(const QHostAddress &address);   // IPv4-mapped IPv6 -> IPv4
bool addressFromSockaddr(const sockaddr_storage &ss, socklen_t len,
                         QHostAddress *address, quint16 *port);

// What the session needs from a probing backend, so it drives every protocol
// the same way. The 16-bit seq of the session identifies each probe; answers
// come back as ProbeEvents from drain().
class ProbeBackend
{
public:
    using EventSink = std::function<void(const ProbeEvent &)>;

    ProbeBackend() = default;
    virtual ~ProbeBackend() = default;
    ProbeBackend(const ProbeBackend &) = delete;
    ProbeBackend &operator=(const ProbeBackend &) = delete;

    virtual int open() = 0;
    virtual int socketCount() const = 0;
    virtual int send(int ttl, quint16 seq, int slot) = 0;
    virtual void drain(const EventSink &sink) = 0;
    virtual void attachNotifiers(QObject *context, const std::function<void()> &onActivity) = 0;

    // False while the backend cannot take one more probe (resource cap). The
    // session then skips the send and does not count it.
    virtual bool canSend() const { return true; }

    // True when an answer can arrive without the seq of the probe. The session
    // then matches it by TTL and socket only (UDP: routers that quote only the
    // UDP header). Such a match is exact only while each socket has at most one
    // probe per TTL in flight. Thus the session never sends a probe on a socket
    // that still has one pending for the same TTL.
    virtual bool matchesByPort() const { return false; }

    // The reply timeout of the probe expired and the session stopped waiting.
    // The backend releases what the probe holds.
    virtual void discard(quint16 seq) { Q_UNUSED(seq); }
};

// Non-blocking datagram socket(s) that send TTL-limited probes to one target and
// decode both the normal receive queue and the IP_RECVERR error queue.
// The ICMP backend uses one socket. The UDP backend can use a small pool, one
// socket per in-flight round slot, so that the socket identifies the answer.
class Prober : public ProbeBackend
{
public:
    ~Prober() override;
    Prober(const Prober &) = delete;
    Prober &operator=(const Prober &) = delete;

    // Creates and configures all sockets. Returns 0 or an errno value; on
    // failure no socket stays open.
    int open() override;
    int socketCount() const override { return m_socketCount; }

    // Sends one probe with the given TTL on socket (slot % socketCount()).
    // Returns 0 or an errno value.
    int send(int ttl, quint16 seq, int slot) override;

    // Reads both queues of every socket until EAGAIN, at most 1024 messages per
    // call. Each message decodes into zero or more events for the sink. Every
    // event carries the slot of the socket it arrived on and the arrival stamp
    // of its message. Never blocks.
    void drain(const EventSink &sink) override;

    // Creates Read and Exception notifiers (children of context) on each socket.
    // When the prober goes away, it disables them and releases them with
    // deleteLater(). Thus onActivity can safely destroy the prober.
    void attachNotifiers(QObject *context, const std::function<void()> &onActivity) override;

    bool isIPv6() const { return m_v6; }
    const QHostAddress &target() const { return m_target; }

protected:
    Prober(const QHostAddress &target, int payloadBytes, int socketCount);

    virtual int socketProtocol() const = 0;
    virtual int configureSocket(int fd);
    virtual void buildPacket(int ttl, quint16 seq, std::vector<quint8> &packet) const = 0;
    virtual quint16 destinationPort(int ttl) const = 0;
    virtual void handleData(const QHostAddress &from, quint16 fromPort,
                            const quint8 *data, std::size_t len, const EventSink &sink) = 0;
    virtual void handleError(const ErrQueueInfo &info,
                             const quint8 *data, std::size_t len, const EventSink &sink) = 0;

    int payloadBytes() const { return m_payloadBytes; }

private:
    enum class ReadResult { Empty, Consumed };

    ReadResult readOne(int slot, bool errQueue, const EventSink &sink);
    bool makeDestination(quint16 port, sockaddr_storage *ss, socklen_t *len) const;
    void detachNotifiers();
    void closeSockets();

    QHostAddress m_target;
    bool m_v6 = false;
    quint32 m_scopeId = 0;
    int m_payloadBytes = 0;
    int m_socketCount = 1;
    int m_nextDrainSlot = 0;
    std::vector<quint8> m_txBuf;
    std::array<quint8, 2048> m_rxBuf{};
    QList<QPointer<QSocketNotifier>> m_notifiers;
    std::vector<UniqueFd> m_fds;
};

} // namespace linmtr::core

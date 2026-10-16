// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Wyn

#include <QDateTime>
#include <QElapsedTimer>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTcpServer>
#include <QtTest>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <linux/errqueue.h>

#include "cli/ReportRunner.h"
#include "core/Prober.h"
#include "core/TraceSession.h"

using linmtr::core::ErrQueueInfo;
using linmtr::core::IcmpVerdict;
using linmtr::core::ProbeEvent;
using linmtr::core::UniqueFd;
using linmtr::core::classifyIcmp;
using linmtr::core::decodeArrivalStamp;
using linmtr::core::decodeExtendedError;
using linmtr::core::replyRttNs;

Q_DECLARE_METATYPE(ErrQueueInfo::Origin)
Q_DECLARE_METATYPE(IcmpVerdict::Kind)
Q_DECLARE_METATYPE(TraceOptions::Protocol)

namespace {

constexpr std::size_t kEeSize = sizeof(sock_extended_err);
constexpr std::size_t kHeaderSize = sizeof(cmsghdr);

// Holds crafted control messages for the decoders. put() sets cmsg_len
// apart from the payload size, and header() sets msg_controllen, so that a test
// can cut either one.
struct Control
{
    alignas(cmsghdr) unsigned char bytes[512] = {};
    msghdr msg{};

    // Writes a message at offset with the given header fields and payload.
    // Returns the offset of the next message.
    std::size_t put(std::size_t offset, int level, int type, const std::vector<unsigned char> &payload,
                    std::size_t cmsgLen)
    {
        auto *c = reinterpret_cast<cmsghdr *>(bytes + offset);
        c->cmsg_level = level;
        c->cmsg_type = type;
        c->cmsg_len = cmsgLen;
        std::memcpy(CMSG_DATA(c), payload.data(), payload.size());
        return offset + CMSG_SPACE(payload.size());
    }

    msghdr *header(std::size_t controlLen)
    {
        msg.msg_control = bytes;
        msg.msg_controllen = controlLen;
        return &msg;
    }
};

std::vector<unsigned char> errPayload(quint8 origin, quint8 type, quint8 code,
                                      const void *offender, std::size_t offenderLen)
{
    sock_extended_err ee{};
    ee.ee_errno = EHOSTUNREACH;
    ee.ee_origin = origin;
    ee.ee_type = type;
    ee.ee_code = code;
    std::vector<unsigned char> out(kEeSize + offenderLen);
    std::memcpy(out.data(), &ee, kEeSize);
    if (offenderLen > 0)
        std::memcpy(out.data() + kEeSize, offender, offenderLen);
    return out;
}

// The SO_TIMESTAMPNS payload of the decoder tests, and its value in ns.
constexpr qint64 kStampSec = 1'760'000'000;
constexpr long kStampNsec = 123'456'789;
constexpr qint64 kStampNs = kStampSec * 1'000'000'000 + kStampNsec;

std::vector<unsigned char> stampPayload()
{
    timespec ts{};
    ts.tv_sec = kStampSec;
    ts.tv_nsec = kStampNsec;
    std::vector<unsigned char> out(sizeof(ts));
    std::memcpy(out.data(), &ts, sizeof(ts));
    return out;
}

// The time from the send to the read in the replyRtt() rows, in ns.
constexpr qint64 kReadNs = 300'000'000;

// How long the live RTT test blocks the thread while an answer waits, in ms.
constexpr int kStallMs = 300;

// The reply timeout of the late-answer test, in ns: between two ticks of the 20 ms pump.
constexpr qint64 kTimeoutNs = 110'000'000;

sockaddr_in v4(const char *text)
{
    sockaddr_in sin{};
    sin.sin_family = AF_INET;
    inet_pton(AF_INET, text, &sin.sin_addr);
    return sin;
}

sockaddr_in6 v6(const char *text)
{
    sockaddr_in6 sin6{};
    sin6.sin6_family = AF_INET6;
    inet_pton(AF_INET6, text, &sin6.sin6_addr);
    return sin6;
}

// Returns an info value that decodeExtendedError() must not change when it
// returns false.
ErrQueueInfo sentinel()
{
    ErrQueueInfo info;
    info.icmpType = 0xAA;
    info.icmpCode = 0xBB;
    return info;
}

void verifyUntouched(const ErrQueueInfo &info)
{
    QCOMPARE(info.icmpType, quint8(0xAA));
    QCOMPARE(info.icmpCode, quint8(0xBB));
    QVERIFY(info.offender.isNull());
}

// Answers each probe with the event that answer() gives for its TTL. The session
// reads an answer at the first pump tick at least holdNs after the send. An answer
// that sets its TTL gets no seq. The session then matches it by TTL and socket, as
// an answer that quotes only the UDP header.
class FakeBackend : public linmtr::core::ProbeBackend
{
public:
    explicit FakeBackend(std::function<ProbeEvent(int)> answer, qint64 holdNs = 0)
        : m_answer(std::move(answer)), m_holdNs(holdNs)
    {
        m_clock.start();
    }

    int open() override { return 0; }
    int socketCount() const override { return 1; }
    int send(int ttl, quint16 seq, int slot) override
    {
        ProbeEvent ev = m_answer(ttl);
        ev.hasSeq = ev.ttl == 0;
        ev.seq = seq;
        ev.slot = slot;
        m_answers.push_back({m_clock.nsecsElapsed() + m_holdNs, ev});
        return 0;
    }
    void drain(const EventSink &sink) override
    {
        // All answers have the same hold, so the ready ones come first.
        const qint64 now = m_clock.nsecsElapsed();
        std::vector<ProbeEvent> ready;
        while (!m_answers.empty() && m_answers.front().first <= now) {
            ready.push_back(m_answers.front().second);
            m_answers.erase(m_answers.begin());
        }
        for (const ProbeEvent &ev : ready)
            sink(ev);
    }
    void attachNotifiers(QObject *, const std::function<void()> &) override {}

private:
    std::function<ProbeEvent(int)> m_answer;
    qint64 m_holdNs = 0;
    QElapsedTimer m_clock;
    std::vector<std::pair<qint64, ProbeEvent>> m_answers;   // ready time, answer
};

// CLOCK_REALTIME, the clock of the kernel arrival stamps, in ns since the epoch.
qint64 wallNowNs()
{
    timespec now{};
    ::clock_gettime(CLOCK_REALTIME, &now);
    return qint64(now.tv_sec) * 1'000'000'000 + now.tv_nsec;
}

// Returns an empty string when this process can send an ICMP echo request to
// 127.0.0.1 through an unprivileged ping socket, else the reason it cannot.
QString loopbackPingUnavailable()
{
    const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_ICMP);
    if (fd < 0)
        return QStringLiteral("no ICMP ping socket: %1").arg(qt_error_string(errno));
    const sockaddr_in to = v4("127.0.0.1");
    const unsigned char echo[8] = {8, 0, 0, 0, 0, 0, 0, 0};   // the kernel sets id and checksum
    const ssize_t sent = ::sendto(fd, echo, sizeof(echo), MSG_DONTWAIT,
                                  reinterpret_cast<const sockaddr *>(&to), sizeof(to));
    const int err = errno;
    ::close(fd);
    if (sent < 0)
        return QStringLiteral("cannot send to 127.0.0.1: %1").arg(qt_error_string(err));
    return {};
}

// Opens a UDP socket with SO_TIMESTAMPNS on 127.0.0.1 into *hold and waits until
// the kernel stamps its datagrams when they arrive. The kernel turns the stamps
// on with a delay after the first socket asks for them. Until then, recvmsg()
// gives the read time as the stamp. Keep *hold open during the test, so that the
// stamps stay on. Returns an empty string, else the reason.
QString arrivalStampsUnavailable(UniqueFd *hold)
{
    hold->reset(::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    sockaddr_in self = v4("127.0.0.1");
    socklen_t selfLen = sizeof(self);
    const int one = 1;
    if (!hold->isValid()
        || ::setsockopt(hold->get(), SOL_SOCKET, SO_TIMESTAMPNS, &one, sizeof(one)) != 0
        || ::bind(hold->get(), reinterpret_cast<const sockaddr *>(&self), sizeof(self)) != 0
        || ::getsockname(hold->get(), reinterpret_cast<sockaddr *>(&self), &selfLen) != 0)
        return QStringLiteral("no time-stamped UDP socket: %1").arg(qt_error_string(errno));

    for (int attempt = 0; attempt < 200; ++attempt) {
        const char byte = 0;
        if (::sendto(hold->get(), &byte, 1, 0, reinterpret_cast<const sockaddr *>(&self), sizeof(self)) < 0)
            return QStringLiteral("cannot send to 127.0.0.1: %1").arg(qt_error_string(errno));
        QTest::qSleep(10);
        // A stamp that the kernel takes at the read is not earlier than this time.
        const qint64 beforeReadNs = QDateTime::currentMSecsSinceEpoch() * 1'000'000;
        char data = 0;
        iovec iov{&data, 1};
        alignas(cmsghdr) unsigned char control[64];
        msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);
        if (::recvmsg(hold->get(), &msg, MSG_DONTWAIT) < 0)
            continue;
        // Not decodeArrivalStamp(): a bug there must fail the RTT test, not skip it.
        const cmsghdr *c = CMSG_FIRSTHDR(&msg);
        timespec ts{};
        if (c != nullptr && c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMPNS) {
            std::memcpy(&ts, CMSG_DATA(c), sizeof(ts));
            if (qint64(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec < beforeReadNs)
                return {};
        }
    }
    return QStringLiteral("the kernel did not turn on the arrival stamps");
}

} // namespace

// Feeds crafted control data to decodeExtendedError() and decodeArrivalStamp(),
// and checks the classifyIcmp() table and the replyRttNs() rule. Also drives a
// session through a fake backend and runs short live traces over loopback.
class TestCoreParsers : public QObject
{
    Q_OBJECT

private slots:
    void decodeValidV4();
    void decodeValidV6();
    void decodeTruncated_data();
    void decodeTruncated();
    void decodeUnspecOffender();
    void decodeWrongLevelOrType_data();
    void decodeWrongLevelOrType();
    void decodeSkipsOtherMessages();
    void decodeZeroLengthControl();
    void decodeStamp_data();
    void decodeStamp();
    void decodeStampSkipsOtherMessages();
    void replyRtt_data();
    void replyRtt();
    void classify_data();
    void classify();
    void hopSampleOrderAndCap();
    void sessionIcmpError_data();
    void sessionIcmpError();
    void sessionLateAnswer_data();
    void sessionLateAnswer();
    void liveLoopbackTrace();
    void liveStalledReadRtt_data();
    void liveStalledReadRtt();
    void reportRunnerStopsOnSignal();
};

void TestCoreParsers::decodeValidV4()
{
    const sockaddr_in off = v4("192.0.2.1");
    const auto payload = errPayload(SO_EE_ORIGIN_ICMP, 11, 0, &off, sizeof(off));
    Control ctl;
    const std::size_t end = ctl.put(0, IPPROTO_IP, IP_RECVERR, payload, CMSG_LEN(payload.size()));

    ErrQueueInfo info;
    info.hasDestination = true;
    info.destinationPort = 7;
    QVERIFY(decodeExtendedError(ctl.header(end), sizeof(ctl.bytes), &info));
    QCOMPARE(info.origin, ErrQueueInfo::Origin::Icmp);
    QCOMPARE(info.icmpType, quint8(11));
    QCOMPARE(info.icmpCode, quint8(0));
    QCOMPARE(info.offender, QHostAddress(QStringLiteral("192.0.2.1")));
    // Success resets the destination fields; the caller fills them in.
    QVERIFY(!info.hasDestination);
    QCOMPARE(info.destinationPort, quint16(0));
}

void TestCoreParsers::decodeValidV6()
{
    const sockaddr_in6 off = v6("2001:db8::1");
    const auto payload = errPayload(SO_EE_ORIGIN_ICMP6, 3, 0, &off, sizeof(off));
    Control ctl;
    const std::size_t end = ctl.put(0, IPPROTO_IPV6, IPV6_RECVERR, payload, CMSG_LEN(payload.size()));

    ErrQueueInfo info;
    QVERIFY(decodeExtendedError(ctl.header(end), sizeof(ctl.bytes), &info));
    QCOMPARE(info.origin, ErrQueueInfo::Origin::Icmp6);
    QCOMPARE(info.icmpType, quint8(3));
    QCOMPARE(info.icmpCode, quint8(0));
    QCOMPARE(info.offender, QHostAddress(QStringLiteral("2001:db8::1")));

    // An IPv4-mapped offender decodes as plain IPv4.
    const sockaddr_in6 mapped = v6("::ffff:192.0.2.9");
    const auto mappedPayload = errPayload(SO_EE_ORIGIN_ICMP6, 1, 4, &mapped, sizeof(mapped));
    Control ctl2;
    const std::size_t end2 =
        ctl2.put(0, IPPROTO_IPV6, IPV6_RECVERR, mappedPayload, CMSG_LEN(mappedPayload.size()));
    QVERIFY(decodeExtendedError(ctl2.header(end2), sizeof(ctl2.bytes), &info));
    QCOMPARE(info.icmpType, quint8(1));
    QCOMPARE(info.icmpCode, quint8(4));
    QCOMPARE(info.offender.protocol(), QHostAddress::IPv4Protocol);
    QCOMPARE(info.offender, QHostAddress(QStringLiteral("192.0.2.9")));
}

void TestCoreParsers::decodeTruncated_data()
{
    // ipv6: the message is IPV6_RECVERR with a sockaddr_in6 offender.
    // cmsgLen: the cmsg_len field. controlLen: msg_controllen. capacity: the
    // real buffer size that the caller passes.
    QTest::addColumn<bool>("ipv6");
    QTest::addColumn<qulonglong>("cmsgLen");
    QTest::addColumn<qulonglong>("controlLen");
    QTest::addColumn<qulonglong>("capacity");
    QTest::addColumn<bool>("decoded");
    QTest::addColumn<bool>("hasOffender");

    const qulonglong in4 = sizeof(sockaddr_in);
    const qulonglong in6 = sizeof(sockaddr_in6);
    const qulonglong full4 = CMSG_LEN(kEeSize + in4);
    const qulonglong full6 = CMSG_LEN(kEeSize + in6);
    const qulonglong cap = sizeof(Control::bytes);
    const auto lenFor = [](qulonglong payload) { return qulonglong(CMSG_LEN(payload)); };
    const auto spaceFor = [](qulonglong cmsgLen) { return qulonglong(CMSG_ALIGN(cmsgLen)); };

    // cmsg_len stops at every boundary; msg_controllen matches it.
    const struct { const char *name; qulonglong payload; bool decoded; bool offender; } cuts4[] = {
        {"cmsg_len: no payload", 0, false, false},
        {"cmsg_len: extended error - 1", kEeSize - 1, false, false},
        {"cmsg_len: extended error only", kEeSize, true, false},
        {"cmsg_len: offender 1 byte", kEeSize + 1, true, false},
        {"cmsg_len: offender family only", kEeSize + sizeof(sa_family_t), true, false},
        {"cmsg_len: sockaddr_in - 1", kEeSize + in4 - 1, true, false},
        {"cmsg_len: complete v4", kEeSize + in4, true, true},
    };
    for (const auto &c : cuts4) {
        QTest::newRow(c.name) << false << lenFor(c.payload) << spaceFor(lenFor(c.payload)) << cap
                              << c.decoded << c.offender;
    }
    QTest::newRow("cmsg_len: v6 sockaddr_in6 - 1")
        << true << lenFor(kEeSize + in6 - 1) << spaceFor(lenFor(kEeSize + in6 - 1)) << cap << true << false;
    QTest::newRow("cmsg_len: v6 only sockaddr_in bytes")
        << true << lenFor(kEeSize + in4) << spaceFor(lenFor(kEeSize + in4)) << cap << true << false;
    QTest::newRow("cmsg_len: complete v6") << true << full6 << spaceFor(full6) << cap << true << true;

    // cmsg_len below the header size: the loop must stop, not spin.
    QTest::newRow("cmsg_len: zero") << false << 0ull << spaceFor(full4) << cap << false << false;
    QTest::newRow("cmsg_len: header - 1")
        << false << qulonglong(kHeaderSize - 1) << spaceFor(full4) << cap << false << false;

    // cmsg_len claims the complete message; msg_controllen cuts it.
    QTest::newRow("controllen: header - 1")
        << false << full4 << qulonglong(kHeaderSize - 1) << cap << false << false;
    QTest::newRow("controllen: header only") << false << full4 << qulonglong(kHeaderSize) << cap << false << false;
    QTest::newRow("controllen: extended error - 1")
        << false << full4 << lenFor(kEeSize - 1) << cap << false << false;
    QTest::newRow("controllen: extended error only") << false << full4 << lenFor(kEeSize) << cap << true << false;
    QTest::newRow("controllen: sockaddr_in - 1")
        << false << full4 << lenFor(kEeSize + in4 - 1) << cap << true << false;
    QTest::newRow("controllen: v6 sockaddr_in6 - 1")
        << true << full6 << lenFor(kEeSize + in6 - 1) << cap << true << false;

    // msg_controllen claims the whole buffer; the capacity cuts it.
    QTest::newRow("capacity: extended error - 1") << false << full4 << cap << lenFor(kEeSize - 1) << false << false;
    QTest::newRow("capacity: sockaddr_in - 1") << false << full4 << cap << lenFor(kEeSize + in4 - 1) << true << false;
    QTest::newRow("capacity: complete v4") << false << full4 << cap << full4 << true << true;
}

void TestCoreParsers::decodeTruncated()
{
    QFETCH(bool, ipv6);
    QFETCH(qulonglong, cmsgLen);
    QFETCH(qulonglong, controlLen);
    QFETCH(qulonglong, capacity);
    QFETCH(bool, decoded);
    QFETCH(bool, hasOffender);

    Control ctl;
    if (ipv6) {
        const sockaddr_in6 off = v6("2001:db8::2");
        ctl.put(0, IPPROTO_IPV6, IPV6_RECVERR, errPayload(SO_EE_ORIGIN_ICMP6, 3, 0, &off, sizeof(off)),
                cmsgLen);
    } else {
        const sockaddr_in off = v4("198.51.100.2");
        ctl.put(0, IPPROTO_IP, IP_RECVERR, errPayload(SO_EE_ORIGIN_ICMP, 11, 0, &off, sizeof(off)),
                cmsgLen);
    }

    ErrQueueInfo info = sentinel();
    msghdr *msg = ctl.header(controlLen);
    QCOMPARE(decodeExtendedError(msg, capacity, &info), decoded);
    QCOMPARE(qulonglong(msg->msg_controllen), qMin(controlLen, capacity));
    if (!decoded) {
        verifyUntouched(info);
        return;
    }
    QCOMPARE(info.origin, ipv6 ? ErrQueueInfo::Origin::Icmp6 : ErrQueueInfo::Origin::Icmp);
    QCOMPARE(info.icmpType, quint8(ipv6 ? 3 : 11));
    QCOMPARE(info.icmpCode, quint8(0));
    QCOMPARE(!info.offender.isNull(), hasOffender);
    if (hasOffender) {
        QCOMPARE(info.offender, QHostAddress(ipv6 ? QStringLiteral("2001:db8::2")
                                                  : QStringLiteral("198.51.100.2")));
    }
}

void TestCoreParsers::decodeUnspecOffender()
{
    // Local errors carry an AF_UNSPEC offender of sockaddr_in size.
    sockaddr_in off{};
    off.sin_family = AF_UNSPEC;
    const auto payload = errPayload(SO_EE_ORIGIN_LOCAL, 0, 0, &off, sizeof(off));
    Control ctl;
    const std::size_t end = ctl.put(0, IPPROTO_IP, IP_RECVERR, payload, CMSG_LEN(payload.size()));

    ErrQueueInfo info = sentinel();
    QVERIFY(decodeExtendedError(ctl.header(end), sizeof(ctl.bytes), &info));
    QCOMPARE(info.origin, ErrQueueInfo::Origin::Local);
    QCOMPARE(info.icmpType, quint8(0));
    QCOMPARE(info.icmpCode, quint8(0));
    QVERIFY(info.offender.isNull());
}

void TestCoreParsers::decodeWrongLevelOrType_data()
{
    QTest::addColumn<int>("level");
    QTest::addColumn<int>("type");

    QTest::newRow("SOL_SOCKET, IP_RECVERR") << int(SOL_SOCKET) << int(IP_RECVERR);
    QTest::newRow("IPPROTO_IP, IPV6_RECVERR") << int(IPPROTO_IP) << int(IPV6_RECVERR);
    QTest::newRow("IPPROTO_IPV6, IP_RECVERR") << int(IPPROTO_IPV6) << int(IP_RECVERR);
    QTest::newRow("IPPROTO_IP, IP_TTL") << int(IPPROTO_IP) << int(IP_TTL);
}

void TestCoreParsers::decodeWrongLevelOrType()
{
    QFETCH(int, level);
    QFETCH(int, type);

    const sockaddr_in off = v4("192.0.2.1");
    const auto payload = errPayload(SO_EE_ORIGIN_ICMP, 11, 0, &off, sizeof(off));
    Control ctl;
    const std::size_t end = ctl.put(0, level, type, payload, CMSG_LEN(payload.size()));

    ErrQueueInfo info = sentinel();
    QVERIFY(!decodeExtendedError(ctl.header(end), sizeof(ctl.bytes), &info));
    verifyUntouched(info);
}

void TestCoreParsers::decodeSkipsOtherMessages()
{
    const int ttl = 64;
    std::vector<unsigned char> ttlPayload(sizeof(ttl));
    std::memcpy(ttlPayload.data(), &ttl, sizeof(ttl));
    const sockaddr_in off = v4("192.0.2.77");
    const auto payload = errPayload(SO_EE_ORIGIN_ICMP, 3, 13, &off, sizeof(off));

    Control ctl;
    std::size_t end = ctl.put(0, IPPROTO_IP, IP_TTL, ttlPayload, CMSG_LEN(ttlPayload.size()));
    end = ctl.put(end, IPPROTO_IP, IP_RECVERR, payload, CMSG_LEN(payload.size()));

    ErrQueueInfo info;
    QVERIFY(decodeExtendedError(ctl.header(end), sizeof(ctl.bytes), &info));
    QCOMPARE(info.icmpType, quint8(3));
    QCOMPARE(info.icmpCode, quint8(13));
    QCOMPARE(info.offender, QHostAddress(QStringLiteral("192.0.2.77")));
}

void TestCoreParsers::decodeZeroLengthControl()
{
    Control ctl;
    ErrQueueInfo info = sentinel();
    QVERIFY(!decodeExtendedError(ctl.header(0), sizeof(ctl.bytes), &info));
    verifyUntouched(info);

    // A non-zero msg_controllen with a capacity of zero.
    QVERIFY(!decodeExtendedError(ctl.header(sizeof(ctl.bytes)), 0, &info));
    QCOMPARE(ctl.msg.msg_controllen, std::size_t(0));
    verifyUntouched(info);

    msghdr empty{};
    QVERIFY(!decodeExtendedError(&empty, 0, &info));
    verifyUntouched(info);
}

void TestCoreParsers::decodeStamp_data()
{
    // level, type: the message header. cmsgLen: the cmsg_len field. controlLen:
    // msg_controllen. capacity: the real buffer size that the caller passes.
    QTest::addColumn<int>("level");
    QTest::addColumn<int>("type");
    QTest::addColumn<qulonglong>("cmsgLen");
    QTest::addColumn<qulonglong>("controlLen");
    QTest::addColumn<qulonglong>("capacity");
    QTest::addColumn<qlonglong>("stampNs");

    const int sol = SOL_SOCKET;
    const int ns = SCM_TIMESTAMPNS;
    const qulonglong full = CMSG_LEN(sizeof(timespec));
    const qulonglong cap = sizeof(Control::bytes);

    QTest::newRow("complete") << sol << ns << full << full << cap << kStampNs;
    QTest::newRow("capacity: complete") << sol << ns << full << cap << full << kStampNs;
    QTest::newRow("SOL_SOCKET, SCM_TIMESTAMP") << sol << int(SCM_TIMESTAMP) << full << full << cap << 0ll;
    QTest::newRow("IPPROTO_IP, SCM_TIMESTAMPNS") << int(IPPROTO_IP) << ns << full << full << cap << 0ll;
    QTest::newRow("cmsg_len: timespec - 1") << sol << ns << full - 1 << full << cap << 0ll;
    QTest::newRow("cmsg_len: zero") << sol << ns << 0ull << full << cap << 0ll;
    QTest::newRow("controllen: timespec - 1") << sol << ns << full << full - 1 << cap << 0ll;
    QTest::newRow("controllen: zero") << sol << ns << full << 0ull << cap << 0ll;
    QTest::newRow("capacity: timespec - 1") << sol << ns << full << cap << full - 1 << 0ll;
}

void TestCoreParsers::decodeStamp()
{
    QFETCH(int, level);
    QFETCH(int, type);
    QFETCH(qulonglong, cmsgLen);
    QFETCH(qulonglong, controlLen);
    QFETCH(qulonglong, capacity);
    QFETCH(qlonglong, stampNs);

    Control ctl;
    ctl.put(0, level, type, stampPayload(), cmsgLen);
    msghdr *msg = ctl.header(controlLen);
    QCOMPARE(decodeArrivalStamp(msg, capacity), stampNs);
    QCOMPARE(qulonglong(msg->msg_controllen), qMin(controlLen, capacity));
}

void TestCoreParsers::decodeStampSkipsOtherMessages()
{
    const sockaddr_in off = v4("192.0.2.5");
    const auto err = errPayload(SO_EE_ORIGIN_ICMP, 11, 0, &off, sizeof(off));
    const auto stamp = stampPayload();

    Control ctl;
    std::size_t end = ctl.put(0, IPPROTO_IP, IP_RECVERR, err, CMSG_LEN(err.size()));
    end = ctl.put(end, SOL_SOCKET, SCM_TIMESTAMPNS, stamp, CMSG_LEN(stamp.size()));
    QCOMPARE(decodeArrivalStamp(ctl.header(end), sizeof(ctl.bytes)), kStampNs);
}

void TestCoreParsers::replyRtt_data()
{
    // rttUs and arrivalNs: the kernel values of the event. The read is kReadNs
    // after the send, when the wall clock shows kStampNs.
    QTest::addColumn<uint>("rttUs");
    QTest::addColumn<qlonglong>("arrivalNs");
    QTest::addColumn<qlonglong>("rttNs");

    const qlonglong sent = kStampNs - kReadNs;   // the send time on the wall clock
    QTest::newRow("no kernel value") << 0u << 0ll << kReadNs;
    QTest::newRow("stamp: 1 ms after the send") << 0u << sent + 1'000'000 << 1'000'000ll;
    QTest::newRow("stamp: at the send") << 0u << sent << kReadNs;
    QTest::newRow("stamp: before the send (forward step, suspend)") << 0u << sent - 1 << kReadNs;
    QTest::newRow("stamp: after the read (backward step)") << 0u << kStampNs + 1 << kReadNs;
    QTest::newRow("tcpi_rtt: 23 us") << 23u << 0ll << 23'000ll;
    QTest::newRow("tcpi_rtt: above the read") << 300'001u << 0ll << kReadNs;
    QTest::newRow("tcpi_rtt and a stamp") << 23u << sent + 1'000'000 << 23'000ll;
}

void TestCoreParsers::replyRtt()
{
    QFETCH(uint, rttUs);
    QFETCH(qlonglong, arrivalNs);
    QFETCH(qlonglong, rttNs);

    ProbeEvent ev;
    ev.rttUs = rttUs;
    ev.arrivalNs = arrivalNs;
    QCOMPARE(replyRttNs(ev, kReadNs, kStampNs), rttNs);
}

void TestCoreParsers::classify_data()
{
    QTest::addColumn<ErrQueueInfo::Origin>("origin");
    QTest::addColumn<int>("type");
    QTest::addColumn<int>("code");
    QTest::addColumn<IcmpVerdict::Kind>("kind");
    QTest::addColumn<QString>("text");
    QTest::addColumn<bool>("portUnreachable");

    using O = ErrQueueInfo::Origin;
    using K = IcmpVerdict::Kind;
    const QString netUnreach = QStringLiteral("Destination network unreachable.");
    const QString hostUnreach = QStringLiteral("Destination host unreachable.");
    const QString prohibited = QStringLiteral("Destination host unreachable (administratively prohibited).");
    const QString portUnreach = QStringLiteral("Destination port unreachable.");
    const QString tooBig = QStringLiteral("Packet was too big.");
    const QString reassembly = QStringLiteral("The time to live expired during fragment reassembly.");
    const QString paramProblem = QStringLiteral("Parameter problem.");

    QTest::newRow("v4 3/0") << O::Icmp << 3 << 0 << K::Unreachable << netUnreach << false;
    QTest::newRow("v4 3/1") << O::Icmp << 3 << 1 << K::Unreachable << hostUnreach << false;
    QTest::newRow("v4 3/2") << O::Icmp << 3 << 2 << K::Unreachable
                            << QStringLiteral("Destination protocol unreachable.") << false;
    QTest::newRow("v4 3/3") << O::Icmp << 3 << 3 << K::Unreachable << portUnreach << true;
    QTest::newRow("v4 3/4") << O::Icmp << 3 << 4 << K::Unreachable << tooBig << false;
    QTest::newRow("v4 3/13") << O::Icmp << 3 << 13 << K::Unreachable << prohibited << false;
    QTest::newRow("v4 3/9") << O::Icmp << 3 << 9 << K::Unreachable
                            << QStringLiteral("Destination unreachable (code 9).") << false;
    QTest::newRow("v4 11/0") << O::Icmp << 11 << 0 << K::TimeExceeded << QString() << false;
    QTest::newRow("v4 11/1") << O::Icmp << 11 << 1 << K::Unreachable << reassembly << false;
    QTest::newRow("v4 12/0") << O::Icmp << 12 << 0 << K::Unreachable << paramProblem << false;
    QTest::newRow("v4 5/0 redirect") << O::Icmp << 5 << 0 << K::Ignore << QString() << false;
    QTest::newRow("v4 5/1 redirect") << O::Icmp << 5 << 1 << K::Ignore << QString() << false;
    QTest::newRow("v4 4/0 source quench") << O::Icmp << 4 << 0 << K::Ignore << QString() << false;

    QTest::newRow("v6 1/0") << O::Icmp6 << 1 << 0 << K::Unreachable << netUnreach << false;
    QTest::newRow("v6 1/1") << O::Icmp6 << 1 << 1 << K::Unreachable << prohibited << false;
    QTest::newRow("v6 1/3") << O::Icmp6 << 1 << 3 << K::Unreachable << hostUnreach << false;
    QTest::newRow("v6 1/4") << O::Icmp6 << 1 << 4 << K::Unreachable << portUnreach << true;
    QTest::newRow("v6 1/2") << O::Icmp6 << 1 << 2 << K::Unreachable
                            << QStringLiteral("Destination unreachable (code 2).") << false;
    QTest::newRow("v6 2/0") << O::Icmp6 << 2 << 0 << K::Unreachable << tooBig << false;
    QTest::newRow("v6 3/0") << O::Icmp6 << 3 << 0 << K::TimeExceeded << QString() << false;
    QTest::newRow("v6 3/1") << O::Icmp6 << 3 << 1 << K::Unreachable << reassembly << false;
    QTest::newRow("v6 4/0") << O::Icmp6 << 4 << 0 << K::Unreachable << paramProblem << false;
    QTest::newRow("v6 137/0 redirect") << O::Icmp6 << 137 << 0 << K::Ignore << QString() << false;

    // Only ICMP origins classify; classifyIcmp() ignores the same numbers from
    // other origins.
    QTest::newRow("local 3/3") << O::Local << 3 << 3 << K::Ignore << QString() << false;
    QTest::newRow("other 11/0") << O::Other << 11 << 0 << K::Ignore << QString() << false;
}

void TestCoreParsers::classify()
{
    QFETCH(ErrQueueInfo::Origin, origin);
    QFETCH(int, type);
    QFETCH(int, code);
    QFETCH(IcmpVerdict::Kind, kind);
    QFETCH(QString, text);
    QFETCH(bool, portUnreachable);

    const IcmpVerdict v = classifyIcmp(origin, static_cast<quint8>(type), static_cast<quint8>(code));
    QCOMPARE(v.kind, kind);
    QCOMPARE(v.text, text);
    QCOMPARE(v.portUnreachable, portUnreachable);
}

// A timeout can resolve after a reply of a later round: the history stays in
// round order and keeps only the newest kHistoryRounds samples.
void TestCoreParsers::hopSampleOrderAndCap()
{
    HopStats hop;
    hop.addSample(1, 1.5f, 1000);
    hop.addSample(3, 3.5f, 3000);
    hop.addSample(2, -1.0f, 2000);
    QCOMPARE(hop.history.size(), qsizetype(3));
    for (int i = 0; i < 3; ++i) {
        QCOMPARE(hop.history.at(i).round, i + 1);
        QCOMPARE(hop.history.at(i).sentAtMs, qint64(1000) * (i + 1));
    }
    QVERIFY(hop.history.at(1).ms < 0.0f);

    HopStats full;
    for (int r = 1; r <= HopStats::kHistoryRounds + 5; ++r)
        full.addSample(r, static_cast<float>(r), r);
    QCOMPARE(full.history.size(), qsizetype(HopStats::kHistoryRounds));
    QCOMPARE(full.history.constFirst().round, 6);
    QCOMPARE(full.history.constLast().round, HopStats::kHistoryRounds + 5);
}

void TestCoreParsers::sessionIcmpError_data()
{
    QTest::addColumn<QString>("sender");   // sends the ICMP error for each TTL from 3 up
    QTest::addColumn<int>("rows");         // hops() while the target is down

    QTest::newRow("error from a router") << QStringLiteral("192.0.2.2") << 5;
    QTest::newRow("error from the target") << QStringLiteral("192.0.2.9") << 3;
}

// Hops 1 and 2 are the routers 192.0.2.1 and 192.0.2.2, and the target
// 192.0.2.9 is at hop 3. While the target is down, the sender answers each TTL
// from 3 to maxHops with "Destination host unreachable.". An error from a router
// gives no hop address. Else all later hops get the address of router 2, and the
// probes stop at TTL 2 until the trace ends. An error from the target makes hop 3
// the target hop. When the target is up again, hop 3 is the target hop in both cases.
// An error after that does not change the target hop.
void TestCoreParsers::sessionIcmpError()
{
    QFETCH(QString, sender);
    QFETCH(int, rows);

    const QHostAddress target(QStringLiteral("192.0.2.9"));
    const bool fromTarget = QHostAddress(sender) == target;
    bool targetUp = false;
    const auto answer = [&](int ttl) {
        ProbeEvent ev;
        if (ttl < 3) {
            ev.responder = QHostAddress(QStringLiteral("192.0.2.%1").arg(ttl));
        } else if (targetUp) {
            ev.responder = target;
            ev.reachedTarget = true;
        } else {
            ev.kind = ProbeEvent::Kind::Error;
            ev.responder = QHostAddress(sender);
            ev.errorText = QStringLiteral("Destination host unreachable.");
        }
        return ev;
    };
    linmtr::core::setBackendFactoryForTests([&answer] { return std::make_unique<FakeBackend>(answer); });
    const auto restore = qScopeGuard([] { linmtr::core::setBackendFactoryForTests({}); });

    TraceOptions options;
    options.intervalSec = 0.1;
    options.maxHops = 5;
    options.resolveNames = false;
    TraceSession session;
    QSignalSpy errors(&session, &TraceSession::errorOccurred);
    session.start(target.toString(), options);
    QTRY_VERIFY_WITH_TIMEOUT(session.roundsCompleted() >= 3 || !errors.isEmpty(), 10000);
    QVERIFY2(errors.isEmpty(), qPrintable(errors.value(0).value(0).toString()));

    const QList<HopStats> down = session.hops();
    QCOMPARE(down.size(), rows);
    for (int i = 2; i < rows; ++i) {
        const HopStats &hop = down.at(i);
        QCOMPARE(hop.errorText, QStringLiteral("Destination host unreachable."));
        QVERIFY(hop.sent >= 3);   // each round probes it
        QCOMPARE(hop.received, quint64(0));
        QCOMPARE(hop.address, fromTarget ? target : QHostAddress());
        QCOMPARE(hop.isTarget, fromTarget);
    }

    targetUp = true;
    QTRY_VERIFY_WITH_TIMEOUT(session.hops().size() == 3 && session.hops().at(2).received > 0, 10000);
    const HopStats hop = session.hops().at(2);
    QCOMPARE(hop.address, target);
    QVERIFY(hop.isTarget);
    QVERIFY(hop.errorText.isEmpty());

    targetUp = false;
    QTRY_VERIFY_WITH_TIMEOUT(!session.hops().value(2).errorText.isEmpty(), 10000);
    QCOMPARE(session.hops().size(), 3);
    QCOMPARE(session.hops().at(2).address, target);
    QVERIFY(session.hops().at(2).isTarget);
    session.stop();
}

void TestCoreParsers::sessionLateAnswer_data()
{
    // error: an ICMP error from a router, else a reply from the target. untagged:
    // the answer has no seq, so the session matches it by TTL and socket. rttUs:
    // its tcpi_rtt. stampMs: its arrival stamp, in ms after the send (0 = none).
    // counted: the answer counts; else the probe times out.
    QTest::addColumn<bool>("error");
    QTest::addColumn<bool>("untagged");
    QTest::addColumn<uint>("rttUs");
    QTest::addColumn<int>("stampMs");
    QTest::addColumn<bool>("counted");

    QTest::newRow("reply: tcpi_rtt below the timeout") << false << false << 109'999u << 0 << true;
    QTest::newRow("reply: tcpi_rtt at the timeout") << false << false << 110'000u << 0 << false;
    QTest::newRow("reply: stamp before the timeout") << false << false << 0u << 100 << true;
    QTest::newRow("reply: stamp after the timeout") << false << false << 0u << 110 << false;
    QTest::newRow("reply: no kernel value") << false << false << 0u << 0 << true;
    QTest::newRow("untagged reply: stamp after the timeout") << false << true << 0u << 110 << false;
    QTest::newRow("error: stamp before the timeout") << true << false << 0u << 100 << true;
    QTest::newRow("error: stamp after the timeout") << true << false << 0u << 110 << false;
}

// The reply timeout is 110 ms, and the fake backend holds the answer for 110 ms.
// Thus the session reads it after the timeout, at the pump tick that also expires
// the probe. Each tick reads the answers before it looks for expired probes. An
// answer whose kernel value shows that it arrived after the timeout does not count,
// also without a seq: the probe times out. An answer without a kernel value counts.
void TestCoreParsers::sessionLateAnswer()
{
    QFETCH(bool, error);
    QFETCH(bool, untagged);
    QFETCH(uint, rttUs);
    QFETCH(int, stampMs);
    QFETCH(bool, counted);

    const QHostAddress target(QStringLiteral("192.0.2.9"));
    const QString unreachable = QStringLiteral("Destination host unreachable.");
    const auto answer = [&](int ttl) {
        ProbeEvent ev;
        if (untagged)
            ev.ttl = ttl;
        if (error) {
            ev.kind = ProbeEvent::Kind::Error;
            ev.responder = QHostAddress(QStringLiteral("192.0.2.1"));
            ev.errorText = unreachable;
        } else {
            ev.responder = target;
            ev.reachedTarget = true;
        }
        ev.rttUs = rttUs;
        if (stampMs > 0)
            ev.arrivalNs = wallNowNs() + qint64(stampMs) * 1'000'000;
        return ev;
    };
    linmtr::core::setBackendFactoryForTests(
        [&answer] { return std::make_unique<FakeBackend>(answer, kTimeoutNs); });
    const auto restore = qScopeGuard([] { linmtr::core::setBackendFactoryForTests({}); });

    TraceOptions options;
    options.maxHops = 1;
    options.maxRounds = 1;
    options.timeoutSec = kTimeoutNs / 1e9;
    options.resolveNames = false;
    TraceSession session;
    session.start(target.toString(), options);
    QTRY_VERIFY_WITH_TIMEOUT(session.roundsCompleted() == 1, 5000);

    const QList<HopStats> hops = session.hops();
    QCOMPARE(hops.size(), 1);
    const HopStats &hop = hops.constFirst();
    const bool reply = counted && !error;
    QCOMPARE(hop.sent, quint64(1));
    QCOMPARE(hop.received, quint64(reply ? 1 : 0));
    QCOMPARE(hop.timedOut, quint64(reply ? 0 : 1));
    QCOMPARE(hop.history.size(), qsizetype(1));
    QCOMPARE(hop.history.constFirst().ms >= 0.0f, reply);
    QCOMPARE(hop.errorText, counted && error ? unreachable : QString());
    QCOMPARE(hop.address, reply ? target : QHostAddress());
    if (reply) {
        const QByteArray rtt = QByteArray::number(hop.lastMs);
        if (rttUs > 0)
            QCOMPARE(hop.lastMs, rttUs / 1000.0);
        else if (stampMs > 0)
            QVERIFY2(hop.lastMs >= stampMs && hop.lastMs < kTimeoutNs / 1e6, rtt.constData());
        else
            QVERIFY2(hop.lastMs >= kTimeoutNs / 1e6, rtt.constData());   // the RTT at the read
    }
    session.stop();
}

void TestCoreParsers::liveLoopbackTrace()
{
    if (const QString reason = loopbackPingUnavailable(); !reason.isEmpty())
        QSKIP(qPrintable(reason));

    TraceOptions options;
    options.protocol = TraceOptions::Protocol::Icmp;
    options.maxRounds = 3;
    options.intervalSec = 0.2;
    options.resolveNames = false;

    TraceSession session;
    QSignalSpy errors(&session, &TraceSession::errorOccurred);
    const qint64 before = QDateTime::currentMSecsSinceEpoch();
    session.start(QStringLiteral("127.0.0.1"), options);
    QTRY_VERIFY_WITH_TIMEOUT(session.roundsCompleted() >= 3 || !errors.isEmpty(), 15000);
    const qint64 after = QDateTime::currentMSecsSinceEpoch();
    QVERIFY2(errors.isEmpty(), qPrintable(errors.value(0).value(0).toString()));

    QCOMPARE(session.roundsCompleted(), 3);
    QCOMPARE(session.options().protocol, TraceOptions::Protocol::Icmp);   // no UDP fallback
    const QList<HopStats> hops = session.hops();
    QCOMPARE(hops.size(), 1);
    const HopStats &hop = hops.constFirst();
    QCOMPARE(hop.ttl, 1);
    QCOMPARE(hop.address, QHostAddress(QHostAddress::LocalHost));
    QVERIFY(hop.isTarget);
    QCOMPARE(hop.sent, quint64(3));
    QCOMPARE(hop.received + hop.timedOut, quint64(3));
    QCOMPARE(hop.history.size(), qsizetype(3));
    QCOMPARE(quint64(hop.history.size()), hop.received + hop.timedOut);
    for (int i = 0; i < hop.history.size(); ++i) {
        QCOMPARE(hop.history.at(i).round, i + 1);
        QVERIFY(hop.history.at(i).sentAtMs >= before && hop.history.at(i).sentAtMs <= after);
        if (hop.received == 3)
            QVERIFY(hop.history.at(i).ms >= 0.0f);
    }

    // No fourth round starts: wait for more than two intervals.
    QTest::qWait(500);
    QCOMPARE(session.state(), TraceSession::State::Tracing);
    QCOMPARE(session.roundsCompleted(), 3);
    QCOMPARE(session.hops().constFirst().sent, quint64(3));
    session.stop();
}

void TestCoreParsers::liveStalledReadRtt_data()
{
    QTest::addColumn<TraceOptions::Protocol>("protocol");
    QTest::addColumn<bool>("listening");   // TCP: the port accepts the connection, else it resets it
    QTest::addColumn<bool>("kernelRtt");   // the RTT comes from the kernel, else from the read

    using P = TraceOptions::Protocol;
    QTest::newRow("icmp echo reply: stamp") << P::Icmp << false << true;
    QTest::newRow("udp port unreachable: stamp") << P::Udp << false << true;
    QTest::newRow("tcp handshake: tcpi_rtt") << P::Tcp << true << true;
    QTest::newRow("tcp reset: read time") << P::Tcp << false << false;
}

// The answer waits in the socket while the thread is busy (for example, a
// repaint). An RTT from the kernel does not include the wait. A TCP reset has
// no kernel value: its RTT ends at the read, so it includes the wait.
// Not covered: the stamps of TCP probes and of the UDP sockets after slot 0.
// Loopback sends no ICMP error to a TCP SYN, and round 1 uses slot 0.
void TestCoreParsers::liveStalledReadRtt()
{
    QFETCH(TraceOptions::Protocol, protocol);
    QFETCH(bool, listening);
    QFETCH(bool, kernelRtt);

    if (protocol == TraceOptions::Protocol::Icmp) {
        if (const QString reason = loopbackPingUnavailable(); !reason.isEmpty())
            QSKIP(qPrintable(reason));
    }
    // UDP: the probe for TTL 1 goes to 127.0.0.1:33434. A socket that receives on
    // that port takes the probe, and no port unreachable comes back. This socket
    // holds the port, so that the other sockets of the test cannot get it. Its
    // connect() makes it take only datagrams from port 9, thus not the probe.
    UniqueFd portHold;
    if (protocol == TraceOptions::Protocol::Udp) {
        portHold.reset(::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0));
        sockaddr_in port = v4("127.0.0.1");
        port.sin_port = htons(linmtr::core::kUdpBasePort);
        sockaddr_in discard = v4("127.0.0.1");
        discard.sin_port = htons(9);
        if (::bind(portHold.get(), reinterpret_cast<const sockaddr *>(&port), sizeof(port)) != 0
            || ::connect(portHold.get(), reinterpret_cast<const sockaddr *>(&discard), sizeof(discard)) != 0)
            QSKIP(qPrintable(QStringLiteral("UDP port 33434 on 127.0.0.1: %1").arg(qt_error_string(errno))));
    }
    UniqueFd stampHold;
    if (const QString reason = arrivalStampsUnavailable(&stampHold); !reason.isEmpty())
        QSKIP(qPrintable(reason));

    // A listening port completes the handshake. A connection to a bound port that
    // does not listen gets a reset, and no other program can take that port.
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    UniqueFd closedPort(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    sockaddr_in closed = v4("127.0.0.1");
    socklen_t closedLen = sizeof(closed);
    QVERIFY(closedPort.isValid());
    QVERIFY(::bind(closedPort.get(), reinterpret_cast<const sockaddr *>(&closed), sizeof(closed)) == 0);
    QVERIFY(::getsockname(closedPort.get(), reinterpret_cast<sockaddr *>(&closed), &closedLen) == 0);

    TraceOptions options;
    options.protocol = protocol;
    options.maxRounds = 1;
    options.resolveNames = false;
    options.tcpPort = listening ? server.serverPort() : ntohs(closed.sin_port);

    TraceSession session;
    QSignalSpy errors(&session, &TraceSession::errorOccurred);
    session.start(QStringLiteral("127.0.0.1"), options);
    // start() posts the socket setup, which sends TTL 1 at once. This event runs
    // next, before the event loop reads the sockets, so the answer waits.
    bool stalledInFlight = false;
    QTimer::singleShot(0, &session, [&session, &stalledInFlight] {
        const QList<HopStats> hops = session.hops();
        stalledInFlight = hops.size() == 1 && hops.constFirst().sent == 1
                          && hops.constFirst().received == 0;
        QTest::qSleep(kStallMs);
    });
    QTRY_VERIFY_WITH_TIMEOUT(session.roundsCompleted() >= 1 || !errors.isEmpty(), 10000);
    QVERIFY2(errors.isEmpty(), qPrintable(errors.value(0).value(0).toString()));
    QVERIFY(stalledInFlight);
    QCOMPARE(session.options().protocol, protocol);

    const QList<HopStats> hops = session.hops();
    QCOMPARE(hops.size(), 1);
    const HopStats &hop = hops.constFirst();
    QCOMPARE(hop.received, quint64(1));
    QVERIFY(hop.isTarget);
    const QByteArray rtt = QByteArray::number(hop.lastMs);
    // 0.001 ms: one microsecond, the unit of tcpi_rtt. A smaller RTT has a wrong unit.
    if (kernelRtt)
        QVERIFY2(hop.lastMs >= 0.001 && hop.lastMs < kStallMs / 3.0, rtt.constData());
    else
        QVERIFY2(hop.lastMs >= kStallMs, rtt.constData());
    session.stop();
}

// Report mode with -c 0 runs until a signal, then prints the report and exits
// with 0. Without the runner's handler, raise(SIGINT) ends the test process.
void TestCoreParsers::reportRunnerStopsOnSignal()
{
    if (const QString reason = loopbackPingUnavailable(); !reason.isEmpty())
        QSKIP(qPrintable(reason));

    TraceOptions options;
    options.protocol = TraceOptions::Protocol::Icmp;
    options.intervalSec = 0.2;
    options.resolveNames = false;

    ReportRunner runner(QStringLiteral("127.0.0.1"), options, 0, false, ReportRunner::Text);
    QSignalSpy finished(&runner, &ReportRunner::finished);

    // Send stdout to a file. No check may fail before stdout is back.
    QTemporaryFile capture;
    QVERIFY(capture.open());
    std::fflush(stdout);
    const int savedStdout = ::dup(STDOUT_FILENO);
    QVERIFY(savedStdout >= 0);
    ::dup2(capture.handle(), STDOUT_FILENO);

    runner.start();
    QTest::qWait(700);
    ::raise(SIGINT);
    const bool stopped = QTest::qWaitFor([&finished] { return !finished.isEmpty(); }, 5000);
    struct sigaction term = {};
    ::sigaction(SIGTERM, nullptr, &term);

    std::fflush(stdout);
    ::dup2(savedStdout, STDOUT_FILENO);
    ::close(savedStdout);

    QVERIFY(stopped);
    // After the first signal, a second signal of another kind takes the default action.
    QVERIFY(term.sa_handler == SIG_DFL);
    QCOMPARE(finished.size(), 1);
    QCOMPARE(finished.at(0).at(0).toInt(), int(ReportRunner::Success));

    QFile file(capture.fileName());
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QString report = QString::fromUtf8(file.readAll());
    QVERIFY2(report.contains(QLatin1String("LinMTR statistics")), qPrintable(report));
    // Row: "|<host> - <loss> | <sent> | <recv> | ..."
    const QStringList rows = report.split(QLatin1Char('\n')).filter(QLatin1String(" 127.0.0.1 - "));
    QCOMPARE(rows.size(), 1);
    QVERIFY2(rows.constFirst().split(QLatin1Char('|')).value(2).trimmed().toInt() >= 2,
             qPrintable(rows.constFirst()));
}

QTEST_GUILESS_MAIN(TestCoreParsers)
#include "tst_core_parsers.moc"

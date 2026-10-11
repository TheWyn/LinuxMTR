// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Wyn

#include <QSignalSpy>
#include <QtTest>

#include <cerrno>
#include <cstring>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <linux/errqueue.h>

#include "core/Prober.h"
#include "core/TraceSession.h"

using linmtr::core::ErrQueueInfo;
using linmtr::core::IcmpVerdict;
using linmtr::core::classifyIcmp;
using linmtr::core::decodeExtendedError;

Q_DECLARE_METATYPE(ErrQueueInfo::Origin)
Q_DECLARE_METATYPE(IcmpVerdict::Kind)

namespace {

constexpr std::size_t kEeSize = sizeof(sock_extended_err);
constexpr std::size_t kHeaderSize = sizeof(cmsghdr);

// Holds crafted control messages for decodeExtendedError(). put() sets cmsg_len
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

} // namespace

// Feeds crafted MSG_ERRQUEUE control data to decodeExtendedError(), checks the
// classifyIcmp() table, and runs one short live trace over loopback.
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
    void classify_data();
    void classify();
    void liveLoopbackTrace();
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
    session.start(QStringLiteral("127.0.0.1"), options);
    QTRY_VERIFY_WITH_TIMEOUT(session.roundsCompleted() >= 3 || !errors.isEmpty(), 15000);
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

    // No fourth round starts: wait for more than two intervals.
    QTest::qWait(500);
    QCOMPARE(session.state(), TraceSession::State::Tracing);
    QCOMPARE(session.roundsCompleted(), 3);
    QCOMPARE(session.hops().constFirst().sent, quint64(3));
    session.stop();
}

QTEST_GUILESS_MAIN(TestCoreParsers)
#include "tst_core_parsers.moc"

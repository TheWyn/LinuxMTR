// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Wyn

#include <QtTest>

#include "core/HopStats.h"
#include "gui/ReportBuilder.h"

namespace {

HopStats makeHop(int ttl, const char *address, const char *hostname, const char *errorText,
                 quint64 sent, quint64 received, quint64 timedOut,
                 double lastMs, double bestMs, double worstMs, double totalMs)
{
    HopStats hop;
    hop.ttl = ttl;
    if (address)
        hop.address = QHostAddress(QString::fromLatin1(address));
    hop.hostname = QString::fromLatin1(hostname);
    hop.errorText = QString::fromLatin1(errorText);
    hop.sent = sent;
    hop.received = received;
    hop.timedOut = timedOut;
    hop.lastMs = lastMs;
    hop.bestMs = bestMs;
    hop.worstMs = worstMs;
    hop.totalMs = totalMs;
    return hop;
}

// The hops are, in order: a resolved name, no reply, an ICMP error text that
// wins over the name, and a name with HTML metacharacters. The last hop has a
// 39-character name and times above INT_MAX milliseconds. Its loss checks the
// integer formula: 100 - 200 / 3 = 34.
QList<HopStats> sampleHops()
{
    return {
        makeHop(1, "192.168.1.1", "router.lan", "", 10, 10, 0, 1.4, 0.9, 2.7, 14.2),
        makeHop(2, nullptr, "", "", 10, 0, 10, 0.0, 0.0, 0.0, 0.0),
        makeHop(3, "10.0.0.1", "gw.example", "Destination host unreachable.",
                10, 4, 6, 12.6, 11.2, 15.9, 52.0),
        makeHop(4, "203.0.113.5", "evil<b>&host.example", "", 10, 7, 3, 20.5, 19.1, 25.8, 147.7),
        makeHop(5, "198.51.100.7", "very-long-hostname.backbone.example.net", "",
                4, 2, 1, 1e12, 30.2, 1e12, 1e12 + 30.2),
    };
}

const char kTextHead[] =
    "|------------------------------------------------------------------------------------------|\n"
    "|                                      LinMTR statistics                                   |\n"
    "|                       Host              -   %  | Sent | Recv | Best | Avrg | Wrst | Last |\n"
    "|------------------------------------------------|------|------|------|------|------|------|\n";

const char kTextTail[] =
    "|________________________________________________|______|______|______|______|______|______|\n"
    "   LinMTR v" LINMTR_VERSION " GPL V2 - WinMTR port for Linux";

const char kTextRowsNoIps[] =
    "|                              router.lan -    0 |   10 |   10 |    0 |    1 |    2 |    1 |\n"
    "|                   No response from host -  100 |   10 |    0 |    0 |    0 |    0 |    0 |\n"
    "|           Destination host unreachable. -   60 |   10 |    4 |   11 |   13 |   15 |   12 |\n"
    "|                    evil<b>&host.example -   30 |   10 |    7 |   19 |   21 |   25 |   20 |\n"
    "| very-long-hostname.backbone.example.net -   34 |    4 |    2 |   30 | 2147483647 | 2147483647 | 2147483647 |\n";

const char kTextRowsIps[] =
    "|                router.lan (192.168.1.1) -    0 |   10 |   10 |    0 |    1 |    2 |    1 |\n"
    "|                   No response from host -  100 |   10 |    0 |    0 |    0 |    0 |    0 |\n"
    "|           Destination host unreachable. -   60 |   10 |    4 |   11 |   13 |   15 |   12 |\n"
    "|      evil<b>&host.example (203.0.113.5) -   30 |   10 |    7 |   19 |   21 |   25 |   20 |\n"
    "|very-long-hostname.backbone.example.net (198.51.100.7) -   34 |    4 |    2 |   30 | 2147483647 | 2147483647 | 2147483647 |\n";

const char kHtmlHead[] =
    "<html><head><title>LinMTR Statistics</title></head><body bgcolor=\"white\">\n"
    "<center><h2>LinMTR statistics</h2></center>\n"
    "<p align=\"center\"> <table border=\"1\" align=\"center\">\n"
    "<tr><td>Host</td> <td>%</td> <td>Sent</td> <td>Recv</td> <td>Best</td> <td>Avrg</td> <td>Wrst</td> <td>Last</td></tr>\n";

const char kHtmlTail[] = "</table></body></html>\n";

const char kHtmlRowsNoIps[] =
    "<tr><td>router.lan</td> <td>   0</td> <td>  10</td> <td>  10</td> <td>   0</td> <td>   1</td> <td>   2</td> <td>   1</td></tr>\n"
    "<tr><td>No response from host</td> <td> 100</td> <td>  10</td> <td>   0</td> <td>   0</td> <td>   0</td> <td>   0</td> <td>   0</td></tr>\n"
    "<tr><td>Destination host unreachable.</td> <td>  60</td> <td>  10</td> <td>   4</td> <td>  11</td> <td>  13</td> <td>  15</td> <td>  12</td></tr>\n"
    "<tr><td>evil&lt;b&gt;&amp;host.example</td> <td>  30</td> <td>  10</td> <td>   7</td> <td>  19</td> <td>  21</td> <td>  25</td> <td>  20</td></tr>\n"
    "<tr><td>very-long-hostname.backbone.example.net</td> <td>  34</td> <td>   4</td> <td>   2</td> <td>  30</td> <td>2147483647</td> <td>2147483647</td> <td>2147483647</td></tr>\n";

const char kHtmlRowsIps[] =
    "<tr><td>router.lan (192.168.1.1)</td> <td>   0</td> <td>  10</td> <td>  10</td> <td>   0</td> <td>   1</td> <td>   2</td> <td>   1</td></tr>\n"
    "<tr><td>No response from host</td> <td> 100</td> <td>  10</td> <td>   0</td> <td>   0</td> <td>   0</td> <td>   0</td> <td>   0</td></tr>\n"
    "<tr><td>Destination host unreachable.</td> <td>  60</td> <td>  10</td> <td>   4</td> <td>  11</td> <td>  13</td> <td>  15</td> <td>  12</td></tr>\n"
    "<tr><td>evil&lt;b&gt;&amp;host.example (203.0.113.5)</td> <td>  30</td> <td>  10</td> <td>   7</td> <td>  19</td> <td>  21</td> <td>  25</td> <td>  20</td></tr>\n"
    "<tr><td>very-long-hostname.backbone.example.net (198.51.100.7)</td> <td>  34</td> <td>   4</td> <td>   2</td> <td>  30</td> <td>2147483647</td> <td>2147483647</td> <td>2147483647</td></tr>\n";

QString expected(const char *head, const char *rows, const char *tail)
{
    return QString::fromLatin1(head) + QString::fromLatin1(rows) + QString::fromLatin1(tail);
}

} // namespace

// Locks the WinMTR text and HTML report formats byte for byte.
class TestReportBuilder : public QObject
{
    Q_OBJECT

private slots:
    void text_data();
    void text();
    void html_data();
    void html();
    void toDisplayMs();
};

void TestReportBuilder::text_data()
{
    QTest::addColumn<QList<HopStats>>("hops");
    QTest::addColumn<bool>("showIps");
    QTest::addColumn<QString>("report");

    QTest::newRow("empty") << QList<HopStats>() << false << expected(kTextHead, "", kTextTail);
    QTest::newRow("empty, IPs") << QList<HopStats>() << true << expected(kTextHead, "", kTextTail);
    QTest::newRow("hops") << sampleHops() << false << expected(kTextHead, kTextRowsNoIps, kTextTail);
    QTest::newRow("hops, IPs") << sampleHops() << true << expected(kTextHead, kTextRowsIps, kTextTail);
}

void TestReportBuilder::text()
{
    QFETCH(QList<HopStats>, hops);
    QFETCH(bool, showIps);
    QFETCH(QString, report);
    QCOMPARE(ReportBuilder::text(hops, true, showIps), report);
}

void TestReportBuilder::html_data()
{
    QTest::addColumn<QList<HopStats>>("hops");
    QTest::addColumn<bool>("showIps");
    QTest::addColumn<QString>("report");

    QTest::newRow("empty") << QList<HopStats>() << false << expected(kHtmlHead, "", kHtmlTail);
    QTest::newRow("empty, IPs") << QList<HopStats>() << true << expected(kHtmlHead, "", kHtmlTail);
    QTest::newRow("hops") << sampleHops() << false << expected(kHtmlHead, kHtmlRowsNoIps, kHtmlTail);
    QTest::newRow("hops, IPs") << sampleHops() << true << expected(kHtmlHead, kHtmlRowsIps, kHtmlTail);
}

void TestReportBuilder::html()
{
    QFETCH(QList<HopStats>, hops);
    QFETCH(bool, showIps);
    QFETCH(QString, report);
    QCOMPARE(ReportBuilder::html(hops, true, showIps), report);
}

void TestReportBuilder::toDisplayMs()
{
    QCOMPARE(ReportBuilder::toDisplayMs(0.0), 0);
    QCOMPARE(ReportBuilder::toDisplayMs(-5.0), 0);
    QCOMPARE(ReportBuilder::toDisplayMs(qQNaN()), 0);
    QCOMPARE(ReportBuilder::toDisplayMs(0.99), 0);
    QCOMPARE(ReportBuilder::toDisplayMs(41.9), 41);
    QCOMPARE(ReportBuilder::toDisplayMs(2147483646.5), 2147483646);
    QCOMPARE(ReportBuilder::toDisplayMs(2147483647.0), 2147483647);
    QCOMPARE(ReportBuilder::toDisplayMs(qInf()), 2147483647);
}

QTEST_GUILESS_MAIN(TestReportBuilder)
#include "tst_reportbuilder.moc"

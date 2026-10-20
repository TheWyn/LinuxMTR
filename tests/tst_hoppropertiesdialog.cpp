// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Wyn

#include <QApplication>
#include <QLineEdit>
#include <QtTest>

#include <cmath>

#include "gui/HopPropertiesDialog.h"
#include "gui/HopTableModel.h"

namespace {

HopStats makeHop(int ttl, quint64 sent)
{
    HopStats hop;
    hop.ttl = ttl;
    hop.address = QHostAddress(QStringLiteral("10.0.0.%1").arg(ttl));
    hop.sent = sent;
    hop.received = sent;
    return hop;
}

} // namespace

class TestHopPropertiesDialog : public QObject
{
    Q_OBJECT

private slots:
    void followsHopByTtl();
    void lossOneDecimal_data();
    void lossOneDecimal();
    void jitterField();
};

void TestHopPropertiesDialog::followsHopByTtl()
{
    HopTableModel model;
    model.setHops({makeHop(1, 5), makeHop(2, 5)});
    HopPropertiesDialog dialog(&model, 1);

    // Creation order: Name, IP Address, comment, Sent, Received, Loss, Last,
    // Best, Average, Worst, Jitter.
    const QList<QLineEdit *> fields = dialog.findChildren<QLineEdit *>();
    QCOMPARE(fields.size(), 11);
    QLineEdit *ip = fields.at(1);
    QLineEdit *comment = fields.at(2);
    QLineEdit *sent = fields.at(3);
    const QString alive = QStringLiteral("Host alive.");
    const QString gone = QStringLiteral("This hop is not in the current trace.");
    QCOMPARE(ip->text(), QStringLiteral("10.0.0.2"));
    QCOMPARE(comment->text(), alive);
    QCOMPARE(sent->text(), QStringLiteral("5"));

    // dataChanged.
    model.setHops({makeHop(1, 6), makeHop(2, 6)});
    QCOMPARE(sent->text(), QStringLiteral("6"));

    // The hop leaves the table (rowsRemoved): the comment says so, the last values stay.
    model.setHops({makeHop(1, 7)});
    QCOMPARE(comment->text(), gone);
    QCOMPARE(sent->text(), QStringLiteral("6"));
    QCOMPARE(ip->text(), QStringLiteral("10.0.0.2"));

    // A row with the same TTL comes back (rowsInserted only).
    model.setHops({makeHop(1, 7), makeHop(2, 1)});
    QCOMPARE(sent->text(), QStringLiteral("1"));
    QCOMPARE(comment->text(), alive);

    // Start clears the table (rowsRemoved only).
    model.clear();
    QCOMPARE(comment->text(), gone);
    QCOMPARE(sent->text(), QStringLiteral("1"));
    QCOMPARE(ip->text(), QStringLiteral("10.0.0.2"));
}

void TestHopPropertiesDialog::lossOneDecimal_data()
{
    QTest::addColumn<int>("received");
    QTest::addColumn<int>("timedOut");
    QTest::addColumn<QString>("loss");   // the dialog text
    QTest::addColumn<int>("table");      // HopStats::lossPercent()

    QTest::newRow("no probe resolved") << 0 << 0 << QStringLiteral("0.0") << 0;
    QTest::newRow("no loss") << 10 << 0 << QStringLiteral("0.0") << 0;
    QTest::newRow("1 lost of 1000") << 999 << 1 << QStringLiteral("0.1") << 1;
    QTest::newRow("1 lost of 3000: up, not nearest") << 2999 << 1 << QStringLiteral("0.1") << 1;
    QTest::newRow("1 lost of 3: up, not down") << 2 << 1 << QStringLiteral("33.4") << 34;
    QTest::newRow("exact") << 3 << 1 << QStringLiteral("25.0") << 25;
    QTest::newRow("1 reply of 2001: not 100.0") << 1 << 2000 << QStringLiteral("99.9") << 100;
    QTest::newRow("no reply") << 0 << 5 << QStringLiteral("100.0") << 100;
}

// The dialog shows the loss with one decimal, rounded up. The table value is the
// dialog value rounded up to an integer. A probe in flight does not count.
void TestHopPropertiesDialog::lossOneDecimal()
{
    QFETCH(int, received);
    QFETCH(int, timedOut);
    QFETCH(QString, loss);
    QFETCH(int, table);

    HopStats hop = makeHop(1, quint64(received + timedOut + 1));
    hop.received = quint64(received);
    hop.timedOut = quint64(timedOut);
    QCOMPARE(hop.lossPercent(), table);

    HopTableModel model;
    model.setHops({hop});
    HopPropertiesDialog dialog(&model, 0);
    QCOMPARE(dialog.findChildren<QLineEdit *>().at(5)->text(), loss);
    QCOMPARE(int(std::ceil(loss.toDouble())), table);
}

// The Ping time group shows HopStats::jitterMs() with one decimal, as the other times.
void TestHopPropertiesDialog::jitterField()
{
    HopStats hop = makeHop(1, 4);
    hop.jitterTotalMs = 10.0;   // three differences
    HopTableModel model;
    model.setHops({hop});
    HopPropertiesDialog dialog(&model, 0);
    QLineEdit *jitter = dialog.findChildren<QLineEdit *>().at(10);
    QCOMPARE(jitter->text(), QStringLiteral("3.3"));

    hop.received = 2;   // one difference
    hop.jitterTotalMs = 5.0;
    model.setHops({hop});
    QCOMPARE(jitter->text(), QStringLiteral("5.0"));

    hop.received = 1;   // no difference
    hop.jitterTotalMs = 0.0;
    QCOMPARE(hop.jitterMs(), 0.0);
    model.setHops({hop});
    QCOMPARE(jitter->text(), QStringLiteral("0.0"));
}

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    TestHopPropertiesDialog test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_hoppropertiesdialog.moc"

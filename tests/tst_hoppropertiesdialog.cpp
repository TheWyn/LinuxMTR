// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Wyn

#include <QApplication>
#include <QLineEdit>
#include <QtTest>

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
};

void TestHopPropertiesDialog::followsHopByTtl()
{
    HopTableModel model;
    model.setHops({makeHop(1, 5), makeHop(2, 5)});
    HopPropertiesDialog dialog(&model, 1);

    // Creation order: Name, IP Address, comment, Sent, Received, Loss, Last,
    // Best, Average, Worst.
    const QList<QLineEdit *> fields = dialog.findChildren<QLineEdit *>();
    QCOMPARE(fields.size(), 10);
    QLineEdit *ip = fields.at(1);
    QLineEdit *sent = fields.at(3);
    QCOMPARE(ip->text(), QStringLiteral("10.0.0.2"));
    QCOMPARE(sent->text(), QStringLiteral("5"));

    // dataChanged.
    model.setHops({makeHop(1, 6), makeHop(2, 6)});
    QCOMPARE(sent->text(), QStringLiteral("6"));

    // The hop leaves the table (rowsRemoved): the last values stay.
    model.setHops({makeHop(1, 7)});
    QCOMPARE(sent->text(), QStringLiteral("6"));
    QCOMPARE(ip->text(), QStringLiteral("10.0.0.2"));

    // A row with the same TTL comes back (rowsInserted only).
    model.setHops({makeHop(1, 7), makeHop(2, 1)});
    QCOMPARE(sent->text(), QStringLiteral("1"));
}

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    TestHopPropertiesDialog test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_hoppropertiesdialog.moc"

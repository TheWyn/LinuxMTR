// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Wyn

#include <QAbstractItemModelTester>
#include <QSignalSpy>
#include <QtTest>

#include "gui/HopTableModel.h"

namespace {

HopStats makeHop(int ttl, quint64 sent)
{
    HopStats hop;
    hop.ttl = ttl;
    hop.address = QHostAddress(QStringLiteral("10.0.0.%1").arg(ttl));
    hop.hostname = QStringLiteral("hop%1.example").arg(ttl);
    hop.sent = sent;
    hop.received = sent;
    hop.lastMs = 1.5 * ttl;
    hop.bestMs = hop.lastMs;
    hop.worstMs = hop.lastMs;
    hop.totalMs = hop.lastMs * static_cast<double>(sent);
    return hop;
}

QList<HopStats> makeHops(int count, quint64 sent)
{
    QList<HopStats> hops;
    for (int ttl = 1; ttl <= count; ++ttl)
        hops.append(makeHop(ttl, sent));
    return hops;
}

QString cell(const HopTableModel &model, int row, int column, int role = Qt::DisplayRole)
{
    return model.data(model.index(row, column), role).toString();
}

// Returns the first row, last row, first column and last column of one
// dataChanged() emission.
QList<int> changedRange(const QSignalSpy &spy, qsizetype emission)
{
    const auto topLeft = spy.at(emission).at(0).value<QModelIndex>();
    const auto bottomRight = spy.at(emission).at(1).value<QModelIndex>();
    return {topLeft.row(), bottomRight.row(), topLeft.column(), bottomRight.column()};
}

} // namespace

// Drives HopTableModel through setHops() sequences under QAbstractItemModelTester,
// which aborts the test on the first model contract violation.
class TestHopTableModel : public QObject
{
    Q_OBJECT

private slots:
    void setHopsSequence();
    void displayToggles();
};

void TestHopTableModel::setHopsSequence()
{
    HopTableModel model;
    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Fatal);
    QSignalSpy inserted(&model, &QAbstractItemModel::rowsInserted);
    QSignalSpy removed(&model, &QAbstractItemModel::rowsRemoved);
    QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
    const int lastColumn = HopTableModel::ColumnCount - 1;

    // Grow 0 -> 5.
    QList<HopStats> hops = makeHops(5, 1);
    model.setHops(hops);
    QCOMPARE(model.rowCount(), 5);
    QCOMPARE(inserted.size(), 1);
    QCOMPARE(changed.size(), 0);
    QCOMPARE(cell(model, 0, HopTableModel::ColHostname), QStringLiteral("hop1.example (10.0.0.1)"));
    QCOMPARE(cell(model, 4, HopTableModel::ColNr), QStringLiteral("5"));
    QCOMPARE(cell(model, 4, HopTableModel::ColLast), QStringLiteral("7"));

    // Change the middle rows only: one dataChanged() for rows 1 to 3.
    hops[1].sent = 2;
    hops[2].hostname = QStringLiteral("renamed.example");
    hops[3].timedOut = 1;
    model.setHops(hops);
    QCOMPARE(model.rowCount(), 5);
    QCOMPARE(inserted.size(), 1);
    QCOMPARE(removed.size(), 0);
    QCOMPARE(changed.size(), 1);
    QCOMPARE(changedRange(changed, 0), (QList<int>{1, 3, 0, lastColumn}));
    QCOMPARE(cell(model, 1, HopTableModel::ColSent), QStringLiteral("2"));
    QCOMPARE(cell(model, 2, HopTableModel::ColHostname), QStringLiteral("renamed.example (10.0.0.3)"));
    QCOMPARE(cell(model, 3, HopTableModel::ColLoss), QStringLiteral("50"));

    // The same snapshot again changes nothing.
    model.setHops(hops);
    QCOMPARE(changed.size(), 1);

    // Shrink 5 -> 2.
    model.setHops(hops.mid(0, 2));
    QCOMPARE(model.rowCount(), 2);
    QCOMPARE(removed.size(), 1);
    QCOMPARE(removed.at(0).at(1).toInt(), 2);
    QCOMPARE(removed.at(0).at(2).toInt(), 4);
    QCOMPARE(changed.size(), 1);
    QCOMPARE(cell(model, 1, HopTableModel::ColSent), QStringLiteral("2"));

    // Grow 2 -> 4. The two old rows also get new values.
    hops = makeHops(4, 3);
    model.setHops(hops);
    QCOMPARE(model.rowCount(), 4);
    QCOMPARE(inserted.size(), 2);
    QCOMPARE(inserted.at(1).at(1).toInt(), 2);
    QCOMPARE(inserted.at(1).at(2).toInt(), 3);
    QCOMPARE(changed.size(), 2);
    QCOMPARE(changedRange(changed, 1), (QList<int>{0, 1, 0, lastColumn}));
    QCOMPARE(cell(model, 0, HopTableModel::ColSent), QStringLiteral("3"));
    QCOMPARE(cell(model, 3, HopTableModel::ColNr), QStringLiteral("4"));

    // Clear, then clear and set an empty snapshot again: no more signals.
    model.clear();
    QCOMPARE(model.rowCount(), 0);
    QCOMPARE(removed.size(), 2);
    model.clear();
    model.setHops({});
    QCOMPARE(removed.size(), 2);
    QCOMPARE(inserted.size(), 2);
    QCOMPARE(changed.size(), 2);
    QCOMPARE(model.hopAt(0).ttl, 0);
}

void TestHopTableModel::displayToggles()
{
    HopTableModel model;
    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Fatal);
    QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);

    // Toggles on an empty model emit nothing.
    model.setShowIps(false);
    model.setResolveNames(false);
    QCOMPARE(changed.size(), 0);
    model.setShowIps(true);
    model.setResolveNames(true);

    model.setHops(makeHops(3, 1));
    QCOMPARE(changed.size(), 0);
    QCOMPARE(cell(model, 1, HopTableModel::ColHostname), QStringLiteral("hop2.example (10.0.0.2)"));
    QCOMPARE(cell(model, 1, HopTableModel::ColHostname, Qt::ToolTipRole), QString());

    // showIps off: the name only, and the address goes to the tool tip.
    model.setShowIps(false);
    QCOMPARE(changed.size(), 1);
    QCOMPARE(changedRange(changed, 0), (QList<int>{0, 2, 0, 0}));
    QCOMPARE(cell(model, 1, HopTableModel::ColHostname), QStringLiteral("hop2.example"));
    QCOMPARE(cell(model, 1, HopTableModel::ColHostname, Qt::ToolTipRole), QStringLiteral("10.0.0.2"));
    model.setShowIps(false);
    QCOMPARE(changed.size(), 1);

    // resolveNames off: the address only.
    model.setResolveNames(false);
    QCOMPARE(changed.size(), 2);
    QCOMPARE(changedRange(changed, 1), (QList<int>{0, 2, 0, 0}));
    QCOMPARE(cell(model, 2, HopTableModel::ColHostname), QStringLiteral("10.0.0.3"));
    QCOMPARE(cell(model, 2, HopTableModel::ColHostname, Qt::ToolTipRole), QString());
    model.setResolveNames(false);
    QCOMPARE(changed.size(), 2);

    // Both on again.
    model.setResolveNames(true);
    model.setShowIps(true);
    QCOMPARE(changed.size(), 4);
    QCOMPARE(cell(model, 0, HopTableModel::ColHostname), QStringLiteral("hop1.example (10.0.0.1)"));
    QCOMPARE(model.rowCount(), 3);
}

QTEST_GUILESS_MAIN(TestHopTableModel)
#include "tst_hoptablemodel.moc"

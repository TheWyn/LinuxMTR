// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Wyn

#include <QApplication>
#include <QDateTime>
#include <QImage>
#include <QtTest>

#include <cmath>

#include "gui/HopGraphWidget.h"

namespace {

// The geometry that HopGraphWidget.cpp documents, for a 400 x 200 widget
// without a legend. The window has one round for each 3 px of plot width.
constexpr int kWidth = 400;
constexpr int kHeight = 200;
constexpr int kPlotLeft = 44;
constexpr int kPlotRight = kWidth - 8;
constexpr int kBaseline = kHeight - 18;
constexpr int kSpan = (kPlotRight - kPlotLeft) / 3;   // 116 rounds
constexpr qint64 kEpochMs = 1760198400000;            // send time of round 0

const QColor kRed(255, 0, 0);
const QColor kBlue(0, 0, 255);

bool isClose(QRgb pixel, const QColor &color)
{
    return qAbs(qRed(pixel) - color.red()) <= 40
        && qAbs(qGreen(pixel) - color.green()) <= 40
        && qAbs(qBlue(pixel) - color.blue()) <= 40;
}

int countClose(const QImage &image, const QColor &color)
{
    int count = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (isClose(image.pixel(x, y), color))
                ++count;
        }
    }
    return count;
}

QImage render(HopGraphWidget &graph)
{
    graph.resize(kWidth, kHeight);
    // Not grab(): a scale factor in the environment (QT_SCALE_FACTOR) would change its size.
    QImage image(kWidth, kHeight, QImage::Format_ARGB32);
    graph.render(&image);
    return image;
}

// Rounds 1..30 on a curve, one round each second; lostRound gets no reply.
QList<HopSample> curve(double base, double amplitude, int lostRound = 0)
{
    QList<HopSample> samples;
    for (int round = 1; round <= 30; ++round) {
        const float ms = round == lostRound
            ? -1.0f
            : static_cast<float>(base + amplitude * std::sin(round / 3.0));
        samples.append(HopSample{round, ms, kEpochMs + round * 1000});
    }
    return samples;
}

} // namespace

class TestHopGraphWidget : public QObject
{
    Q_OBJECT

private slots:
    void niceCeiling_data();
    void niceCeiling();
    void drawsLinesAndLostTicks();
    void emptySeries();
    void highlightDrawnLast();
    void oldRoundsScrollOff();
    void toolTipAtSample();
};

void TestHopGraphWidget::niceCeiling_data()
{
    QTest::addColumn<double>("maxMs");
    QTest::addColumn<double>("ceiling");
    QTest::newRow("0") << 0.0 << 10.0;
    QTest::newRow("7") << 7.0 << 10.0;
    QTest::newRow("10") << 10.0 << 10.0;
    QTest::newRow("10.1") << 10.1 << 20.0;
    QTest::newRow("49") << 49.0 << 50.0;
    QTest::newRow("51") << 51.0 << 100.0;
    QTest::newRow("120") << 120.0 << 200.0;
    QTest::newRow("999") << 999.0 << 1000.0;
    QTest::newRow("1001") << 1001.0 << 2000.0;
    QTest::newRow("20000") << 20000.0 << 20000.0;
}

void TestHopGraphWidget::niceCeiling()
{
    QFETCH(double, maxMs);
    QFETCH(double, ceiling);
    QCOMPARE(HopGraphWidget::niceCeiling(maxMs), ceiling);
}

void TestHopGraphWidget::drawsLinesAndLostTicks()
{
    HopGraphWidget graph;
    graph.setSeries({{QStringLiteral("1"), kRed, curve(20.0, 10.0, 15)},
                     {QStringLiteral("2"), kBlue, curve(60.0, 15.0)}});
    const QImage image = render(graph);

    QVERIFY(countClose(image, kRed) > 20);
    QVERIFY(countClose(image, kBlue) > 20);

    // Rounds 1..30 are the newest of the window: it starts at 30 - kSpan + 1.
    const int startRound = 30 - kSpan + 1;
    const int tickX = qRound(kPlotLeft + (15 - startRound) * double(kPlotRight - kPlotLeft)
                             / (kSpan - 1));
    bool tick = false;
    for (int x = tickX - 2; x <= tickX + 2; ++x) {
        for (int y = kBaseline + 1; y < kHeight; ++y)
            tick = tick || isClose(image.pixel(x, y), kRed);
    }
    QVERIFY(tick);
}

void TestHopGraphWidget::emptySeries()
{
    HopGraphWidget graph;
    graph.setSeries({{QStringLiteral("1"), kRed, {}}, {QStringLiteral("2"), kBlue, {}}});
    QImage image = render(graph);
    QCOMPARE(countClose(image, kRed), 0);
    QCOMPARE(countClose(image, kBlue), 0);

    graph.setSeries({});
    image = render(graph);
    QCOMPARE(countClose(image, kRed), 0);
}

void TestHopGraphWidget::highlightDrawnLast()
{
    // Same samples: the line that is drawn last covers the other. Series 0 is
    // the highlight, so plain list order would leave series 1 on top.
    HopGraphWidget graph;
    graph.setSeries({{QStringLiteral("1"), kRed, curve(40.0, 20.0)},
                     {QStringLiteral("2"), kBlue, curve(40.0, 20.0)}},
                    0);
    const QImage image = render(graph);
    QVERIFY(countClose(image, kRed) > 20);
    QCOMPARE(countClose(image, kBlue), 0);
}

// 1000 rounds do not fit into the window: round 1 leaves at the left edge and
// the first round of the window is at the left edge of the plot.
void TestHopGraphWidget::oldRoundsScrollOff()
{
    const int firstVisible = 1000 - kSpan + 1;
    QList<HopSample> samples;
    for (int round = 1; round <= 1000; ++round) {
        const bool reply = round == 1 || round == firstVisible;
        samples.append(HopSample{round, reply ? 50.0f : -1.0f});
    }
    HopGraphWidget graph;
    graph.setSeries({{QStringLiteral("1"), kRed, samples}});
    const QImage image = render(graph);

    bool atLeft = false;
    for (int y = 0; y < kBaseline; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            if (!isClose(image.pixel(x, y), kRed))
                continue;
            QVERIFY2(x >= kPlotLeft - 1 && x <= kPlotLeft + 2,
                     qPrintable(QStringLiteral("red at %1,%2").arg(x).arg(y)));
            atLeft = true;
        }
    }
    QVERIFY(atLeft);

    bool tick = false;
    for (int y = kBaseline + 1; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x)
            tick = tick || isClose(image.pixel(x, y), kRed);
    }
    QVERIFY(tick);

    // The lost round before the window is 3 px left of the plot; it must not answer.
    QVERIFY(graph.toolTipAt(QPoint(kPlotLeft - 3, kBaseline + 4)).isEmpty());
}

void TestHopGraphWidget::toolTipAtSample()
{
    const QList<HopSample> samples = curve(20.0, 10.0, 15);
    HopGraphWidget graph;
    // Hop 2 has the same samples: on equal distance the first series wins.
    graph.setSeries({{QStringLiteral("1"), kRed, samples, QStringLiteral("host.example")},
                     {QStringLiteral("2"), kBlue, samples, QStringLiteral("other")}});
    graph.resize(kWidth, kHeight);

    double maxMs = 0.0;
    for (const HopSample &v : samples)
        maxMs = qMax(maxMs, static_cast<double>(v.ms));
    QCOMPARE(HopGraphWidget::niceCeiling(maxMs), 50.0);

    // Round 30 is at the right edge of the plot.
    const HopSample &last = samples.constLast();
    const int y = qRound(kBaseline - last.ms / 50.0 * (kBaseline - 8));
    QCOMPARE(graph.toolTipAt(QPoint(kPlotRight, y)),
             QStringLiteral("Hop 1: host.example\n")
                 + QDateTime::fromMSecsSinceEpoch(last.sentAtMs)
                       .toString(QStringLiteral("yyyy-MM-dd hh:mm:ss"))
                 + QStringLiteral("  ") + QString::number(last.ms, 'f', 1) + QStringLiteral(" ms"));

    // The point of a lost probe is the middle of its tick, 4 px below the zero
    // line: 5 px further down is still within reach, the zero line is not.
    const int startRound = 30 - kSpan + 1;
    const int tickX = qRound(kPlotLeft + (15 - startRound) * double(kPlotRight - kPlotLeft)
                             / (kSpan - 1));
    QVERIFY(graph.toolTipAt(QPoint(tickX, kBaseline + 4)).endsWith(QStringLiteral("Lost")));
    QVERIFY(graph.toolTipAt(QPoint(tickX, kBaseline + 9)).endsWith(QStringLiteral("Lost")));

    QVERIFY(graph.toolTipAt(QPoint(kPlotLeft + 2, 10)).isEmpty());
}

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    TestHopGraphWidget test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_hopgraphwidget.moc"

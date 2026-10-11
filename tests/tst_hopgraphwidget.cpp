// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Wyn

#include <QApplication>
#include <QImage>
#include <QtTest>

#include <cmath>

#include "gui/HopGraphWidget.h"

namespace {

// The geometry that HopGraphWidget.cpp documents, for a 400 x 200 widget
// without a legend.
constexpr int kWidth = 400;
constexpr int kHeight = 200;
constexpr int kPlotLeft = 44;
constexpr int kPlotRight = kWidth - 8;
constexpr int kBaseline = kHeight - 18;

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

// Rounds 1..30 on a curve; lostRound gets no reply.
QList<HopSample> curve(double base, double amplitude, int lostRound = 0)
{
    QList<HopSample> samples;
    for (int round = 1; round <= 30; ++round) {
        const float ms = round == lostRound
            ? -1.0f
            : static_cast<float>(base + amplitude * std::sin(round / 3.0));
        samples.append(HopSample{round, ms});
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

    // Rounds 1..30 fill the newest 60 rounds: -29..30.
    const int tickX = qRound(kPlotLeft + (15 - -29) * double(kPlotRight - kPlotLeft) / 59);
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

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    TestHopGraphWidget test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_hopgraphwidget.moc"

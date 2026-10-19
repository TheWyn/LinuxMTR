// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Wyn

#include <QApplication>
#include <QDateTime>
#include <QImage>
#include <QMouseEvent>
#include <QToolTip>
#include <QtTest>

#include <algorithm>
#include <cmath>
#include <ctime>

#include "gui/HopGraphWidget.h"

namespace {

// The geometry that HopGraphWidget.cpp documents, for a 400 x 200 widget
// without a legend. The bottom margin of 28 px holds the lost-probe ticks and
// the time labels. The window has one round for each 3 px of plot width.
constexpr int kWidth = 400;
constexpr int kHeight = 200;
constexpr int kPlotLeft = 44;
constexpr int kPlotRight = kWidth - 8;
constexpr int kBaseline = kHeight - 28;
constexpr int kSpan = (kPlotRight - kPlotLeft) / 3;   // 116 rounds
constexpr qint64 kEpochMs = 1760198400000;            // send time of round 0; a multiple of 30 s

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

// The CPU time of this thread in ns. The time of other tasks on a busy machine does not count.
qint64 threadCpuNs()
{
    timespec ts{};
    ::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return qint64(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

// Counts the paints that update() causes; render() also counts.
class PaintCounter : public HopGraphWidget
{
public:
    int paints = 0;

protected:
    void paintEvent(QPaintEvent *event) override
    {
        ++paints;
        HopGraphWidget::paintEvent(event);
    }
};

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
    void hoverMarker();
    void labelStepMs_data();
    void labelStepMs();
    void timeLabels();
    void labelsDoNotOverlap();
    void thinLineIsOnePolyline();
    void wideLinesStayFast_data();
    void wideLinesStayFast();
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

    // The highlight is wider than the same line that is not the highlight.
    HopGraphWidget thin;
    thin.setSeries({{QStringLiteral("1"), kRed, curve(40.0, 20.0)}});
    QVERIFY(countClose(image, kRed) > 3 * countClose(render(thin), kRed) / 2);
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

// A flat line at 12 ms (ceiling 20): round 30 is at x 392, y 73.6. The probe
// pixel is 2 px below the line, inside the marker circle (radius 4) and off
// the 1 px line, so only the marker can make it red. The tooltip shows only
// while a window of the application is active, as the tooltips of Qt do.
void TestHopGraphWidget::hoverMarker()
{
    const auto flat = [](float ms) {
        QList<HopSample> samples;
        for (int round = 1; round <= 30; ++round)
            samples.append(HopSample{round, ms});
        return samples;
    };
    PaintCounter graph;
    graph.setSeries({{QStringLiteral("1"), kRed, flat(12.0f)}});
    const QColor base = graph.palette().base().color();
    const double y = kBaseline - 12.0 / 20.0 * (kBaseline - 8);
    const QPoint probe(kPlotRight, static_cast<int>(y + 2.0));
    QVERIFY(isClose(render(graph).pixel(probe), base));

    // Mouse tracking: without it, QApplication drops a move with no button down.
    const QPoint pointer(kPlotRight, qRound(y));
    QMouseEvent move(QEvent::MouseMove, QPointF(pointer), graph.mapToGlobal(QPointF(pointer)),
                     Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&graph, &move);
    QVERIFY(isClose(render(graph).pixel(probe), kRed));
    QVERIFY(QApplication::activeWindow() == nullptr);
    QVERIFY(!QToolTip::isVisible());

    graph.show();
    graph.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&graph));
    QApplication::sendEvent(&graph, &move);
    QCOMPARE(QToolTip::text(), graph.toolTipAt(pointer));
    QVERIFY(!QToolTip::text().isEmpty());
    // The tooltip follows new samples under the pointer that does not move.
    graph.setSeries({{QStringLiteral("1"), kRed, flat(12.0f), QStringLiteral("host")}});
    QCOMPARE(QToolTip::text(), graph.toolTipAt(pointer));
    QVERIFY(QToolTip::text().startsWith(QStringLiteral("Hop 1: host\n")));

    // New samples far from the pointer that does not move: no marker.
    graph.setSeries({{QStringLiteral("1"), kRed, flat(2.0f)}});
    QVERIFY(isClose(render(graph).pixel(probe), base));
    graph.setSeries({{QStringLiteral("1"), kRed, flat(12.0f)}});
    QVERIFY(isClose(render(graph).pixel(probe), kRed));

    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(&graph, &leave);
    QVERIFY(isClose(render(graph).pixel(probe), base));

    // A move repaints only when the marker moves, because a paint delays the
    // read of replies. After the leave, a move onto the same sample moves it.
    QTest::qWait(50);
    int paints = graph.paints;
    QApplication::sendEvent(&graph, &move);
    QTRY_VERIFY(graph.paints > paints);
    QTest::qWait(50);
    paints = graph.paints;
    const QPoint nearby = pointer + QPoint(0, 1);
    QMouseEvent moveNearby(QEvent::MouseMove, QPointF(nearby), graph.mapToGlobal(QPointF(nearby)),
                           Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&graph, &moveNearby);
    QTest::qWait(50);
    QCOMPARE(graph.paints, paints);
    // A move onto the sample of the previous round moves the marker.
    const QPoint previous = pointer - QPoint(3, 0);
    QMouseEvent movePrevious(QEvent::MouseMove, QPointF(previous), graph.mapToGlobal(QPointF(previous)),
                             Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&graph, &movePrevious);
    QTRY_VERIFY(graph.paints > paints);
}

void TestHopGraphWidget::labelStepMs_data()
{
    QTest::addColumn<double>("msPerRound");
    QTest::addColumn<int>("minLabelPx");
    QTest::addColumn<qint64>("step");
    QTest::newRow("1 s") << 1000.0 << 80 << qint64(30000);
    QTest::newRow("100 ms") << 100.0 << 80 << qint64(5000);
    QTest::newRow("500 ms") << 500.0 << 80 << qint64(15000);
    QTest::newRow("5 s") << 5000.0 << 80 << qint64(300000);
    QTest::newRow("1 min") << 60000.0 << 80 << qint64(1800000);
    QTest::newRow("1 h") << 3600000.0 << 80 << qint64(86400000);
    QTest::newRow("0") << 0.0 << 80 << qint64(0);
}

void TestHopGraphWidget::labelStepMs()
{
    QFETCH(double, msPerRound);
    QFETCH(int, minLabelPx);
    QFETCH(qint64, step);
    QCOMPARE(HopGraphWidget::labelStepMs(msPerRound, minLabelPx), step);
}

// Rounds 1..kSpan, one each second: the window starts at round 1, and the
// 30 s labels are at rounds 30, 60 and 90. Without send times, no labels.
void TestHopGraphWidget::timeLabels()
{
    const auto labelPixels = [](bool withTimes, int fromX, int toX) {
        QList<HopSample> samples;
        for (int round = 1; round <= kSpan; ++round)
            samples.append(HopSample{round, 20.0f, withTimes ? kEpochMs + round * 1000 : 0});
        HopGraphWidget graph;
        graph.setSeries({{QStringLiteral("1"), kRed, samples}});
        const QImage image = render(graph);
        const QColor text = graph.palette().text().color();
        int count = 0;
        for (int y = kBaseline + 8; y < kHeight; ++y) {
            for (int x = qMax(0, fromX); x <= qMin(kWidth - 1, toX); ++x)
                count += isClose(image.pixel(x, y), text) ? 1 : 0;
        }
        return count;
    };

    const int x60 = kPlotLeft + (60 - 1) * (kPlotRight - kPlotLeft) / (kSpan - 1);
    QVERIFY(labelPixels(true, x60 - 30, x60 + 30) >= 5);
    QCOMPARE(labelPixels(false, 0, kWidth - 1), 0);
}

// A wall-clock jump (suspend and resume) between rounds 100 and 101: round 106
// is 2 h after the start, on a step boundary. The rounds before and after the
// jump are in different buckets, so rounds 101 and 106 both get a label, 15 px
// apart. Only one of them may be drawn.
void TestHopGraphWidget::labelsDoNotOverlap()
{
    QList<HopSample> samples;
    for (int round = 1; round <= kSpan; ++round) {
        const qint64 sentAtMs = round <= 100 ? kEpochMs + round * 1000
                                             : kEpochMs + 7200000 + (round - 106) * 1000;
        samples.append(HopSample{round, 20.0f, sentAtMs});
    }
    HopGraphWidget graph;
    graph.setSeries({{QStringLiteral("1"), kRed, samples}});
    const QImage image = render(graph);
    const QColor text = graph.palette().text().color();
    int minX = kWidth;
    int maxX = -1;
    for (int y = kBaseline + 8; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            if (isClose(image.pixel(x, y), text)) {
                minX = qMin(minX, x);
                maxX = qMax(maxX, x);
            }
        }
    }
    QVERIFY(maxX >= 0);
    QVERIFY2(maxX - minX < graph.fontMetrics().horizontalAdvance(QStringLiteral("00:00")),
             qPrintable(QStringLiteral("labels from x %1 to %2").arg(minX).arg(maxX)));
}

// A 1 px line stays one polyline. Separate lines would blend twice where they
// meet and make those pixels darker. The flat line at 12 ms (ceiling 20) is at
// y 73.6. One blend gives row 73 the colour of x 390, between rounds 29 and 30,
// and no pixel of the row may be darker (green falls as red covers the base).
void TestHopGraphWidget::thinLineIsOnePolyline()
{
    QList<HopSample> samples;
    for (int round = 1; round <= 30; ++round)
        samples.append(HopSample{round, 12.0f});
    HopGraphWidget graph;
    graph.setSeries({{QStringLiteral("1"), kRed, samples}});
    const QImage image = render(graph);
    const int row = static_cast<int>(kBaseline - 12.0 / 20.0 * (kBaseline - 8));
    const int oneBlend = qGreen(image.pixel(kPlotRight - 2, row));
    QVERIFY(oneBlend < 128);
    for (int x = kPlotLeft; x <= kPlotRight; ++x)
        QVERIFY2(qGreen(image.pixel(x, row)) >= oneBlend, qPrintable(QStringLiteral("x %1").arg(x)));
}

void TestHopGraphWidget::wideLinesStayFast_data()
{
    QTest::addColumn<int>("highlight");     // the series drawn 2 px wide; -1 = none
    QTest::addColumn<qreal>("dpr");         // device pixel ratio of the image
    QTest::addColumn<int>("lossEvery");     // a lost probe every lossEvery rounds; 0 = none
    QTest::addColumn<double>("maxRatio");   // largest render time against 1 px lines at scale 1

    QTest::newRow("highlight, scale 1") << 0 << qreal(1) << 0 << 5.0;
    QTest::newRow("scale 2") << -1 << qreal(2) << 0 << 12.0;
    // Many short runs: each run draws only its own lines.
    QTest::newRow("highlight, a lost probe every 10 rounds") << 0 << qreal(1) << 10 << 5.0;
}

// Qt strokes a polyline that is wider than 1 device pixel as one path. For the
// graph of this test, that takes about 40 ms for the highlight and 0.34 s at
// scale 2. The test compares the render times with those of the 1 px lines at
// scale 1, so that the speed of the machine cancels out. It counts the CPU time
// of this thread, so that other tasks on a busy machine do not change the result.
void TestHopGraphWidget::wideLinesStayFast()
{
    QFETCH(int, highlight);
    QFETCH(qreal, dpr);
    QFETCH(int, lossEvery);
    QFETCH(double, maxRatio);

    constexpr int kRounds = 1200;
    constexpr int kWideWidth = kPlotLeft + 3 * kRounds + 8;   // the window holds all rounds
    QList<HopGraphWidget::Series> series;
    for (int i = 0; i < 4; ++i) {
        QList<HopSample> samples;
        for (int round = 1; round <= kRounds; ++round) {
            // Jitter: the line changes direction at each sample, as real RTTs do.
            const float ms = lossEvery > 0 && round % lossEvery == 0
                ? -1.0f
                : static_cast<float>(20 + 10 * i + ((round + i) % 2 == 0 ? 1.5 : 0.0));
            samples.append(HopSample{round, ms, kEpochMs + round * 1000});
        }
        series.append({QString::number(i + 1), HopGraphWidget::seriesColor(i), samples});
    }
    HopGraphWidget plain;
    HopGraphWidget wide;
    for (HopGraphWidget *graph : {&plain, &wide})
        graph->resize(kWideWidth, kHeight);
    plain.setSeries(series);
    wide.setSeries(series, highlight);
    // The format of the backing store of a window.
    QImage plainImage(kWideWidth, kHeight, QImage::Format_ARGB32_Premultiplied);
    QImage wideImage(QSize(kWideWidth, kHeight) * dpr, QImage::Format_ARGB32_Premultiplied);
    wideImage.setDevicePixelRatio(dpr);

    // Interleaved, so that a change of the CPU speed affects both alike; medians.
    QList<qint64> plainNs;
    QList<qint64> wideNs;
    for (int i = 0; i < 9; ++i) {
        qint64 start = threadCpuNs();
        plain.render(&plainImage);
        plainNs.append(threadCpuNs() - start);
        start = threadCpuNs();
        wide.render(&wideImage);
        wideNs.append(threadCpuNs() - start);
    }
    std::sort(plainNs.begin(), plainNs.end());
    std::sort(wideNs.begin(), wideNs.end());
    const double ratio = double(wideNs.at(4)) / double(qMax<qint64>(1, plainNs.at(4)));
    QVERIFY2(ratio < maxRatio, qPrintable(QStringLiteral("%1 ms against %2 ms")
                                              .arg(wideNs.at(4) / 1e6).arg(plainNs.at(4) / 1e6)));
}

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    // Scale 1 for the widgets: a test sets a scale only through its image.
    qunsetenv("QT_SCALE_FACTOR");
    qunsetenv("QT_SCREEN_SCALE_FACTORS");
    QApplication app(argc, argv);
    TestHopGraphWidget test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_hopgraphwidget.moc"

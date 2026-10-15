// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Wyn

#include "gui/HopGraphWidget.h"

#include <QApplication>
#include <QDateTime>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QPolygonF>
#include <QToolTip>

#include <algorithm>
#include <array>
#include <limits>
#include <vector>

namespace {

constexpr int kMarginLeft = 44;     // Y labels
constexpr int kMarginRight = 8;
constexpr int kMarginTop = 8;
constexpr int kLegendHeight = 16;
constexpr int kMarginBottom = 28;   // lost-probe ticks and the time labels
constexpr int kPxPerRound = 3;     // X scale: older rounds scroll off instead of compressing
constexpr int kHitPx = 6;          // tooltip: the largest pointer distance to a sample

// The Tableau 10 palette: ten hues that are easy to tell apart.
constexpr std::array<QRgb, 10> kPalette = {
    0x4e79a7, 0xf28e2b, 0xe15759, 0x76b7b2, 0x59a14f,
    0xedc948, 0xb07aa1, 0xff9da7, 0x9c755f, 0xbab0ac
};

} // namespace

HopGraphWidget::HopGraphWidget(QWidget *parent)
    : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    // The help event comes only after the pointer stops for some time; the lines move each round.
    setMouseTracking(true);
}

void HopGraphWidget::setSeries(const QList<Series> &series, int highlight)
{
    m_series = series;
    m_highlight = highlight;
    if (m_pointer)   // the samples moved under a pointer that does not move
        updateToolTip();
    update();
}

void HopGraphWidget::setLegendVisible(bool on)
{
    m_legendVisible = on;
    update();
}

double HopGraphWidget::niceCeiling(double maxMs)
{
    if (!(maxMs > 10.0))   // also NaN, which would never end the loop
        return 10.0;
    for (double decade = 10.0;; decade *= 10.0) {
        for (const double step : {1.0, 2.0, 5.0}) {
            if (decade * step >= maxMs)
                return decade * step;
        }
    }
}

qint64 HopGraphWidget::labelStepMs(double msPerRound, int minLabelPx)
{
    if (!(msPerRound > 0.0))   // also NaN
        return 0;
    constexpr qint64 s = 1000;
    constexpr qint64 m = 60 * s;
    constexpr qint64 h = 60 * m;
    const double minStep = minLabelPx * msPerRound / kPxPerRound;
    for (const qint64 step : {1 * s, 2 * s, 5 * s, 10 * s, 15 * s, 30 * s,
                              1 * m, 2 * m, 5 * m, 10 * m, 15 * m, 30 * m,
                              1 * h, 2 * h, 3 * h, 6 * h, 12 * h}) {
        if (static_cast<double>(step) >= minStep)
            return step;
    }
    return 24 * h;
}

QColor HopGraphWidget::seriesColor(int index)
{
    const int n = static_cast<int>(kPalette.size());
    return QColor(kPalette[static_cast<size_t>((index % n + n) % n)]);
}

QSize HopGraphWidget::minimumSizeHint() const
{
    return {240, 120};
}

HopGraphWidget::Layout HopGraphWidget::layout() const
{
    Layout l;
    l.left = kMarginLeft;
    l.right = width() - kMarginRight;
    l.top = kMarginTop + (m_legendVisible ? kLegendHeight : 0);
    l.bottom = height() - kMarginBottom;
    l.plotW = l.right - l.left;
    l.plotH = l.bottom - l.top;

    int lastRound = std::numeric_limits<int>::min();
    for (const Series &s : m_series) {
        if (!s.samples.isEmpty())
            lastRound = std::max(lastRound, s.samples.constLast().round);
    }
    l.empty = lastRound == std::numeric_limits<int>::min();
    l.span = std::clamp(static_cast<int>(l.plotW / kPxPerRound), 2, HopStats::kHistoryRounds);
    // Without samples, lastRound is INT_MIN and the subtraction would overflow.
    l.startRound = l.empty ? 0 : lastRound - l.span + 1;

    double maxMs = 0.0;
    for (const Series &s : m_series) {
        for (const HopSample &v : s.samples) {
            if (v.round >= l.startRound)
                maxMs = std::max(maxMs, static_cast<double>(v.ms));
        }
    }
    l.ceiling = niceCeiling(maxMs);
    return l;
}

HopGraphWidget::Hit HopGraphWidget::hitAt(const QPoint &pos) const
{
    Hit hit;
    const Layout l = layout();
    if (l.empty)
        return hit;
    const HopSample *sample = nullptr;
    double best = std::numeric_limits<double>::max();
    for (int i = 0; i < m_series.size(); ++i) {
        for (const HopSample &v : m_series.at(i).samples) {
            if (v.round < l.startRound)
                continue;
            const QPointF point(l.x(v.round), v.ms < 0 ? l.bottom + 4 : l.y(v.ms));
            const double dx = point.x() - pos.x();
            const double dy = point.y() - pos.y();
            const double d = dx * dx + dy * dy;
            if (d <= kHitPx * kHitPx && d < best) {   // strict: the first found wins a tie
                best = d;
                hit.series = i;
                hit.point = point;
                sample = &v;
            }
        }
    }
    if (sample == nullptr)
        return hit;
    const Series &s = m_series.at(hit.series);
    const QString sentAt = QDateTime::fromMSecsSinceEpoch(sample->sentAtMs)
                               .toString(QStringLiteral("yyyy-MM-dd hh:mm:ss"));
    hit.text = tr("Hop %1: %2").arg(s.label, s.host) + QLatin1Char('\n') + sentAt
        + QStringLiteral("  ")
        + (sample->ms < 0 ? tr("Lost") : tr("%1 ms").arg(static_cast<double>(sample->ms), 0, 'f', 1));
    return hit;
}

QString HopGraphWidget::toolTipAt(const QPoint &pos) const
{
    return hitAt(pos).text;
}

void HopGraphWidget::updateToolTip()
{
    m_hit = hitAt(*m_pointer);
    // Like Qt: no tooltip while another application is active. No tip rect: with
    // a rect, Qt hides the tip when the pointer leaves it.
    if (m_hit.text.isEmpty() || QApplication::activeWindow() == nullptr)
        QToolTip::hideText();
    else
        QToolTip::showText(mapToGlobal(*m_pointer), m_hit.text, this);
}

void HopGraphWidget::mouseMoveEvent(QMouseEvent *event)
{
    const Hit old = m_hit;
    m_pointer = event->position().toPoint();
    updateToolTip();
    // A repaint costs CPU and delays the read of replies. An answer without a kernel
    // RTT, for example a TCP reset, then gets a larger RTT (DESIGN.md §3.2).
    if (m_hit.series != old.series || m_hit.point != old.point)
        update();   // the marker moved
}

void HopGraphWidget::leaveEvent(QEvent *)
{
    m_pointer.reset();
    m_hit = {};
    QToolTip::hideText();
    update();
}

void HopGraphWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), palette().base());

    const Layout l = layout();

    // Grid and labels before antialiasing, so that the 1 px lines stay sharp.
    const QColor textColor = palette().text().color();
    const QColor midColor = palette().mid().color();
    const int textH = fontMetrics().height();
    for (int i = 0; i <= 4; ++i) {
        const int y = qRound(l.bottom - i * l.plotH / 4);
        p.setPen(QPen(midColor, 1));
        p.drawLine(l.left, y, l.right, y);
        p.setPen(textColor);
        p.drawText(QRect(0, y - textH / 2, l.left - 4, textH), Qt::AlignRight | Qt::AlignVCenter,
                   QString::number(l.ceiling * i / 4));
    }

    // Time labels: the send time of each visible round, the earliest of all series.
    std::vector<qint64> roundTime(l.span, 0);
    for (const Series &s : m_series) {
        for (const HopSample &v : s.samples) {
            if (v.round < l.startRound || v.sentAtMs <= 0)
                continue;
            qint64 &t = roundTime[v.round - l.startRound];
            if (t == 0 || v.sentAtMs < t)
                t = v.sentAtMs;
        }
    }
    int firstIdx = -1;
    int lastIdx = -1;
    for (int idx = 0; idx < l.span; ++idx) {
        if (roundTime[idx] > 0) {
            if (firstIdx < 0)
                firstIdx = idx;
            lastIdx = idx;
        }
    }
    if (lastIdx > firstIdx) {
        const double msPerRound =
            double(roundTime[lastIdx] - roundTime[firstIdx]) / (lastIdx - firstIdx);
        const qint64 step = labelStepMs(
            msPerRound, fontMetrics().horizontalAdvance(QStringLiteral("00:00:00")) * 3 / 2);
        if (step > 0) {
            // shortcut: one offset per paint; labels after a DST change in the window shift by an hour until it scrolls out, upgrade if anyone notices
            const qint64 offsetMs =
                QDateTime::fromMSecsSinceEpoch(roundTime[firstIdx]).offsetFromUtc() * 1000LL;
            const auto bucket = [&](qint64 t) { return (t + offsetMs) / step; };
            qint64 prev = bucket(roundTime[firstIdx]);
            int lastRight = std::numeric_limits<int>::min();
            for (int idx = firstIdx + 1; idx <= lastIdx; ++idx) {
                const qint64 t = roundTime[idx];
                if (t <= 0 || bucket(t) == prev)
                    continue;
                prev = bucket(t);
                const int x = qRound(l.x(l.startRound + idx));
                const QString text = QDateTime::fromMSecsSinceEpoch(prev * step - offsetMs)
                    .toString(step < 60000 ? QStringLiteral("hh:mm:ss") : QStringLiteral("hh:mm"));
                const int labelW = fontMetrics().horizontalAdvance(text);
                const int left = qBound(0, x - labelW / 2, width() - labelW);
                // A wall-clock jump in the window can put two boundaries within one label width.
                if (left < lastRight + 4)
                    continue;
                lastRight = left + labelW;
                p.setPen(QPen(midColor, 1));
                p.drawLine(x, l.top, x, l.bottom);
                p.setPen(textColor);
                p.drawText(QRect(left, l.bottom + 8, labelW, height() - (l.bottom + 8)),
                           Qt::AlignHCenter | Qt::AlignTop, text);
            }
        }
    }

    if (m_legendVisible) {
        const int centreY = (kMarginTop + kLegendHeight) / 2;
        int x = l.left;
        for (const Series &s : m_series) {
            const int itemW = 8 + 4 + fontMetrics().horizontalAdvance(s.label);
            if (x + itemW > l.right)
                break;
            p.fillRect(x, centreY - 4, 8, 8, s.color);
            p.drawText(QRect(x + 12, centreY - textH / 2, itemW - 12, textH),
                       Qt::AlignLeft | Qt::AlignVCenter, s.label);
            x += itemW + 12;
        }
    }

    if (l.empty) {
        p.setPen(midColor);
        p.drawText(QRectF(l.left, l.top, l.plotW, l.plotH), Qt::AlignCenter, tr("No samples yet."));
        return;
    }

    p.setRenderHint(QPainter::Antialiasing);
    const auto drawSeries = [&](const Series &s, int penWidth) {
        p.setPen(QPen(s.color, penWidth));
        // Qt strokes a polyline that is wider than 1 device pixel (the highlight,
        // a scaled screen) as one path, which takes from tens of ms to seconds for
        // a full window. Separate lines go to its line rasterizer. Their square caps
        // overlap at the joints, so the line is a little heavier there. The colours
        // must be opaque, else each joint shows as a dark dot. A thin line stays one
        // polyline, which is faster and has no overlap.
        const bool wide = penWidth * p.deviceTransform().m11() > 1;
        QPolygonF run;   // replies in consecutive rounds
        QList<QLineF> lines;
        const auto flush = [&] {
            if (run.size() == 1) {
                p.drawPoint(run.constFirst());
            } else if (run.size() > 1 && wide) {
                lines.clear();
                for (qsizetype i = 1; i < run.size(); ++i)
                    lines.append(QLineF(run.at(i - 1), run.at(i)));
                p.drawLines(lines);
            } else if (run.size() > 1) {
                p.drawPolyline(run);
            }
            run.clear();
        };
        int prevRound = 0;
        for (const HopSample &v : s.samples) {
            if (v.round < l.startRound)
                continue;
            if (v.ms < 0) {
                flush();
                // Whole pixels: an antialiased 1 px tick would be a pale smear.
                p.fillRect(QRect(qRound(l.x(v.round) - penWidth / 2.0), l.bottom + 2, penWidth, 4),
                           s.color);
                continue;
            }
            if (!run.isEmpty() && v.round != prevRound + 1)
                flush();
            run.append(QPointF(l.x(v.round), l.y(v.ms)));
            prevRound = v.round;
        }
        flush();
    };

    for (int i = 0; i < m_series.size(); ++i) {
        if (i != m_highlight)
            drawSeries(m_series.at(i), 1);
    }
    if (m_highlight >= 0 && m_highlight < m_series.size())
        drawSeries(m_series.at(m_highlight), 2);

    // The marker of the hovered sample, on top of all lines.
    if (m_pointer) {
        const Hit hit = hitAt(*m_pointer);
        if (hit.series >= 0) {
            p.setPen(QPen(palette().windowText().color(), 1.5));
            p.setBrush(m_series.at(hit.series).color);
            p.drawEllipse(hit.point, 4.0, 4.0);
        }
    }
}

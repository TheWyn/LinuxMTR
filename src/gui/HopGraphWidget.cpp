// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Wyn

#include "gui/HopGraphWidget.h"

#include <QPainter>
#include <QPolygonF>

#include <algorithm>
#include <limits>

namespace {

constexpr int kMarginLeft = 44;     // Y labels
constexpr int kMarginRight = 8;
constexpr int kMarginTop = 8;
constexpr int kLegendHeight = 16;
constexpr int kMarginBottom = 18;   // lost-probe ticks
constexpr int kMinRounds = 60;

} // namespace

HopGraphWidget::HopGraphWidget(QWidget *parent)
    : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void HopGraphWidget::setSeries(const QList<Series> &series, int highlight)
{
    m_series = series;
    m_highlight = highlight;
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

QSize HopGraphWidget::minimumSizeHint() const
{
    return {240, 120};
}

void HopGraphWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), palette().base());

    const int left = kMarginLeft;
    const int right = width() - kMarginRight;
    const int top = kMarginTop + (m_legendVisible ? kLegendHeight : 0);
    const int bottom = height() - kMarginBottom;
    const double plotW = right - left;
    const double plotH = bottom - top;

    int firstRound = std::numeric_limits<int>::max();
    int lastRound = std::numeric_limits<int>::min();
    for (const Series &s : m_series) {
        if (s.samples.isEmpty())
            continue;
        firstRound = std::min(firstRound, s.samples.constFirst().round);
        lastRound = std::max(lastRound, s.samples.constLast().round);
    }
    const bool empty = lastRound < firstRound;
    const int span = empty ? kMinRounds
                           : std::max(kMinRounds, std::min(HopStats::kHistoryRounds,
                                                           lastRound - firstRound + 1));
    // Without samples, lastRound is INT_MIN and the subtraction would overflow.
    const int startRound = empty ? 0 : lastRound - span + 1;

    double maxMs = 0.0;
    for (const Series &s : m_series) {
        for (const HopSample &v : s.samples) {
            if (v.round >= startRound)
                maxMs = std::max(maxMs, static_cast<double>(v.ms));
        }
    }
    const double ceiling = niceCeiling(maxMs);
    const auto xAt = [&](int round) { return left + (round - startRound) * plotW / (span - 1); };
    const auto yAt = [&](float ms) { return bottom - ms / ceiling * plotH; };

    // Grid and labels before antialiasing, so that the 1 px lines stay sharp.
    const QColor textColor = palette().text().color();
    const QColor midColor = palette().mid().color();
    const int textH = fontMetrics().height();
    for (int i = 0; i <= 4; ++i) {
        const int y = qRound(bottom - i * plotH / 4);
        p.setPen(QPen(midColor, 1));
        p.drawLine(left, y, right, y);
        p.setPen(textColor);
        p.drawText(QRect(0, y - textH / 2, left - 4, textH), Qt::AlignRight | Qt::AlignVCenter,
                   QString::number(ceiling * i / 4));
    }

    if (m_legendVisible) {
        const int centreY = (kMarginTop + kLegendHeight) / 2;
        int x = left;
        for (const Series &s : m_series) {
            const int itemW = 8 + 4 + fontMetrics().horizontalAdvance(s.label);
            if (x + itemW > right)
                break;
            p.fillRect(x, centreY - 4, 8, 8, s.color);
            p.drawText(QRect(x + 12, centreY - textH / 2, itemW - 12, textH),
                       Qt::AlignLeft | Qt::AlignVCenter, s.label);
            x += itemW + 12;
        }
    }

    if (empty) {
        p.setPen(midColor);
        p.drawText(QRectF(left, top, plotW, plotH), Qt::AlignCenter, tr("No samples yet."));
        return;
    }

    p.setRenderHint(QPainter::Antialiasing);
    const auto drawSeries = [&](const Series &s, int penWidth) {
        p.setPen(QPen(s.color, penWidth));
        QPolygonF run;   // replies in consecutive rounds
        const auto flush = [&] {
            if (run.size() == 1)
                p.drawPoint(run.constFirst());
            else if (run.size() > 1)
                p.drawPolyline(run);
            run.clear();
        };
        int prevRound = 0;
        for (const HopSample &v : s.samples) {
            if (v.round < startRound)
                continue;
            if (v.ms < 0) {
                flush();
                // Whole pixels: an antialiased 1 px tick would be a pale smear.
                p.fillRect(QRect(qRound(xAt(v.round) - penWidth / 2.0), bottom + 2, penWidth, 4),
                           s.color);
                continue;
            }
            if (!run.isEmpty() && v.round != prevRound + 1)
                flush();
            run.append(QPointF(xAt(v.round), yAt(v.ms)));
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
}

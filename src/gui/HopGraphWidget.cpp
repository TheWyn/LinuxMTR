// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Wyn

#include "gui/HopGraphWidget.h"

#include <QDateTime>
#include <QHelpEvent>
#include <QPainter>
#include <QPolygonF>
#include <QToolTip>

#include <algorithm>
#include <array>
#include <limits>

namespace {

constexpr int kMarginLeft = 44;     // Y labels
constexpr int kMarginRight = 8;
constexpr int kMarginTop = 8;
constexpr int kLegendHeight = 16;
constexpr int kMarginBottom = 18;   // lost-probe ticks
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

QString HopGraphWidget::toolTipAt(const QPoint &pos) const
{
    const Layout l = layout();
    if (l.empty)
        return {};
    const Series *hitSeries = nullptr;
    const HopSample *hit = nullptr;
    double best = std::numeric_limits<double>::max();
    for (const Series &s : m_series) {
        for (const HopSample &v : s.samples) {
            if (v.round < l.startRound)
                continue;
            const double dx = l.x(v.round) - pos.x();
            const double dy = (v.ms < 0 ? l.bottom + 4 : l.y(v.ms)) - pos.y();
            const double d = dx * dx + dy * dy;
            if (d <= kHitPx * kHitPx && d < best) {   // strict: the first found wins a tie
                best = d;
                hitSeries = &s;
                hit = &v;
            }
        }
    }
    if (hit == nullptr)
        return {};
    const QString sentAt = QDateTime::fromMSecsSinceEpoch(hit->sentAtMs)
                               .toString(QStringLiteral("yyyy-MM-dd hh:mm:ss"));
    return tr("Hop %1: %2").arg(hitSeries->label, hitSeries->host) + QLatin1Char('\n') + sentAt
        + QStringLiteral("  ")
        + (hit->ms < 0 ? tr("Lost") : tr("%1 ms").arg(static_cast<double>(hit->ms), 0, 'f', 1));
}

bool HopGraphWidget::event(QEvent *e)
{
    if (e->type() != QEvent::ToolTip)
        return QWidget::event(e);
    const auto *help = static_cast<QHelpEvent *>(e);
    const QString text = toolTipAt(help->pos());
    if (text.isEmpty()) {
        QToolTip::hideText();
        e->ignore();
    } else {
        QToolTip::showText(help->globalPos(), text, this);
    }
    return true;
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
}

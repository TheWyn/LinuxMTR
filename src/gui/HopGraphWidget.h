// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Wyn

#pragma once

#include <QColor>
#include <QList>
#include <QString>
#include <QWidget>

#include "core/HopStats.h"

// Line graph of round-trip times over probe rounds, one line per series. A lost
// probe is a short tick below the zero line. The X axis is a window of the
// newest rounds, one round for each 3 px of plot width (at least 2, at most
// HopStats::kHistoryRounds); older rounds leave at the left edge.
class HopGraphWidget : public QWidget
{
    Q_OBJECT
public:
    struct Series
    {
        QString label;
        QColor color;
        QList<HopSample> samples;   // ascending round order
    };

    explicit HopGraphWidget(QWidget *parent = nullptr);

    // Replaces the series and repaints. highlight is the index of the series
    // that has a wider line on top of the others; -1 for none.
    void setSeries(const QList<Series> &series, int highlight = -1);
    void setLegendVisible(bool on);

    // The smallest value of 10, 20, 50, 100, 200, 500, 1000, ... that is not
    // less than maxMs.
    static double niceCeiling(double maxMs);

    // The colour for index from a palette of 10 colours; index 10 gets the
    // colour of index 0 again.
    static QColor seriesColor(int index);

    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    // The plot geometry and the visible rounds for the current size and series.
    struct Layout
    {
        int left = 0;
        int right = 0;
        int top = 0;
        int bottom = 0;     // the zero line
        double plotW = 0.0;
        double plotH = 0.0;
        bool empty = true;  // no series has a sample
        int span = 2;       // rounds in the window
        int startRound = 0; // the round at the left edge
        double ceiling = 10.0;

        double x(int round) const { return left + (round - startRound) * plotW / (span - 1); }
        double y(float ms) const { return bottom - ms / ceiling * plotH; }
    };

    Layout layout() const;

    QList<Series> m_series;
    int m_highlight = -1;
    bool m_legendVisible = false;
};

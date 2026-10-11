// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Wyn

#pragma once

#include <QAbstractTableModel>
#include <QList>

#include "core/HopStats.h"

// Flat table of hops with the nine WinMTR columns. setHops() updates the rows in
// place from TraceSession::hops() snapshots, so that the view keeps its selection.
class HopTableModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    enum Column {
        ColHostname,
        ColNr,
        ColLoss,
        ColSent,
        ColRecv,
        ColBest,
        ColAvrg,
        ColWorst,
        ColLast,
        ColumnCount
    };

    explicit HopTableModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

    // Replaces the contents with a new snapshot: changed rows emit
    // dataChanged, and rows come and go only at the end.
    void setHops(const QList<HopStats> &hops);
    void clear();

    bool resolveNames() const { return m_resolveNames; }
    void setResolveNames(bool resolve);

    // True: the Hostname column appends the address to resolved names (upstream issue #4).
    bool showIps() const { return m_showIps; }
    void setShowIps(bool show);

    const QList<HopStats> &hops() const { return m_hops; }
    HopStats hopAt(int row) const;

    // Initial column width in pixels (WinMTR widths scaled for modern fonts);
    // 0 for an invalid column.
    static int defaultColumnWidth(int column);

private:
    void hostnamesChanged();

    QList<HopStats> m_hops;
    bool m_resolveNames = true;
    bool m_showIps = true;
};

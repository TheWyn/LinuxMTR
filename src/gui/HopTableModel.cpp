// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Wyn

#include "gui/HopTableModel.h"

#include "gui/ReportBuilder.h"

#include <array>

namespace {

const std::array<const char *, HopTableModel::ColumnCount> kColumnNames = {
    "Hostname", "Nr", "Loss %", "Sent", "Recv", "Best", "Avrg", "Worst", "Last"
};

const std::array<int, HopTableModel::ColumnCount> kColumnWidths = {   // pixels
    285, 45, 75, 60, 60, 75, 75, 75, 75
};

bool sameHop(const HopStats &a, const HopStats &b)
{
    return a.ttl == b.ttl
        && a.address == b.address
        && a.hostname == b.hostname
        && a.errorText == b.errorText
        && a.sent == b.sent
        && a.received == b.received
        && a.timedOut == b.timedOut
        && a.lastMs == b.lastMs
        && a.bestMs == b.bestMs
        && a.worstMs == b.worstMs
        && a.totalMs == b.totalMs
        && a.isTarget == b.isTarget;
}

} // namespace

HopTableModel::HopTableModel(QObject *parent)
    : QAbstractTableModel(parent)
{
}

int HopTableModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_hops.size());
}

int HopTableModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant HopTableModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_hops.size()
        || index.column() < 0 || index.column() >= ColumnCount)
        return {};

    const HopStats &hop = m_hops.at(index.row());

    if (role == Qt::ToolTipRole) {
        if (index.column() != ColHostname || hop.address.isNull())
            return {};
        const QString shown = hop.displayHost(m_resolveNames, m_showIps);
        const QString addr = hop.address.toString();
        const bool addrShown = shown == addr
            || shown.endsWith(QLatin1String(" (") + addr + QLatin1Char(')'));
        return addrShown ? QVariant() : QVariant(addr);
    }

    if (role != Qt::DisplayRole)
        return {};

    // Numbers go out as strings, so that the view never adds locale group
    // separators: WinMTR prints them with plain "%d".
    switch (index.column()) {
    case ColHostname: return hop.displayHost(m_resolveNames, m_showIps);
    case ColNr:       return QString::number(hop.ttl);
    case ColLoss:     return QString::number(hop.lossPercent());
    case ColSent:     return QString::number(hop.sent);
    case ColRecv:     return QString::number(hop.received);
    case ColBest:     return QString::number(ReportBuilder::toDisplayMs(hop.bestMs));
    case ColAvrg:     return QString::number(ReportBuilder::toDisplayMs(hop.averageMs()));
    case ColWorst:    return QString::number(ReportBuilder::toDisplayMs(hop.worstMs));
    case ColLast:     return QString::number(ReportBuilder::toDisplayMs(hop.lastMs));
    default:          return {};
    }
}

QVariant HopTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole
        || section < 0 || section >= ColumnCount)
        return {};
    return QString::fromLatin1(kColumnNames[static_cast<size_t>(section)]);
}

Qt::ItemFlags HopTableModel::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemNeverHasChildren;
}

void HopTableModel::setHops(const QList<HopStats> &hops)
{
    const int oldCount = static_cast<int>(m_hops.size());
    const int newCount = static_cast<int>(hops.size());

    if (newCount < oldCount) {
        beginRemoveRows(QModelIndex(), newCount, oldCount - 1);
        m_hops.erase(m_hops.begin() + newCount, m_hops.end());
        endRemoveRows();
    }

    const int common = qMin(oldCount, newCount);
    int firstChanged = -1;
    int lastChanged = -1;
    for (int i = 0; i < common; ++i) {
        if (sameHop(m_hops.at(i), hops.at(i)))
            continue;
        m_hops[i] = hops.at(i);
        if (firstChanged < 0)
            firstChanged = i;
        lastChanged = i;
    }
    if (firstChanged >= 0) {
        emit dataChanged(index(firstChanged, 0), index(lastChanged, ColumnCount - 1),
                         {Qt::DisplayRole, Qt::ToolTipRole});
    }

    if (newCount > oldCount) {
        beginInsertRows(QModelIndex(), oldCount, newCount - 1);
        for (int i = oldCount; i < newCount; ++i)
            m_hops.append(hops.at(i));
        endInsertRows();
    }
}

void HopTableModel::clear()
{
    if (m_hops.isEmpty())
        return;
    beginRemoveRows(QModelIndex(), 0, static_cast<int>(m_hops.size()) - 1);
    m_hops.clear();
    endRemoveRows();
}

void HopTableModel::setResolveNames(bool resolve)
{
    if (m_resolveNames == resolve)
        return;
    m_resolveNames = resolve;
    hostnamesChanged();
}

void HopTableModel::setShowIps(bool show)
{
    if (m_showIps == show)
        return;
    m_showIps = show;
    hostnamesChanged();
}

void HopTableModel::hostnamesChanged()
{
    if (!m_hops.isEmpty()) {
        emit dataChanged(index(0, ColHostname),
                         index(static_cast<int>(m_hops.size()) - 1, ColHostname),
                         {Qt::DisplayRole, Qt::ToolTipRole});
    }
}

HopStats HopTableModel::hopAt(int row) const
{
    if (row < 0 || row >= m_hops.size())
        return {};
    return m_hops.at(row);
}

int HopTableModel::defaultColumnWidth(int column)
{
    if (column < 0 || column >= ColumnCount)
        return 0;
    return kColumnWidths[static_cast<size_t>(column)];
}

// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Wyn

#include "gui/HopPropertiesDialog.h"

#include "gui/HopGraphWidget.h"
#include "gui/HopTableModel.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

namespace {

QLineEdit *readOnlyField(QWidget *parent, Qt::Alignment align = Qt::AlignRight)
{
    auto *edit = new QLineEdit(parent);
    edit->setReadOnly(true);
    edit->setAlignment(align);
    return edit;
}

// An unchanged field keeps its text, so that a refresh keeps the selection.
void setField(QLineEdit *edit, const QString &text)
{
    if (edit->text() == text)
        return;
    edit->setText(text);
    edit->setCursorPosition(0);
}

QString pingText(double ms)
{
    // Upstream formats ping times with "%.1f".
    return QString::number(ms > 0.0 ? ms : 0.0, 'f', 1);
}

// The loss in percent with one decimal: the lossPercent() formula in tenths, so it
// also rounds up. With a reply, at most 99.9 (DESIGN.md §4).
QString lossText(const HopStats &hop)
{
    const quint64 done = hop.received + hop.timedOut;
    if (done == 0)
        return QStringLiteral("0.0");
    quint64 tenths = 1000 - (1000 * hop.received) / done;
    if (hop.received > 0)
        tenths = std::min<quint64>(tenths, 999);
    return QString::number(tenths / 10.0, 'f', 1);
}

} // namespace

HopPropertiesDialog::HopPropertiesDialog(HopTableModel *model, int row, QWidget *parent)
    : QDialog(parent)
    , m_model(model)
    , m_ttl(model->hopAt(row).ttl)
{
    setWindowTitle(tr("Host properties"));

    auto *hostBox = new QGroupBox(tr("Host"), this);
    auto *hostGrid = new QGridLayout(hostBox);
    m_name = readOnlyField(hostBox);
    m_name->setMinimumWidth(240);
    m_ip = readOnlyField(hostBox);
    m_comment = readOnlyField(hostBox, Qt::AlignLeft);
    hostGrid->addWidget(new QLabel(tr("Name:"), hostBox), 0, 0);
    hostGrid->addWidget(m_name, 0, 1);
    hostGrid->addWidget(new QLabel(tr("IP Address:"), hostBox), 1, 0);
    hostGrid->addWidget(m_ip, 1, 1);
    hostGrid->addWidget(m_comment, 2, 0, 1, 2);

    auto *packetsBox = new QGroupBox(tr("Packets"), this);
    auto *packetsForm = new QFormLayout(packetsBox);
    m_sent = readOnlyField(packetsBox);
    m_received = readOnlyField(packetsBox);
    m_loss = readOnlyField(packetsBox);
    packetsForm->addRow(tr("Sent:"), m_sent);
    packetsForm->addRow(tr("Received:"), m_received);
    packetsForm->addRow(tr("Loss (%):"), m_loss);

    auto *pingBox = new QGroupBox(tr("Ping time"), this);
    auto *pingForm = new QFormLayout(pingBox);
    m_last = readOnlyField(pingBox);
    m_best = readOnlyField(pingBox);
    m_average = readOnlyField(pingBox);
    m_worst = readOnlyField(pingBox);
    m_jitter = readOnlyField(pingBox);
    pingForm->addRow(tr("Last:"), m_last);
    pingForm->addRow(tr("Best:"), m_best);
    pingForm->addRow(tr("Average:"), m_average);
    pingForm->addRow(tr("Worst:"), m_worst);
    pingForm->addRow(tr("Jitter:"), m_jitter);

    auto *statsRow = new QHBoxLayout;
    statsRow->addWidget(packetsBox);
    statsRow->addWidget(pingBox);

    m_graph = new HopGraphWidget(this);
    m_graph->setMinimumHeight(140);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok, Qt::Horizontal, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("OK"));
    buttons->button(QDialogButtonBox::Ok)->setDefault(true);
    buttons->setCenterButtons(true);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(hostBox);
    layout->addLayout(statsRow);
    layout->addWidget(m_graph, 1);
    layout->addWidget(buttons);

    // Removed rows too, so that a hop that leaves the trace says so.
    connect(model, &QAbstractItemModel::dataChanged, this, &HopPropertiesDialog::refresh);
    connect(model, &QAbstractItemModel::rowsInserted, this, &HopPropertiesDialog::refresh);
    connect(model, &QAbstractItemModel::rowsRemoved, this, &HopPropertiesDialog::refresh);
    refresh();

    buttons->button(QDialogButtonBox::Ok)->setFocus();
}

void HopPropertiesDialog::refresh()
{
    const QList<HopStats> &hops = m_model->hops();
    const auto it = std::find_if(hops.cbegin(), hops.cend(),
                                 [this](const HopStats &h) { return h.ttl == m_ttl; });
    if (it == hops.cend()) {
        setField(m_comment, tr("This hop is not in the current trace."));
        return;
    }
    const HopStats &hop = *it;

    // Both cases deviate from upstream on purpose (DESIGN.md §8 item 9).
    QString name;
    QString ip;
    QString comment;
    if (hop.address.isNull()) {
        comment = hop.displayHost(m_model->resolveNames(), m_model->showIps());
    } else {
        ip = hop.address.toString();
        name = (m_model->resolveNames() && !hop.hostname.isEmpty()) ? hop.hostname : ip;
        comment = hop.errorText.isEmpty() ? QStringLiteral("Host alive.") : hop.errorText;
    }

    setField(m_name, name);
    setField(m_ip, ip);
    setField(m_comment, comment);
    setField(m_sent, QString::number(hop.sent));
    setField(m_received, QString::number(hop.received));
    setField(m_loss, lossText(hop));
    setField(m_last, pingText(hop.lastMs));
    setField(m_best, pingText(hop.bestMs));
    setField(m_average, pingText(hop.averageMs()));
    setField(m_worst, pingText(hop.worstMs));
    setField(m_jitter, pingText(hop.jitterMs()));
    m_graph->setSeries({{QString::number(hop.ttl), HopGraphWidget::seriesColor(hop.ttl - 1),
                         hop.history,
                         hop.displayHost(m_model->resolveNames(), m_model->showIps())}});
}

// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Wyn

#include "gui/HopPropertiesDialog.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace {

QLineEdit *readOnlyField(const QString &text, QWidget *parent, Qt::Alignment align = Qt::AlignRight)
{
    auto *edit = new QLineEdit(text, parent);
    edit->setReadOnly(true);
    edit->setAlignment(align);
    edit->setCursorPosition(0);
    return edit;
}

QString pingText(double ms)
{
    // Upstream formats ping times with "%.1f".
    return QString::number(ms > 0.0 ? ms : 0.0, 'f', 1);
}

} // namespace

HopPropertiesDialog::HopPropertiesDialog(const HopStats &hop, bool resolveNames, bool showIps,
                                         QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Host properties"));

    // A hop without an address shows an empty name and IP, and the display text
    // as comment. A responding hop shows its name and IP, and "Host alive." or
    // its ICMP error text as comment. Both cases deviate from upstream on purpose.
    QString name;
    QString ip;
    QString comment;
    if (hop.address.isNull()) {
        comment = hop.displayHost(resolveNames, showIps);
    } else {
        ip = hop.address.toString();
        name = (resolveNames && !hop.hostname.isEmpty()) ? hop.hostname : ip;
        comment = hop.errorText.isEmpty() ? QStringLiteral("Host alive.") : hop.errorText;
    }

    auto *hostBox = new QGroupBox(tr("Host"), this);
    auto *hostGrid = new QGridLayout(hostBox);
    auto *nameEdit = readOnlyField(name, hostBox);
    nameEdit->setMinimumWidth(240);
    hostGrid->addWidget(new QLabel(tr("Name:"), hostBox), 0, 0);
    hostGrid->addWidget(nameEdit, 0, 1);
    hostGrid->addWidget(new QLabel(tr("IP Address:"), hostBox), 1, 0);
    hostGrid->addWidget(readOnlyField(ip, hostBox), 1, 1);
    hostGrid->addWidget(readOnlyField(comment, hostBox, Qt::AlignLeft), 2, 0, 1, 2);

    auto *packetsBox = new QGroupBox(tr("Packets"), this);
    auto *packetsForm = new QFormLayout(packetsBox);
    packetsForm->addRow(tr("Sent:"), readOnlyField(QString::number(hop.sent), packetsBox));
    packetsForm->addRow(tr("Received:"), readOnlyField(QString::number(hop.received), packetsBox));
    packetsForm->addRow(tr("Loss (%):"), readOnlyField(QString::number(hop.lossPercent()), packetsBox));

    auto *pingBox = new QGroupBox(tr("Ping time"), this);
    auto *pingForm = new QFormLayout(pingBox);
    pingForm->addRow(tr("Last:"), readOnlyField(pingText(hop.lastMs), pingBox));
    pingForm->addRow(tr("Best:"), readOnlyField(pingText(hop.bestMs), pingBox));
    pingForm->addRow(tr("Average:"), readOnlyField(pingText(hop.averageMs()), pingBox));
    pingForm->addRow(tr("Worst:"), readOnlyField(pingText(hop.worstMs), pingBox));

    auto *statsRow = new QHBoxLayout;
    statsRow->addWidget(packetsBox);
    statsRow->addWidget(pingBox);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok, Qt::Horizontal, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("OK"));
    buttons->button(QDialogButtonBox::Ok)->setDefault(true);
    buttons->setCenterButtons(true);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(hostBox);
    layout->addLayout(statsRow);
    layout->addWidget(buttons);
    layout->setSizeConstraint(QLayout::SetFixedSize);

    buttons->button(QDialogButtonBox::Ok)->setFocus();
}

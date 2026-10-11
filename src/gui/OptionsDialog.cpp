// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Wyn

#include "gui/OptionsDialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#ifndef LINMTR_VERSION
#define LINMTR_VERSION "0.1.0"
#endif

OptionsDialog::OptionsDialog(const TraceOptions &options, int maxLru, bool showIps,
                             bool showGraph, QWidget *parent)
    : QDialog(parent)
    , m_base(options)
{
    setWindowTitle(tr("Options"));

    // About box: replaces the upstream advert and "License and Credits" dialog.
    auto *aboutBox = new QGroupBox(this);
    auto *aboutLayout = new QHBoxLayout(aboutBox);
    const QIcon icon = QApplication::windowIcon();
    if (!icon.isNull()) {
        auto *iconLabel = new QLabel(aboutBox);
        iconLabel->setPixmap(icon.pixmap(32, 32));
        iconLabel->setAlignment(Qt::AlignTop);
        aboutLayout->addWidget(iconLabel);
    }
    auto *aboutLabel = new QLabel(
        tr("LinMTR v%1 is offered under GPL V2\n"
           "A Linux port of WinMTR by Appnor MSP - www.winmtr.net")
            .arg(QStringLiteral(LINMTR_VERSION)),
        aboutBox);
    aboutLabel->setAlignment(Qt::AlignCenter);
    aboutLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    aboutLayout->addWidget(aboutLabel, 1);

    // Upstream fields, same labels and arrangement as WinMTR.rc.
    auto *mainBox = new QGroupBox(this);
    auto *mainGrid = new QGridLayout(mainBox);

    m_interval = new QDoubleSpinBox(mainBox);
    m_interval->setRange(kIntervalMin, kIntervalMax);
    m_interval->setDecimals(1);
    m_interval->setSingleStep(0.1);
    m_interval->setValue(options.intervalSec);

    m_pingSize = new QSpinBox(mainBox);
    m_pingSize->setRange(kPingSizeMin, kPingSizeMax);
    m_pingSize->setValue(options.pingSizeBytes);

    m_maxLru = new QSpinBox(mainBox);
    m_maxLru->setRange(kMaxLruMin, kMaxLruMax);
    m_maxLru->setValue(maxLru);

    m_resolveNames = new QCheckBox(tr("Resolve names"), mainBox);
    m_resolveNames->setChecked(options.resolveNames);

    // Upstream issue #4: "one.one.one.one (1.1.1.1)".
    m_showIps = new QCheckBox(tr("Show IP next to hostname"), mainBox);
    m_showIps->setChecked(showIps);

    m_showGraph = new QCheckBox(tr("Show route graph"), mainBox);
    m_showGraph->setChecked(showGraph);

    auto *intervalLabel = new QLabel(tr("Interval (sec):"), mainBox);
    intervalLabel->setBuddy(m_interval);
    auto *pingSizeLabel = new QLabel(tr("Ping size (bytes):"), mainBox);
    pingSizeLabel->setBuddy(m_pingSize);
    auto *maxLruLabel = new QLabel(tr("Max. hosts in LRU list:"), mainBox);
    maxLruLabel->setBuddy(m_maxLru);

    mainGrid->addWidget(intervalLabel, 0, 0);
    mainGrid->addWidget(m_interval, 0, 1);
    mainGrid->addWidget(pingSizeLabel, 0, 2);
    mainGrid->addWidget(m_pingSize, 0, 3);
    mainGrid->addWidget(maxLruLabel, 1, 0);
    mainGrid->addWidget(m_maxLru, 1, 1);
    mainGrid->addWidget(m_resolveNames, 1, 2, 1, 2);
    mainGrid->addWidget(m_showIps, 2, 2, 1, 2);
    mainGrid->addWidget(m_showGraph, 3, 2, 1, 2);
    mainGrid->setColumnStretch(1, 1);
    mainGrid->setColumnStretch(3, 1);

    // Linux additions.
    auto *probeBox = new QGroupBox(tr("Probing"), this);
    auto *probeGrid = new QGridLayout(probeBox);

    m_protocol = new QComboBox(probeBox);
    m_protocol->addItem(QStringLiteral("ICMP"), static_cast<int>(TraceOptions::Protocol::Icmp));
    m_protocol->addItem(QStringLiteral("UDP"), static_cast<int>(TraceOptions::Protocol::Udp));
    m_protocol->addItem(QStringLiteral("TCP"), static_cast<int>(TraceOptions::Protocol::Tcp));
    m_protocol->setCurrentIndex(m_protocol->findData(static_cast<int>(options.protocol)));

    m_tcpPort = new QSpinBox(probeBox);
    m_tcpPort->setRange(kTcpPortMin, kTcpPortMax);
    m_tcpPort->setValue(options.tcpPort);

    m_ipVersion = new QComboBox(probeBox);
    m_ipVersion->addItem(tr("Auto"), static_cast<int>(TraceOptions::IpVersion::Auto));
    m_ipVersion->addItem(QStringLiteral("IPv4"), static_cast<int>(TraceOptions::IpVersion::V4));
    m_ipVersion->addItem(QStringLiteral("IPv6"), static_cast<int>(TraceOptions::IpVersion::V6));
    m_ipVersion->setCurrentIndex(m_ipVersion->findData(static_cast<int>(options.ipVersion)));

    m_timeout = new QDoubleSpinBox(probeBox);
    m_timeout->setRange(kTimeoutMin, kTimeoutMax);
    m_timeout->setDecimals(1);
    m_timeout->setSingleStep(0.5);
    m_timeout->setValue(options.timeoutSec);

    m_maxHops = new QSpinBox(probeBox);
    m_maxHops->setRange(kMaxHopsMin, kMaxHopsMax);
    m_maxHops->setValue(options.maxHops);

    auto *protocolLabel = new QLabel(tr("Protocol:"), probeBox);
    protocolLabel->setBuddy(m_protocol);
    auto *tcpPortLabel = new QLabel(tr("TCP port:"), probeBox);
    tcpPortLabel->setBuddy(m_tcpPort);
    auto *ipVersionLabel = new QLabel(tr("IP version:"), probeBox);
    ipVersionLabel->setBuddy(m_ipVersion);
    auto *timeoutLabel = new QLabel(tr("Reply timeout (sec):"), probeBox);
    timeoutLabel->setBuddy(m_timeout);
    auto *maxHopsLabel = new QLabel(tr("Max hops:"), probeBox);
    maxHopsLabel->setBuddy(m_maxHops);

    probeGrid->addWidget(protocolLabel, 0, 0);
    probeGrid->addWidget(m_protocol, 0, 1);
    probeGrid->addWidget(tcpPortLabel, 0, 2);
    probeGrid->addWidget(m_tcpPort, 0, 3);
    probeGrid->addWidget(ipVersionLabel, 1, 0);
    probeGrid->addWidget(m_ipVersion, 1, 1);
    probeGrid->addWidget(timeoutLabel, 1, 2);
    probeGrid->addWidget(m_timeout, 1, 3);
    probeGrid->addWidget(maxHopsLabel, 2, 0);
    probeGrid->addWidget(m_maxHops, 2, 1);
    probeGrid->setColumnStretch(1, 1);
    probeGrid->setColumnStretch(3, 1);

    // The port applies only to TCP probes.
    const auto updateTcpPort = [this, tcpPortLabel] {
        const bool tcp = m_protocol->currentData().toInt()
            == static_cast<int>(TraceOptions::Protocol::Tcp);
        tcpPortLabel->setEnabled(tcp);
        m_tcpPort->setEnabled(tcp);
    };
    connect(m_protocol, &QComboBox::currentIndexChanged, this, updateTcpPort);
    updateTcpPort();

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                         Qt::Horizontal, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("&OK"));
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("&Cancel"));
    buttons->button(QDialogButtonBox::Ok)->setDefault(true);
    buttons->setCenterButtons(true);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(aboutBox);
    layout->addWidget(mainBox);
    layout->addWidget(probeBox);
    layout->addWidget(buttons);
    layout->setSizeConstraint(QLayout::SetFixedSize);

    m_interval->setFocus();
    m_interval->selectAll();
}

TraceOptions OptionsDialog::options() const
{
    TraceOptions o = m_base;
    o.intervalSec = m_interval->value();
    o.pingSizeBytes = m_pingSize->value();
    o.resolveNames = m_resolveNames->isChecked();
    o.protocol = static_cast<TraceOptions::Protocol>(m_protocol->currentData().toInt());
    o.tcpPort = m_tcpPort->value();
    o.ipVersion = static_cast<TraceOptions::IpVersion>(m_ipVersion->currentData().toInt());
    o.timeoutSec = m_timeout->value();
    o.maxHops = m_maxHops->value();
    return o;
}

int OptionsDialog::maxLru() const
{
    return m_maxLru->value();
}

bool OptionsDialog::showIps() const
{
    return m_showIps->isChecked();
}

bool OptionsDialog::showGraph() const
{
    return m_showGraph->isChecked();
}

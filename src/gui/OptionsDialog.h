// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Wyn

#pragma once

#include <QDialog>

#include "core/TraceOptions.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QSpinBox;

// WinMTR "Options" dialog plus the Linux-only "Probing" group.
class OptionsDialog : public QDialog
{
    Q_OBJECT
public:
    // Input ranges and defaults. The settings loader and the CLI validation use them too.
    static constexpr double kIntervalMin = 0.1;     // seconds
    static constexpr double kIntervalMax = 60.0;    // seconds
    static constexpr int kPingSizeMin = 0;          // bytes
    static constexpr int kPingSizeMax = 8192;       // bytes
    static constexpr int kMaxLruMin = 1;            // hosts
    static constexpr int kMaxLruMax = 1024;         // hosts
    static constexpr int kDefaultMaxLru = 128;      // hosts (WinMTR default)
    static constexpr double kTimeoutMin = 0.5;      // seconds
    static constexpr double kTimeoutMax = 10.0;     // seconds
    static constexpr int kMaxHopsMin = 1;           // TTL
    static constexpr int kMaxHopsMax = 64;          // TTL
    static constexpr int kTcpPortMin = 1;
    static constexpr int kTcpPortMax = 65535;
    static constexpr bool kDefaultShowIps = true;   // upstream issue #4

    OptionsDialog(const TraceOptions &options, int maxLru, bool showIps, bool showGraph,
                  QWidget *parent = nullptr);

    TraceOptions options() const;
    int maxLru() const;
    bool showIps() const;
    bool showGraph() const;

private:
    TraceOptions m_base;
    QDoubleSpinBox *m_interval = nullptr;
    QSpinBox *m_pingSize = nullptr;
    QSpinBox *m_maxLru = nullptr;
    QCheckBox *m_resolveNames = nullptr;
    QCheckBox *m_showIps = nullptr;
    QCheckBox *m_showGraph = nullptr;
    QComboBox *m_protocol = nullptr;
    QSpinBox *m_tcpPort = nullptr;
    QComboBox *m_ipVersion = nullptr;
    QDoubleSpinBox *m_timeout = nullptr;
    QSpinBox *m_maxHops = nullptr;
};

// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Wyn

#pragma once

#include <QHostAddress>
#include <QList>
#include <QString>

#include "core/HopStats.h"

// Builds the WinMTR-compatible text and HTML reports and the LinMTR JSON
// report. The GUI (clipboard, export) and the headless --report mode both use
// it, so it depends on QtCore only. Host names in the text and HTML reports
// follow HopStats::displayHost(resolveNames, showIps).
class ReportBuilder
{
public:
    // The trace data of the JSON report that is not in the hops.
    struct JsonInfo
    {
        QString version;
        QString target;          // the host as given
        QHostAddress address;    // the resolved target; null until resolved
        QString protocol;        // "icmp", "udp" or "tcp"
        int rounds = 0;
    };

    // WinMTR "Copy Text to clipboard" / "Export TEXT" layout, brand swapped.
    // Like upstream, the result ends with the footer line and no newline.
    // A name longer than the 40-character column stays complete.
    static QString text(const QList<HopStats> &hops, bool resolveNames, bool showIps);

    // WinMTR "Copy HTML to clipboard" / "Export HTML" layout, brand swapped.
    static QString html(const QList<HopStats> &hops, bool resolveNames, bool showIps);

    // LinMTR JSON report (DESIGN.md section 5), indented, keys in alphabetical
    // order, ends with one newline. Times are milliseconds with 3 decimals.
    static QString json(const QList<HopStats> &hops, const JsonInfo &info);

    // Integer milliseconds as WinMTR shows them (sprintf "%d" of an int ms).
    // Returns 0 for zero, negative and NaN values, and INT_MAX on overflow.
    static int toDisplayMs(double ms);
};

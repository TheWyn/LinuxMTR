// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Wyn

#pragma once

#include <QList>
#include <QString>

#include "core/HopStats.h"

// Builds the WinMTR-compatible text and HTML reports. The GUI (clipboard,
// export) and the headless --report mode both use it, so it depends on QtCore
// only. Host names follow HopStats::displayHost(resolveNames, showIps).
class ReportBuilder
{
public:
    // WinMTR "Copy Text to clipboard" / "Export TEXT" layout, brand swapped.
    // Like upstream, the result ends with the footer line and no newline.
    // A name longer than the 40-character column stays complete.
    static QString text(const QList<HopStats> &hops, bool resolveNames, bool showIps);

    // WinMTR "Copy HTML to clipboard" / "Export HTML" layout, brand swapped.
    static QString html(const QList<HopStats> &hops, bool resolveNames, bool showIps);

    // Integer milliseconds as WinMTR shows them (sprintf "%d" of an int ms).
    // Returns 0 for zero, negative and NaN values, and INT_MAX on overflow.
    static int toDisplayMs(double ms);
};

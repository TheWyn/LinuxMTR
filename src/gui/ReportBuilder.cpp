// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Wyn

#include "gui/ReportBuilder.h"

#include <limits>

#ifndef LINMTR_VERSION
#define LINMTR_VERSION "0.1.0"
#endif

namespace {

// Equivalent of printf "%4d": right-justified to 4, never truncated.
template <typename T>
QString field4(T value)
{
    return QString::number(value).rightJustified(4, QLatin1Char(' '));
}

QString footer()
{
    return QStringLiteral("LinMTR v" LINMTR_VERSION " GPL V2 - WinMTR port for Linux");
}

} // namespace

int ReportBuilder::toDisplayMs(double ms)
{
    if (!(ms > 0.0)) // also catches NaN
        return 0;
    if (ms >= static_cast<double>(std::numeric_limits<int>::max()))
        return std::numeric_limits<int>::max();
    return static_cast<int>(ms);
}

QString ReportBuilder::text(const QList<HopStats> &hops, bool resolveNames, bool showIps)
{
    QString out;
    out += QStringLiteral("|------------------------------------------------------------------------------------------|\n");
    out += QStringLiteral("|                                      LinMTR statistics                                   |\n");
    out += QStringLiteral("|                       Host              -   %  | Sent | Recv | Best | Avrg | Wrst | Last |\n");
    out += QStringLiteral("|------------------------------------------------|------|------|------|------|------|------|\n");

    // Upstream: "|%40s - %4d | %4d | %4d | %4d | %4d | %4d | %4d |"
    // with name, percent, xmit, returned, best, avg, worst, last.
    for (const HopStats &hop : hops) {
        out += QLatin1Char('|');
        out += hop.displayHost(resolveNames, showIps).rightJustified(40, QLatin1Char(' '));
        out += QLatin1String(" - ");
        out += field4(hop.lossPercent());
        out += QLatin1String(" | ");
        out += field4(hop.sent);
        out += QLatin1String(" | ");
        out += field4(hop.received);
        out += QLatin1String(" | ");
        out += field4(toDisplayMs(hop.bestMs));
        out += QLatin1String(" | ");
        out += field4(toDisplayMs(hop.averageMs()));
        out += QLatin1String(" | ");
        out += field4(toDisplayMs(hop.worstMs));
        out += QLatin1String(" | ");
        out += field4(toDisplayMs(hop.lastMs));
        out += QLatin1String(" |\n");
    }

    out += QStringLiteral("|________________________________________________|______|______|______|______|______|______|\n");
    out += QLatin1String("   ");
    out += footer();
    return out;
}

QString ReportBuilder::html(const QList<HopStats> &hops, bool resolveNames, bool showIps)
{
    QString out;
    out += QStringLiteral("<html><head><title>LinMTR Statistics</title></head><body bgcolor=\"white\">\n");
    out += QStringLiteral("<center><h2>LinMTR statistics</h2></center>\n");
    out += QStringLiteral("<p align=\"center\"> <table border=\"1\" align=\"center\">\n");
    out += QStringLiteral("<tr><td>Host</td> <td>%</td> <td>Sent</td> <td>Recv</td> <td>Best</td> <td>Avrg</td> <td>Wrst</td> <td>Last</td></tr>\n");

    for (const HopStats &hop : hops) {
        out += QLatin1String("<tr><td>");
        // Reverse-DNS names come from the network: escape them. Valid host
        // names and the fixed error strings contain nothing to escape, so the
        // output stays identical to upstream for them.
        out += hop.displayHost(resolveNames, showIps).toHtmlEscaped();
        out += QLatin1String("</td> <td>");
        out += field4(hop.lossPercent());
        out += QLatin1String("</td> <td>");
        out += field4(hop.sent);
        out += QLatin1String("</td> <td>");
        out += field4(hop.received);
        out += QLatin1String("</td> <td>");
        out += field4(toDisplayMs(hop.bestMs));
        out += QLatin1String("</td> <td>");
        out += field4(toDisplayMs(hop.averageMs()));
        out += QLatin1String("</td> <td>");
        out += field4(toDisplayMs(hop.worstMs));
        out += QLatin1String("</td> <td>");
        out += field4(toDisplayMs(hop.lastMs));
        out += QLatin1String("</td></tr>\n");
    }

    out += QStringLiteral("</table></body></html>\n");
    return out;
}

#pragma once

#include <QHostAddress>
#include <QList>
#include <QString>

// One probe result of a hop, for the latency graphs. ms < 0 means no reply
// (timed out, or an ICMP error).
struct HopSample
{
    int round = 0;
    float ms = -1.0f;
};

// One row of the trace table. All times are milliseconds.
struct HopStats
{
    int ttl = 0;                 // 1-based hop number (the "Nr" column)
    QHostAddress address;        // latest responder; isNull() until a reply arrives
    QString hostname;            // reverse-DNS result; empty until resolved
    QString errorText;           // WinMTR ICMP error string; displayHost() shows it first
    quint64 sent = 0;            // probes sent, including those still in flight
    quint64 received = 0;
    quint64 timedOut = 0;        // probes that exceeded the reply timeout
    double lastMs = 0.0;
    double bestMs = 0.0;
    double worstMs = 0.0;
    double totalMs = 0.0;        // sum of reply RTTs, for the average
    bool isTarget = false;       // the trace reached the target at this hop

    // The newest kHistoryRounds probe results in ascending round order. A round
    // in which the hop got no probe has no entry. Reports do not use it.
    static constexpr int kHistoryRounds = 3600;
    QList<HopSample> history;

    // Records one probe result. Results can resolve out of round order (a
    // timeout resolves after a later reply), so the insert keeps the order.
    void addSample(int round, float ms)
    {
        qsizetype i = history.size();
        while (i > 0 && history[i - 1].round > round)
            --i;
        history.insert(i, HopSample{round, ms});
        if (history.size() > kHistoryRounds)
            history.removeFirst();
    }

    // WinMTR integer formula (100 - 100*recv/sent) over resolved probes only
    // (received + timedOut), so that a probe in flight never shows as loss.
    // 0 before the first probe resolves.
    int lossPercent() const
    {
        const quint64 done = received + timedOut;
        return done == 0 ? 0 : static_cast<int>(100 - (100 * received) / done);
    }

    double averageMs() const
    {
        return received == 0 ? 0.0 : totalMs / static_cast<double>(received);
    }

    // Display rule for the table, the reports and the properties dialog. Order:
    // errorText, the host name (when resolveNames), the address,
    // "No response from host". showIps appends the address to a resolved name
    // (upstream issue #4): "one.one.one.one (1.1.1.1)".
    QString displayHost(bool resolveNames, bool showIps = false) const
    {
        if (!errorText.isEmpty())
            return errorText;
        if (resolveNames && !hostname.isEmpty()) {
            const QString ip = address.isNull() ? QString() : address.toString();
            if (showIps && !ip.isEmpty() && hostname != ip)
                return hostname + QLatin1String(" (") + ip + QLatin1Char(')');
            return hostname;
        }
        if (!address.isNull())
            return address.toString();
        return QStringLiteral("No response from host");
    }
};

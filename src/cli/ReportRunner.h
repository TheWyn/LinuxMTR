// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Wyn

#pragma once

#include <QObject>
#include <QString>

#include "core/TraceOptions.h"
#include "core/TraceSession.h"

class QSocketNotifier;
class QTimer;

// Headless --report mode: traces one host for a number of rounds, prints the
// report in the given format to stdout and reports an exit code. cycles
// replaces options.maxRounds; cycles 0 traces until a signal. showIps is the
// -b option of mtr. A watchdog fires after cycles * interval + timeout + 10 s
// of tracing (not for cycles 0): it prints the partial report and still
// finishes with Success. start() catches SIGINT, SIGTERM and SIGHUP: the first
// one prints the report of the rounds so far and finishes with Success. A
// second signal ends the process with the default action.
// The signal handlers are process-wide: only one runner may exist in a process.
class ReportRunner : public QObject
{
    Q_OBJECT
public:
    enum ExitCode { Success = 0, TraceFailed = 1, BadArguments = 2 };
    enum Format { Text, Html, Json }; // the ReportBuilder report to print

    ReportRunner(const QString &host, const TraceOptions &options, int cycles, bool showIps,
                 Format format, QObject *parent = nullptr);
    ~ReportRunner() override;

public slots:
    // Call this after the event loop starts; finished() comes from inside the loop.
    void start();

signals:
    void finished(int exitCode);

private:
    void onStateChanged(TraceSession::State state);
    void onRoundCompleted(int rounds);
    void onErrorOccurred(const QString &message);
    void onInfoMessage(const QString &message);
    void onWatchdog();
    void onStopSignal();
    void printReport();
    void finish(int exitCode);

    TraceSession *m_session = nullptr;
    QTimer *m_watchdog = nullptr;
    QSocketNotifier *m_signalNotifier = nullptr;
    QString m_host;
    TraceOptions m_options;
    int m_cycles = 10;
    bool m_showIps = false;
    Format m_format = Text;
    bool m_done = false;
};

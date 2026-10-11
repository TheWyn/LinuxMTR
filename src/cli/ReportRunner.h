// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Wyn

#pragma once

#include <QObject>
#include <QString>

#include "core/TraceOptions.h"
#include "core/TraceSession.h"

class QTimer;

// Headless --report mode: traces one host for a fixed number of rounds,
// prints the WinMTR text report to stdout and reports an exit code.
// cycles replaces options.maxRounds; showIps is the -b option of mtr. A
// watchdog fires after cycles * interval + timeout + 10 s of tracing: it prints
// the partial report and still finishes with Success.
class ReportRunner : public QObject
{
    Q_OBJECT
public:
    enum ExitCode { Success = 0, TraceFailed = 1, BadArguments = 2 };

    ReportRunner(const QString &host, const TraceOptions &options, int cycles, bool showIps,
                 QObject *parent = nullptr);
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
    void printReport();
    void finish(int exitCode);

    TraceSession *m_session = nullptr;
    QTimer *m_watchdog = nullptr;
    QString m_host;
    TraceOptions m_options;
    int m_cycles = 10;
    bool m_showIps = false;
    bool m_done = false;
};

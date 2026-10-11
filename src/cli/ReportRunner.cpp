// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Wyn

#include "cli/ReportRunner.h"

#include "gui/ReportBuilder.h"

#include <QTimer>

#include <cstdio>
#include <limits>

namespace {

void writeTo(FILE *stream, const QString &text)
{
    const QByteArray bytes = text.toUtf8();
    std::fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), stream);
    std::fflush(stream);
}

void printError(const QString &message)
{
    writeTo(stderr, QStringLiteral("linmtr: ") + message + QLatin1Char('\n'));
}

} // namespace

ReportRunner::ReportRunner(const QString &host, const TraceOptions &options, int cycles,
                           bool showIps, QObject *parent)
    : QObject(parent)
    , m_session(new TraceSession(this))
    , m_watchdog(new QTimer(this))
    , m_host(host)
    , m_options(options)
    , m_cycles(cycles < 1 ? 1 : cycles)
    , m_showIps(showIps)
{
    // Send exactly m_cycles rounds and wait for them, so that Sent equals
    // --cycles and no probe is still in flight when the report prints.
    m_options.maxRounds = m_cycles;
    m_watchdog->setSingleShot(true);
    connect(m_watchdog, &QTimer::timeout, this, &ReportRunner::onWatchdog);
    connect(m_session, &TraceSession::stateChanged, this, &ReportRunner::onStateChanged);
    connect(m_session, &TraceSession::roundCompleted, this, &ReportRunner::onRoundCompleted);
    connect(m_session, &TraceSession::errorOccurred, this, &ReportRunner::onErrorOccurred);
    connect(m_session, &TraceSession::infoMessage, this, &ReportRunner::onInfoMessage);
}

ReportRunner::~ReportRunner()
{
    disconnect(m_session, nullptr, this, nullptr);
    delete m_session;
    m_session = nullptr;
}

void ReportRunner::start()
{
    m_session->start(m_host, m_options);
}

void ReportRunner::onStateChanged(TraceSession::State state)
{
    if (m_done)
        return;

    if (state == TraceSession::State::Tracing) {
        // Guard against a stalled engine: all rounds resolve within
        // cycles * interval + timeout. The watchdog adds 10 s of slack.
        const double ms = (m_cycles * m_options.intervalSec + m_options.timeoutSec) * 1000.0
            + 10000.0;
        const double capped = ms < static_cast<double>(std::numeric_limits<int>::max())
            ? ms
            : static_cast<double>(std::numeric_limits<int>::max());
        m_watchdog->start(static_cast<int>(capped));
        return;
    }

    if (state == TraceSession::State::Idle) {
        // A fatal error usually emits errorOccurred immediately after this
        // signal. Let it arrive first, so that the real message prints.
        QTimer::singleShot(0, this, [this] {
            if (m_done)
                return;
            const QString err = m_session->lastError();
            printError(err.isEmpty() ? QStringLiteral("trace stopped unexpectedly") : err);
            finish(TraceFailed);
        });
    }
}

void ReportRunner::onRoundCompleted(int rounds)
{
    if (m_done || rounds < m_cycles)
        return;
    printReport();
    finish(Success);
}

void ReportRunner::onErrorOccurred(const QString &message)
{
    if (m_done)
        return;
    printError(message);
    finish(TraceFailed);
}

void ReportRunner::onInfoMessage(const QString &message)
{
    if (!m_done)
        printError(message);
}

void ReportRunner::onWatchdog()
{
    if (m_done)
        return;
    printError(QStringLiteral("timed out waiting for %1 rounds (%2 completed)")
                   .arg(m_cycles)
                   .arg(m_session->roundsCompleted()));
    printReport();
    finish(Success);
}

void ReportRunner::printReport()
{
    // The report ends with the upstream footer line and no newline; add one.
    writeTo(stdout, ReportBuilder::text(m_session->hops(), m_options.resolveNames, m_showIps)
                        + QLatin1Char('\n'));
}

void ReportRunner::finish(int exitCode)
{
    if (m_done)
        return;
    m_done = true;
    m_watchdog->stop();
    // finish() usually runs inside a signal of the session. Stop the session
    // from the event loop; do not re-enter it here.
    QMetaObject::invokeMethod(m_session, &TraceSession::stop, Qt::QueuedConnection);
    emit finished(exitCode);
}

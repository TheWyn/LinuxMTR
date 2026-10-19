// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Wyn

#include "cli/ReportRunner.h"

#include "gui/ReportBuilder.h"

#include <QSocketNotifier>
#include <QTimer>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <limits>

#include <sys/socket.h>
#include <unistd.h>

namespace {

// Self-pipe: the signal handler writes one byte, a QSocketNotifier reads it.
int g_signalFds[2] = {-1, -1};
constexpr int kStopSignals[] = {SIGINT, SIGTERM, SIGHUP};

void onSignal(int)
{
    const int savedErrno = errno;
    const char byte = 0;
    // Only async-signal-safe calls here. A second signal of any kind ends the process.
    for (int sig : kStopSignals)
        ::signal(sig, SIG_DFL);
    // A full socket already holds a byte.
    [[maybe_unused]] const ssize_t n = ::write(g_signalFds[1], &byte, 1);
    errno = savedErrno;
}

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
                           bool showIps, Format format, QObject *parent)
    : QObject(parent)
    , m_session(new TraceSession(this))
    , m_watchdog(new QTimer(this))
    , m_host(host)
    , m_options(options)
    , m_cycles(cycles < 0 ? 0 : cycles)
    , m_showIps(showIps)
    , m_format(format)
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
    if (m_signalNotifier) {
        for (int sig : kStopSignals)
            std::signal(sig, SIG_DFL);
        delete m_signalNotifier;
        m_signalNotifier = nullptr;
        ::close(g_signalFds[0]);
        ::close(g_signalFds[1]);
        g_signalFds[0] = g_signalFds[1] = -1;
    }
}

void ReportRunner::start()
{
    // Without the socket pair, the signals keep their default action.
    if (!m_signalNotifier
        && ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, g_signalFds) == 0) {
        m_signalNotifier = new QSocketNotifier(g_signalFds[0], QSocketNotifier::Read, this);
        connect(m_signalNotifier, &QSocketNotifier::activated, this, &ReportRunner::onStopSignal);
        struct sigaction action = {};
        action.sa_handler = onSignal;
        sigemptyset(&action.sa_mask);
        for (int sig : kStopSignals)
            ::sigaction(sig, &action, nullptr);
    }
    m_session->start(m_host, m_options);
}

void ReportRunner::onStateChanged(TraceSession::State state)
{
    if (m_done)
        return;

    if (state == TraceSession::State::Tracing) {
        if (m_cycles == 0)
            return; // runs until a signal: no time limit
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
    if (m_done || m_cycles == 0 || rounds < m_cycles)
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

void ReportRunner::onStopSignal()
{
    char buffer[16];
    while (::read(g_signalFds[0], buffer, sizeof(buffer)) > 0) {
    }
    if (m_done)
        return;
    printReport();
    finish(Success);
}

void ReportRunner::printReport()
{
    const QList<HopStats> hops = m_session->hops();
    switch (m_format) {
    case Text:
        // The report ends with the upstream footer line and no newline; add one.
        writeTo(stdout, ReportBuilder::text(hops, m_options.resolveNames, m_showIps)
                            + QLatin1Char('\n'));
        break;
    case Html:
        writeTo(stdout, ReportBuilder::html(hops, m_options.resolveNames, m_showIps));
        break;
    case Json: {
        // The session options hold the protocol after a fallback to UDP.
        const TraceOptions::Protocol protocol = m_session->options().protocol;
        const ReportBuilder::JsonInfo info{
            QStringLiteral(LINMTR_VERSION), m_host, m_session->targetAddress(),
            protocol == TraceOptions::Protocol::Udp   ? QStringLiteral("udp")
            : protocol == TraceOptions::Protocol::Tcp ? QStringLiteral("tcp")
                                                      : QStringLiteral("icmp"),
            m_session->roundsCompleted()};
        writeTo(stdout, ReportBuilder::json(hops, info));
        break;
    }
    }
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

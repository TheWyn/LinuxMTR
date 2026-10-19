// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Wyn

#pragma once

#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QString>

#include "HopStats.h"
#include "TraceOptions.h"

// Drives one continuous trace toward one target: resolves the host, sends one
// probe per TTL per round, collects replies and per-hop statistics.
// Lives on the main thread; all I/O is non-blocking through the event loop.
class TraceSession : public QObject
{
    Q_OBJECT
public:
    enum class State { Idle, Resolving, Tracing };
    Q_ENUM(State)

    explicit TraceSession(QObject *parent = nullptr);
    ~TraceSession() override;

    State state() const;
    QString target() const;              // host string as passed to start()
    QHostAddress targetAddress() const;  // valid after resolution succeeded
    TraceOptions options() const;
    int roundsCompleted() const;         // rounds whose probes have all resolved
    QString lastError() const;

    // Snapshot of the probed hops 1..N in TTL order, trimmed by the WinMTR GetMax() rule.
    // N can be after a silent target hop (DESIGN.md §3.3). Search probes do not count in
    // the statistics of a hop.
    QList<HopStats> hops() const;

public slots:
    // A start while the session is active calls stop() first.
    void start(const QString &target, const TraceOptions &options);
    void stop();

signals:
    void stateChanged(TraceSession::State state);
    void hopsChanged();                        // coalesced: at most one per 100 ms
    void roundCompleted(int rounds);
    void errorOccurred(const QString &message); // fatal: session returned to Idle
    void infoMessage(const QString &message);   // non-fatal notice (for example UDP fallback)

private:
    struct Private;
    Private *d = nullptr;
};

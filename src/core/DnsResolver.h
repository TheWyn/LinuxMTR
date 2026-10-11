// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Wyn

#pragma once

#include <QHash>
#include <QHostAddress>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QString>

#include <functional>

class QHostInfo;

namespace linmtr::core {

// Asynchronous reverse DNS through QHostInfo with an address -> name cache,
// in-flight de-duplication and at most kMaxConcurrent lookups at a time.
// The cache keeps only names. After a failed lookup, the callback gets an empty
// name and the caller shows the address; the next resolve() of it tries again.
class DnsResolver : public QObject
{
public:
    // The resolver calls it on the owning thread when a queued lookup finishes.
    using Callback = std::function<void(const QHostAddress &address, const QString &name)>;

    // Most lookups in flight at the same time; the others wait in a queue.
    static constexpr int kMaxConcurrent = 8;
    // Most cached names. It limits memory use: at this size, the next result
    // clears the full cache first.
    static constexpr int kMaxCacheEntries = 4096;

    explicit DnsResolver(QObject *parent = nullptr);
    ~DnsResolver() override;

    void setCallback(Callback callback);

    // Returns true and sets *name when the cache has the name. Otherwise
    // schedules a lookup (none while one for this address is queued or in
    // flight) and returns false. A null address returns false and schedules
    // nothing.
    bool resolve(const QHostAddress &address, QString *name);

    // Cancels all queued and in-flight lookups; their callbacks never run.
    // The cache stays.
    void abortAll();

private:
    void startQueued();
    void finished(quint64 generation, const QString &key, const QHostAddress &address,
                  const QHostInfo &info);

    QHash<QString, QString> m_cache;
    QHash<QString, int> m_inFlight;     // address key -> QHostInfo lookup id
    QQueue<QHostAddress> m_queue;
    QSet<QString> m_queued;
    Callback m_callback;
    quint64 m_generation = 0;
};

} // namespace linmtr::core

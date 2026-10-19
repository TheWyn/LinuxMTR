// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Wyn

#include "DnsResolver.h"

#include <QHostInfo>

#include <utility>

namespace linmtr::core {

namespace {

// PTR records are attacker-controlled: accept only plausible host names, so that
// no markup, control characters or huge strings reach the display or the HTML
// report. 253 characters is the longest valid DNS name.
QString sanitizeHostName(const QString &name, const QString &addressKey)
{
    if (name.isEmpty() || name.size() > 253 || name == addressKey)
        return QString();
    for (const QChar ch : name) {
        if (ch.isLetterOrNumber() || ch == u'-' || ch == u'.' || ch == u'_')
            continue;
        return QString();
    }
    if (QHostAddress(name).protocol() != QHostAddress::UnknownNetworkLayerProtocol)
        return QString();   // the resolver echoed a numeric address
    return name;
}

} // namespace

DnsResolver::DnsResolver(QObject *parent)
    : QObject(parent)
{
}

DnsResolver::~DnsResolver()
{
    abortAll();
}

void DnsResolver::setCallback(Callback callback)
{
    m_callback = std::move(callback);
}

bool DnsResolver::resolve(const QHostAddress &address, QString *name)
{
    if (address.isNull())
        return false;
    const QString key = address.toString();
    const auto cached = m_cache.constFind(key);
    if (cached != m_cache.constEnd()) {
        if (name)
            *name = cached.value();
        return true;
    }
    if (m_inFlight.contains(key) || m_queued.contains(key))
        return false;
    m_queue.enqueue(address);
    m_queued.insert(key);
    startQueued();
    return false;
}

void DnsResolver::abortAll()
{
    ++m_generation;
    for (auto it = m_inFlight.cbegin(); it != m_inFlight.cend(); ++it) {
        if (it.value() >= 0)
            QHostInfo::abortHostLookup(it.value());
    }
    m_inFlight.clear();
    m_queue.clear();
    m_queued.clear();
}

void DnsResolver::startQueued()
{
    while (m_inFlight.size() < kMaxConcurrent && !m_queue.isEmpty()) {
        const QHostAddress address = m_queue.dequeue();
        const QString key = address.toString();
        m_queued.remove(key);
        if (m_cache.contains(key) || m_inFlight.contains(key))
            continue;

        const quint64 generation = m_generation;
        m_inFlight.insert(key, -1);   // placeholder, if the result arrives before the id
        const int id = QHostInfo::lookupHost(key, this,
            [this, generation, key, address](const QHostInfo &info) {
                finished(generation, key, address, info);
            });
        const auto it = m_inFlight.find(key);
        if (it != m_inFlight.end() && generation == m_generation)
            it.value() = id;
    }
}

void DnsResolver::finished(quint64 generation, const QString &key, const QHostAddress &address,
                           const QHostInfo &info)
{
    QString name;
    if (info.error() == QHostInfo::NoError)
        name = sanitizeHostName(info.hostName(), key);

    // Qt gives a failed PTR lookup (NXDOMAIN or timeout) as the numeric address
    // with NoError, so it is an empty name here. Do not cache it: retry later.
    if (!name.isEmpty()) {
        if (m_cache.size() >= kMaxCacheEntries)
            m_cache.clear();
        m_cache.insert(key, name);
    }

    if (generation != m_generation)
        return;   // aborted: no callback

    m_inFlight.remove(key);
    if (m_callback)
        m_callback(address, name);
    startQueued();
}

} // namespace linmtr::core

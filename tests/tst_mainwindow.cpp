// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Wyn

#include <QApplication>
#include <QHostInfo>
#include <QLabel>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

#include "core/Prober.h"
#include "gui/MainWindow.h"

namespace {

// Sends no packet and gets no answer: the status test needs only the states.
class SilentBackend : public linmtr::core::ProbeBackend
{
public:
    int open() override { return 0; }
    int socketCount() const override { return 1; }
    int send(int, quint16, int) override { return 0; }
    void drain(const EventSink &) override {}
    void attachNotifiers(QObject *, const std::function<void()> &) override {}
};

} // namespace

class TestMainWindow : public QObject
{
    Q_OBJECT

private slots:
    void tracingText_data();
    void tracingText();
    void statusShowsProbedAddress();
};

void TestMainWindow::tracingText_data()
{
    QTest::addColumn<QString>("host");
    QTest::addColumn<QString>("address");   // empty: null address
    QTest::addColumn<QString>("text");

    QTest::newRow("name, no address yet") << "example.com" << "" << "Tracing example.com";
    QTest::newRow("name, IPv4") << "example.com" << "192.0.2.1" << "Tracing example.com (192.0.2.1)";
    QTest::newRow("name, IPv6") << "example.com" << "2001:db8::1" << "Tracing example.com (2001:db8::1)";
    QTest::newRow("name with %2") << "a%2b" << "192.0.2.1" << "Tracing a%2b (192.0.2.1)";
    QTest::newRow("IPv4 literal") << "192.0.2.1" << "192.0.2.1" << "Tracing 192.0.2.1";
    QTest::newRow("IPv6 literal, other notation") << "2001:DB8:0::1" << "2001:db8::1" << "Tracing 2001:DB8:0::1";
    QTest::newRow("bracketed IPv6 literal") << "[2001:db8::1]" << "2001:db8::1" << "Tracing [2001:db8::1]";
    QTest::newRow("IPv4-mapped literal") << "::ffff:192.0.2.1" << "192.0.2.1" << "Tracing ::ffff:192.0.2.1";
}

// The status text shows the probed address after a host name, and no second
// copy of an IP address.
void TestMainWindow::tracingText()
{
    QFETCH(QString, host);
    QFETCH(QString, address);
    QFETCH(QString, text);
    QCOMPARE(MainWindow::tracingText(host, QHostAddress(address)), text);
}

// The window shows the address that the session probes. localhost needs no
// network: /etc/hosts or nss-myhostname resolves it. Only a host that cannot
// resolve localhost (for example a bare chroot) skips the test.
void TestMainWindow::statusShowsProbedAddress()
{
    const QHostInfo localhost = QHostInfo::fromName(QStringLiteral("localhost"));
    if (localhost.error() != QHostInfo::NoError || localhost.addresses().isEmpty())
        QSKIP("localhost does not resolve");

    linmtr::core::setBackendFactoryForTests([] { return std::make_unique<SilentBackend>(); });
    const auto restore = qScopeGuard([] { linmtr::core::setBackendFactoryForTests({}); });

    MainWindow window;
    QLabel *status = nullptr;
    for (QLabel *label : window.statusBar()->findChildren<QLabel *>()) {
        if (label->text() == QLatin1String("Ready"))
            status = label;
    }
    QVERIFY(status != nullptr);
    window.startTrace(QStringLiteral("localhost"));
    QTRY_VERIFY_WITH_TIMEOUT(!status->text().startsWith(QLatin1String("Resolving")), 10000);
    static const QRegularExpression probed(QStringLiteral("^Tracing localhost \\((127\\.0\\.0\\.1|::1)\\)$"));
    QVERIFY2(probed.match(status->text()).hasMatch(), qPrintable(status->text()));
}

int main(int argc, char *argv[])
{
    // MainWindow reads and writes QSettings: keep them out of the user's configuration.
    QTemporaryDir config;
    if (!config.isValid())
        return 1;   // an empty XDG_CONFIG_HOME would mean ~/.config
    qputenv("XDG_CONFIG_HOME", config.path().toLocal8Bit());
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    TestMainWindow test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_mainwindow.moc"

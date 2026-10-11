// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Wyn

#include "cli/ReportRunner.h"
#include "core/TraceOptions.h"
#include "gui/MainWindow.h"
#include "gui/OptionsDialog.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QIcon>
#include <QTimer>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>

#ifndef LINMTR_VERSION
#define LINMTR_VERSION "0.1.0"
#endif

namespace {

constexpr int kDefaultCycles = 10;      // report rounds without -c
constexpr int kMaxCycles = 1000000;     // largest -c value

void printError(const QString &message)
{
    const QByteArray bytes = (QStringLiteral("linmtr: ") + message + QLatin1Char('\n')).toUtf8();
    std::fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), stderr);
    std::fflush(stderr);
}

// Selects the application class before any Q*Application exists. Report mode,
// --help and --version must work without a display. --help-all needs a
// QApplication, because Qt lists the generic options of the application class
// that exists (-platform, -style). *helpAll tells main() to make one on the
// offscreen platform. *noDisplay is true when a QCoreApplication is necessary
// only because no display is available: a QApplication then aborts before the
// parser can show an error. Returns true when a QCoreApplication is sufficient.
bool needsCoreApplication(int argc, char **argv, bool *reportMode, bool *helpAll,
                          bool *noDisplay)
{
    bool core = false;
    *reportMode = false;
    *helpAll = false;
    *noDisplay = false;
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (std::strcmp(arg, "--") == 0)
            break;
        // "--report=VALUE" is a parse error. It also selects QCoreApplication, so
        // that the parser reports it with status 2 on a host without a display.
        if (std::strcmp(arg, "--report") == 0 || std::strncmp(arg, "--report=", 9) == 0) {
            *reportMode = true;
            core = true;
        } else if (std::strcmp(arg, "--help-all") == 0) {
            *helpAll = true;
        } else if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0
                   || std::strcmp(arg, "-?") == 0
                   || std::strcmp(arg, "--version") == 0 || std::strcmp(arg, "-v") == 0) {
            core = true;
        }
    }
    if (*helpAll)
        return false;
    if (core)
        return true;
    // Qt reads -platform also after "--", so this scan does not stop there.
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-platform") == 0 || std::strcmp(argv[i], "--platform") == 0)
            return false;
    }
    // A wayland or x11 session can have a usable display socket when the
    // display variables are not set.
    const QByteArray sessionType = qgetenv("XDG_SESSION_TYPE");
    *noDisplay = qEnvironmentVariableIsEmpty("DISPLAY")
                 && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")
                 && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")
                 && sessionType != "wayland" && sessionType != "x11";
    return *noDisplay;
}

QString longName(const QCommandLineOption &option)
{
    const QString name = option.names().constLast();
    return (name.size() == 1 ? QStringLiteral("-") : QStringLiteral("--")) + name;
}

bool parseInt(const QCommandLineParser &parser, const QCommandLineOption &option,
              int lo, int hi, std::optional<int> *out, QString *error)
{
    if (!parser.isSet(option))
        return true;
    const QString text = parser.value(option).trimmed();
    bool ok = false;
    const int v = text.toInt(&ok);
    if (!ok || v < lo || v > hi) {
        *error = QStringLiteral("invalid value for %1: '%2' (expected an integer from %3 to %4)")
                     .arg(longName(option), text)
                     .arg(lo)
                     .arg(hi);
        return false;
    }
    *out = v;
    return true;
}

bool parseDouble(const QCommandLineParser &parser, const QCommandLineOption &option,
                 double lo, double hi, std::optional<double> *out, QString *error)
{
    if (!parser.isSet(option))
        return true;
    const QString text = parser.value(option).trimmed();
    bool ok = false;
    const double v = text.toDouble(&ok); // C locale, independent of LANG
    if (!ok || !std::isfinite(v) || v < lo || v > hi) {
        *error = QStringLiteral("invalid value for %1: '%2' (expected a number from %3 to %4)")
                     .arg(longName(option), text, QString::number(lo), QString::number(hi));
        return false;
    }
    *out = v;
    return true;
}

// Command-line settings; only the ones given override the base options.
struct Overrides
{
    std::optional<int> cycles;
    std::optional<int> size;
    std::optional<double> interval;
    std::optional<double> timeout;
    std::optional<int> maxHops;
    std::optional<int> port;
    bool noDns = false;
    bool showIps = false; // not a TraceOptions field: display only
    bool udp = false;
    bool tcp = false;
    bool ipv4 = false;
    bool ipv6 = false;

    void applyTo(TraceOptions &o) const
    {
        if (size)
            o.pingSizeBytes = *size;
        if (interval)
            o.intervalSec = *interval;
        if (timeout)
            o.timeoutSec = *timeout;
        if (maxHops)
            o.maxHops = *maxHops;
        if (port)
            o.tcpPort = *port;
        if (noDns)
            o.resolveNames = false;
        if (udp)
            o.protocol = TraceOptions::Protocol::Udp;
        if (tcp)
            o.protocol = TraceOptions::Protocol::Tcp;
        if (ipv4)
            o.ipVersion = TraceOptions::IpVersion::V4;
        if (ipv6)
            o.ipVersion = TraceOptions::IpVersion::V6;
    }
};

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication::setOrganizationName(QStringLiteral("LinMTR"));
    QCoreApplication::setApplicationName(QStringLiteral("LinMTR"));
    QCoreApplication::setApplicationVersion(QStringLiteral(LINMTR_VERSION));

    bool reportMode = false;
    bool helpAllMode = false;
    bool noDisplay = false;
    const bool coreOnly =
        needsCoreApplication(argc, argv, &reportMode, &helpAllMode, &noDisplay);

    std::unique_ptr<QCoreApplication> app;
    if (coreOnly) {
        app = std::make_unique<QCoreApplication>(argc, argv);
    } else if (helpAllMode) {
        // Only prints text: no display is necessary.
        qputenv("QT_QPA_PLATFORM", "offscreen");
        app = std::make_unique<QApplication>(argc, argv);
    } else {
        QGuiApplication::setDesktopFileName(QStringLiteral("linmtr"));
        app = std::make_unique<QApplication>(argc, argv);
        QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("linmtr"),
                                                     QIcon::fromTheme(QStringLiteral("network-wired"))));
    }

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("LinMTR - WinMTR port for Linux. Combines traceroute and ping:\n"
                       "every hop to HOST is probed continuously."));
    const QCommandLineOption helpOption = parser.addHelpOption();
    const QCommandLineOption versionOption = parser.addVersionOption();

    const QCommandLineOption reportOption(
        QStringLiteral("report"),
        QStringLiteral("Headless report mode: no GUI, run --cycles rounds, print the text "
                       "report to stdout and exit."));
    const QCommandLineOption cyclesOption(
        {QStringLiteral("c"), QStringLiteral("cycles")},
        QStringLiteral("Rounds for --report (default %1).").arg(kDefaultCycles),
        QStringLiteral("N"));
    const QCommandLineOption noDnsOption(
        {QStringLiteral("n"), QStringLiteral("no-dns")},
        QStringLiteral("Disable reverse DNS."));
    const QCommandLineOption showIpsOption(
        {QStringLiteral("b"), QStringLiteral("show-ips")},
        QStringLiteral("Show both host name and IP address for each hop, like mtr -b "
                       "(report mode default: off; the GUI uses its saved setting, "
                       "default on)."));
    const QCommandLineOption ipv4Option(QStringLiteral("4"), QStringLiteral("Force IPv4."));
    const QCommandLineOption ipv6Option(QStringLiteral("6"), QStringLiteral("Force IPv6."));
    const QCommandLineOption udpOption(
        QStringLiteral("udp"), QStringLiteral("UDP probes instead of ICMP."));
    const QCommandLineOption tcpOption(
        QStringLiteral("tcp"),
        QStringLiteral("TCP connect() probes instead of ICMP (not with --udp)."));
    const QCommandLineOption portOption(
        {QStringLiteral("P"), QStringLiteral("port")},
        QStringLiteral("Destination port for --tcp (default %1).").arg(TraceOptions().tcpPort),
        QStringLiteral("n"));
    const QCommandLineOption sizeOption(
        {QStringLiteral("s"), QStringLiteral("size")},
        QStringLiteral("Payload size in bytes (default 64)."), QStringLiteral("bytes"));
    const QCommandLineOption intervalOption(
        {QStringLiteral("i"), QStringLiteral("interval")},
        QStringLiteral("Probe interval in seconds (default 1.0)."), QStringLiteral("s"));
    const QCommandLineOption timeoutOption(
        QStringLiteral("timeout"),
        QStringLiteral("Reply timeout in seconds (default 3.0)."), QStringLiteral("s"));
    const QCommandLineOption maxHopsOption(
        {QStringLiteral("m"), QStringLiteral("max-hops")},
        QStringLiteral("Maximum TTL (default 30)."), QStringLiteral("N"));

    parser.addOptions({reportOption, cyclesOption, noDnsOption, showIpsOption, ipv4Option,
                       ipv6Option, udpOption, tcpOption, portOption, sizeOption, intervalOption,
                       timeoutOption, maxHopsOption});
    parser.addPositionalArgument(QStringLiteral("host"),
                                 QStringLiteral("Target host name or IP address. In GUI mode "
                                                "the trace starts immediately."),
                                 QStringLiteral("[HOST]"));

    if (!parser.parse(app->arguments())) {
        printError(parser.errorText());
        return ReportRunner::BadArguments;
    }
    if (parser.isSet(helpOption))
        parser.showHelp(0);
    if (parser.optionNames().contains(QStringLiteral("help-all"))) {
        // QCommandLineParser prints the generic Qt options only from process().
        // parse() already accepted the arguments, so this call prints the help
        // (or the version, if also given) and exits with status 0.
        parser.process(app->arguments());
    }
    if (parser.isSet(versionOption))
        parser.showVersion();

    Overrides ov;
    QString error;
    const bool valid =
        parseInt(parser, cyclesOption, 1, kMaxCycles, &ov.cycles, &error)
        && parseInt(parser, sizeOption, OptionsDialog::kPingSizeMin, OptionsDialog::kPingSizeMax,
                    &ov.size, &error)
        && parseDouble(parser, intervalOption, OptionsDialog::kIntervalMin,
                       OptionsDialog::kIntervalMax, &ov.interval, &error)
        && parseDouble(parser, timeoutOption, OptionsDialog::kTimeoutMin,
                       OptionsDialog::kTimeoutMax, &ov.timeout, &error)
        && parseInt(parser, maxHopsOption, OptionsDialog::kMaxHopsMin,
                    OptionsDialog::kMaxHopsMax, &ov.maxHops, &error)
        && parseInt(parser, portOption, OptionsDialog::kTcpPortMin,
                    OptionsDialog::kTcpPortMax, &ov.port, &error);
    if (!valid) {
        printError(error);
        return ReportRunner::BadArguments;
    }
    ov.noDns = parser.isSet(noDnsOption);
    ov.showIps = parser.isSet(showIpsOption);
    ov.udp = parser.isSet(udpOption);
    ov.tcp = parser.isSet(tcpOption);
    ov.ipv4 = parser.isSet(ipv4Option);
    ov.ipv6 = parser.isSet(ipv6Option);
    if (ov.ipv4 && ov.ipv6) {
        printError(QStringLiteral("options -4 and -6 are mutually exclusive"));
        return ReportRunner::BadArguments;
    }
    if (ov.udp && ov.tcp) {
        printError(QStringLiteral("options --udp and --tcp are mutually exclusive"));
        return ReportRunner::BadArguments;
    }

    const QStringList positional = parser.positionalArguments();
    if (positional.size() > 1) {
        printError(QStringLiteral("too many arguments: only one HOST may be given"));
        return ReportRunner::BadArguments;
    }
    const QString host = positional.value(0).trimmed();

    if (reportMode) {
        if (host.isEmpty()) {
            printError(QStringLiteral("No host specified!"));
            return ReportRunner::BadArguments;
        }
        TraceOptions options; // CLI defaults, independent of saved GUI settings
        ov.applyTo(options);
        ReportRunner runner(host, options, ov.cycles.value_or(kDefaultCycles), ov.showIps);
        QObject::connect(
            &runner, &ReportRunner::finished, app.get(),
            [](int code) { QCoreApplication::exit(code); }, Qt::QueuedConnection);
        QTimer::singleShot(0, &runner, &ReportRunner::start);
        return app->exec();
    }

    if (noDisplay) {
        printError(QStringLiteral("no display available (use --report for headless operation)."));
        return EXIT_FAILURE;
    }
    if (coreOnly || helpAllMode) {
        // Reachable only when an option took a help or version token as its
        // value. Widgets cannot exist without a QApplication on a display.
        printError(QStringLiteral("invalid arguments; see --help"));
        return ReportRunner::BadArguments;
    }

    MainWindow window;
    TraceOptions options = window.traceOptions(); // saved settings
    ov.applyTo(options);
    window.setTraceOptions(options);
    if (ov.showIps)
        window.setShowIps(true);
    window.show();
    if (!host.isEmpty())
        QTimer::singleShot(0, &window, [&window, host] { window.startTrace(host); });
    return app->exec();
}

// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Wyn

#include "gui/MainWindow.h"

#include "gui/HopPropertiesDialog.h"
#include "gui/HopTableModel.h"
#include "gui/OptionsDialog.h"
#include "gui/ReportBuilder.h"

#include <QAction>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QCompleter>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QStringListModel>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>

#ifndef LINMTR_VERSION
#define LINMTR_VERSION "0.1.0"
#endif

namespace {

const QString kKeyInterval = QStringLiteral("options/interval");
const QString kKeyPingSize = QStringLiteral("options/pingSize");
const QString kKeyMaxLru = QStringLiteral("options/maxLru");
const QString kKeyResolveNames = QStringLiteral("options/resolveNames");
const QString kKeyShowIps = QStringLiteral("options/showIps");
const QString kKeyTimeout = QStringLiteral("options/timeout");
const QString kKeyMaxHops = QStringLiteral("options/maxHops");
const QString kKeyProtocol = QStringLiteral("options/protocol");
const QString kKeyTcpPort = QStringLiteral("options/tcpPort");
const QString kKeyIpVersion = QStringLiteral("options/ipVersion");
const QString kKeyGeometry = QStringLiteral("ui/geometry");
const QString kKeyLruHosts = QStringLiteral("lru/hosts");

// Upstream IDS_STRING_CLEAR_HISTORY: always the last entry of the host box.
const QString kClearHistory = QStringLiteral("Clear History");

double readDouble(const QSettings &s, const QString &key, double def, double lo, double hi)
{
    bool ok = false;
    const double v = s.value(key).toDouble(&ok);
    if (!ok || !std::isfinite(v))
        return def;
    return qBound(lo, v, hi);
}

int readInt(const QSettings &s, const QString &key, int def, int lo, int hi)
{
    bool ok = false;
    const int v = s.value(key).toInt(&ok);
    if (!ok)
        return def;
    return qBound(lo, v, hi);
}

QString protocolKey(TraceOptions::Protocol p)
{
    switch (p) {
    case TraceOptions::Protocol::Udp: return QStringLiteral("udp");
    case TraceOptions::Protocol::Tcp: return QStringLiteral("tcp");
    case TraceOptions::Protocol::Icmp: break;
    }
    return QStringLiteral("icmp");
}

QString ipVersionKey(TraceOptions::IpVersion v)
{
    switch (v) {
    case TraceOptions::IpVersion::V4: return QStringLiteral("ipv4");
    case TraceOptions::IpVersion::V6: return QStringLiteral("ipv6");
    case TraceOptions::IpVersion::Auto: break;
    }
    return QStringLiteral("auto");
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("LinMTR v" LINMTR_VERSION " - WinMTR port for Linux"));

    m_session = new TraceSession(this);
    buildUi();
    loadSettings();

    connect(m_session, &TraceSession::stateChanged, this, &MainWindow::onStateChanged);
    connect(m_session, &TraceSession::hopsChanged, this, &MainWindow::onHopsChanged);
    connect(m_session, &TraceSession::errorOccurred, this, &MainWindow::onErrorOccurred);
    connect(m_session, &TraceSession::infoMessage, this, &MainWindow::onInfoMessage);

    m_hostCombo->setFocus();
}

MainWindow::~MainWindow()
{
    // QWidget deletes children before QObject drops connections; make sure the
    // session cannot call back into this half-destroyed window.
    disconnect(m_session, nullptr, this, nullptr);
    delete m_session;
    m_session = nullptr;
}

void MainWindow::buildUi()
{
    auto *central = new QWidget(this);

    // Row 1, left: host box and Start/Stop.
    auto *hostBox = new QGroupBox(central);
    auto *hostLayout = new QHBoxLayout(hostBox);
    auto *hostLabel = new QLabel(tr("Host:"), hostBox);
    m_hostCombo = new QComboBox(hostBox);
    m_hostCombo->setEditable(true);
    m_hostCombo->setInsertPolicy(QComboBox::NoInsert);
    m_hostCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_hostCombo->lineEdit()->setMaxLength(255);
    // Complete from the history only, never from the "Clear History" entry.
    m_completionModel = new QStringListModel(this);
    auto *completer = new QCompleter(m_completionModel, this);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setCompletionMode(QCompleter::InlineCompletion);
    m_hostCombo->setCompleter(completer);
    hostLabel->setBuddy(m_hostCombo);
    m_startButton = new QPushButton(tr("&Start"), hostBox);
    m_startButton->setDefault(true);
    hostLayout->addWidget(hostLabel);
    hostLayout->addWidget(m_hostCombo, 1);
    hostLayout->addWidget(m_startButton);

    // Row 1, right: Options and Exit.
    auto *actionBox = new QGroupBox(central);
    auto *actionLayout = new QHBoxLayout(actionBox);
    m_optionsButton = new QPushButton(tr("&Options"), actionBox);
    auto *exitButton = new QPushButton(tr("E&xit"), actionBox);
    actionLayout->addWidget(m_optionsButton);
    actionLayout->addWidget(exitButton);

    auto *row1 = new QHBoxLayout;
    row1->addWidget(hostBox, 1);
    row1->addWidget(actionBox);

    // Row 2: clipboard and export.
    auto *reportBox = new QGroupBox(central);
    auto *reportLayout = new QHBoxLayout(reportBox);
    auto *copyTextButton = new QPushButton(tr("&Copy Text to clipboard"), reportBox);
    auto *copyHtmlButton = new QPushButton(tr("Co&py HTML to clipboard"), reportBox);
    auto *exportTextButton = new QPushButton(tr("Export &TEXT"), reportBox);
    auto *exportHtmlButton = new QPushButton(tr("Export &HTML"), reportBox);
    reportLayout->addWidget(copyTextButton);
    reportLayout->addWidget(copyHtmlButton);
    reportLayout->addStretch(1);
    reportLayout->addWidget(exportTextButton);
    reportLayout->addWidget(exportHtmlButton);

    m_model = new HopTableModel(this);
    m_table = new QTableView(central);
    m_table->setModel(m_model);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSortingEnabled(false);
    m_table->setShowGrid(false);
    m_table->setWordWrap(false);
    m_table->setTabKeyNavigation(false);
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setDefaultSectionSize(m_table->fontMetrics().height() + 6);
    QHeaderView *header = m_table->horizontalHeader();
    header->setSectionsClickable(false);
    header->setHighlightSections(false);
    header->setStretchLastSection(false);
    header->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    for (int c = 0; c < HopTableModel::ColumnCount; ++c)
        header->resizeSection(c, HopTableModel::defaultColumnWidth(c));
    header->setSectionResizeMode(HopTableModel::ColHostname, QHeaderView::Stretch);

    // Hop copy actions (upstream issue #10). The shortcut applies only to the
    // table, so Ctrl+C in the host box still copies the typed text.
    m_copyHostAction = new QAction(tr("Copy hostname"), m_table);
    m_copyHostAction->setShortcut(QKeySequence::Copy);
    m_copyHostAction->setShortcutContext(Qt::WidgetShortcut);
    m_table->addAction(m_copyHostAction);
    m_copyAddressAction = new QAction(tr("Copy IP address"), m_table);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);

    auto *root = new QVBoxLayout(central);
    root->addLayout(row1);
    root->addWidget(reportBox);
    root->addWidget(m_table, 1);
    setCentralWidget(central);

    // Status bar: stretching message pane plus the permanent hint.
    m_statusLabel = new QLabel(tr("Ready"), this);
    statusBar()->addWidget(m_statusLabel, 1);
    statusBar()->addPermanentWidget(new QLabel(tr("Double click on a hop for details"), this));

    setMinimumSize(600, 250); // upstream OnSizing limits
    resize(900, 480);

    connect(m_startButton, &QPushButton::clicked, this, &MainWindow::onStartStopClicked);
    connect(m_hostCombo->lineEdit(), &QLineEdit::returnPressed, this, &MainWindow::startFromHostBox);
    connect(m_hostCombo, &QComboBox::activated, this, &MainWindow::onHostActivated);
    connect(m_optionsButton, &QPushButton::clicked, this, &MainWindow::onOptionsClicked);
    connect(exitButton, &QPushButton::clicked, this, &QWidget::close);
    connect(copyTextButton, &QPushButton::clicked, this, &MainWindow::copyText);
    connect(copyHtmlButton, &QPushButton::clicked, this, &MainWindow::copyHtml);
    connect(exportTextButton, &QPushButton::clicked, this, &MainWindow::exportText);
    connect(exportHtmlButton, &QPushButton::clicked, this, &MainWindow::exportHtml);
    connect(m_table, &QAbstractItemView::doubleClicked, this, &MainWindow::onRowDoubleClicked);
    connect(m_table, &QWidget::customContextMenuRequested, this, &MainWindow::onTableContextMenu);
    connect(m_copyHostAction, &QAction::triggered, this, &MainWindow::copySelectedHost);
    connect(m_copyAddressAction, &QAction::triggered, this, &MainWindow::copySelectedAddress);
}

void MainWindow::loadSettings()
{
    const QSettings s;
    const TraceOptions def;

    m_options.intervalSec = readDouble(s, kKeyInterval, def.intervalSec,
                                       OptionsDialog::kIntervalMin, OptionsDialog::kIntervalMax);
    m_options.pingSizeBytes = readInt(s, kKeyPingSize, def.pingSizeBytes,
                                      OptionsDialog::kPingSizeMin, OptionsDialog::kPingSizeMax);
    m_maxLru = readInt(s, kKeyMaxLru, OptionsDialog::kDefaultMaxLru,
                       OptionsDialog::kMaxLruMin, OptionsDialog::kMaxLruMax);
    m_options.resolveNames = s.value(kKeyResolveNames, def.resolveNames).toBool();
    m_showIps = s.value(kKeyShowIps, OptionsDialog::kDefaultShowIps).toBool();
    m_options.timeoutSec = readDouble(s, kKeyTimeout, def.timeoutSec,
                                      OptionsDialog::kTimeoutMin, OptionsDialog::kTimeoutMax);
    m_options.maxHops = readInt(s, kKeyMaxHops, def.maxHops,
                                OptionsDialog::kMaxHopsMin, OptionsDialog::kMaxHopsMax);

    const QString protocol = s.value(kKeyProtocol).toString().trimmed();
    if (protocol.compare(QLatin1String("udp"), Qt::CaseInsensitive) == 0)
        m_options.protocol = TraceOptions::Protocol::Udp;
    else if (protocol.compare(QLatin1String("tcp"), Qt::CaseInsensitive) == 0)
        m_options.protocol = TraceOptions::Protocol::Tcp;
    else
        m_options.protocol = TraceOptions::Protocol::Icmp;
    m_options.tcpPort = readInt(s, kKeyTcpPort, def.tcpPort,
                                OptionsDialog::kTcpPortMin, OptionsDialog::kTcpPortMax);

    const QString ipVersion = s.value(kKeyIpVersion).toString().trimmed();
    if (ipVersion.compare(QLatin1String("ipv4"), Qt::CaseInsensitive) == 0)
        m_options.ipVersion = TraceOptions::IpVersion::V4;
    else if (ipVersion.compare(QLatin1String("ipv6"), Qt::CaseInsensitive) == 0)
        m_options.ipVersion = TraceOptions::IpVersion::V6;
    else
        m_options.ipVersion = TraceOptions::IpVersion::Auto;

    m_lru.clear();
    const QStringList stored = s.value(kKeyLruHosts).toStringList();
    for (const QString &raw : stored) {
        const QString host = raw.trimmed().left(255);
        if (host.isEmpty() || host.compare(kClearHistory, Qt::CaseInsensitive) == 0
            || m_lru.contains(host, Qt::CaseInsensitive))
            continue;
        m_lru.append(host);
        if (m_lru.size() >= m_maxLru)
            break;
    }
    rebuildHostCombo();

    m_model->setResolveNames(m_options.resolveNames);
    m_model->setShowIps(m_showIps);

    restoreGeometry(s.value(kKeyGeometry).toByteArray());
}

void MainWindow::saveOptions() const
{
    QSettings s;
    s.setValue(kKeyInterval, m_options.intervalSec);
    s.setValue(kKeyPingSize, m_options.pingSizeBytes);
    s.setValue(kKeyMaxLru, m_maxLru);
    s.setValue(kKeyResolveNames, m_options.resolveNames);
    s.setValue(kKeyShowIps, m_showIps);
    s.setValue(kKeyTimeout, m_options.timeoutSec);
    s.setValue(kKeyMaxHops, m_options.maxHops);
    s.setValue(kKeyProtocol, protocolKey(m_options.protocol));
    s.setValue(kKeyTcpPort, m_options.tcpPort);
    s.setValue(kKeyIpVersion, ipVersionKey(m_options.ipVersion));
}

void MainWindow::saveLru() const
{
    QSettings s;
    s.setValue(kKeyLruHosts, m_lru);
}

void MainWindow::rebuildHostCombo()
{
    const QString text = m_hostCombo->currentText();
    {
        const QSignalBlocker blocker(m_hostCombo);
        m_hostCombo->clear();
        m_hostCombo->addItems(m_lru);
        m_hostCombo->addItem(kClearHistory);
        m_hostCombo->setCurrentIndex(-1);
        m_hostCombo->setEditText(text);
    }
    m_completionModel->setStringList(m_lru);
}

void MainWindow::addToLru(const QString &host)
{
    // The sentinel is never history: it is always the combo's own last entry.
    if (host.isEmpty() || host.compare(kClearHistory, Qt::CaseInsensitive) == 0)
        return;
    m_lru.removeIf([&host](const QString &h) {
        return h.compare(host, Qt::CaseInsensitive) == 0;
    });
    m_lru.prepend(host);
    while (m_lru.size() > m_maxLru)
        m_lru.removeLast();
    saveLru();
    rebuildHostCombo();
}

void MainWindow::clearHistory()
{
    m_lru.clear();
    saveLru();
    rebuildHostCombo();
    m_hostCombo->clearEditText();
    m_hostCombo->setFocus();
}

void MainWindow::setStatusText(const QString &text)
{
    m_statusLabel->setText(text);
}

void MainWindow::setTraceOptions(const TraceOptions &options)
{
    m_options = options;
    m_model->setResolveNames(options.resolveNames);
}

void MainWindow::setShowIps(bool show)
{
    m_showIps = show;
    m_model->setShowIps(show);
}

void MainWindow::startTrace(const QString &host)
{
    // Stop before beginTrace() sets its pending state, so that the Idle signal
    // of the previous trace cannot clear that state.
    m_session->stop();
    // A host from the program (the command line) is always a trace target,
    // also when it reads "Clear History". Only the host box clears the history.
    beginTrace(host.trimmed());
}

void MainWindow::startFromHostBox()
{
    if (m_session->state() != TraceSession::State::Idle)
        return;

    const QString host = m_hostCombo->currentText().trimmed();
    if (host.compare(kClearHistory, Qt::CaseInsensitive) == 0) {
        clearHistory();
        return;
    }
    beginTrace(host);
}

void MainWindow::beginTrace(const QString &host)
{
    if (host.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("LinMTR"), tr("No host specified!"));
        m_hostCombo->setFocus();
        return;
    }

    m_hostCombo->setEditText(host);
    // Upstream adds the host to the history only after it resolved
    // (OnRestart: InitMTRNet()), so a typo never enters it: see onStateChanged().
    m_pendingLruHost = host;
    m_model->clear();
    m_errorShown = false;
    m_currentHost = host;
    statusBar()->clearMessage();
    m_session->start(host, m_options);
}

void MainWindow::onStartStopClicked()
{
    if (m_session->state() != TraceSession::State::Idle)
        m_session->stop();
    else
        startFromHostBox();
}

void MainWindow::onHostActivated(int index)
{
    if (index < 0 || index != m_hostCombo->count() - 1)
        return;
    // Defer the call: the combo box is still inside its own activation handler.
    QTimer::singleShot(0, this, &MainWindow::clearHistory);
}

void MainWindow::onOptionsClicked()
{
    OptionsDialog dlg(m_options, m_maxLru, m_showIps, this);
    if (dlg.exec() != QDialog::Accepted)
        return;

    m_options = dlg.options();
    m_maxLru = dlg.maxLru();
    m_model->setResolveNames(m_options.resolveNames);
    setShowIps(dlg.showIps());
    saveOptions();

    if (m_lru.size() > m_maxLru) {
        m_lru = m_lru.mid(0, m_maxLru);
        saveLru();
        rebuildHostCombo();
    }
}

void MainWindow::onStateChanged(TraceSession::State state)
{
    const bool idle = state == TraceSession::State::Idle;
    m_startButton->setText(idle ? tr("&Start") : tr("&Stop"));
    m_hostCombo->setEnabled(idle);
    m_optionsButton->setEnabled(idle);

    switch (state) {
    case TraceSession::State::Idle: {
        m_pendingLruHost.clear();   // stopped or failed before the host resolved
        if (!m_errorShown)
            setStatusText(tr("Ready"));
        const QList<HopStats> hops = m_session->hops();
        if (!hops.isEmpty())
            m_model->setHops(hops);
        m_hostCombo->setFocus();
        break;
    }
    case TraceSession::State::Resolving:
        setStatusText(tr("Resolving host %1...").arg(m_currentHost));
        break;
    case TraceSession::State::Tracing:
        setStatusText(tr("Tracing %1").arg(m_currentHost));
        if (!m_pendingLruHost.isEmpty()) {
            // The host resolved: now it goes into the history.
            const QString host = m_pendingLruHost;
            m_pendingLruHost.clear();
            addToLru(host);
        }
        break;
    }
}

void MainWindow::onHopsChanged()
{
    const QList<HopStats> hops = m_session->hops();
    // Keep the last results on screen when a late notification arrives and
    // the idle session has no hops.
    if (hops.isEmpty() && m_session->state() == TraceSession::State::Idle)
        return;
    m_model->setHops(hops);
}

void MainWindow::onErrorOccurred(const QString &message)
{
    m_errorShown = true;
    statusBar()->clearMessage();
    setStatusText(message);
}

void MainWindow::onInfoMessage(const QString &message)
{
    statusBar()->showMessage(message, 10000);
}

void MainWindow::onRowDoubleClicked(const QModelIndex &index)
{
    if (!index.isValid() || index.row() >= m_model->rowCount())
        return;
    HopPropertiesDialog dlg(m_model->hopAt(index.row()), m_model->resolveNames(),
                            m_model->showIps(), this);
    dlg.exec();
}

void MainWindow::onTableContextMenu(const QPoint &pos)
{
    // pos is in viewport coordinates.
    const QModelIndex index = m_table->indexAt(pos);
    if (!index.isValid() || index.row() >= m_model->rowCount())
        return;
    m_table->selectionModel()->setCurrentIndex(
        index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    m_copyAddressAction->setEnabled(!m_model->hopAt(index.row()).address.isNull());

    QMenu menu(this);
    menu.addAction(m_copyHostAction);
    menu.addAction(m_copyAddressAction);
    menu.exec(m_table->viewport()->mapToGlobal(pos));
}

int MainWindow::selectedHopRow() const
{
    const QModelIndexList rows = m_table->selectionModel()->selectedRows();
    if (rows.isEmpty() || rows.constFirst().row() >= m_model->rowCount())
        return -1;
    return rows.constFirst().row();
}

void MainWindow::copySelectedHost()
{
    const int row = selectedHopRow();
    if (row < 0)
        return;
    QGuiApplication::clipboard()->setText(
        m_model->hopAt(row).displayHost(m_model->resolveNames(), m_model->showIps()));
}

void MainWindow::copySelectedAddress()
{
    const int row = selectedHopRow();
    if (row < 0)
        return;
    // The table refreshes while the menu is open; check the address again.
    const HopStats hop = m_model->hopAt(row);
    if (hop.address.isNull())
        return;
    QGuiApplication::clipboard()->setText(hop.address.toString());
}

void MainWindow::copyText()
{
    QGuiApplication::clipboard()->setText(
        ReportBuilder::text(m_model->hops(), m_model->resolveNames(), m_model->showIps()));
}

void MainWindow::copyHtml()
{
    const QString html =
        ReportBuilder::html(m_model->hops(), m_model->resolveNames(), m_model->showIps());
    auto *mime = new QMimeData;
    mime->setHtml(html);
    mime->setText(html); // upstream puts the HTML source on the plain-text clipboard
    QGuiApplication::clipboard()->setMimeData(mime);
}

void MainWindow::exportText()
{
    exportReport(false);
}

void MainWindow::exportHtml()
{
    exportReport(true);
}

void MainWindow::exportReport(bool html)
{
    const QString title = html ? tr("Export HTML") : tr("Export TEXT");
    const QString filter = html ? tr("HTML Files (*.htm *.html)") : tr("Text Files (*.txt)");
    const QString suffix = html ? QStringLiteral("html") : QStringLiteral("txt");

    QString selectedFilter = filter;
    QString path = QFileDialog::getSaveFileName(this, title, QDir::homePath(),
                                                filter + QStringLiteral(";;") + tr("All Files (*)"),
                                                &selectedFilter);
    if (path.isEmpty())
        return;

    // The upstream save dialog appends the default extension. Native dialogs
    // possibly do not, so add it here and ask before the export replaces a file.
    if (QFileInfo(path).suffix().isEmpty() && selectedFilter == filter) {
        path += QLatin1Char('.') + suffix;
        if (QFileInfo::exists(path)
            && QMessageBox::question(this, title,
                                     tr("%1 already exists.\nDo you want to replace it?")
                                         .arg(QDir::toNativeSeparators(path)))
                   != QMessageBox::Yes)
            return;
    }

    // Build after the dialog closes, so that the export has the newest data.
    const QString report = html
        ? ReportBuilder::html(m_model->hops(), m_model->resolveNames(), m_model->showIps())
        : ReportBuilder::text(m_model->hops(), m_model->resolveNames(), m_model->showIps());

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(report.toUtf8()) < 0 || !file.commit()) {
        QMessageBox::warning(this, title,
                             tr("Unable to write %1:\n%2")
                                 .arg(QDir::toNativeSeparators(path), file.errorString()));
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    m_session->stop();
    QSettings s;
    s.setValue(kKeyGeometry, saveGeometry());
    QMainWindow::closeEvent(event);
}

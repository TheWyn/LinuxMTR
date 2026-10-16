// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Wyn

#pragma once

#include <QMainWindow>
#include <QStringList>

#include "core/TraceOptions.h"
#include "core/TraceSession.h"

class QAction;
class QComboBox;
class QCompleter;
class QLabel;
class QModelIndex;
class QPoint;
class QPushButton;
class QSplitter;
class QStringListModel;
class QTableView;
class HopGraphWidget;
class HopTableModel;

// The WinMTR main dialog as a Qt main window. TraceSession signals drive all
// refreshes; there is no polling timer.
class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    // Options for the next trace (read from QSettings at start).
    TraceOptions traceOptions() const { return m_options; }
    // Applies options without saving them (command-line overrides).
    void setTraceOptions(const TraceOptions &options);

    // Sets "Show IP next to hostname" and does not save it (command line).
    void setShowIps(bool show);

    // Puts host in the host box and starts tracing it. It stops an active trace
    // first. The host is always a trace target, never the host box's "Clear
    // History" command.
    void startTrace(const QString &host);

    // Returns the status text of a running trace: "Tracing <host> (<address>)".
    // The text has no address when address is null, or when host is an IP
    // address in a form that TraceSession::start() accepts (also in brackets).
    static QString tracingText(const QString &host, const QHostAddress &address);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void buildUi();
    void loadSettings();
    void saveOptions() const;
    void saveLru() const;
    void rebuildHostCombo();
    void addToLru(const QString &host);
    void clearHistory();
    void setStatusText(const QString &text);
    // Shows or hides the route graph under the table; does not save it.
    void setShowGraph(bool show);
    void refreshGraph();
    void startFromHostBox();
    void beginTrace(const QString &host);

    void onStartStopClicked();
    void onHostActivated(int index);
    void onOptionsClicked();
    void onStateChanged(TraceSession::State state);
    void onHopsChanged();
    void onErrorOccurred(const QString &message);
    void onInfoMessage(const QString &message);
    void onRowDoubleClicked(const QModelIndex &index);
    void onTableContextMenu(const QPoint &pos);

    // Hop copy actions (upstream issue #10); they act on the selected row.
    int selectedHopRow() const;
    void copySelectedHost();
    void copySelectedAddress();

    void copyText();
    void copyHtml();
    void exportText();
    void exportHtml();
    void exportReport(bool html);

    TraceSession *m_session = nullptr;
    HopTableModel *m_model = nullptr;

    QComboBox *m_hostCombo = nullptr;
    QStringListModel *m_completionModel = nullptr;
    QPushButton *m_startButton = nullptr;
    QPushButton *m_optionsButton = nullptr;
    QSplitter *m_splitter = nullptr;
    QTableView *m_table = nullptr;
    HopGraphWidget *m_graph = nullptr;
    QAction *m_copyHostAction = nullptr;
    QAction *m_copyAddressAction = nullptr;
    QLabel *m_statusLabel = nullptr;

    TraceOptions m_options;
    int m_maxLru = 128;
    bool m_showIps = true;
    bool m_showGraph = false;
    QStringList m_lru;
    QString m_pendingLruHost;   // host of the trace that has not resolved yet
    QString m_currentHost;
    bool m_errorShown = false;
};

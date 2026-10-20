// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Wyn

#pragma once

#include <QDialog>

class QLineEdit;
class HopGraphWidget;
class HopTableModel;

// WinMTR "Host properties" dialog for one hop, with a graph of its round-trip
// times. It follows the hop by TTL while the model changes; when no row has
// that TTL, the comment says so and the other fields keep the last values. The
// address has its own field, so the name field never repeats it.
class HopPropertiesDialog : public QDialog
{
    Q_OBJECT
public:
    HopPropertiesDialog(HopTableModel *model, int row, QWidget *parent = nullptr);

private:
    void refresh();

    HopTableModel *m_model = nullptr;
    int m_ttl = 0;
    QLineEdit *m_name = nullptr;
    QLineEdit *m_ip = nullptr;
    QLineEdit *m_comment = nullptr;
    QLineEdit *m_sent = nullptr;
    QLineEdit *m_received = nullptr;
    QLineEdit *m_loss = nullptr;
    QLineEdit *m_last = nullptr;
    QLineEdit *m_best = nullptr;
    QLineEdit *m_average = nullptr;
    QLineEdit *m_worst = nullptr;
    QLineEdit *m_jitter = nullptr;
    HopGraphWidget *m_graph = nullptr;
};

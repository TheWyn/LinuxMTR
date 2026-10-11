// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Wyn

#pragma once

#include <QDialog>

#include "core/HopStats.h"

// WinMTR "Host properties" dialog: a static, read-only snapshot of one hop.
// The address has its own field, so the name field never repeats it.
class HopPropertiesDialog : public QDialog
{
    Q_OBJECT
public:
    HopPropertiesDialog(const HopStats &hop, bool resolveNames, bool showIps,
                        QWidget *parent = nullptr);
};

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "util/background_tasks.h"

BackgroundTasks &BackgroundTasks::instance() {
    static BackgroundTasks inst;
    return inst;
}

int BackgroundTasks::begin() {
    const int id = _nextId++;
    _active.insert(id);
    emit countChanged(count());
    return id;
}

void BackgroundTasks::end(int id) {
    if (_active.remove(id))
        emit countChanged(count());
}

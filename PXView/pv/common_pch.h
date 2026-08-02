/*
 * Precompiled Header for PXView
 *
 * This header is precompiled by CMake's target_precompile_headers() and
 * included automatically at the beginning of every .cpp file in the
 * PXView target. It contains the most frequently included headers to
 * eliminate redundant parsing across compilation units.
 *
 * Statistics (include count across 363 source files):
 *   <vector>      66    <algorithm>   38    <cstdint>    33
 *   <map>         28    <memory>      27    <string>     23
 *   <QString>     51    <QObject>     32    <QTimer>     33
 *   "log.h"       68    "pxvdef.h"    54    "appconfig.h" 63
 *
 * DO NOT add headers that are only used by a few files — that wastes
 * memory. Only add headers that appear in 25+ compilation units.
 */

#pragma once

// --- STL headers (most frequently included) ---
#include <vector>
#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <list>
#include <mutex>
#include <atomic>
#include <functional>
#include <utility>

// --- Qt Core headers (most frequently included) ---
#include <QString>
#include <QObject>
#include <QTimer>
#include <QVariant>
#include <QDebug>

// --- Project lightweight headers (relative to PXView/ include root) ---
#include "pv/log.h"
#include "pv/pxvdef.h"

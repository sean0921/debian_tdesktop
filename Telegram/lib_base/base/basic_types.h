// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#pragma once

#include "base/build_config.h"
#include "base/ordered_set.h"
#include "base/unique_function.h"
#include "base/functors.h"

#include <QtGlobal>
#include <QtCore/QByteArray>

#include <string>
#include <exception>
#include <memory>
#include <ctime>
#include <functional>
#include <gsl/gsl>

namespace func = base::functors;

using gsl::not_null;
using index_type = gsl::index;
using size_type = gsl::index;

template <typename Signature>
using Fn = std::function<Signature>;

template <typename Signature>
using FnMut = base::unique_function<Signature>;

//using uchar = unsigned char; // Qt has uchar
using int8 = qint8;
using uint8 = quint8;
using int16 = qint16;
using uint16 = quint16;
using int32 = qint32;
using uint32 = quint32;
using int64 = qint64;
using uint64 = quint64;
using float32 = float;
using float64 = double;

using TimeId = int32;

#define MACRO_TO_STRING_HELPER(V) #V
#define MACRO_TO_STRING(V) MACRO_TO_STRING_HELPER(V)

// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "ui/platform/linux/ui_window_linux.h"

namespace Ui {
namespace Platform {

std::unique_ptr<BasicWindowHelper> CreateWindowHelper(
		not_null<RpWidget*> window) {
	return nullptr;
}

} // namespace Platform
} // namespace Ui

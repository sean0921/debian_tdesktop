// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "ui/widgets/window.h"

#include "ui/platform/ui_platform_window.h"

namespace Ui {

Window::Window(QWidget *parent)
: RpWidget(parent)
, _helper(Platform::CreateWindowHelper(this)) {
	hide();
}

Window::~Window() = default;

not_null<RpWidget*> Window::body() {
	return _helper ? _helper->body() : this;
}

not_null<const RpWidget*> Window::body() const {
	return _helper ? _helper->body().get() : this;
}

void Window::setTitle(const QString &title) {
	if (_helper) {
		_helper->setTitle(title);
	} else {
		setWindowTitle(title);
	}
}

void Window::setTitleStyle(const style::WindowTitle &st) {
	if (_helper) {
		_helper->setTitleStyle(st);
	}
}

void Window::setMinimumSize(QSize size) {
	if (_helper) {
		_helper->setMinimumSize(size);
	} else {
		RpWidget::setMinimumSize(size);
	}
}

void Window::setFixedSize(QSize size) {
	if (_helper) {
		_helper->setFixedSize(size);
	} else {
		RpWidget::setFixedSize(size);
	}
}

void Window::setGeometry(QRect rect) {
	if (_helper) {
		_helper->setGeometry(rect);
	} else {
		RpWidget::setGeometry(rect);
	}
}

} // namespace Ui

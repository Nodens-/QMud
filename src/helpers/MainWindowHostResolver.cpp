/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: MainWindowHostResolver.cpp
 * Role: Resolution logic that maps current UI context to the appropriate MainWindowHost for command/runtime operations.
 */

#include "MainWindowHostResolver.h"

#include "MainWindowHost.h"
#include "WorldRuntime.h"

#include <QApplication>
#include <QWidget>

namespace
{
	MainWindowHost *hostFromObject(QObject *object)
	{
		if (!object)
			return nullptr;
		return dynamic_cast<MainWindowHost *>(object);
	}

	MainWindowHost *hostFromWidgetTree(QWidget *widget)
	{
		if (!widget)
			return nullptr;
		if (MainWindowHost *host = hostFromObject(widget))
			return host;
		QWidget *top = widget->window();
		if (!top || top == widget)
			return nullptr;
		return hostFromObject(top);
	}
} // namespace

MainWindowHost *resolveMainWindowHost(QObject *context)
{
	for (QObject *current = context; current; current = current->parent())
	{
		if (MainWindowHost *host = hostFromObject(current))
			return host;
		if (QWidget *widget = qobject_cast<QWidget *>(current))
		{
			if (MainWindowHost *host = hostFromWidgetTree(widget))
				return host;
		}
	}

	if (MainWindowHost *host = hostFromWidgetTree(QApplication::activeWindow()))
		return host;

	const QList<QWidget *> windows = QApplication::topLevelWidgets();
	for (QWidget *window : windows)
	{
		if (MainWindowHost *host = hostFromWidgetTree(window))
			return host;
	}

	return nullptr;
}

MainWindowHost *resolveMainWindowHostForRuntime(const WorldRuntime *runtime)
{
	if (!runtime)
		return nullptr;
	return resolveMainWindowHost(runtime->parent());
}

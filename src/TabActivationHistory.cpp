/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: TabActivationHistory.cpp
 * Role: Activation-history implementation for selecting prior active tab after close.
 */

#include "TabActivationHistory.h"

#include <QMdiSubWindow>

void TabActivationHistory::pruneHistory(const QList<QMdiSubWindow *> &currentWindows)
{
	for (qsizetype i = m_activationHistory.size(); i-- > 0;)
	{
		QPointer<QMdiSubWindow> sub = m_activationHistory.at(i);
		if (!sub || !currentWindows.contains(sub))
			m_activationHistory.removeAt(i);
	}
}

void TabActivationHistory::onActivated(QMdiSubWindow *window, const QList<QMdiSubWindow *> &currentWindows)
{
	pruneHistory(currentWindows);
	if (!window)
		return;
	for (qsizetype i = m_activationHistory.size(); i-- > 0;)
	{
		QPointer<QMdiSubWindow> sub = m_activationHistory.at(i);
		if (!sub || sub == window)
			m_activationHistory.removeAt(i);
	}
	m_activationHistory.push_back(window);
}

void TabActivationHistory::onCloseEvent(QMdiSubWindow *closingSubWindow, const QMdiSubWindow *activeSubWindow,
                                        const QList<QMdiSubWindow *> &currentWindows)
{
	pruneHistory(currentWindows);
	if (!closingSubWindow || closingSubWindow != activeSubWindow)
	{
		if (closingSubWindow)
			m_closeFallbackByWindow.remove(closingSubWindow);
		return;
	}

	QPointer<QMdiSubWindow> fallback;
	for (qsizetype i = m_activationHistory.size(); i-- > 0;)
	{
		QMdiSubWindow *sub = m_activationHistory.at(i);
		if (!sub || sub == closingSubWindow || !currentWindows.contains(sub))
			continue;
		fallback = sub;
		break;
	}

	if (fallback)
		m_closeFallbackByWindow.insert(closingSubWindow, fallback);
	else
		m_closeFallbackByWindow.remove(closingSubWindow);
}

QPointer<QMdiSubWindow>
TabActivationHistory::takeFallbackOnDestroyed(QObject                      *destroyedObject,
                                              const QList<QMdiSubWindow *> &currentWindows)
{
	for (qsizetype i = m_activationHistory.size(); i-- > 0;)
	{
		QPointer<QMdiSubWindow> sub = m_activationHistory.at(i);
		if (!sub || sub.data() == destroyedObject || !currentWindows.contains(sub))
			m_activationHistory.removeAt(i);
	}

	QPointer<QMdiSubWindow> fallback = m_closeFallbackByWindow.take(destroyedObject);
	if (!fallback || !currentWindows.contains(fallback))
		return nullptr;
	return fallback;
}

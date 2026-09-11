/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: GuiSystemUtils.cpp
 * Role: Shared GUI/system snapshot helpers for Lua-compatible API values.
 */

#include "GuiSystemUtils.h"

#include "ColorPacking.h"

#include <QApplication>
#include <QColor>
#include <QFont>
#include <QGuiApplication>
#include <QPalette>
#include <QRect>
#include <QScreen>
#include <QStyleHints>
#include <QtMath>

#include <limits>

QString qmudGuiSystemValueKey(const QString &category, const int index)
{
	return QStringLiteral("%1:%2").arg(category, QString::number(index));
}

long qmudGuiSystemColor(const int index)
{
	const QPalette palette = QGuiApplication::palette();
	QColor         color;
	switch (index)
	{
	case 0:
		color = palette.color(QPalette::Mid);
		break;
	case 1:
	case 5:
		color = palette.color(QPalette::Window);
		break;
	case 2:
	case 13:
		color = palette.color(QPalette::Highlight);
		break;
	case 3:
	case 10:
	case 21:
		color = palette.color(QPalette::Dark);
		break;
	case 4:
	case 15:
		color = palette.color(QPalette::Button);
		break;
	case 6:
	case 16:
		color = palette.color(QPalette::Shadow);
		break;
	case 7:
	case 18:
		color = palette.color(QPalette::ButtonText);
		break;
	case 8:
	case 19:
		color = palette.color(QPalette::WindowText);
		break;
	case 9:
	case 14:
		color = palette.color(QPalette::HighlightedText);
		break;
	case 11:
	case 17:
		color = palette.color(QPalette::Mid);
		break;
	case 12:
		color = palette.color(QPalette::Base);
		break;
	case 20:
	case 22:
		color = palette.color(QPalette::Light);
		break;
	case 23:
		color = palette.color(QPalette::ToolTipText);
		break;
	case 24:
		color = palette.color(QPalette::ToolTipBase);
		break;
	default:
		return 0;
	}
	return static_cast<long>(qmudRgb(color.red(), color.green(), color.blue()));
}

QVariantMap qmudCollectGuiSystemValues()
{
	QVariantMap values;
	auto        setDeviceCap = [&values](const int index, const double value)
	{ values.insert(qmudGuiSystemValueKey(QStringLiteral("device"), index), value); };
	auto setSystemMetric = [&values](const int index, const double value)
	{ values.insert(qmudGuiSystemValueKey(QStringLiteral("metric"), index), value); };
	auto setSystemColor = [&values](const int index, const long value)
	{ values.insert(qmudGuiSystemValueKey(QStringLiteral("syscolor"), index), QVariant::fromValue(value)); };

	if (QScreen *screen = QGuiApplication::primaryScreen(); screen)
	{
		QRect                  virtualGeom;
		const QList<QScreen *> screens = QGuiApplication::screens();
		for (QScreen *candidate : screens)
			virtualGeom = virtualGeom.united(candidate->geometry());
		if (virtualGeom.isNull())
			virtualGeom = screen->geometry();

		const QRect geom  = screen->geometry();
		const QRect avail = screen->availableGeometry();
		setDeviceCap(2, 1);
		setDeviceCap(8, geom.width());
		setDeviceCap(4, screen->physicalSize().width());
		setDeviceCap(10, geom.height());
		setDeviceCap(6, screen->physicalSize().height());
		setDeviceCap(12, screen->depth());
		setDeviceCap(14, 1);
		setDeviceCap(24, screen->depth() > 8 ? -1 : 1 << qMax(0, screen->depth()));
		setDeviceCap(88, screen->logicalDotsPerInchX());
		setDeviceCap(90, screen->logicalDotsPerInchY());
		setDeviceCap(40, geom.width());
		setDeviceCap(42, geom.height());
		setDeviceCap(44, qSqrt(static_cast<double>(geom.width()) * geom.width() +
		                       static_cast<double>(geom.height()) * geom.height()));
		setDeviceCap(116, screen->refreshRate());
		setDeviceCap(117, virtualGeom.height());
		setDeviceCap(118, virtualGeom.width());

		setSystemMetric(0, geom.width());
		setSystemMetric(1, geom.height());
		setSystemMetric(76, virtualGeom.x());
		setSystemMetric(77, virtualGeom.y());
		setSystemMetric(78, virtualGeom.width());
		setSystemMetric(79, virtualGeom.height());
		setSystemMetric(16, avail.width());
		setSystemMetric(17, avail.height());
		const QStyleHints *hints = QGuiApplication::styleHints();
		const int          drag  = hints ? hints->startDragDistance() : 4;
		setSystemMetric(68, drag > 0 ? drag : 4);
		setSystemMetric(69, drag > 0 ? drag : 4);
		setSystemMetric(75, 1);
		const int screenCount =
		    screens.isEmpty() ? 1
		                      : static_cast<int>(qMin(
		                            screens.size(), static_cast<qsizetype>(std::numeric_limits<int>::max())));
		setSystemMetric(80, screenCount);
		bool sameDisplayFormat = true;
		if (!screens.isEmpty())
		{
			const int depth = screens.first()->depth();
			for (qsizetype i = 1; i < screens.size(); ++i)
			{
				if (screens.at(i)->depth() != depth)
				{
					sameDisplayFormat = false;
					break;
				}
			}
		}
		setSystemMetric(81, sameDisplayFormat ? 1 : 0);
	}

	for (int index = 0; index <= 24; ++index)
		setSystemColor(index, qmudGuiSystemColor(index));

	long                        inputMask = 0;
	const Qt::KeyboardModifiers mods      = QGuiApplication::keyboardModifiers();
	if (mods & Qt::ShiftModifier)
		inputMask |= 0x01;
	if (mods & Qt::ControlModifier)
		inputMask |= 0x02;
	if (mods & Qt::AltModifier)
		inputMask |= 0x04;
	const Qt::MouseButtons buttons = QGuiApplication::mouseButtons();
	if (buttons & Qt::LeftButton)
		inputMask |= 0x10000;
	if (buttons & Qt::RightButton)
		inputMask |= 0x20000;
	if (buttons & Qt::MiddleButton)
		inputMask |= 0x40000;
	values.insert(QStringLiteral("inputMask"), QVariant::fromValue(inputMask));
	values.insert(QStringLiteral("menuFontSize"), QApplication::font("QMenu").pointSizeF());
	return values;
}

/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: AcceleratorUtils.h
 * Role: Helper APIs for normalizing key sequences and translating shortcut definitions into command-routing accelerator
 * data.
 */

#ifndef QMUD_ACCELERATORUTILS_H
#define QMUD_ACCELERATORUTILS_H

#include <QString>
#include <QtCore/Qt>
#include <QtGlobal>

namespace AcceleratorUtils
{

	constexpr quint32 kVirtKeyFlag  = 0x0001;
	constexpr quint32 kNoInvertFlag = 0x0000;
	constexpr quint32 kShiftFlag    = 0x0004;
	constexpr quint32 kControlFlag  = 0x0008;
	constexpr quint32 kAltFlag      = 0x0010;

	/**
	 * @brief Parses textual accelerator into virtual-key flags and key code.
	 * @param text Accelerator text.
	 * @param virt Output virtual-key flags.
	 * @param key Output virtual-key code.
	 * @return `true` when parse succeeds.
	 */
	bool              stringToAccelerator(const QString &text, quint32 &virt, quint16 &key);
	/**
	 * @brief Returns true when virtual key implies keypad modifier usage.
	 * @param key Virtual-key code.
	 * @return `true` when keypad modifier should be used.
	 */
	bool              virtualKeyUsesKeypadModifier(quint16 key);
	/**
	 * @brief Formats accelerator flags/key to display string.
	 * @param virt Virtual-key flags.
	 * @param key Virtual-key code.
	 * @return Formatted accelerator text.
	 */
	QString           acceleratorToString(quint32 virt, quint16 key);
	/**
	 * @brief Converts Windows-style virtual key to Qt key enum.
	 * @param key Virtual-key code.
	 * @return Equivalent Qt key.
	 */
	Qt::Key           virtualKeyToQtKey(quint16 key);
	/**
	 * @brief Converts Qt key enum to Windows-style virtual key.
	 * @param key Qt key.
	 * @param keypad Treat as keypad key when `true`.
	 * @return Equivalent virtual-key code.
	 */
	quint16           qtKeyToVirtualKey(Qt::Key key, bool keypad);

} // namespace AcceleratorUtils

#endif // QMUD_ACCELERATORUTILS_H

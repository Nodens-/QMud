/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: SpeedwalkParser.h
 * Role: Pure speedwalk parser/evaluator used by command processing and test seams.
 */

#ifndef QMUD_SPEEDWALKPARSER_H
#define QMUD_SPEEDWALKPARSER_H

#include <QString>
#include <functional>

namespace QMudSpeedwalk
{
	/**
	 * @brief Resolves one direction token to final command text.
	 */
	using DirectionResolver = std::function<QString(const QString &)>;

	/**
	 * @brief Formats user-facing speedwalk parse error string.
	 * @param message Parse failure detail.
	 * @return Localized/normalized error text.
	 */
	QString makeSpeedWalkErrorString(const QString &message);

	/**
	 * @brief Parses and expands speedwalk text into dispatch-ready command stream.
	 * @param speedWalkString Raw speedwalk expression.
	 * @param speedWalkFiller Command separator inserted between expanded steps.
	 * @param resolveDirection Direction resolver callback.
	 * @return Expanded command stream or error text.
	 */
	QString evaluateSpeedwalk(const QString &speedWalkString, const QString &speedWalkFiller,
	                          const DirectionResolver &resolveDirection);
} // namespace QMudSpeedwalk

#endif // QMUD_SPEEDWALKPARSER_H

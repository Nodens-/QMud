/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: TimeFormatUtils.h
 * Role: Shared world-time formatting helpers used by runtime and async restore paths.
 */

#ifndef QMUD_TIMEFORMATUTILS_H
#define QMUD_TIMEFORMATUTILS_H

#include <QDateTime>
#include <QString>

namespace TimeFormatUtils
{
	struct WorldTimeFormatContext
	{
			QString workingDir;
			QString worldName;
			QString playerName;
			QString worldDir;
			QString logDir;
	};

	using HtmlFixupFn = QString (*)(const QString &);

	[[nodiscard]] QString resolveWorkingDir(const QString &startupDir);
	[[nodiscard]] QString makeAbsolutePath(const QString &fileName, const QString &workingDir);
	[[nodiscard]] QString formatWorldTime(const QDateTime &time, const QString &format,
	                                      const WorldTimeFormatContext &context, bool fixHtml,
	                                      HtmlFixupFn fixupHtml);
} // namespace TimeFormatUtils

#endif // QMUD_TIMEFORMATUTILS_H

/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: FontUtils.cpp
 * Role: Font-selection helper implementations that enforce cross-platform monospace fallbacks for QMud editors and
 * views.
 */

#include "FontUtils.h"

#include <QFont>
#include <QStringList>

namespace
{
	QStringList monospaceFamilyPreference(const QString &preferredFamily)
	{
		QStringList   families;
		const QString preferred = preferredFamily.trimmed();
		if (!preferred.isEmpty())
			families << preferred;
		families << QStringLiteral("DejaVu Sans Mono") << QStringLiteral("Consolas")
		         << QStringLiteral("Menlo") << QStringLiteral("Monaco") << QStringLiteral("Courier New");
		families.removeDuplicates();
		return families;
	}
} // namespace

void qmudApplyMonospaceFallback(QFont &font, const QString &preferredFamily)
{
	const QString baseFamily = preferredFamily.isEmpty() ? font.family() : preferredFamily;
	font.setFamilies(monospaceFamilyPreference(baseFamily));
	font.setStyleHint(QFont::Monospace);
	font.setFixedPitch(true);
}

QFont qmudPreferredMonospaceFont(const QString &preferredFamily, const int pointSize)
{
	QFont font;
	qmudApplyMonospaceFallback(font, preferredFamily);
	if (pointSize > 0)
		font.setPointSize(pointSize);
	return font;
}

bool qmudMapWindowsCharsetToWritingSystem(const int charset, QFontDatabase::WritingSystem *outWritingSystem)
{
	if (!outWritingSystem)
		return false;

	switch (charset)
	{
	case 128:
		*outWritingSystem = QFontDatabase::Japanese;
		return true; // SHIFTJIS_CHARSET
	case 129:
		*outWritingSystem = QFontDatabase::Korean;
		return true; // HANGEUL_CHARSET / HANGUL_CHARSET
	case 134:
		*outWritingSystem = QFontDatabase::SimplifiedChinese;
		return true; // GB2312_CHARSET
	case 136:
		*outWritingSystem = QFontDatabase::TraditionalChinese;
		return true; // CHINESEBIG5_CHARSET
	case 161:
		*outWritingSystem = QFontDatabase::Greek;
		return true; // GREEK_CHARSET
	case 162:
		*outWritingSystem = QFontDatabase::Latin;
		return true; // TURKISH_CHARSET
	case 163:
		*outWritingSystem = QFontDatabase::Vietnamese;
		return true; // VIETNAMESE_CHARSET
	case 177:
		*outWritingSystem = QFontDatabase::Hebrew;
		return true; // HEBREW_CHARSET
	case 178:
		*outWritingSystem = QFontDatabase::Arabic;
		return true; // ARABIC_CHARSET
	case 186:
		*outWritingSystem = QFontDatabase::Latin;
		return true; // BALTIC_CHARSET
	case 204:
		*outWritingSystem = QFontDatabase::Cyrillic;
		return true; // RUSSIAN_CHARSET
	case 222:
		*outWritingSystem = QFontDatabase::Thai;
		return true; // THAI_CHARSET
	default:
		return false;
	}
}

QString qmudFamilyForCharset(const QString &preferredFamily, const int charset)
{
	QFontDatabase::WritingSystem writingSystem = QFontDatabase::Any;
	if (!qmudMapWindowsCharsetToWritingSystem(charset, &writingSystem))
		return preferredFamily;

	if (!preferredFamily.isEmpty())
	{
		const QList<QFontDatabase::WritingSystem> supported = QFontDatabase::writingSystems(preferredFamily);
		if (supported.contains(writingSystem))
			return preferredFamily;
	}

	const QStringList families = QFontDatabase::families(writingSystem);
	if (!families.isEmpty())
		return families.first();
	return preferredFamily;
}

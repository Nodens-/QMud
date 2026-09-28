/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: XmlImportResult.h
 * Role: Shared result payload for XML imports into world and plugin runtime state.
 */

#ifndef QMUD_XMLIMPORTRESULT_H
#define QMUD_XMLIMPORTRESULT_H

#include <QString>

/**
 * @brief Summarizes one XML import operation.
 */
struct XmlImportResult
{
		bool    ok{false};
		QString errorMessage;
		int     triggers{0};
		int     aliases{0};
		int     timers{0};
		int     macros{0};
		int     variables{0};
		int     colours{0};
		int     keypad{0};
		int     printing{0};
		int     duplicates{0};
};

#endif // QMUD_XMLIMPORTRESULT_H

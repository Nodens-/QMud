/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_StartTlsFallbackUtils.cpp
 * Role: QTest coverage for START-TLS timeout fallback guard decisions.
 */

#include "StartTlsFallbackUtils.h"

#include <QtTest/QTest>

/**
 * @brief QTest fixture covering START-TLS timeout fallback guard behavior.
 */
class tst_StartTlsFallbackUtils : public QObject
{
		Q_OBJECT

		// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		void fallbackAppliesOnlyWhenAllConditionsMatch()
		{
			constexpr StartTlsFallbackContext context = {12, 12, true, true, false, true, eTlsStartTls};
			QVERIFY(shouldFallbackToPlainOnStartTlsTimeout(context));
		}

		void fallbackDoesNotApplyWhenGenerationIsStale()
		{
			constexpr StartTlsFallbackContext context = {11, 12, true, true, false, true, eTlsStartTls};
			QVERIFY(!shouldFallbackToPlainOnStartTlsTimeout(context));
		}

		void fallbackDoesNotApplyWhenSocketIsUnavailableOrDisconnected()
		{
			constexpr StartTlsFallbackContext missingSocket = {12,    12,   false,       false,
			                                                   false, true, eTlsStartTls};
			QVERIFY(!shouldFallbackToPlainOnStartTlsTimeout(missingSocket));

			constexpr StartTlsFallbackContext disconnectedSocket = {12,    12,   true,        false,
			                                                        false, true, eTlsStartTls};
			QVERIFY(!shouldFallbackToPlainOnStartTlsTimeout(disconnectedSocket));
		}

		void fallbackDoesNotApplyWhenSessionAlreadyReady()
		{
			constexpr StartTlsFallbackContext context = {12, 12, true, true, true, true, eTlsStartTls};
			QVERIFY(!shouldFallbackToPlainOnStartTlsTimeout(context));
		}

		void fallbackDoesNotApplyWhenTlsStartTlsIsNotActiveMode()
		{
			constexpr StartTlsFallbackContext tlsDisabled = {12, 12, true, true, false, false, eTlsStartTls};
			QVERIFY(!shouldFallbackToPlainOnStartTlsTimeout(tlsDisabled));

			constexpr StartTlsFallbackContext nonStartTlsMethod = {12,    12,   true,      true,
			                                                       false, true, eTlsDirect};
			QVERIFY(!shouldFallbackToPlainOnStartTlsTimeout(nonStartTlsMethod));
		}
		// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_APPLESS_MAIN(tst_StartTlsFallbackUtils)

#if __has_include("tst_StartTlsFallbackUtils.moc")
#include "tst_StartTlsFallbackUtils.moc"
#endif

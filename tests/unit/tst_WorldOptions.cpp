/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_WorldOptions.cpp
 * Role: QTest coverage for WorldOptions behavior.
 */

#include "ColorPacking.h"
#include "WorldOptions.h"

#include <QtTest/QTest>

#include <cmath>

namespace
{
	/**
	 * @brief QTest fixture covering WorldOptions scenarios.
	 */
	class tst_WorldOptions : public QObject
	{
			Q_OBJECT

			// NOLINTBEGIN(readability-convert-member-functions-to-static)
		private slots:
			void numericTableSanity()
			{
				const WorldNumericOption *table = worldNumericOptions();
				const int                 count = worldNumericOptionCount();

				QVERIFY(table != nullptr);
				QVERIFY(count > 0);
				QVERIFY(table[count].name == nullptr);

				QSet<QString> names;
				for (int i = 0; table[i].name; ++i)
				{
					const QString name = QString::fromLatin1(table[i].name);
					QVERIFY(!names.contains(name));
					names.insert(name);
					QVERIFY(table[i].minValue <= table[i].maxValue);
				}

				QCOMPARE(names.size(), count);
			}

			void findNormalizesInput()
			{
				const WorldNumericOption *opt =
				    QMudWorldOptions::findWorldNumericOption(QStringLiteral("  WRAP_COLUMN "));
				QVERIFY(opt != nullptr);
				QCOMPARE(QString::fromLatin1(opt->name), QStringLiteral("wrap_column"));
				QCOMPARE(opt->minValue, 20LL);
				QCOMPARE(opt->maxValue, static_cast<long long>(MAX_LINE_WIDTH));
			}

			void regexpMatchEmptyOptionIsExposed()
			{
				const WorldNumericOption *opt =
				    QMudWorldOptions::findWorldNumericOption(QStringLiteral("regexp_match_empty"));
				QVERIFY(opt != nullptr);
				QCOMPARE(opt->defaultValue, 1LL);
				QCOMPARE(opt->minValue, 0LL);
				QCOMPARE(opt->maxValue, 0LL);
			}

			void sendKeepAlivesOptionIsBooleanAndDefaultOff()
			{
				const WorldNumericOption *opt =
				    QMudWorldOptions::findWorldNumericOption(QStringLiteral("send_keep_alives"));
				QVERIFY(opt != nullptr);
				QCOMPARE(opt->defaultValue, 0LL);
				QCOMPARE(opt->minValue, 0LL);
				QCOMPARE(opt->maxValue, 0LL);
			}

			void tlsDisableCertificateValidationOptionIsBooleanAndDefaultOff()
			{
				const WorldNumericOption *opt = QMudWorldOptions::findWorldNumericOption(
				    QStringLiteral("tls_disable_certificate_validation"));
				QVERIFY(opt != nullptr);
				QCOMPARE(opt->defaultValue, 0LL);
				QCOMPARE(opt->minValue, 0LL);
				QCOMPARE(opt->maxValue, 0LL);
			}

			void tabCompletionExcludesSymbolPrefixIsBooleanAndDefaultOn()
			{
				const WorldNumericOption *opt = QMudWorldOptions::findWorldNumericOption(
				    QStringLiteral("tab_completion_excludes_symbol_prefix"));
				QVERIFY(opt != nullptr);
				QCOMPARE(opt->defaultValue, 1LL);
				QCOMPARE(opt->minValue, 0LL);
				QCOMPARE(opt->maxValue, 0LL);
				QCOMPARE(opt->flags, 0);
			}

			void tabCompletionExcludesSymbolSuffixIsBooleanAndDefaultOn()
			{
				const WorldNumericOption *opt = QMudWorldOptions::findWorldNumericOption(
				    QStringLiteral("tab_completion_excludes_symbol_suffix"));
				QVERIFY(opt != nullptr);
				QCOMPARE(opt->defaultValue, 1LL);
				QCOMPARE(opt->minValue, 0LL);
				QCOMPARE(opt->maxValue, 0LL);
				QCOMPARE(opt->flags, 0);
			}

			void partialSaveCharacterThresholdHasRequestedRangeAndDefault()
			{
				const WorldNumericOption *opt = QMudWorldOptions::findWorldNumericOption(
				    QStringLiteral("partial_save_character_threshold"));
				QVERIFY(opt != nullptr);
				QCOMPARE(opt->defaultValue, 10LL);
				QCOMPARE(opt->minValue, 0LL);
				QCOMPARE(opt->maxValue, 200LL);
				QCOMPARE(opt->flags, 0);
				QCOMPARE(opt->binding, WorldNumericOptionBinding::PartialSaveCharacterThreshold);

				const WorldNumericOption *saveDeleted =
				    QMudWorldOptions::findWorldNumericOption(QStringLiteral("save_deleted_command"));
				QVERIFY(saveDeleted != nullptr);
				QCOMPARE(saveDeleted->binding, WorldNumericOptionBinding::SaveDeletedCommand);
			}

			void rgbOptionStorageNormalizesColorRefToInternalRgbText()
			{
				const WorldNumericOption *option =
				    QMudWorldOptions::findWorldNumericOption(QStringLiteral("hyperlink_colour"));
				QVERIFY(option != nullptr);
				QVERIFY(option->flags & OPT_RGB_COLOUR);

				constexpr auto colourRef = static_cast<long long>(qmudRgb(0x12, 0x34, 0x56));
				QCOMPARE(QMudWorldOptions::storedNumericOptionText(*option, colourRef),
				         QStringLiteral("#123456"));
			}

			void publicNumericValueConvertsInternalRepresentations()
			{
				const WorldNumericOption *rgb =
				    QMudWorldOptions::findWorldNumericOption(QStringLiteral("hyperlink_colour"));
				const WorldNumericOption *custom =
				    QMudWorldOptions::findWorldNumericOption(QStringLiteral("echo_colour"));
				const WorldNumericOption *boolean =
				    QMudWorldOptions::findWorldNumericOption(QStringLiteral("underline_hyperlinks"));
				QVERIFY(rgb);
				QVERIFY(custom);
				QVERIFY(boolean);

				QCOMPARE(QMudWorldOptions::publicNumericOptionValue(*rgb, QStringLiteral("#123456")),
				         std::optional<long long>(0x563412));
				QCOMPARE(QMudWorldOptions::publicNumericOptionValue(*rgb, QStringLiteral("5649426")),
				         std::optional<long long>(0x563412));
				QCOMPARE(QMudWorldOptions::publicNumericOptionValue(*custom, QStringLiteral("10")),
				         std::optional<long long>(10));
				QCOMPARE(QMudWorldOptions::storedNumericOptionText(*custom, 10), QStringLiteral("10"));
				QCOMPARE(QMudWorldOptions::publicNumericOptionValue(*boolean, QStringLiteral("true")),
				         std::optional<long long>(1));
				QCOMPARE(QMudWorldOptions::publicNumericOptionValue(*boolean, QStringLiteral("YES")),
				         std::optional<long long>(1));
				QCOMPARE(QMudWorldOptions::publicNumericOptionValue(*boolean, QStringLiteral("no")),
				         std::optional<long long>(0));
				QVERIFY(!QMudWorldOptions::publicNumericOptionValue(*rgb, QStringLiteral("invalid")));
			}

			void luaNumericRangeRejectsRoundedValueAboveIntegerMaximum()
			{
				const WorldNumericOption *option =
				    QMudWorldOptions::findWorldNumericOption(QStringLiteral("chat_max_bytes_per_message"));
				QVERIFY(option != nullptr);

				const auto roundedMaximum = static_cast<double>(option->maxValue);
				if (static_cast<long double>(roundedMaximum) > static_cast<long double>(option->maxValue))
					QVERIFY(!QMudWorldOptions::numericOptionValueInRange(*option, roundedMaximum));

				const double representableBelowMaximum = std::nextafter(roundedMaximum, 0.0);
				QVERIFY(QMudWorldOptions::numericOptionValueInRange(*option, representableBelowMaximum));
			}

			void findUnknownReturnsNull()
			{
				QCOMPARE(QMudWorldOptions::findWorldNumericOption(QStringLiteral("not_an_option")),
				         static_cast<const WorldNumericOption *>(nullptr));
			}

			void pluginSequenceParserEnforcesLegacyRange()
			{
				QCOMPARE(QMudPluginSequence::parse(QStringLiteral("-10000")), std::optional<int>(-10000));
				QCOMPARE(QMudPluginSequence::parse(QStringLiteral(" 5000 ")), std::optional<int>(5000));
				QCOMPARE(QMudPluginSequence::parse(QStringLiteral("10000")), std::optional<int>(10000));
				QVERIFY(!QMudPluginSequence::parse(QStringLiteral("-10001")));
				QVERIFY(!QMudPluginSequence::parse(QStringLiteral("10001")));
				QVERIFY(!QMudPluginSequence::parse(QStringLiteral("invalid")));
				QVERIFY(!QMudPluginSequence::parse(QString()));
			}
			// NOLINTEND(readability-convert-member-functions-to-static)
	};
} // namespace

QTEST_APPLESS_MAIN(tst_WorldOptions)

#if __has_include("tst_WorldOptions.moc")
#include "tst_WorldOptions.moc"
#endif

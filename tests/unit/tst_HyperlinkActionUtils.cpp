/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_HyperlinkActionUtils.cpp
 * Role: QTest coverage for HyperlinkActionUtils hyperlink decode and dispatch policy behavior.
 */

#include "HyperlinkActionUtils.h"

#include <QtTest/QTest>

namespace
{
	WorldRuntime::LineEntry makeLine(const int flags, const int actionType, const QString &action)
	{
		WorldRuntime::LineEntry entry;
		entry.flags = flags;
		WorldRuntime::StyleSpan span;
		span.length     = 1;
		span.actionType = actionType;
		span.action     = action;
		entry.spans.push_back(span);
		return entry;
	}
} // namespace

class tst_HyperlinkActionUtils : public QObject
{
		Q_OBJECT

		// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		void decodeMxpActionTextDecodesPercentAndEntities()
		{
			const QString encoded = QStringLiteral("mapper%20goto%2072930&amp;foo=%23bar");
			QCOMPARE(decodeMxpActionText(encoded), QStringLiteral("mapper goto 72930&foo=#bar"));
		}

		void firstMxpSendActionReturnsFirstNonEmptySegment()
		{
			const QString href = QStringLiteral(" | mapper goto 72930 | mapper goto 42 ");
			QCOMPARE(firstMxpSendAction(href), QStringLiteral("mapper goto 72930"));
		}

		void resolvePolicyReturnsPromptInputForPromptSpan()
		{
			QVector<WorldRuntime::LineEntry> lines;
			lines.push_back(
			    makeLine(WorldRuntime::LineOutput, WorldRuntime::ActionPrompt, QStringLiteral("look")));

			const auto policy = resolveMxpHyperlinkDispatchPolicy(lines, QStringLiteral("look"));
			QCOMPARE(policy, MxpHyperlinkDispatchPolicy::PromptInput);
		}

		void resolvePolicyRoutesNoteSendToCommandProcessing()
		{
			QVector<WorldRuntime::LineEntry> lines;
			lines.push_back(makeLine(WorldRuntime::LineNote, WorldRuntime::ActionSend,
			                         QStringLiteral("mapper%20goto%2072930")));

			const auto policy = resolveMxpHyperlinkDispatchPolicy(lines, QStringLiteral("mapper goto 72930"));
			QCOMPARE(policy, MxpHyperlinkDispatchPolicy::CommandProcessing);
		}

		void resolvePolicyKeepsNonNoteSendAsDirectSend()
		{
			QVector<WorldRuntime::LineEntry> lines;
			lines.push_back(makeLine(WorldRuntime::LineOutput, WorldRuntime::ActionSend,
			                         QStringLiteral("mapper goto 72930")));

			const auto policy = resolveMxpHyperlinkDispatchPolicy(lines, QStringLiteral("mapper goto 72930"));
			QCOMPARE(policy, MxpHyperlinkDispatchPolicy::DirectSend);
		}
		// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_APPLESS_MAIN(tst_HyperlinkActionUtils)

#if __has_include("tst_HyperlinkActionUtils.moc")
#include "tst_HyperlinkActionUtils.moc"
#endif

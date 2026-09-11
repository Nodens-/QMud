/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_DialogUtils.cpp
 * Role: GUI tests for small dialogs and activity widgets with self-contained behavior.
 */

#include "ActivityWindow.h"
#include "AppController.h"
#include "NameGeneration.h"
#include "WorldView.h"
#include "dialogs/CommandHistoryDialog.h"
#include "dialogs/ConfirmPreambleDialog.h"
#include "dialogs/GeneratedNameDialog.h"
#include "dialogs/HighlightPhraseDialog.h"
#include "dialogs/SpellCheckDialog.h"
#include "dialogs/TipDialog.h"

#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
// ReSharper disable once CppUnusedIncludeDirective
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMap>
#include <QPushButton>
#include <QTableWidget>
// ReSharper disable once CppUnusedIncludeDirective
#include <QTemporaryDir>
#include <QtTest/QTest>

namespace
{
	/**
	 * @brief Exposes protected command-history behavior for direct fixture setup.
	 */
	class TestCommandHistoryDialog final : public CommandHistoryDialog
	{
		public:
			using CommandHistoryDialog::doFind;
			using CommandHistoryDialog::matchLine;
			using CommandHistoryDialog::updateSelection;
	};

	/**
	 * @brief Exposes valid highlight acceptance for non-modal testing.
	 */
	class TestHighlightPhraseDialog final : public HighlightPhraseDialog
	{
		public:
			using HighlightPhraseDialog::accept;
	};

	/**
	 * @brief Restores the process working directory after a fixture changes it.
	 */
	class CurrentDirectoryRestorer final
	{
		public:
			/** Captures the current working directory. */
			CurrentDirectoryRestorer() : m_path(QDir::currentPath())
			{
			}

			/** Restores the captured working directory. */
			~CurrentDirectoryRestorer()
			{
				QDir::setCurrent(m_path);
			}

			CurrentDirectoryRestorer(const CurrentDirectoryRestorer &)            = delete;
			CurrentDirectoryRestorer &operator=(const CurrentDirectoryRestorer &) = delete;

		private:
			QString m_path;
	};

	/**
	 * @brief Finds a push button by its displayed action text.
	 * @param parent Widget containing the button.
	 * @param text Exact button text.
	 * @return Matching button, or `nullptr`.
	 */
	QPushButton *findButton(const QWidget &parent, const QString &text)
	{
		for (QPushButton *button : parent.findChildren<QPushButton *>())
		{
			if (button->text() == text)
				return button;
		}
		return nullptr;
	}

	/**
	 * @brief Writes a text fixture below a test-owned directory.
	 * @param path Destination path.
	 * @param contents Fixture contents.
	 * @return `true` when the complete fixture was written.
	 */
	bool writeFixture(const QString &path, const QByteArray &contents)
	{
		QFile file(path);
		return file.open(QIODevice::WriteOnly | QIODevice::Text) && file.write(contents) == contents.size();
	}

	/**
	 * @brief QTest fixture covering self-contained dialog and activity-widget behavior.
	 */
	class tst_DialogUtils final : public QObject
	{
			Q_OBJECT

		private slots:
			static void confirmPreambleRoundTripsAndClampsValues()
			{
				ConfirmPreambleDialog dialog;
				dialog.setPasteMessage(QStringLiteral("Review these commands"));
				dialog.setFilePreamble(QStringLiteral("begin"));
				dialog.setLinePreamble(QStringLiteral("<"));
				dialog.setLinePostamble(QStringLiteral(">"));
				dialog.setFilePostamble(QStringLiteral("end"));
				dialog.setCommentedSoftcode(true);
				dialog.setEcho(true);
				dialog.setLineDelayMs(20000);
				dialog.setDelayPerLines(0);

				QCOMPARE(dialog.filePreamble(), QStringLiteral("begin"));
				QCOMPARE(dialog.linePreamble(), QStringLiteral("<"));
				QCOMPARE(dialog.linePostamble(), QStringLiteral(">"));
				QCOMPARE(dialog.filePostamble(), QStringLiteral("end"));
				QVERIFY(dialog.commentedSoftcode());
				QVERIFY(dialog.echo());
				QCOMPARE(dialog.lineDelayMs(), 10000);
				QCOMPARE(dialog.delayPerLines(), 1);

				auto *buttons = dialog.findChild<QDialogButtonBox *>();
				QVERIFY(buttons);
				QVERIFY(buttons->button(QDialogButtonBox::Ok));
				buttons->button(QDialogButtonBox::Ok)->click();
				QCOMPARE(dialog.result(), static_cast<int>(QDialog::Accepted));
			}

			static void highlightPhraseRoundTripsSelectionAndSwatches()
			{
				QVector<QColor>  textColours;
				QVector<QColor>  backColours;
				QVector<QString> names;
				for (int index = 0; index < 16; ++index)
				{
					textColours.append(QColor(index + 1, index + 2, index + 3));
					backColours.append(QColor(index + 20, index + 21, index + 22));
					names.append(index == 0 ? QString() : QStringLiteral("Colour %1").arg(index + 1));
				}

				TestHighlightPhraseDialog dialog;
				dialog.setCustomColours(textColours, backColours, names);
				dialog.setOtherColours(QColor(80, 90, 100), QColor(110, 120, 130));
				dialog.setInitialText(QStringLiteral("danger"));
				dialog.setWholeWord(true);
				dialog.setMatchCase(true);

				auto *combo = dialog.findChild<QComboBox *>();
				QVERIFY(combo);
				QCOMPARE(combo->count(), 18);
				QCOMPARE(combo->itemText(1), QStringLiteral("Custom1"));
				dialog.setSelectedColourIndex(-1);
				QCOMPARE(dialog.selectedColourIndex(), 0);
				dialog.setSelectedColourIndex(100);
				QCOMPARE(dialog.selectedColourIndex(), 17);
				QCOMPARE(dialog.otherTextColour(), QColor(80, 90, 100));
				QCOMPARE(dialog.otherBackColour(), QColor(110, 120, 130));

				dialog.setSelectedColourIndex(1);
				QCOMPARE(dialog.phraseText(), QStringLiteral("danger"));
				QVERIFY(dialog.wholeWord());
				QVERIFY(dialog.matchCase());
				dialog.accept();
				QCOMPARE(dialog.result(), static_cast<int>(QDialog::Accepted));
			}

			static void spellCheckActionsPreserveOrSelectReplacement()
			{
				SpellCheckDialog change(QStringLiteral("wierd"),
				                        {QStringLiteral("weird"), QStringLiteral("wired")});
				QVERIFY(change.action().isEmpty());
				QCOMPARE(change.replacement(), QStringLiteral("wierd"));
				QPushButton *changeButton = findButton(change, QStringLiteral("Change"));
				QVERIFY(changeButton);
				changeButton->click();
				QCOMPARE(change.action(), QStringLiteral("change"));
				QCOMPARE(change.replacement(), QStringLiteral("weird"));

				SpellCheckDialog custom(QStringLiteral("wierd"), {QStringLiteral("weird")});
				auto            *edit = custom.findChild<QLineEdit *>();
				QVERIFY(edit);
				edit->setText(QStringLiteral("word"));
				QPushButton *changeAllButton = findButton(custom, QStringLiteral("Change All"));
				QVERIFY(changeAllButton);
				changeAllButton->click();
				QCOMPARE(custom.action(), QStringLiteral("changeall"));
				QCOMPARE(custom.replacement(), QStringLiteral("word"));

				const QList<QPair<QString, QString>> actions = {
				    {QStringLiteral("Ignore"),     QStringLiteral("ignore")   },
				    {QStringLiteral("Ignore All"), QStringLiteral("ignoreall")},
				    {QStringLiteral("Add"),        QStringLiteral("add")      }
                };
				for (const auto &[buttonText, action] : actions)
				{
					SpellCheckDialog dialog(QStringLiteral("word"), {});
					QPushButton     *button = findButton(dialog, buttonText);
					QVERIFY(button);
					button->click();
					QCOMPARE(dialog.action(), action);
					QCOMPARE(dialog.result(), static_cast<int>(QDialog::Accepted));
				}
			}

			static void commandHistoryPopulatesAndFindsWithoutAWorldView()
			{
				const QString longEntry(600, QLatin1Char('x'));
				QStringList   history = {QStringLiteral("north"), QStringLiteral("find me"), longEntry};

				TestCommandHistoryDialog dialog;
				dialog.m_msgList = &history;
				dialog.populateList();

				auto *list = dialog.findChild<QListWidget *>();
				auto *edit = dialog.findChild<QLineEdit *>();
				QVERIFY(list);
				QVERIFY(edit);
				QCOMPARE(list->count(), 3);
				QCOMPARE(list->item(2)->text().size(), 504);
				QCOMPARE(list->item(2)->data(Qt::UserRole).toString(), longEntry);
				QCOMPARE(edit->text(), longEntry);

				list->setCurrentRow(-1);
				dialog.updateSelection();
				QVERIFY(edit->text().isEmpty());
				QVERIFY(TestCommandHistoryDialog::matchLine(QStringLiteral("Find Me"), QStringLiteral("find"),
				                                            false, false, QRegularExpression()));
				QVERIFY(!TestCommandHistoryDialog::matchLine(
				    QStringLiteral("Find Me"), QStringLiteral("find"), true, false, QRegularExpression()));
				QVERIFY(TestCommandHistoryDialog::matchLine(QStringLiteral("go north"), QString(), false,
				                                            true,
				                                            QRegularExpression(QStringLiteral("^go \\w+$"))));

				CommandHistoryFindState findState;
				findState.history         = {QStringLiteral("find")};
				findState.lastFindText    = QStringLiteral("stale");
				findState.forwards        = true;
				findState.currentLine     = 0;
				dialog.m_pHistoryFindInfo = &findState;
				dialog.doFind(true);
				QCOMPARE(list->currentRow(), 1);
				QCOMPARE(edit->text(), QStringLiteral("find me"));
				QCOMPARE(findState.lastFindText, QStringLiteral("find"));
			}

			static void tipDialogReadsAdvancesAndPersistsState()
			{
				QTemporaryDir directory(QDir::current().filePath(QStringLiteral("tst_TipDialog-XXXXXX")));
				QVERIFY(directory.isValid());
				QVERIFY(QDir(directory.path()).mkpath(QStringLiteral("docs")));
				QVERIFY(
				    writeFixture(directory.filePath(QStringLiteral("docs/tips.txt")),
				                 QByteArrayLiteral("; comment\n\n First ignored\nFirst tip\nSecond tip\n")));

				CurrentDirectoryRestorer currentDirectory;
				QVERIFY(QDir::setCurrent(directory.path()));
				AppController          app;
				QMap<QString, int>     integers;
				QMap<QString, QString> strings;
				const auto             key = [](const QString &section, const QString &entry)
				{ return section + QLatin1Char('/') + entry; };
				const auto getInt =
				    [&integers, &key](const QString &section, const QString &entry, const int defaultValue)
				{ return integers.value(key(section, entry), defaultValue); };
				const auto getString = [&strings, &key](const QString &section, const QString &entry,
				                                        const QString &defaultValue)
				{ return strings.value(key(section, entry), defaultValue); };
				const auto writeInt =
				    [&integers, &key](const QString &section, const QString &entry, const int value)
				{
					integers.insert(key(section, entry), value);
					return 0;
				};
				const auto writeString =
				    [&strings, &key](const QString &section, const QString &entry, const QString &value)
				{
					strings.insert(key(section, entry), value);
					return 0;
				};

				{
					TipDialog dialog(getInt, getString, writeInt, writeString);
					QLabel   *tipLabel = nullptr;
					for (QLabel *label : dialog.findChildren<QLabel *>())
					{
						if (label->wordWrap())
							tipLabel = label;
					}
					QVERIFY(tipLabel);
					QCOMPARE(tipLabel->text(), QStringLiteral("First tip"));
					QPushButton *next = findButton(dialog, QStringLiteral("Next Tip"));
					QVERIFY(next);

					auto *startup = dialog.findChild<QCheckBox *>();
					QVERIFY(startup);
					startup->setChecked(false);
					auto *buttons = dialog.findChild<QDialogButtonBox *>();
					QVERIFY(buttons);
					buttons->button(QDialogButtonBox::Ok)->click();
				}

				QCOMPARE(integers.value(QStringLiteral("control/Tip_StartUp")), 1);
				QVERIFY(integers.value(QStringLiteral("control/Tip_FilePos")) > 0);
				QVERIFY(!strings.value(QStringLiteral("control/Tip_TimeStamp")).isEmpty());

				{
					TipDialog resumed(getInt, getString, writeInt, writeString);
					QLabel   *tipLabel = nullptr;
					for (QLabel *label : resumed.findChildren<QLabel *>())
					{
						if (label->wordWrap())
							tipLabel = label;
					}
					QVERIFY(tipLabel);
					QCOMPARE(tipLabel->text(), QStringLiteral("Second tip"));

					QPushButton *next = findButton(resumed, QStringLiteral("Next Tip"));
					QVERIFY(next);
					next->click();
					QCOMPARE(tipLabel->text(), QStringLiteral("First tip"));
				}
			}

			static void tipDialogHandlesFilesWithoutEligibleTips_data()
			{
				QTest::addColumn<QByteArray>("contents");

				QTest::newRow("empty") << QByteArray();
				QTest::newRow("filtered-lines") << QByteArrayLiteral("; comment\n\n indented\n\tindented\n");
			}

			static void tipDialogHandlesFilesWithoutEligibleTips()
			{
				QFETCH(QByteArray, contents);

				QTemporaryDir directory(QDir::current().filePath(QStringLiteral("tst_TipDialog-XXXXXX")));
				QVERIFY(directory.isValid());
				QVERIFY(QDir(directory.path()).mkpath(QStringLiteral("docs")));
				QVERIFY(writeFixture(directory.filePath(QStringLiteral("docs/tips.txt")), contents));

				CurrentDirectoryRestorer currentDirectory;
				QVERIFY(QDir::setCurrent(directory.path()));
				AppController app;
				TipDialog     dialog(
				    [](const QString &, const QString &, const int defaultValue) { return defaultValue; },
				    [](const QString &, const QString &, const QString &defaultValue)
				    { return defaultValue; }, [](const QString &, const QString &, const int) { return 0; },
				    [](const QString &, const QString &, const QString &) { return 0; });

				QLabel *tipLabel = nullptr;
				for (QLabel *label : dialog.findChildren<QLabel *>())
				{
					if (label->wordWrap())
						tipLabel = label;
				}
				QVERIFY(tipLabel);
				QCOMPARE(tipLabel->text(), QStringLiteral("No tips available."));
				QPushButton *next = findButton(dialog, QStringLiteral("Next Tip"));
				QVERIFY(next);
				QVERIFY(!next->isEnabled());
			}

			static void generatedNameDialogCopiesLoadedNameAndHandlesMissingRuntime()
			{
				QTemporaryDir directory(QDir::current().filePath(QStringLiteral("tst_GeneratedName-XXXXXX")));
				QVERIFY(directory.isValid());
				const QString dictionary = directory.filePath(QStringLiteral("names.nam"));
				QVERIFY(writeFixture(dictionary,
				                     QByteArrayLiteral("[start]\nRo\n[middle]\nsa\n[end]\nlie\n[stop]\n")));

				AppController app;
				qmudReadNames(dictionary, true);
				GeneratedNameDialog dialog(nullptr);

				QLineEdit          *nameEdit = nullptr;
				QLineEdit          *fileEdit = nullptr;
				for (QLineEdit *edit : dialog.findChildren<QLineEdit *>())
				{
					if (edit->isReadOnly())
						fileEdit = edit;
					else
						nameEdit = edit;
				}
				QVERIFY(nameEdit);
				QVERIFY(fileEdit);
				QCOMPARE(nameEdit->text(), QStringLiteral("Rosalie"));
				QCOMPARE(fileEdit->text(),
				         app.getGlobalOption(QStringLiteral("DefaultNameGenerationFile")).toString());

				nameEdit->setText(QStringLiteral("Copied Name"));
				QPushButton *copy  = findButton(dialog, QStringLiteral("Copy"));
				QPushButton *send  = findButton(dialog, QStringLiteral("Send To World"));
				QPushButton *again = findButton(dialog, QStringLiteral("Try Again"));
				QVERIFY(copy);
				QVERIFY(send);
				QVERIFY(again);
				copy->click();
				QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("Copied Name"));
				send->click();
				again->click();
				QCOMPARE(nameEdit->text(), QStringLiteral("Rosalie"));
			}

			static void activityWindowHandlesMissingHostAndInvalidRows()
			{
				ActivityWindow window;
				auto          *table = window.findChild<QTableWidget *>();
				QVERIFY(table);
				QCOMPARE(table->columnCount(), 7);
				QCOMPARE(table->rowCount(), 0);
				window.setGridLinesVisible(false);
				QVERIFY(!table->showGrid());
				window.setGridLinesVisible(true);
				QVERIFY(table->showGrid());
				window.refresh();
				QVERIFY(QMetaObject::invokeMethod(&window, "onCellActivated", Q_ARG(int, -1), Q_ARG(int, 0)));
				QVERIFY(QMetaObject::invokeMethod(&window, "showContextMenu", Q_ARG(QPoint, QPoint(-1, -1))));
			}
	};
} // namespace

QTEST_MAIN(tst_DialogUtils)

#if __has_include("tst_DialogUtils.moc")
#include "tst_DialogUtils.moc"
#endif

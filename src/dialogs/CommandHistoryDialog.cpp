/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: CommandHistoryDialog.cpp
 * Role: Command history dialog implementation supporting recall and reinsertion of prior commands into the input
 * workflow.
 */

#include "CommandHistoryDialog.h"
#include "MainWindowHost.h"
#include "MainWindowHostResolver.h"
#include "WorldChildWindow.h"
#include "WorldRuntime.h"
#include "WorldView.h"
#include "dialogs/FindDialog.h"

#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QRegularExpression>
#include <QScopedPointer>
#include <QVBoxLayout>
#include <limits>
#include <memory>

namespace
{
	int sizeToInt(const qsizetype value)
	{
		return static_cast<int>(qBound(static_cast<qsizetype>(0), value,
		                               static_cast<qsizetype>(std::numeric_limits<int>::max())));
	}
} // namespace

CommandHistoryDialog::CommandHistoryDialog(QWidget *parent) : QDialog(parent)
{
	setWindowTitle(QStringLiteral("Command History"));
	setMinimumSize(760, 420);

	auto  mainLayout    = std::make_unique<QVBoxLayout>();
	auto *mainLayoutPtr = mainLayout.get();

	m_list = new QListWidget(this);
	m_list->setSelectionMode(QAbstractItemView::SingleSelection);
	mainLayoutPtr->addWidget(m_list);

	m_historyItem = new QLineEdit(this);
	m_historyItem->setReadOnly(true);
	mainLayoutPtr->addWidget(m_historyItem);

	auto actionLayout = std::make_unique<QHBoxLayout>();
	m_findButton      = new QPushButton(QStringLiteral("Find"), this);
	m_findNextButton  = new QPushButton(QStringLiteral("Find Next"), this);
	actionLayout->addWidget(m_findButton);
	actionLayout->addWidget(m_findNextButton);
	actionLayout->addStretch();

	m_doButton      = new QPushButton(QStringLiteral("Do"), this);
	m_notepadButton = new QPushButton(QStringLiteral("Notepad"), this);
	m_helpButton    = new QPushButton(QStringLiteral("Help"), this);
	actionLayout->addWidget(m_doButton);
	actionLayout->addWidget(m_notepadButton);
	actionLayout->addWidget(m_helpButton);
	mainLayoutPtr->addLayout(actionLayout.release());

	auto  buttons =
	    std::make_unique<QDialogButtonBox>(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	auto *buttonsPtr = buttons.get();
	mainLayoutPtr->addWidget(buttons.release());

	connect(buttonsPtr, &QDialogButtonBox::accepted, this,
	        [this]
	        {
		        updateSelection();
		        if (!m_sendview || !m_historyItem)
		        {
			        accept();
			        return;
		        }
		        const QString text = m_historyItem->text();
		        if (!m_sendview->confirmReplaceTyping(text))
			        return;
		        m_sendview->setInputText(text, true);
		        accept();
	        });
	connect(buttonsPtr, &QDialogButtonBox::rejected, this, &QDialog::reject);

	connect(m_list, &QListWidget::currentRowChanged, this, [this](const int) { updateSelection(); });
	connect(m_findButton, &QPushButton::clicked, this, [this] { doFind(false); });
	connect(m_findNextButton, &QPushButton::clicked, this, [this] { doFind(true); });
	connect(m_doButton, &QPushButton::clicked, this,
	        [this]
	        {
		        updateSelection();
		        if (!m_sendview || !m_historyItem)
			        return;
		        m_sendview->sendCommandFromHistory(m_historyItem->text());
	        });
	connect(m_notepadButton, &QPushButton::clicked, this,
	        [this]
	        {
		        updateSelection();
		        if (!m_sendview || !m_historyItem)
			        return;
		        const QString text = m_historyItem->text();
		        if (text.isEmpty())
			        return;
		        MainWindowHost *main = resolveMainWindowHost(m_sendview->window());
		        if (!main)
			        return;
		        if (main->switchToNotepad())
			        return;
		        QString worldName;
		        if (const WorldRuntime *runtime = m_sendview->runtime())
			        worldName = runtime->worldAttributes().value(QStringLiteral("name"));
		        if (worldName.isEmpty())
			        worldName = m_sendview->windowTitle();
		        if (worldName.isEmpty())
			        worldName = QStringLiteral("World");
		        if (const QString title = QStringLiteral("Notepad: %1").arg(worldName);
		            main->sendToNotepad(title, text, m_sendview->runtime()))
			        accept();
	        });
	connect(m_helpButton, &QPushButton::clicked, this,
	        [this]
	        {
		        QMessageBox::information(this, QStringLiteral("Command History"),
		                                 QStringLiteral("Command history help is not available yet."));
	        });
	setLayout(mainLayout.release());
}

void CommandHistoryDialog::populateList() const
{
	if (!m_list || !m_msgList)
		return;

	m_list->clear();

	for (const QString &entry : *m_msgList)
	{
		QString display = entry;
		// truncate long strings, or we might get a nasty crash with long strings
		if (display.size() > 500)
			display = display.left(500) + QStringLiteral(" ...");
		auto item = std::make_unique<QListWidgetItem>(display);
		item->setData(Qt::UserRole, entry);
		m_list->addItem(item.release());
	}

	if (m_list->count() > 0)
		m_list->setCurrentRow(m_list->count() - 1);

	updateSelection();
}

void CommandHistoryDialog::updateSelection() const
{
	if (!m_list || !m_historyItem)
		return;

	const QListWidgetItem *item = m_list->currentItem();
	if (!item)
	{
		m_historyItem->clear();
		return;
	}

	const QString fullText = item->data(Qt::UserRole).toString();
	m_historyItem->setText(fullText);
}

bool CommandHistoryDialog::matchLine(const QString &line, const QString &needle, const bool matchCase,
                                     const bool regexp, const QRegularExpression &regex)
{
	if (regexp)
	{
		const QRegularExpressionMatch match = regex.match(line);
		return match.hasMatch();
	}

	const Qt::CaseSensitivity sensitivity = matchCase ? Qt::CaseSensitive : Qt::CaseInsensitive;
	return line.contains(needle, sensitivity);
}

void CommandHistoryDialog::doFind(const bool again)
{
	if (!m_list || !m_msgList || !m_pHistoryFindInfo)
		return;
	const int totalLines = sizeToInt(m_msgList->size());
	if (totalLines <= 0)
		return;

	auto &[history, title, lastFindText, matchCase, forwards, regexp, againFlag, currentLine, regex] =
	    *m_pHistoryFindInfo;
	againFlag = again;

	QString findText;
	if (!againFlag || history.isEmpty())
	{
		FindDialog dlg(history, this);
		dlg.setTitleText(title);
		if (!history.isEmpty())
			dlg.setFindText(history.front());
		dlg.setMatchCase(matchCase);
		dlg.setForwards(forwards);
		dlg.setRegexp(regexp);

		if (dlg.execModal() != Accepted)
			return;

		matchCase = dlg.matchCase();
		forwards  = dlg.forwards();
		regexp    = dlg.regexp();
		findText  = dlg.findText();

		if (!findText.isEmpty() && (history.isEmpty() || history.front() != findText))
			history.prepend(findText);

		lastFindText = findText;
		currentLine  = forwards ? 0 : totalLines - 1;

		if (regexp)
		{
			QRegularExpression::PatternOptions options = QRegularExpression::NoPatternOption;
			if (!matchCase)
				options |= QRegularExpression::CaseInsensitiveOption;
			regex = QRegularExpression(findText, options);
			if (!regex.isValid())
			{
				QMessageBox::warning(this, QStringLiteral("Find"),
				                     QStringLiteral("Regular expression error: %1").arg(regex.errorString()));
				return;
			}
		}
	}
	else
	{
		if (!history.isEmpty())
			lastFindText = history.front();
		findText = lastFindText;
		currentLine += forwards ? 1 : -1;
	}

	MainWindowHost                 *main = resolveMainWindowHost(window());
	QScopedPointer<QProgressDialog> progress;
	if (const int remaining = forwards ? totalLines - currentLine : currentLine; remaining > 500)
	{
		progress.reset(new QProgressDialog(QStringLiteral("Finding: %1").arg(findText),
		                                   QStringLiteral("Cancel"), 0, totalLines, this));
		progress->setWindowTitle(QStringLiteral("Finding..."));
		progress->setAutoClose(true);
		progress->setAutoReset(false);
		progress->setMinimumDuration(0);
	}

	auto wrapUp = [&]
	{
		if (main)
			main->setStatusNormal();
		if (progress)
			progress->close();
	};

	auto notFound = [&]
	{
		const QString typeLabel = regexp ? QStringLiteral("regular expression") : QStringLiteral("text");
		const QString suffix    = againFlag ? QStringLiteral(" again.") : QStringLiteral(".");
		QMessageBox::information(
		    this, QStringLiteral("Find"),
		    QStringLiteral("The %1 \"%2\" was not found%3").arg(typeLabel, findText, suffix));
		m_list->setCurrentRow(-1);
		updateSelection();
		wrapUp();
	};

	if (currentLine < 0 || currentLine >= totalLines)
	{
		notFound();
		return;
	}

	if (main)
		main->setStatusMessageNow(QStringLiteral("Finding: %1").arg(findText));

	int milestone = 0;
	for (int i = currentLine; forwards ? i < totalLines : i >= 0; i += forwards ? 1 : -1)
	{
		if (const QString line = m_msgList->at(i); matchLine(line, findText, matchCase, regexp, regex))
		{
			currentLine = i;
			m_list->setCurrentRow(i);
			m_list->scrollToItem(m_list->currentItem());
			updateSelection();
			wrapUp();
			return;
		}

		++milestone;
		if (progress && milestone > 31)
		{
			milestone       = 0;
			const int value = forwards ? i : totalLines - i;
			progress->setValue(qBound(0, value, totalLines));
			if (progress->wasCanceled())
			{
				wrapUp();
				return;
			}
		}
	}

	notFound();
}

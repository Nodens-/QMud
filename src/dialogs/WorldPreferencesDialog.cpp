/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: WorldPreferencesDialog.cpp
 * Role: World preferences dialog implementation that edits and applies per-world option groups across tabs.
 */

#include "dialogs/WorldPreferencesDialog.h"
#include "AppController.h"
#include "FileExtensions.h"
#include "StringUtils.h"
#include "Version.h"
#include "WorldDocument.h"
#include "WorldOptions.h"
#include "WorldRuntime.h"
#include "WorldView.h"
#include "dialogs/ColourPickerDialog.h"
#include "dialogs/FindDialog.h"
#include "dialogs/WorldAliasDialog.h"
#include "dialogs/WorldTimerDialog.h"
#include "dialogs/WorldTriggerDialog.h"
#include "scripting/ScriptingErrors.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QColorDialog>
#include <QComboBox>
#include <QDesktopServices>
// ReSharper disable once CppUnusedIncludeDirective
#include <QDialogButtonBox>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDialog>
#include <QFontMetrics>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QLinearGradient>
#include <QLocale>
#include <QMessageBox>
#include <QPainter>
#include <QPalette>
#include <QProcess>
#include <QProgressDialog>
#include <QPushButton>
#include <QRadioButton>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScreen>
#include <QSet>
#include <QSettings>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSpacerItem>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStyleOptionGroupBox>
#include <QTableWidget>
#include <QTemporaryFile>
#include <QTextCursor>
#include <QTextEdit>
#include <QTextStream>
#include <QTreeWidget>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <unistd.h>

#ifdef QMUD_ENABLE_LUA_SCRIPTING
#include "LuaHeaders.h"
#include "LuaSupport.h"
#endif

enum
{
	// ReSharper disable once CppEnumeratorNeverUsed
	ADJUST_COLOUR_NO_OP = 0,
	ADJUST_COLOUR_INVERT,
	ADJUST_COLOUR_LIGHTER,
	ADJUST_COLOUR_DARKER,
	ADJUST_COLOUR_LESS_COLOUR,
	ADJUST_COLOUR_MORE_COLOUR
};

static QColor parseColourValue(const QString &value)
{
	if (value.isEmpty())
		return {};
	if (QColor colour(value); colour.isValid())
		return colour;
	bool       ok  = false;
	const uint rgb = value.toUInt(&ok, 0);
	if (ok)
		return QColor::fromRgb(rgb);
	return {};
}

static bool confirmRemoval(QWidget *parent, const QString &singular)
{
	if (AppController *app = AppController::instance();
	    !app || app->getGlobalOption(QStringLiteral("TriggerRemoveCheck")).toInt() == 0)
		return true;

	const QString message                    = QStringLiteral("Delete this %1 - are you sure?").arg(singular);
	const QMessageBox::StandardButton result = QMessageBox::question(
	    parent, QStringLiteral("Confirm"), message, QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
	return result == QMessageBox::Yes;
}

static quint64 totalPhysicalMemoryBytes()
{
#if defined(Q_OS_UNIX)
	const long pages = sysconf(_SC_PHYS_PAGES);
	if (const long pageSize = sysconf(_SC_PAGE_SIZE); pages > 0 && pageSize > 0)
		return static_cast<quint64>(pages) * static_cast<quint64>(pageSize);
#endif
	return 0;
}

static bool bringOwnedWindowToFrontByTitle(const QString &title)
{
	const QString target = title.trimmed();
	if (target.isEmpty())
		return false;
	for (const auto windows = QApplication::topLevelWidgets(); QWidget *window : windows)
	{
		if (!window)
			continue;
		if (window->windowTitle().trimmed().compare(target, Qt::CaseInsensitive) != 0)
			continue;
		if (!window->isVisible())
			window->show();
		window->raise();
		window->activateWindow();
		return true;
	}
	return false;
}

static QString canonicalSavePath(const QString &fileName, const QString &extensionLower)
{
	QString output = QMudFileExtensions::canonicalizePathExtension(fileName);
	if (const QString suffix = QFileInfo(output).suffix().toLower(); suffix != extensionLower)
		output = QMudFileExtensions::replaceOrAppendExtension(output, extensionLower);
	return output;
}

class GradientHeader : public QWidget
{
	public:
		explicit GradientHeader(QWidget *parent = nullptr) : QWidget(parent)
		{
			setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
			setMinimumHeight(28);
		}

		void setText(const QString &text)
		{
			if (m_text == text)
				return;
			m_text = text;
			update();
		}

		void setGradientEnabled(const bool enabled)
		{
			if (m_gradientEnabled == enabled)
				return;
			m_gradientEnabled = enabled;
			update();
		}

	protected:
		void paintEvent(QPaintEvent *event) override
		{
			Q_UNUSED(event);
			QPainter         painter(this);
			const QRect      rect = this->rect();
			constexpr QColor left(140, 0, 0);
			constexpr QColor right(255, 255, 0);
			if (m_gradientEnabled)
			{
				QLinearGradient gradient(rect.left(), rect.top(), rect.right(), rect.top());
				gradient.setColorAt(0.0, left);
				gradient.setColorAt(1.0, right);
				painter.fillRect(rect, gradient);
			}
			else
			{
				painter.fillRect(rect, left);
			}

			painter.setPen(Qt::white);
			QFont font = painter.font();
			font.setBold(true);
			painter.setFont(font);
			const int x = rect.left() + 8;
			const int y = rect.center().y() + (painter.fontMetrics().ascent() / 2);
			painter.drawText(x, y, m_text);
		}

	private:
		QString m_text;
		bool    m_gradientEnabled{false};
};

static QString formatColourValue(const QColor &colour)
{
	if (!colour.isValid())
		return {};
	return colour.name(QColor::HexRgb);
}

static QString formatFontStyleText(const int pointSize, const int weight, const bool italic)
{
	if (pointSize <= 0)
		return QStringLiteral("-");
	QString style = QStringLiteral("%1 pt.").arg(pointSize);
	if (weight >= 700)
		style += QStringLiteral(" bold");
	if (italic)
		style += QStringLiteral(" italic");
	return style;
}

static QColor swatchButtonColour(const QPushButton *button)
{
	if (!button)
		return {};
	const QVariant swatchValue = button->property("swatchColor");
	const QColor   colour(swatchValue.toString());
	if (colour.isValid())
		return colour;
	return {};
}

static void setSwatchButtonColour(QPushButton *button, const QColor &colour)
{
	if (!button)
		return;
	const QColor resolved = colour.isValid() ? colour : QColor(Qt::black);
	button->setProperty("swatchColor", resolved.name(QColor::HexRgb));
	const int     luminance  = (resolved.red() * 299 + resolved.green() * 587 + resolved.blue() * 114) / 1000;
	const QString textColour = (luminance < 128) ? QStringLiteral("#ffffff") : QStringLiteral("#000000");
	button->setStyleSheet(QStringLiteral("QPushButton{background:%1;border:1px solid #4b4b4b;color:%2;}")
	                          .arg(resolved.name(QColor::HexRgb), textColour));
}

static QString formatLineCountLabel(const int lines)
{
	return QStringLiteral("(%1 line%2)").arg(lines).arg(lines == 1 ? QString() : QStringLiteral("s"));
}

static int saturatingToInt(const qsizetype value)
{
	if (value > std::numeric_limits<int>::max())
		return std::numeric_limits<int>::max();
	if (value < std::numeric_limits<int>::min())
		return std::numeric_limits<int>::min();
	return static_cast<int>(value);
}

static long toColourRef(const QColor &colour)
{
	return (static_cast<long>(colour.blue()) << 16) | (static_cast<long>(colour.green()) << 8) |
	       static_cast<long>(colour.red());
}

static QColor fromColourRef(const long value)
{
	return {static_cast<int>(value & 0xFF), static_cast<int>((value >> 8) & 0xFF),
	        static_cast<int>((value >> 16) & 0xFF)};
}

static long adjustColourValue(const long value, const short method)
{
	const QColor colour = fromColourRef(value);
	if (!colour.isValid())
		return value;

	switch (method)
	{
	case ADJUST_COLOUR_INVERT:
		return toColourRef(QColor(255 - colour.red(), 255 - colour.green(), 255 - colour.blue()));
	case ADJUST_COLOUR_LIGHTER:
	case ADJUST_COLOUR_DARKER:
	case ADJUST_COLOUR_LESS_COLOUR:
	case ADJUST_COLOUR_MORE_COLOUR:
	{
		QColor    hsl = colour.toHsl();
		const int h   = hsl.hslHue();
		int       s   = hsl.hslSaturation();
		int       l   = hsl.lightness();
		if (method == ADJUST_COLOUR_LIGHTER)
			l = qMin(255, l + 5);
		else if (method == ADJUST_COLOUR_DARKER)
			l = qMax(0, l - 5);
		else if (method == ADJUST_COLOUR_MORE_COLOUR)
			s = qMin(255, s + 5);
		else
			s = qMax(0, s - 5);
		hsl.setHsl(h, s, l);
		return toColourRef(hsl.toRgb());
	}
	default:
		break;
	}
	return value;
}

static void setDefaultAnsiColours(unsigned long *normalcolour, unsigned long *boldcolour)
{
	normalcolour[0] = toColourRef(QColor(0, 0, 0));
	normalcolour[1] = toColourRef(QColor(128, 0, 0));
	normalcolour[2] = toColourRef(QColor(0, 128, 0));
	normalcolour[3] = toColourRef(QColor(128, 128, 0));
	normalcolour[4] = toColourRef(QColor(0, 0, 128));
	normalcolour[5] = toColourRef(QColor(128, 0, 128));
	normalcolour[6] = toColourRef(QColor(0, 128, 128));
	normalcolour[7] = toColourRef(QColor(192, 192, 192));

	boldcolour[0] = toColourRef(QColor(128, 128, 128));
	boldcolour[1] = toColourRef(QColor(255, 0, 0));
	boldcolour[2] = toColourRef(QColor(0, 255, 0));
	boldcolour[3] = toColourRef(QColor(255, 255, 0));
	boldcolour[4] = toColourRef(QColor(0, 0, 255));
	boldcolour[5] = toColourRef(QColor(255, 0, 255));
	boldcolour[6] = toColourRef(QColor(0, 255, 255));
	boldcolour[7] = toColourRef(QColor(255, 255, 255));
}

static void setDefaultCustomColours(unsigned long *customtext, unsigned long *customback)
{
	for (int i = 0; i < MAX_CUSTOM; ++i)
	{
		customtext[i] = toColourRef(QColor(255, 255, 255));
		customback[i] = toColourRef(QColor(0, 0, 0));
	}

	customtext[0]  = toColourRef(QColor(255, 128, 128));
	customtext[1]  = toColourRef(QColor(255, 255, 128));
	customtext[2]  = toColourRef(QColor(128, 255, 128));
	customtext[3]  = toColourRef(QColor(128, 255, 255));
	customtext[4]  = toColourRef(QColor(0, 128, 255));
	customtext[5]  = toColourRef(QColor(255, 128, 192));
	customtext[6]  = toColourRef(QColor(255, 0, 0));
	customtext[7]  = toColourRef(QColor(0, 128, 192));
	customtext[8]  = toColourRef(QColor(255, 0, 255));
	customtext[9]  = toColourRef(QColor(128, 64, 64));
	customtext[10] = toColourRef(QColor(255, 128, 64));
	customtext[11] = toColourRef(QColor(0, 128, 128));
	customtext[12] = toColourRef(QColor(0, 64, 128));
	customtext[13] = toColourRef(QColor(255, 0, 128));
	customtext[14] = toColourRef(QColor(0, 128, 0));
	customtext[15] = toColourRef(QColor(0, 0, 255));
}

static QStringList macroDescriptionList()
{
	return {QStringLiteral("up"),        QStringLiteral("down"),      QStringLiteral("north"),
	        QStringLiteral("south"),     QStringLiteral("east"),      QStringLiteral("west"),
	        QStringLiteral("examine"),   QStringLiteral("look"),      QStringLiteral("page"),
	        QStringLiteral("say"),       QStringLiteral("whisper"),   QStringLiteral("doing"),
	        QStringLiteral("who"),       QStringLiteral("drop"),      QStringLiteral("take"),
	        QStringLiteral("F2"),        QStringLiteral("F3"),        QStringLiteral("F4"),
	        QStringLiteral("F5"),        QStringLiteral("F7"),        QStringLiteral("F8"),
	        QStringLiteral("F9"),        QStringLiteral("F10"),       QStringLiteral("F11"),
	        QStringLiteral("F12"),       QStringLiteral("F2+Shift"),  QStringLiteral("F3+Shift"),
	        QStringLiteral("F4+Shift"),  QStringLiteral("F5+Shift"),  QStringLiteral("F6+Shift"),
	        QStringLiteral("F7+Shift"),  QStringLiteral("F8+Shift"),  QStringLiteral("F9+Shift"),
	        QStringLiteral("F10+Shift"), QStringLiteral("F11+Shift"), QStringLiteral("F12+Shift"),
	        QStringLiteral("F2+Ctrl"),   QStringLiteral("F3+Ctrl"),   QStringLiteral("F5+Ctrl"),
	        QStringLiteral("F7+Ctrl"),   QStringLiteral("F8+Ctrl"),   QStringLiteral("F9+Ctrl"),
	        QStringLiteral("F10+Ctrl"),  QStringLiteral("F11+Ctrl"),  QStringLiteral("F12+Ctrl"),
	        QStringLiteral("logout"),    QStringLiteral("quit"),      QStringLiteral("Alt+A"),
	        QStringLiteral("Alt+B"),     QStringLiteral("Alt+J"),     QStringLiteral("Alt+K"),
	        QStringLiteral("Alt+L"),     QStringLiteral("Alt+M"),     QStringLiteral("Alt+N"),
	        QStringLiteral("Alt+O"),     QStringLiteral("Alt+P"),     QStringLiteral("Alt+Q"),
	        QStringLiteral("Alt+R"),     QStringLiteral("Alt+S"),     QStringLiteral("Alt+T"),
	        QStringLiteral("Alt+U"),     QStringLiteral("Alt+X"),     QStringLiteral("Alt+Y"),
	        QStringLiteral("Alt+Z"),     QStringLiteral("F1"),        QStringLiteral("F1+Ctrl"),
	        QStringLiteral("F1+Shift"),  QStringLiteral("F6"),        QStringLiteral("F6+Ctrl")};
}

static QString boolAttributeValue(const bool enabled)
{
	return enabled ? QStringLiteral("1") : QStringLiteral("0");
}

static QString normalizeObjectName(const QString &value)
{
	return value.trimmed().toLower();
}

static int worldMaxOutputLines(const QMap<QString, QString> &attrs)
{
	bool      ok    = false;
	const int lines = attrs.value(QStringLiteral("max_output_lines")).toInt(&ok);
	return ok ? lines : 0;
}

static QString formatListContents(const QString &input)
{
	QString out = input;
	out.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
	out.replace(QLatin1Char('\r'), QStringLiteral("\n"));
	out.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
	return out;
}

static int selectedRow(const QTableWidget *table)
{
	if (!table)
		return -1;
	if (const QList<QTableWidgetItem *> items = table->selectedItems(); !items.isEmpty())
		return items.first()->row();
	return -1;
}

static QList<int> selectedRows(const QTableWidget *table)
{
	QList<int> rows;
	if (!table)
		return rows;
	const QList<QTableWidgetItem *> items = table->selectedItems();
	for (const QTableWidgetItem *item : items)
	{
		if (item && !rows.contains(item->row()))
			rows.append(item->row());
	}
	std::ranges::sort(rows);
	return rows;
}

static int rowToIndex(const QTableWidget *table, const int row)
{
	if (!table || row < 0 || row >= table->rowCount())
		return -1;
	const QTableWidgetItem *item = table->item(row, 0);
	if (!item)
		return row;
	bool ok = false;
	if (const int value = item->data(Qt::UserRole).toInt(&ok); ok)
		return value;
	return row;
}

static int findRowForIndex(const QTableWidget *table, const int index)
{
	if (!table || index < 0)
		return -1;
	for (int row = 0; row < table->rowCount(); ++row)
	{
		const QTableWidgetItem *item = table->item(row, 0);
		if (!item)
			continue;
		bool ok = false;
		if (const int value = item->data(Qt::UserRole).toInt(&ok); ok && value == index)
			return row;
	}
	return -1;
}

static void selectRow(QTableWidget *table, const int row)
{
	if (row < 0 || row >= table->rowCount())
		return;
	table->setCurrentCell(row, 0);
}

static void selectRowByIndex(QTableWidget *table, const int index)
{
	if (const int row = findRowForIndex(table, index); row >= 0)
		selectRow(table, row);
}

static int currentTreeIndex(const QTreeWidget *tree)
{
	if (!tree)
		return -1;
	const QTreeWidgetItem *item = tree->currentItem();
	if (!item || item->childCount() > 0)
		return -1;
	bool ok = false;
	if (const int value = item->data(0, Qt::UserRole).toInt(&ok); ok)
		return value;
	return -1;
}

static QTreeWidgetItem *findTreeItemByIndex(const QTreeWidget *tree, const int index)
{
	if (!tree || index < 0)
		return nullptr;
	for (int i = 0; i < tree->topLevelItemCount(); ++i)
	{
		const QTreeWidgetItem *groupItem = tree->topLevelItem(i);
		if (!groupItem)
			continue;
		for (int j = 0; j < groupItem->childCount(); ++j)
		{
			QTreeWidgetItem *child = groupItem->child(j);
			if (!child)
				continue;
			bool ok = false;
			if (const int childIndex = child->data(0, Qt::UserRole).toInt(&ok); ok && childIndex == index)
				return child;
		}
	}
	return nullptr;
}

static bool lessCaseInsensitive(const QString &lhs, const QString &rhs)
{
	const int ci = lhs.compare(rhs, Qt::CaseInsensitive);
	if (ci != 0)
		return ci < 0;
	return lhs < rhs;
}

static QString groupedTreeRowName(const QTableWidget *table, const int row)
{
	QString group;
	if (const QTableWidgetItem *groupItem = table->item(row, 4))
		group = groupItem->text().trimmed();
	if (group.isEmpty())
		group = QStringLiteral("(ungrouped)");
	return group;
}

static QString timerWhenText(const WorldRuntime::Timer &timer)
{
	const bool   atTime       = qmudIsEnabledFlag(timer.attributes.value(QStringLiteral("at_time")));
	const int    hour         = timer.attributes.value(QStringLiteral("hour")).toInt();
	const int    minute       = timer.attributes.value(QStringLiteral("minute")).toInt();
	const double second       = timer.attributes.value(QStringLiteral("second")).toDouble();
	const int    offsetHour   = timer.attributes.value(QStringLiteral("offset_hour")).toInt();
	const int    offsetMinute = timer.attributes.value(QStringLiteral("offset_minute")).toInt();
	const double offsetSecond = timer.attributes.value(QStringLiteral("offset_second")).toDouble();
	QString      when         = QString::asprintf("%02d:%02d:%04.2f", hour, minute, second);
	if (!atTime && (offsetHour != 0 || offsetMinute != 0 || offsetSecond != 0.0))
		when += QString::asprintf(" offset %02d:%02d:%04.2f", offsetHour, offsetMinute, offsetSecond);
	return when;
}

static void rebuildGroupedTree(const QTableWidget *table, QTreeWidget *tree,
                               const std::function<QString(int)> &descriptionForRow,
                               const QString                     &expandedGroup)
{
	if (!table || !tree)
		return;

	const int selectedIndex = [&]
	{
		const int tableRow = selectedRow(table);
		if (const int fromTable = rowToIndex(table, tableRow); fromTable >= 0)
			return fromTable;
		return currentTreeIndex(tree);
	}();

	tree->clear();

	QStringList   groupNames;
	QSet<QString> seenGroups;
	for (int row = 0; row < table->rowCount(); ++row)
	{
		const QString group = groupedTreeRowName(table, row);
		if (!seenGroups.contains(group))
		{
			seenGroups.insert(group);
			groupNames.append(group);
		}
	}
	std::ranges::sort(groupNames, lessCaseInsensitive);

	QMap<QString, QTreeWidgetItem *> groups;
	for (const QString &group : groupNames)
	{
		auto *parent = new QTreeWidgetItem(tree);
		parent->setText(0, group);
		parent->setFlags(parent->flags() & ~Qt::ItemIsSelectable);
		groups.insert(group, parent);
	}

	for (int row = 0; row < table->rowCount(); ++row)
	{
		QTreeWidgetItem *parent = groups.value(groupedTreeRowName(table, row), nullptr);
		if (!parent)
			continue;
		auto *child = new QTreeWidgetItem(parent);
		child->setText(0, descriptionForRow(row));
		child->setData(0, Qt::UserRole, rowToIndex(table, row));
	}

	bool restoredExpandedGroup = false;
	for (int i = 0; i < tree->topLevelItemCount(); ++i)
	{
		if (QTreeWidgetItem *groupItem = tree->topLevelItem(i); groupItem)
		{
			const bool shouldExpand = !expandedGroup.isEmpty() && groupItem->text(0) == expandedGroup;
			groupItem->setExpanded(shouldExpand);
			restoredExpandedGroup = restoredExpandedGroup || shouldExpand;
		}
	}
	if (!restoredExpandedGroup)
	{
		for (int i = 0; i < tree->topLevelItemCount(); ++i)
		{
			if (QTreeWidgetItem *groupItem = tree->topLevelItem(i); groupItem)
				groupItem->setExpanded(false);
		}
	}
	tree->setCurrentItem(findTreeItemByIndex(tree, selectedIndex));
}

static QString serializeStringMap(const QMap<QString, QString> &values)
{
	QString out;
	out.reserve(values.size() * 24);
	for (auto it = values.constBegin(); it != values.constEnd(); ++it)
	{
		out += it.key();
		out += QLatin1Char('=');
		out += it.value();
		out += QLatin1Char('\x1e');
	}
	return out;
}

static void updateFindHistory(QStringList &history, const QString &text)
{
	if (text.isEmpty())
		return;
	if (history.isEmpty() || history.first() != text)
		history.prepend(text);
}

static bool runFindDialog(QWidget *parent, QStringList &history, QString &text, bool &matchCase, bool &regex,
                          bool &forwards, const QString &title)
{
	FindDialog dlg(history, parent);
	dlg.setTitleText(title);
	dlg.setFindText(text);
	dlg.setMatchCase(matchCase);
	dlg.setRegexp(regex);
	dlg.setForwards(forwards);
	if (dlg.execModal() != QDialog::Accepted)
		return false;
	text      = dlg.findText();
	matchCase = dlg.matchCase();
	regex     = dlg.regexp();
	forwards  = dlg.forwards();
	updateFindHistory(history, text);
	return true;
}

static bool matchFindText(const QString &haystack, const QString &needle, const bool matchCase,
                          const bool regex)
{
	if (needle.isEmpty())
		return false;
	if (!regex)
		return haystack.contains(needle, matchCase ? Qt::CaseSensitive : Qt::CaseInsensitive);
	const QRegularExpression pattern(needle, matchCase ? QRegularExpression::NoPatternOption
	                                                   : QRegularExpression::CaseInsensitiveOption);
	if (!pattern.isValid())
		return false;
	return pattern.match(haystack).hasMatch();
}

static void browseSoundFile(WorldRuntime *runtime, QWidget *parent, QLineEdit *target)
{
	if (!target)
		return;
	QString start = target->text().trimmed();
	if (start == QStringLiteral("(No sound)"))
		start.clear();
	const QString startDir    = runtime ? runtime->fileBrowsingDirectory() : QString();
	const QString initialPath = start.isEmpty() ? startDir : start;
	const QString fileName    = QFileDialog::getOpenFileName(
        parent, QStringLiteral("Select sound to play"), initialPath,
        QStringLiteral("Waveaudio files (*.wav);;MIDI files (*.mid);;Sequencer files (*.rmi)"));
	if (!fileName.isEmpty())
	{
		if (runtime)
			runtime->setFileBrowsingDirectory(QFileInfo(fileName).absolutePath());
		target->setText(fileName);
	}
}

static bool canTestSoundFile(const QLineEdit *target)
{
	if (!target)
		return false;
	const QString value = target->text();
	return !value.isEmpty() && value != QStringLiteral("(No sound)");
}

static void editPlainTextWithDialog(QWidget *parent, QTextEdit *target, const QString &title)
{
	if (!target)
		return;
	QDialog dialog(parent);
	dialog.setWindowTitle(title);
	auto *dialogLayout = new QVBoxLayout(&dialog);
	auto *edit         = new QTextEdit(&dialog);
	edit->setPlainText(target->toPlainText());
	dialogLayout->addWidget(edit);
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
	QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	dialogLayout->addWidget(buttons);
	if (dialog.exec() == QDialog::Accepted)
		target->setPlainText(edit->toPlainText());
}

static bool editFilterDialog(QWidget *parent, QString &target, const QString &title)
{
	QDialog dialog(parent);
	dialog.setWindowTitle(title);
	auto *layout = new QVBoxLayout(&dialog);
	auto *edit   = new QTextEdit(&dialog);
	edit->setPlainText(target);
	layout->addWidget(edit);
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
	QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	layout->addWidget(buttons);
	if (dialog.exec() != QDialog::Accepted)
		return false;
	target = edit->toPlainText();
	return true;
}

static void doFindAdvanced(WorldPreferencesDialog *dialog, QTableWidget *table,
                           const std::function<QString(int)> &rowText, QString &findText, int &findIndex,
                           QStringList &history, bool &matchCase, bool &regex, bool &forwards,
                           const QString &title, const bool continueFromCurrent)
{
	if (!table)
		return;
	if (!continueFromCurrent || findText.isEmpty())
	{
		if (!runFindDialog(dialog, history, findText, matchCase, regex, forwards, title))
			return;
		findIndex = -1;
	}
	const int rowCount = table->rowCount();
	if (rowCount <= 0 || findText.isEmpty())
		return;
	int startRow = 0;
	if (continueFromCurrent)
		startRow = findIndex + (forwards ? 1 : -1);
	else
		startRow = forwards ? 0 : rowCount - 1;
	if (startRow < 0)
		startRow = rowCount - 1;
	if (startRow >= rowCount)
		startRow = 0;
	for (int i = 0; i < rowCount; ++i)
	{
		const int row = forwards ? (startRow + i) % rowCount : (startRow - i + rowCount) % rowCount;
		if (const QString haystack = rowText(row); matchFindText(haystack, findText, matchCase, regex))
		{
			findIndex = row;
			selectRow(table, row);
			return;
		}
	}
	QMessageBox::information(dialog, QStringLiteral("Find"),
	                         QStringLiteral("The requested text was not found."));
}

#ifdef QMUD_ENABLE_LUA_SCRIPTING
class LuaFilterRunner
{
	public:
		explicit LuaFilterRunner(const QString &script, QWidget *owner) : m_owner(owner)
		{
			if (script.trimmed().isEmpty())
				return;
			m_state.reset(QMudLuaSupport::makeLuaState());
			if (!m_state)
			{
				showError(QStringLiteral("Unable to create Lua state for filter."));
				return;
			}
			luaL_openlibs(m_state.get());
			QMudLuaSupport::applyLua51Compat(m_state.get());
			qmudLogLua51CompatState(m_state.get(), "WorldPreferencesDialog filter state");
			if (luaL_loadstring(m_state.get(), script.toUtf8().constData()) != 0 ||
			    lua_pcall(m_state.get(), 0, 0, 0) != 0)
			{
				const char *error = lua_tostring(m_state.get(), -1);
				showError(QStringLiteral("Filter error: %1")
				              .arg(error ? QString::fromUtf8(error) : QStringLiteral("unknown error")));
				lua_settop(m_state.get(), 0);
				return;
			}
			lua_getglobal(m_state.get(), "filter");
			if (!lua_isfunction(m_state.get(), -1))
			{
				showError(QStringLiteral("Filter script must define a global function named 'filter'."));
				lua_settop(m_state.get(), 0);
				return;
			}
			lua_pop(m_state.get(), 1);
			m_valid = true;
		}

		~LuaFilterRunner() = default;

		[[nodiscard]] bool isValid() const
		{
			return m_valid;
		}

		bool matches(const QString &name, const std::function<void(lua_State *)> &pushInfo)
		{
			if (!m_valid || !m_state)
				return true;
			lua_getglobal(m_state.get(), "filter");
			if (!lua_isfunction(m_state.get(), -1))
				return true;
			lua_pushstring(m_state.get(), name.toUtf8().constData());
			pushInfo(m_state.get());
			if (lua_pcall(m_state.get(), 2, 1, 0) != 0)
			{
				const char *error = lua_tostring(m_state.get(), -1);
				showError(QStringLiteral("Filter error: %1")
				              .arg(error ? QString::fromUtf8(error) : QStringLiteral("unknown error")));
				lua_settop(m_state.get(), 0);
				m_valid = false;
				return true;
			}
			bool result = true;
			if (lua_isboolean(m_state.get(), -1))
				result = lua_toboolean(m_state.get(), -1) != 0;
			lua_pop(m_state.get(), 1);
			return result;
		}

	private:
		void showError(const QString &message)
		{
			if (m_errorShown)
				return;
			m_errorShown = true;
			QMessageBox::warning(m_owner, QStringLiteral("Filter"), message);
		}

		LuaStateOwner m_state;
		QWidget      *m_owner{nullptr};
		bool          m_valid{false};
		bool          m_errorShown{false};
};
#endif

static bool gridLinesEnabled()
{
	AppController *const app = AppController::instance();
	if (!app)
		return true;
	return app->getGlobalOption(QStringLiteral("ShowGridLinesInListViews")).toInt() != 0;
}

static QString fixHtmlString(const QString &value)
{
	QString result;
	result.reserve(value.size());
	for (QChar c : value)
	{
		switch (c.unicode())
		{
		case '<':
			result += QStringLiteral("&lt;");
			break;
		case '>':
			result += QStringLiteral("&gt;");
			break;
		case '&':
			result += QStringLiteral("&amp;");
			break;
		case '"':
			result += QStringLiteral("&quot;");
			break;
		default:
			result += c;
			break;
		}
	}
	return result;
}

static QString fixHtmlMultilineString(const QString &value)
{
	QString result;
	result.reserve(value.size());
	for (QChar c : value)
	{
		switch (c.unicode())
		{
		case '<':
			result += QStringLiteral("&lt;");
			break;
		case '>':
			result += QStringLiteral("&gt;");
			break;
		case '&':
			result += QStringLiteral("&amp;");
			break;
		case '\t':
			result += QStringLiteral("&#9;");
			break;
		default:
			result += c;
			break;
		}
	}
	return result;
}

WorldPreferencesDialog::WorldPreferencesDialog(WorldRuntime *runtime, WorldView *view, QWidget *parent)
    : QDialog(parent), m_runtime(runtime), m_view(view)
{
	setWindowTitle(QStringLiteral("World Configuration"));
	resize(700, 520);
	buildUi();
	setSizeGripEnabled(true);
	if (QScreen *screen = QGuiApplication::primaryScreen())
	{
		const QRect avail = screen->availableGeometry();
		setMaximumSize(avail.size());
		QSize start = size();
		start.setWidth(qMin(start.width(), avail.width()));
		start.setHeight(qMin(start.height(), avail.height()));
		resize(start);
	}

	if (AppController *app = AppController::instance())
	{
		QSettings settings(app->iniFilePath(), QSettings::IniFormat);
		settings.beginGroup(QStringLiteral("WorldPreferencesDialog"));
		if (const QByteArray geometry = settings.value(QStringLiteral("Geometry")).toByteArray();
		    !geometry.isEmpty())
			restoreGeometry(geometry);
		if (const int pageIndex = settings.value(QStringLiteral("PageIndex"), 0).toInt();
		    m_pages && m_pageTree && pageIndex >= 0 && pageIndex < m_pages->count())
			setInitialPage(static_cast<Page>(pageIndex));
		settings.endGroup();

		connect(this, &QDialog::finished, this,
		        [this, app](int)
		        {
			        QSettings persistedSettings(app->iniFilePath(), QSettings::IniFormat);
			        persistedSettings.beginGroup(QStringLiteral("WorldPreferencesDialog"));
			        persistedSettings.setValue(QStringLiteral("Geometry"), saveGeometry());
			        if (m_pages)
				        persistedSettings.setValue(QStringLiteral("PageIndex"), m_pages->currentIndex());
			        persistedSettings.endGroup();
		        });
	}
}

void WorldPreferencesDialog::setInitialPage(const Page page)
{
	if (!m_pageTree || !m_pages)
		return;
	const int index = page;
	if (index >= m_pages->count())
		return;
	const auto it = m_pageItems.find(index);
	if (it == m_pageItems.end())
		return;
	QTreeWidgetItem *item = it.value();
	if (!item)
		return;
	if (QTreeWidgetItem *parent = item->parent())
		parent->setExpanded(true);
	m_pageTree->setCurrentItem(item);
	m_pages->setCurrentIndex(index);
	if (m_runtime)
		m_runtime->setLastPreferencesPage(index);
}

void WorldPreferencesDialog::accept()
{
	QMap<QString, QString> worldAttributesBeforeApply;
	QMap<QString, QString> worldMultilineBeforeApply;
	if (m_runtime)
	{
		auto readSwatchValue = [](const QLineEdit *edit) -> QString
		{
			if (!edit)
				return {};
			QString stored = edit->property("colour_value").toString();
			if (!stored.isEmpty())
				return stored;
			return edit->text();
		};
		if (m_worldName && m_worldName->text().trimmed().isEmpty())
		{
			QMessageBox::warning(this, QStringLiteral("World Configuration"),
			                     QStringLiteral("Your world name cannot be blank.\n\n"
			                                    "You must fill in your world name, TCP/IP address and "
			                                    "port number before tabbing to other configuration screens"));
			return;
		}
		if (m_host && m_host->text().trimmed().isEmpty())
		{
			QMessageBox::warning(this, QStringLiteral("World Configuration"),
			                     QStringLiteral("The world IP address cannot be blank."));
			return;
		}
		if (m_port && m_port->value() == 0)
		{
			QMessageBox::warning(this, QStringLiteral("World Configuration"),
			                     QStringLiteral("The world port number must be specified."));
			return;
		}
		if (m_useDefaultColours && m_useDefaultColours->isChecked() &&
		    m_useDefaultColours->isChecked() != m_initialUseDefaultColours && hasDefaultColoursFile())
		{
			const QMessageBox::StandardButton result = QMessageBox::question(
			    this, QStringLiteral("World Configuration"),
			    QStringLiteral("By checking the option \"Override with default colours\" "
			                   " your existing colours will be PERMANENTLY discarded next time "
			                   "you open this world.\n\nAre you SURE you want to do this?"),
			    QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
			if (result != QMessageBox::Yes)
				return;
		}
		if (m_useDefaultMacros && m_useDefaultMacros->isChecked() &&
		    m_useDefaultMacros->isChecked() != m_initialUseDefaultMacros && hasDefaultMacrosFile())
		{
			const QMessageBox::StandardButton result = QMessageBox::question(
			    this, QStringLiteral("World Configuration"),
			    QStringLiteral("By checking the option \"Override with default macros\" "
			                   " your existing macros will be PERMANENTLY discarded next time "
			                   "you open this world.\n\nAre you SURE you want to do this?"),
			    QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
			if (result != QMessageBox::Yes)
				return;
		}
		if (m_useDefaultTriggers && m_useDefaultTriggers->isChecked() &&
		    m_useDefaultTriggers->isChecked() != m_initialUseDefaultTriggers && hasDefaultTriggersFile())
		{
			if (const qsizetype count = m_runtime->triggers().size(); count > 0)
			{
				const QMessageBox::StandardButton result = QMessageBox::question(
				    this, QStringLiteral("World Configuration"),
				    QStringLiteral("By checking the option \"Override with default triggers\" "
				                   " your existing %1 trigger%2 will be PERMANENTLY discarded next time "
				                   "you open this world.\n\nAre you SURE you want to do this?")
				        .arg(count)
				        .arg(count == 1 ? QString() : QStringLiteral("s")),
				    QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
				if (result != QMessageBox::Yes)
					return;
			}
		}
		if (m_useDefaultAliases && m_useDefaultAliases->isChecked() &&
		    m_useDefaultAliases->isChecked() != m_initialUseDefaultAliases && hasDefaultAliasesFile())
		{
			if (const qsizetype count = m_runtime->aliases().size(); count > 0)
			{
				const QMessageBox::StandardButton result = QMessageBox::question(
				    this, QStringLiteral("World Configuration"),
				    QStringLiteral("By checking the option \"Override with default aliases\" "
				                   " your existing %1 alias%2 will be PERMANENTLY discarded next time "
				                   "you open this world.\n\nAre you SURE you want to do this?")
				        .arg(count)
				        .arg(count == 1 ? QString() : QStringLiteral("es")),
				    QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
				if (result != QMessageBox::Yes)
					return;
			}
		}
		if (m_useDefaultTimers && m_useDefaultTimers->isChecked() &&
		    m_useDefaultTimers->isChecked() != m_initialUseDefaultTimers && hasDefaultTimersFile())
		{
			if (const qsizetype count = m_runtime->timers().size(); count > 0)
			{
				const QMessageBox::StandardButton result = QMessageBox::question(
				    this, QStringLiteral("World Configuration"),
				    QStringLiteral("By checking the option \"Override with default timers\" "
				                   " your existing %1 timer%2 will be PERMANENTLY discarded next time "
				                   "you open this world.\n\nAre you SURE you want to do this?")
				        .arg(count)
				        .arg(count == 1 ? QString() : QStringLiteral("s")),
				    QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
				if (result != QMessageBox::Yes)
					return;
			}
		}
		if (m_proxyType)
		{
			if (const int proxyType = m_proxyType->currentData().toInt();
			    proxyType == eProxyServerSocks4 || proxyType == eProxyServerSocks5)
			{
				const QString server = m_proxyServer ? m_proxyServer->text().trimmed() : QString();
				const int     port   = m_proxyPort ? m_proxyPort->value() : 0;
				if (server.isEmpty())
				{
					QMessageBox::warning(this, QStringLiteral("Proxy"),
					                     QStringLiteral("The proxy server address cannot be blank."));
					return;
				}
				if (port == 0)
				{
					QMessageBox::warning(this, QStringLiteral("Proxy"),
					                     QStringLiteral("The proxy server port must be specified."));
					return;
				}
			}
		}
		if (m_connectMethod && m_playerName)
		{
			if (const int method = m_connectMethod->currentData().toInt();
			    method != eNoAutoConnect && m_playerName->text().trimmed().isEmpty())
			{
				QMessageBox::warning(this, QStringLiteral("Connecting"),
				                     QStringLiteral("Your character name cannot be blank for auto-connect."));
				return;
			}
		}
		if (m_enableCommandStack && m_commandStackCharacter)
		{
			if (m_enableCommandStack->isChecked())
			{
				const QString value = m_commandStackCharacter->text();
				if (value.isEmpty())
				{
					QMessageBox::warning(this, QStringLiteral("Commands"),
					                     QStringLiteral("You must supply a command stack character."));
					return;
				}
				if (value.at(0).isSpace() || !value.at(0).isPrint())
				{
					QMessageBox::warning(this, QStringLiteral("Commands"),
					                     QStringLiteral("The command stack character is invalid."));
					return;
				}
			}
		}
		if (m_enableSpeedWalk && m_speedWalkPrefix)
		{
			if (m_enableSpeedWalk->isChecked() && m_speedWalkPrefix->text().isEmpty())
			{
				QMessageBox::warning(this, QStringLiteral("Commands"),
				                     QStringLiteral("You must supply a speed-walk prefix."));
				return;
			}
		}
		if (m_enableAutoSay && m_autoSayString)
		{
			if (m_enableAutoSay->isChecked() && m_autoSayString->text().trimmed().isEmpty())
			{
				QMessageBox::warning(this, QStringLiteral("Auto say"),
				                     QStringLiteral("Your \"auto say\" string cannot be blank"));
				return;
			}
		}
		if (m_maxLines)
		{
			const int oldLines = worldMaxOutputLines(m_runtime->worldAttributes());
			if (const int newLines = m_maxLines->value(); newLines != oldLines && newLines > 1000)
			{
				if (const quint64 totalPhys = totalPhysicalMemoryBytes(); totalPhys > 0)
				{
					const quint64 bytesNeeded =
					    (16ULL * 1024ULL * 1024ULL) + (static_cast<quint64>(newLines) * 60ULL);
					if (bytesNeeded > totalPhys)
					{
						const QString message =
						    QStringLiteral("You are allocating %1 lines for your output buffer, but have "
						                   "only %2 MB of physical "
						                   "RAM. This is not recommended. Do you wish to continue anyway?")
						        .arg(newLines)
						        .arg(totalPhys / (1024ULL * 1024ULL));
						const QMessageBox::StandardButton reply =
						    QMessageBox::question(this, QStringLiteral("Output buffer"), message,
						                          QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
						if (reply != QMessageBox::Yes)
							return;
					}
				}
			}
		}
		if (m_logOutput && m_logRaw)
		{
			if (!m_logOutput->isChecked() && !m_logRaw->isChecked())
			{
				const QMessageBox::StandardButton reply = QMessageBox::question(
				    this, QStringLiteral("Logging"),
				    QStringLiteral("You are not logging output from the MUD - is this intentional?"),
				    QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
				if (reply != QMessageBox::Yes)
					return;
			}
		}

		worldAttributesBeforeApply          = m_runtime->worldAttributes();
		worldMultilineBeforeApply           = m_runtime->worldMultilineAttributes();
		const bool    hadLogOpenBeforeApply = m_runtime->isLogOpen();
		const QString previousAutoLogFileName =
		    worldAttributesBeforeApply.value(QStringLiteral("auto_log_file_name"));

		if (m_worldName)
			m_runtime->setWorldAttribute(QStringLiteral("name"), m_worldName->text().trimmed());
		if (m_infoWorldId && !m_infoWorldId->text().trimmed().isEmpty())
			m_runtime->setWorldAttribute(QStringLiteral("id"), m_infoWorldId->text().trimmed());
		if (m_host)
		{
			const QString host = m_host->text().trimmed();
			m_runtime->setWorldAttribute(QStringLiteral("site"), host);
		}
		if (m_port)
			m_runtime->setWorldAttribute(QStringLiteral("port"), QString::number(m_port->value()));
		if (m_saveWorldAutomatically)
			m_runtime->setWorldAttribute(QStringLiteral("save_world_automatically"),
			                             m_saveWorldAutomatically->isChecked() ? QStringLiteral("1")
			                                                                   : QStringLiteral("0"));
		if (m_autosaveMinutes)
			m_runtime->setWorldAttribute(QStringLiteral("autosave_minutes"),
			                             QString::number(m_autosaveMinutes->value()));
		if (m_proxyType)
			m_runtime->setWorldAttribute(QStringLiteral("proxy_type"),
			                             QString::number(m_proxyType->currentData().toInt()));
		if (m_proxyServer)
			m_runtime->setWorldAttribute(QStringLiteral("proxy_server"), m_proxyServer->text().trimmed());
		if (m_proxyPort)
			m_runtime->setWorldAttribute(QStringLiteral("proxy_port"), QString::number(m_proxyPort->value()));
		m_runtime->setWorldAttribute(QStringLiteral("proxy_username"), m_proxyUsername);
		m_runtime->setWorldAttribute(QStringLiteral("proxy_password"), m_proxyPassword);

		if (m_beepSound)
			m_runtime->setWorldAttribute(QStringLiteral("beep_sound"), m_beepSound->text());
		if (m_newActivitySound)
			m_runtime->setWorldAttribute(QStringLiteral("new_activity_sound"), m_newActivitySound->text());
		if (m_playSoundsInBackground)
			m_runtime->setWorldAttribute(QStringLiteral("play_sounds_in_background"),
			                             boolAttributeValue(m_playSoundsInBackground->isChecked()));

		if (m_logOutput)
			m_runtime->setWorldAttribute(QStringLiteral("log_output"), m_logOutput->isChecked()
			                                                               ? QStringLiteral("1")
			                                                               : QStringLiteral("0"));
		if (m_logInput)
			m_runtime->setWorldAttribute(QStringLiteral("log_input"),
			                             m_logInput->isChecked() ? QStringLiteral("1") : QStringLiteral("0"));
		if (m_logNotes)
			m_runtime->setWorldAttribute(QStringLiteral("log_notes"),
			                             m_logNotes->isChecked() ? QStringLiteral("1") : QStringLiteral("0"));
		if (m_logHtml)
			m_runtime->setWorldAttribute(QStringLiteral("log_html"),
			                             m_logHtml->isChecked() ? QStringLiteral("1") : QStringLiteral("0"));
		if (m_logRaw)
			m_runtime->setWorldAttribute(QStringLiteral("log_raw"),
			                             m_logRaw->isChecked() ? QStringLiteral("1") : QStringLiteral("0"));
		if (m_logInColour)
			m_runtime->setWorldAttribute(QStringLiteral("log_in_colour"), m_logInColour->isChecked()
			                                                                  ? QStringLiteral("1")
			                                                                  : QStringLiteral("0"));
		if (m_useDefaultColours)
			m_runtime->setWorldAttribute(QStringLiteral("use_default_colours"),
			                             m_useDefaultColours->isChecked() ? QStringLiteral("1")
			                                                              : QStringLiteral("0"));
		if (m_useDefaultMacros)
			m_runtime->setWorldAttribute(QStringLiteral("use_default_macros"), m_useDefaultMacros->isChecked()
			                                                                       ? QStringLiteral("1")
			                                                                       : QStringLiteral("0"));
		if (m_custom16IsDefaultColour)
			m_runtime->setWorldAttribute(QStringLiteral("custom_16_is_default_colour"),
			                             m_custom16IsDefaultColour->isChecked() ? QStringLiteral("1")
			                                                                    : QStringLiteral("0"));
		if (m_writeWorldNameToLog)
			m_runtime->setWorldAttribute(QStringLiteral("write_world_name_to_log"),
			                             m_writeWorldNameToLog->isChecked() ? QStringLiteral("1")
			                                                                : QStringLiteral("0"));
		if (m_logRotateMb)
			m_runtime->setWorldAttribute(QStringLiteral("log_rotate_mb"),
			                             QString::number(m_logRotateMb->value()));
		if (m_logRotateGzip)
			m_runtime->setWorldAttribute(QStringLiteral("log_rotate_gzip"), m_logRotateGzip->isChecked()
			                                                                    ? QStringLiteral("1")
			                                                                    : QStringLiteral("0"));
		if (m_autoLogFileName)
			m_runtime->setWorldAttribute(QStringLiteral("auto_log_file_name"), m_autoLogFileName->text());
		if (m_logFilePreamble)
			m_runtime->setWorldMultilineAttribute(QStringLiteral("log_file_preamble"),
			                                      m_logFilePreamble->toPlainText());
		if (m_logFilePostamble)
			m_runtime->setWorldMultilineAttribute(QStringLiteral("log_file_postamble"),
			                                      m_logFilePostamble->toPlainText());
		if (m_logLinePreambleOutput)
			m_runtime->setWorldAttribute(QStringLiteral("log_line_preamble_output"),
			                             m_logLinePreambleOutput->text());
		if (m_logLinePreambleInput)
			m_runtime->setWorldAttribute(QStringLiteral("log_line_preamble_input"),
			                             m_logLinePreambleInput->text());
		if (m_logLinePreambleNotes)
			m_runtime->setWorldAttribute(QStringLiteral("log_line_preamble_notes"),
			                             m_logLinePreambleNotes->text());
		if (m_logLinePostambleOutput)
			m_runtime->setWorldAttribute(QStringLiteral("log_line_postamble_output"),
			                             m_logLinePostambleOutput->text());
		if (m_logLinePostambleInput)
			m_runtime->setWorldAttribute(QStringLiteral("log_line_postamble_input"),
			                             m_logLinePostambleInput->text());
		if (m_logLinePostambleNotes)
			m_runtime->setWorldAttribute(QStringLiteral("log_line_postamble_notes"),
			                             m_logLinePostambleNotes->text());

		if (m_wrapColumn)
			m_runtime->setWorldAttribute(QStringLiteral("wrap_column"),
			                             QString::number(m_wrapColumn->value()));
		if (m_maxLines)
		{
			const QString maxLinesValue = QString::number(m_maxLines->value());
			m_runtime->setWorldAttribute(QStringLiteral("max_output_lines"), maxLinesValue);
		}
		if (m_wrapOutput)
			m_runtime->setWorldAttribute(QStringLiteral("wrap"), m_wrapOutput->isChecked()
			                                                         ? QStringLiteral("1")
			                                                         : QStringLiteral("0"));
		if (m_autoWrapWindow)
			m_runtime->setWorldAttribute(QStringLiteral("auto_wrap_window_width"),
			                             m_autoWrapWindow->isChecked() ? QStringLiteral("1")
			                                                           : QStringLiteral("0"));
		if (m_lineInformation)
			m_runtime->setWorldAttribute(QStringLiteral("line_information"), m_lineInformation->isChecked()
			                                                                     ? QStringLiteral("1")
			                                                                     : QStringLiteral("0"));
		if (m_startPaused)
			m_runtime->setWorldAttribute(QStringLiteral("start_paused"), m_startPaused->isChecked()
			                                                                 ? QStringLiteral("1")
			                                                                 : QStringLiteral("0"));
		if (m_autoPause)
			m_runtime->setWorldAttribute(QStringLiteral("auto_pause"), m_autoPause->isChecked()
			                                                               ? QStringLiteral("1")
			                                                               : QStringLiteral("0"));
		if (m_unpauseOnSend)
			m_runtime->setWorldAttribute(QStringLiteral("unpause_on_send"), m_unpauseOnSend->isChecked()
			                                                                    ? QStringLiteral("1")
			                                                                    : QStringLiteral("0"));
		if (m_keepPauseAtBottomOption)
			m_runtime->setWorldAttribute(QStringLiteral("keep_pause_at_bottom"),
			                             boolAttributeValue(m_keepPauseAtBottomOption->isChecked()));
		if (m_doNotShowOutstandingLines)
			m_runtime->setWorldAttribute(QStringLiteral("do_not_show_outstanding_lines"),
			                             boolAttributeValue(m_doNotShowOutstandingLines->isChecked()));
		if (m_indentParas)
			m_runtime->setWorldAttribute(QStringLiteral("indent_paras"), m_indentParas->isChecked()
			                                                                 ? QStringLiteral("1")
			                                                                 : QStringLiteral("0"));
		if (m_alternativeInverse)
			m_runtime->setWorldAttribute(QStringLiteral("alternative_inverse"),
			                             m_alternativeInverse->isChecked() ? QStringLiteral("1")
			                                                               : QStringLiteral("0"));
		if (m_enableBeeps)
			m_runtime->setWorldAttribute(QStringLiteral("enable_beeps"), m_enableBeeps->isChecked()
			                                                                 ? QStringLiteral("1")
			                                                                 : QStringLiteral("0"));
		if (m_disableCompression)
			m_runtime->setWorldAttribute(QStringLiteral("disable_compression"),
			                             m_disableCompression->isChecked() ? QStringLiteral("1")
			                                                               : QStringLiteral("0"));
		if (m_flashIcon)
			m_runtime->setWorldAttribute(QStringLiteral("flash_taskbar_icon"), m_flashIcon->isChecked()
			                                                                       ? QStringLiteral("1")
			                                                                       : QStringLiteral("0"));
		if (m_showBold)
			m_runtime->setWorldAttribute(QStringLiteral("show_bold"),
			                             m_showBold->isChecked() ? QStringLiteral("1") : QStringLiteral("0"));
		if (m_showItalic)
			m_runtime->setWorldAttribute(QStringLiteral("show_italic"), m_showItalic->isChecked()
			                                                                ? QStringLiteral("1")
			                                                                : QStringLiteral("0"));
		if (m_showUnderline)
			m_runtime->setWorldAttribute(QStringLiteral("show_underline"), m_showUnderline->isChecked()
			                                                                   ? QStringLiteral("1")
			                                                                   : QStringLiteral("0"));
		if (m_useDefaultOutputFont)
			m_runtime->setWorldAttribute(QStringLiteral("use_default_output_font"),
			                             m_useDefaultOutputFont->isChecked() ? QStringLiteral("1")
			                                                                 : QStringLiteral("0"));
		if (m_outputFontName)
			m_runtime->setWorldAttribute(QStringLiteral("output_font_name"), m_outputFontName->text());
		if (m_outputFontHeight)
			m_runtime->setWorldAttribute(QStringLiteral("output_font_height"),
			                             QString::number(m_outputFontHeight->value()));
		m_runtime->setWorldAttribute(QStringLiteral("output_font_weight"),
		                             QString::number(m_outputFontWeight));
		m_runtime->setWorldAttribute(QStringLiteral("output_font_charset"),
		                             QString::number(m_outputFontCharset));
		if (m_lineSpacing)
			m_runtime->setWorldAttribute(QStringLiteral("line_spacing"),
			                             QString::number(m_lineSpacing->value()));
		if (m_pixelOffset)
			m_runtime->setWorldAttribute(QStringLiteral("pixel_offset"),
			                             QString::number(m_pixelOffset->value()));
		if (m_naws)
			m_runtime->setWorldAttribute(QStringLiteral("naws"),
			                             m_naws->isChecked() ? QStringLiteral("1") : QStringLiteral("0"));
		if (m_terminalIdentification)
			m_runtime->setWorldAttribute(QStringLiteral("terminal_identification"),
			                             m_terminalIdentification->text());
		if (m_showConnectDisconnect)
			m_runtime->setWorldAttribute(QStringLiteral("show_connect_disconnect"),
			                             m_showConnectDisconnect->isChecked() ? QStringLiteral("1")
			                                                                  : QStringLiteral("0"));
		if (m_copySelectionToClipboard)
			m_runtime->setWorldAttribute(QStringLiteral("copy_selection_to_clipboard"),
			                             m_copySelectionToClipboard->isChecked() ? QStringLiteral("1")
			                                                                     : QStringLiteral("0"));
		if (m_autoCopyHtml)
			m_runtime->setWorldAttribute(QStringLiteral("auto_copy_to_clipboard_in_html"),
			                             m_autoCopyHtml->isChecked() ? QStringLiteral("1")
			                                                         : QStringLiteral("0"));
		if (m_utf8)
			m_runtime->setWorldAttribute(QStringLiteral("utf_8"),
			                             m_utf8->isChecked() ? QStringLiteral("1") : QStringLiteral("0"));
		if (m_carriageReturnClearsLine)
			m_runtime->setWorldAttribute(QStringLiteral("carriage_return_clears_line"),
			                             m_carriageReturnClearsLine->isChecked() ? QStringLiteral("1")
			                                                                     : QStringLiteral("0"));
		if (m_convertGaToNewline)
			m_runtime->setWorldAttribute(QStringLiteral("convert_ga_to_newline"),
			                             m_convertGaToNewline->isChecked() ? QStringLiteral("1")
			                                                               : QStringLiteral("0"));
		if (m_sendKeepAlives)
			m_runtime->setWorldAttribute(QStringLiteral("send_keep_alives"), m_sendKeepAlives->isChecked()
			                                                                     ? QStringLiteral("1")
			                                                                     : QStringLiteral("0"));
		if (m_persistOutputBuffer)
			m_runtime->setWorldAttribute(QStringLiteral("persist_output_buffer"),
			                             boolAttributeValue(m_persistOutputBuffer->isChecked()));
		if (m_toolTipVisibleTime)
			m_runtime->setWorldAttribute(QStringLiteral("tool_tip_visible_time"),
			                             QString::number(m_toolTipVisibleTime->value()));
		if (m_toolTipStartTime)
			m_runtime->setWorldAttribute(QStringLiteral("tool_tip_start_time"),
			                             QString::number(m_toolTipStartTime->value()));
		if (m_fadeOutputBufferAfterSeconds)
			m_runtime->setWorldAttribute(QStringLiteral("fade_output_buffer_after_seconds"),
			                             QString::number(m_fadeOutputBufferAfterSeconds->value()));
		if (m_fadeOutputOpacityPercent)
			m_runtime->setWorldAttribute(QStringLiteral("fade_output_opacity_percent"),
			                             QString::number(m_fadeOutputOpacityPercent->value()));
		if (m_fadeOutputSeconds)
			m_runtime->setWorldAttribute(QStringLiteral("fade_output_seconds"),
			                             QString::number(m_fadeOutputSeconds->value()));

		if (m_arrowsChangeHistory)
			m_runtime->setWorldAttribute(QStringLiteral("arrows_change_history"),
			                             m_arrowsChangeHistory->isChecked() ? QStringLiteral("1")
			                                                                : QStringLiteral("0"));
		if (m_arrowKeysWrap)
			m_runtime->setWorldAttribute(QStringLiteral("arrow_keys_wrap"), m_arrowKeysWrap->isChecked()
			                                                                    ? QStringLiteral("1")
			                                                                    : QStringLiteral("0"));
		if (m_arrowRecallsPartial)
			m_runtime->setWorldAttribute(QStringLiteral("arrow_recalls_partial"),
			                             m_arrowRecallsPartial->isChecked() ? QStringLiteral("1")
			                                                                : QStringLiteral("0"));
		if (m_altArrowRecallsPartial)
			m_runtime->setWorldAttribute(QStringLiteral("alt_arrow_recalls_partial"),
			                             m_altArrowRecallsPartial->isChecked() ? QStringLiteral("1")
			                                                                   : QStringLiteral("0"));
		if (m_keepCommandsOnSameLine)
			m_runtime->setWorldAttribute(QStringLiteral("keep_commands_on_same_line"),
			                             m_keepCommandsOnSameLine->isChecked() ? QStringLiteral("1")
			                                                                   : QStringLiteral("0"));
		if (m_confirmBeforeReplacingTyping)
			m_runtime->setWorldAttribute(QStringLiteral("confirm_before_replacing_typing"),
			                             m_confirmBeforeReplacingTyping->isChecked() ? QStringLiteral("1")
			                                                                         : QStringLiteral("0"));
		if (m_escapeDeletesInput)
			m_runtime->setWorldAttribute(QStringLiteral("escape_deletes_input"),
			                             m_escapeDeletesInput->isChecked() ? QStringLiteral("1")
			                                                               : QStringLiteral("0"));
		if (m_doubleClickInserts)
			m_runtime->setWorldAttribute(QStringLiteral("double_click_inserts"),
			                             m_doubleClickInserts->isChecked() ? QStringLiteral("1")
			                                                               : QStringLiteral("0"));
		if (m_doubleClickSends)
			m_runtime->setWorldAttribute(QStringLiteral("double_click_sends"), m_doubleClickSends->isChecked()
			                                                                       ? QStringLiteral("1")
			                                                                       : QStringLiteral("0"));
		if (m_saveDeletedCommand)
			m_runtime->setWorldAttribute(QStringLiteral("save_deleted_command"),
			                             m_saveDeletedCommand->isChecked() ? QStringLiteral("1")
			                                                               : QStringLiteral("0"));
		if (m_ctrlZToEnd)
			m_runtime->setWorldAttribute(QStringLiteral("ctrl_z_goes_to_end_of_buffer"),
			                             m_ctrlZToEnd->isChecked() ? QStringLiteral("1")
			                                                       : QStringLiteral("0"));
		if (m_ctrlPToPrev)
			m_runtime->setWorldAttribute(QStringLiteral("ctrl_p_goes_to_previous_command"),
			                             m_ctrlPToPrev->isChecked() ? QStringLiteral("1")
			                                                        : QStringLiteral("0"));
		if (m_ctrlNToNext)
			m_runtime->setWorldAttribute(QStringLiteral("ctrl_n_goes_to_next_command"),
			                             m_ctrlNToNext->isChecked() ? QStringLiteral("1")
			                                                        : QStringLiteral("0"));
		if (m_ctrlBackspaceDeletesLastWord)
			m_runtime->setWorldAttribute(QStringLiteral("ctrl_backspace_deletes_last_word"),
			                             boolAttributeValue(m_ctrlBackspaceDeletesLastWord->isChecked()));
		if (m_enableCommandStack)
			m_runtime->setWorldAttribute(QStringLiteral("enable_command_stack"),
			                             m_enableCommandStack->isChecked() ? QStringLiteral("1")
			                                                               : QStringLiteral("0"));
		if (m_commandStackCharacter)
		{
			QString value = m_commandStackCharacter->text();
			if (m_enableCommandStack && !m_enableCommandStack->isChecked() && value.isEmpty())
				value = QStringLiteral(" ");
			m_runtime->setWorldAttribute(QStringLiteral("command_stack_character"), value);
		}
		if (m_enableSpeedWalk)
			m_runtime->setWorldAttribute(QStringLiteral("enable_speed_walk"), m_enableSpeedWalk->isChecked()
			                                                                      ? QStringLiteral("1")
			                                                                      : QStringLiteral("0"));
		if (m_speedWalkPrefix)
			m_runtime->setWorldAttribute(QStringLiteral("speed_walk_prefix"), m_speedWalkPrefix->text());
		if (m_speedWalkFiller)
			m_runtime->setWorldAttribute(QStringLiteral("speed_walk_filler"), m_speedWalkFiller->text());
		if (m_speedWalkDelay)
			m_runtime->setWorldAttribute(QStringLiteral("speed_walk_delay"),
			                             QString::number(m_speedWalkDelay->value()));
		if (m_displayMyInput)
			m_runtime->setWorldAttribute(QStringLiteral("display_my_input"), m_displayMyInput->isChecked()
			                                                                     ? QStringLiteral("1")
			                                                                     : QStringLiteral("0"));
		if (m_historyLines)
			m_runtime->setWorldAttribute(QStringLiteral("history_lines"),
			                             QString::number(m_historyLines->value()));
		if (m_persistCommandHistory)
			m_runtime->setWorldAttribute(QStringLiteral("persist_command_history"),
			                             boolAttributeValue(m_persistCommandHistory->isChecked()));
		if (m_alwaysRecordCommandHistory)
			m_runtime->setWorldAttribute(QStringLiteral("always_record_command_history"),
			                             boolAttributeValue(m_alwaysRecordCommandHistory->isChecked()));
		if (m_doNotAddMacrosToCommandHistory)
			m_runtime->setWorldAttribute(QStringLiteral("do_not_add_macros_to_command_history"),
			                             boolAttributeValue(m_doNotAddMacrosToCommandHistory->isChecked()));
		if (m_autoResizeCommandWindow)
			m_runtime->setWorldAttribute(QStringLiteral("auto_resize_command_window"),
			                             boolAttributeValue(m_autoResizeCommandWindow->isChecked()));
		if (m_autoResizeMinimumLines)
			m_runtime->setWorldAttribute(QStringLiteral("auto_resize_minimum_lines"),
			                             QString::number(m_autoResizeMinimumLines->value()));
		if (m_autoResizeMaximumLines)
			m_runtime->setWorldAttribute(QStringLiteral("auto_resize_maximum_lines"),
			                             QString::number(m_autoResizeMaximumLines->value()));
		if (m_autoRepeat)
			m_runtime->setWorldAttribute(QStringLiteral("auto_repeat"), m_autoRepeat->isChecked()
			                                                                ? QStringLiteral("1")
			                                                                : QStringLiteral("0"));
		if (m_translateGerman)
			m_runtime->setWorldAttribute(QStringLiteral("translate_german"), m_translateGerman->isChecked()
			                                                                     ? QStringLiteral("1")
			                                                                     : QStringLiteral("0"));
		if (m_spellCheckOnSend)
			m_runtime->setWorldAttribute(QStringLiteral("spell_check_on_send"),
			                             m_spellCheckOnSend->isChecked() ? QStringLiteral("1")
			                                                             : QStringLiteral("0"));
		if (m_lowerCaseTabCompletion)
			m_runtime->setWorldAttribute(QStringLiteral("lower_case_tab_completion"),
			                             m_lowerCaseTabCompletion->isChecked() ? QStringLiteral("1")
			                                                                   : QStringLiteral("0"));
		if (m_translateBackslash)
			m_runtime->setWorldAttribute(QStringLiteral("translate_backslash_sequences"),
			                             m_translateBackslash->isChecked() ? QStringLiteral("1")
			                                                               : QStringLiteral("0"));
		if (m_tabCompletionDefaults)
			m_runtime->setWorldMultilineAttribute(QStringLiteral("tab_completion_defaults"),
			                                      m_tabCompletionDefaults->toPlainText());
		if (m_tabCompletionLines)
			m_runtime->setWorldAttribute(QStringLiteral("tab_completion_lines"),
			                             QString::number(m_tabCompletionLines->value()));
		if (m_tabCompletionSpace)
			m_runtime->setWorldAttribute(QStringLiteral("tab_completion_space"),
			                             m_tabCompletionSpace->isChecked() ? QStringLiteral("1")
			                                                               : QStringLiteral("0"));
		if (m_useDefaultInputFont)
			m_runtime->setWorldAttribute(QStringLiteral("use_default_input_font"),
			                             m_useDefaultInputFont->isChecked() ? QStringLiteral("1")
			                                                                : QStringLiteral("0"));
		if (m_inputFontName)
			m_runtime->setWorldAttribute(QStringLiteral("input_font_name"), m_inputFontName->text());
		if (m_inputFontHeight)
			m_runtime->setWorldAttribute(QStringLiteral("input_font_height"),
			                             QString::number(m_inputFontHeight->value()));
		m_runtime->setWorldAttribute(QStringLiteral("input_font_weight"), QString::number(m_inputFontWeight));
		m_runtime->setWorldAttribute(QStringLiteral("input_font_italic"),
		                             m_inputFontItalic ? QStringLiteral("1") : QStringLiteral("0"));
		m_runtime->setWorldAttribute(QStringLiteral("input_font_charset"),
		                             QString::number(m_inputFontCharset));
		if (m_noEchoOff)
			m_runtime->setWorldAttribute(QStringLiteral("no_echo_off"), m_noEchoOff->isChecked()
			                                                                ? QStringLiteral("1")
			                                                                : QStringLiteral("0"));
		if (m_enableSpamPrevention)
			m_runtime->setWorldAttribute(QStringLiteral("enable_spam_prevention"),
			                             m_enableSpamPrevention->isChecked() ? QStringLiteral("1")
			                                                                 : QStringLiteral("0"));
		if (m_spamLineCount)
			m_runtime->setWorldAttribute(QStringLiteral("spam_line_count"),
			                             QString::number(m_spamLineCount->value()));
		if (m_spamMessage)
			m_runtime->setWorldAttribute(QStringLiteral("spam_message"), m_spamMessage->text());
		if (m_inputTextColour)
			m_runtime->setWorldAttribute(QStringLiteral("input_text_colour"), m_inputTextColour->text());
		if (m_inputBackColour)
			m_runtime->setWorldAttribute(QStringLiteral("input_background_colour"),
			                             m_inputBackColour->text());
		if (m_echoColour)
			m_runtime->setWorldAttribute(QStringLiteral("echo_colour"),
			                             QString::number(m_echoColour->currentIndex()));

		if (m_enableAliases)
			m_runtime->setWorldAttribute(QStringLiteral("enable_aliases"), m_enableAliases->isChecked()
			                                                                   ? QStringLiteral("1")
			                                                                   : QStringLiteral("0"));
		if (m_useDefaultAliases)
			m_runtime->setWorldAttribute(QStringLiteral("use_default_aliases"),
			                             m_useDefaultAliases->isChecked() ? QStringLiteral("1")
			                                                              : QStringLiteral("0"));
		if (m_aliasTreeView)
			m_runtime->setWorldAttribute(QStringLiteral("treeview_aliases"), m_aliasTreeView->isChecked()
			                                                                     ? QStringLiteral("1")
			                                                                     : QStringLiteral("0"));
		if (m_defaultAliasExpandVariables)
			m_runtime->setWorldAttribute(QStringLiteral("default_alias_expand_variables"),
			                             boolAttributeValue(m_defaultAliasExpandVariables->isChecked()));
		if (m_defaultAliasIgnoreCase)
			m_runtime->setWorldAttribute(QStringLiteral("default_alias_ignore_case"),
			                             boolAttributeValue(m_defaultAliasIgnoreCase->isChecked()));
		if (m_defaultAliasKeepEvaluating)
			m_runtime->setWorldAttribute(QStringLiteral("default_alias_keep_evaluating"),
			                             boolAttributeValue(m_defaultAliasKeepEvaluating->isChecked()));
		if (m_defaultAliasRegexp)
			m_runtime->setWorldAttribute(QStringLiteral("default_alias_regexp"),
			                             boolAttributeValue(m_defaultAliasRegexp->isChecked()));
		if (m_defaultAliasSendTo)
			m_runtime->setWorldAttribute(QStringLiteral("default_alias_send_to"),
			                             QString::number(m_defaultAliasSendTo->value()));
		if (m_defaultAliasSequence)
			m_runtime->setWorldAttribute(QStringLiteral("default_alias_sequence"),
			                             QString::number(m_defaultAliasSequence->value()));
		if (m_enableTriggers)
			m_runtime->setWorldAttribute(QStringLiteral("enable_triggers"), m_enableTriggers->isChecked()
			                                                                    ? QStringLiteral("1")
			                                                                    : QStringLiteral("0"));
		if (m_enableTriggerSounds)
			m_runtime->setWorldAttribute(QStringLiteral("enable_trigger_sounds"),
			                             m_enableTriggerSounds->isChecked() ? QStringLiteral("1")
			                                                                : QStringLiteral("0"));
		if (m_useDefaultTriggers)
			m_runtime->setWorldAttribute(QStringLiteral("use_default_triggers"),
			                             m_useDefaultTriggers->isChecked() ? QStringLiteral("1")
			                                                               : QStringLiteral("0"));
		if (m_triggerTreeView)
			m_runtime->setWorldAttribute(QStringLiteral("treeview_triggers"), m_triggerTreeView->isChecked()
			                                                                      ? QStringLiteral("1")
			                                                                      : QStringLiteral("0"));
		if (m_defaultTriggerExpandVariables)
			m_runtime->setWorldAttribute(QStringLiteral("default_trigger_expand_variables"),
			                             boolAttributeValue(m_defaultTriggerExpandVariables->isChecked()));
		if (m_defaultTriggerIgnoreCase)
			m_runtime->setWorldAttribute(QStringLiteral("default_trigger_ignore_case"),
			                             boolAttributeValue(m_defaultTriggerIgnoreCase->isChecked()));
		if (m_defaultTriggerKeepEvaluating)
			m_runtime->setWorldAttribute(QStringLiteral("default_trigger_keep_evaluating"),
			                             boolAttributeValue(m_defaultTriggerKeepEvaluating->isChecked()));
		if (m_defaultTriggerRegexp)
			m_runtime->setWorldAttribute(QStringLiteral("default_trigger_regexp"),
			                             boolAttributeValue(m_defaultTriggerRegexp->isChecked()));
		if (m_defaultTriggerSendTo)
			m_runtime->setWorldAttribute(QStringLiteral("default_trigger_send_to"),
			                             QString::number(m_defaultTriggerSendTo->value()));
		if (m_defaultTriggerSequence)
			m_runtime->setWorldAttribute(QStringLiteral("default_trigger_sequence"),
			                             QString::number(m_defaultTriggerSequence->value()));
		if (m_enableTimers)
			m_runtime->setWorldAttribute(QStringLiteral("enable_timers"), m_enableTimers->isChecked()
			                                                                  ? QStringLiteral("1")
			                                                                  : QStringLiteral("0"));
		if (m_useDefaultTimers)
			m_runtime->setWorldAttribute(QStringLiteral("use_default_timers"), m_useDefaultTimers->isChecked()
			                                                                       ? QStringLiteral("1")
			                                                                       : QStringLiteral("0"));
		if (m_timerTreeView)
			m_runtime->setWorldAttribute(QStringLiteral("treeview_timers"), m_timerTreeView->isChecked()
			                                                                    ? QStringLiteral("1")
			                                                                    : QStringLiteral("0"));
		if (m_defaultTimerSendTo)
			m_runtime->setWorldAttribute(QStringLiteral("default_timer_send_to"),
			                             QString::number(m_defaultTimerSendTo->value()));

		if (m_enableScripts)
			m_runtime->setWorldAttribute(QStringLiteral("enable_scripts"), m_enableScripts->isChecked()
			                                                                   ? QStringLiteral("1")
			                                                                   : QStringLiteral("0"));
		if (m_scriptLanguage && m_scriptLanguage->currentIndex() >= 0)
			m_runtime->setWorldAttribute(QStringLiteral("script_language"),
			                             m_scriptLanguage->currentData().toString());
		if (m_scriptFile)
			m_runtime->setWorldAttribute(QStringLiteral("script_filename"), m_scriptFile->text());
		if (m_scriptPrefix)
			m_runtime->setWorldAttribute(QStringLiteral("script_prefix"), m_scriptPrefix->text());
		if (m_scriptEditor)
			m_runtime->setWorldAttribute(QStringLiteral("script_editor"), m_scriptEditor->text());
		if (m_editorWindowName)
			m_runtime->setWorldAttribute(QStringLiteral("editor_window_name"), m_editorWindowName->text());
		if (m_editScriptWithNotepad)
			m_runtime->setWorldAttribute(QStringLiteral("edit_script_with_notepad"),
			                             m_editScriptWithNotepad->isChecked() ? QStringLiteral("1")
			                                                                  : QStringLiteral("0"));
		if (m_scriptReloadOption)
			m_runtime->setWorldAttribute(QStringLiteral("script_reload_option"),
			                             QString::number(m_scriptReloadOption->currentData().toInt()));
		if (m_scriptTextColour && m_scriptTextColour->currentIndex() >= 0)
		{
			int noteValue = m_scriptTextColour->currentIndex() - 1;
			if (noteValue < 0)
				noteValue = WorldRuntime::kSameColour;
			const QString encoded = fromColourRef(noteValue).name(QColor::HexRgb);
			m_runtime->setWorldAttribute(QStringLiteral("note_text_colour"), encoded);
		}
		if (m_warnIfScriptingInactive)
			m_runtime->setWorldAttribute(QStringLiteral("warn_if_scripting_inactive"),
			                             m_warnIfScriptingInactive->isChecked() ? QStringLiteral("1")
			                                                                    : QStringLiteral("0"));
		if (m_scriptErrorsToOutput)
			m_runtime->setWorldAttribute(QStringLiteral("script_errors_to_output_window"),
			                             m_scriptErrorsToOutput->isChecked() ? QStringLiteral("1")
			                                                                 : QStringLiteral("0"));
		if (m_logScriptErrors)
			m_runtime->setWorldAttribute(QStringLiteral("log_script_errors"),
			                             boolAttributeValue(m_logScriptErrors->isChecked()));
		if (m_onWorldOpen)
			m_runtime->setWorldAttribute(QStringLiteral("on_world_open"), m_onWorldOpen->text().trimmed());
		if (m_onWorldClose)
			m_runtime->setWorldAttribute(QStringLiteral("on_world_close"), m_onWorldClose->text().trimmed());
		if (m_onWorldConnect)
			m_runtime->setWorldAttribute(QStringLiteral("on_world_connect"),
			                             m_onWorldConnect->text().trimmed());
		if (m_onWorldDisconnect)
			m_runtime->setWorldAttribute(QStringLiteral("on_world_disconnect"),
			                             m_onWorldDisconnect->text().trimmed());
		if (m_onWorldSave)
			m_runtime->setWorldAttribute(QStringLiteral("on_world_save"), m_onWorldSave->text().trimmed());
		if (m_onWorldGetFocus)
			m_runtime->setWorldAttribute(QStringLiteral("on_world_get_focus"),
			                             m_onWorldGetFocus->text().trimmed());
		if (m_onWorldLoseFocus)
			m_runtime->setWorldAttribute(QStringLiteral("on_world_lose_focus"),
			                             m_onWorldLoseFocus->text().trimmed());
		if (m_onMxpStart)
			m_runtime->setWorldAttribute(QStringLiteral("on_mxp_start"), m_onMxpStart->text().trimmed());
		if (m_onMxpStop)
			m_runtime->setWorldAttribute(QStringLiteral("on_mxp_stop"), m_onMxpStop->text().trimmed());
		if (m_onMxpOpenTag)
			m_runtime->setWorldAttribute(QStringLiteral("on_mxp_open_tag"), m_onMxpOpenTag->text().trimmed());
		if (m_onMxpCloseTag)
			m_runtime->setWorldAttribute(QStringLiteral("on_mxp_close_tag"),
			                             m_onMxpCloseTag->text().trimmed());
		if (m_onMxpSetVariable)
			m_runtime->setWorldAttribute(QStringLiteral("on_mxp_set_variable"),
			                             m_onMxpSetVariable->text().trimmed());
		if (m_onMxpError)
			m_runtime->setWorldAttribute(QStringLiteral("on_mxp_error"), m_onMxpError->text().trimmed());

		m_runtime->setWorldMultilineAttribute(QStringLiteral("filter_triggers"), m_triggerFilterText);
		m_runtime->setWorldMultilineAttribute(QStringLiteral("filter_aliases"), m_aliasFilterText);
		m_runtime->setWorldMultilineAttribute(QStringLiteral("filter_timers"), m_timerFilterText);
		m_runtime->setWorldMultilineAttribute(QStringLiteral("filter_variables"), m_variableFilterText);

		if (m_sendToWorldFilePreamble)
			m_runtime->setWorldMultilineAttribute(QStringLiteral("send_to_world_file_preamble"),
			                                      m_sendToWorldFilePreamble->toPlainText());
		if (m_sendToWorldFilePostamble)
			m_runtime->setWorldMultilineAttribute(QStringLiteral("send_to_world_file_postamble"),
			                                      m_sendToWorldFilePostamble->toPlainText());
		if (m_sendToWorldLinePreamble)
			m_runtime->setWorldAttribute(QStringLiteral("send_to_world_line_preamble"),
			                             m_sendToWorldLinePreamble->text());
		if (m_sendToWorldLinePostamble)
			m_runtime->setWorldAttribute(QStringLiteral("send_to_world_line_postamble"),
			                             m_sendToWorldLinePostamble->text());
		if (m_sendConfirm)
			m_runtime->setWorldAttribute(QStringLiteral("confirm_on_send"), m_sendConfirm->isChecked()
			                                                                    ? QStringLiteral("1")
			                                                                    : QStringLiteral("0"));
		if (m_sendCommentedSoftcode)
			m_runtime->setWorldAttribute(QStringLiteral("send_file_commented_softcode"),
			                             m_sendCommentedSoftcode->isChecked() ? QStringLiteral("1")
			                                                                  : QStringLiteral("0"));
		if (m_sendLineDelay)
			m_runtime->setWorldAttribute(QStringLiteral("send_file_delay"),
			                             QString::number(m_sendLineDelay->value()));
		if (m_sendDelayPerLines)
			m_runtime->setWorldAttribute(QStringLiteral("send_file_delay_per_lines"),
			                             QString::number(m_sendDelayPerLines->value()));
		if (m_sendEcho)
			m_runtime->setWorldAttribute(QStringLiteral("send_echo"),
			                             m_sendEcho->isChecked() ? QStringLiteral("1") : QStringLiteral("0"));

		if (m_notes)
			m_runtime->setWorldMultilineAttribute(QStringLiteral("notes"), m_notes->toPlainText());

		if (m_keypadEnabled)
			m_runtime->setWorldAttribute(QStringLiteral("keypad_enable"), m_keypadEnabled->isChecked()
			                                                                  ? QStringLiteral("1")
			                                                                  : QStringLiteral("0"));
		if (m_keypadControl)
			m_runtime->setWorldAttribute(QStringLiteral("keypad_ctrl_view"), m_keypadControl->isChecked()
			                                                                     ? QStringLiteral("1")
			                                                                     : QStringLiteral("0"));

		if (m_pastePreamble)
			m_runtime->setWorldMultilineAttribute(QStringLiteral("paste_preamble"),
			                                      m_pastePreamble->toPlainText());
		if (m_pastePostamble)
			m_runtime->setWorldMultilineAttribute(QStringLiteral("paste_postamble"),
			                                      m_pastePostamble->toPlainText());
		if (m_pasteLinePreamble)
			m_runtime->setWorldAttribute(QStringLiteral("paste_line_preamble"), m_pasteLinePreamble->text());
		if (m_pasteLinePostamble)
			m_runtime->setWorldAttribute(QStringLiteral("paste_line_postamble"),
			                             m_pasteLinePostamble->text());
		if (m_confirmOnPaste)
			m_runtime->setWorldAttribute(QStringLiteral("confirm_on_paste"), m_confirmOnPaste->isChecked()
			                                                                     ? QStringLiteral("1")
			                                                                     : QStringLiteral("0"));
		if (m_commentedSoftcodePaste)
			m_runtime->setWorldAttribute(QStringLiteral("paste_commented_softcode"),
			                             m_commentedSoftcodePaste->isChecked() ? QStringLiteral("1")
			                                                                   : QStringLiteral("0"));
		if (m_pasteLineDelay)
			m_runtime->setWorldAttribute(QStringLiteral("paste_delay"),
			                             QString::number(m_pasteLineDelay->value()));
		if (m_pasteDelayPerLines)
			m_runtime->setWorldAttribute(QStringLiteral("paste_delay_per_lines"),
			                             QString::number(m_pasteDelayPerLines->value()));
		if (m_pasteEcho)
			m_runtime->setWorldAttribute(QStringLiteral("paste_echo"), m_pasteEcho->isChecked()
			                                                               ? QStringLiteral("1")
			                                                               : QStringLiteral("0"));

		if (m_enableAutoSay)
			m_runtime->setWorldAttribute(QStringLiteral("enable_auto_say"), m_enableAutoSay->isChecked()
			                                                                    ? QStringLiteral("1")
			                                                                    : QStringLiteral("0"));
		if (m_reEvaluateAutoSay)
			m_runtime->setWorldAttribute(QStringLiteral("re_evaluate_auto_say"),
			                             m_reEvaluateAutoSay->isChecked() ? QStringLiteral("1")
			                                                              : QStringLiteral("0"));
		if (m_autoSayExcludeNonAlpha)
			m_runtime->setWorldAttribute(QStringLiteral("autosay_exclude_non_alpha"),
			                             m_autoSayExcludeNonAlpha->isChecked() ? QStringLiteral("1")
			                                                                   : QStringLiteral("0"));
		if (m_autoSayExcludeMacros)
			m_runtime->setWorldAttribute(QStringLiteral("autosay_exclude_macros"),
			                             m_autoSayExcludeMacros->isChecked() ? QStringLiteral("1")
			                                                                 : QStringLiteral("0"));
		if (m_autoSayString)
			m_runtime->setWorldAttribute(QStringLiteral("auto_say_string"), m_autoSayString->text());
		if (m_autoSayOverridePrefix)
			m_runtime->setWorldAttribute(QStringLiteral("auto_say_override_prefix"),
			                             m_autoSayOverridePrefix->text());

		if (m_playerName)
			m_runtime->setWorldAttribute(QStringLiteral("player"), m_playerName->text().trimmed());
		if (m_password)
			m_runtime->setWorldAttribute(QStringLiteral("password"), m_password->text());
		if (m_connectText)
			m_runtime->setWorldMultilineAttribute(QStringLiteral("connect_text"),
			                                      m_connectText->toPlainText());
		if (m_connectMethod && m_connectMethod->currentIndex() >= 0)
			m_runtime->setWorldAttribute(QStringLiteral("connect_method"),
			                             QString::number(m_connectMethod->currentData().toInt()));
		if (m_onlyNegotiateTelnetOptionsOnce)
			m_runtime->setWorldAttribute(QStringLiteral("only_negotiate_telnet_options_once"),
			                             m_onlyNegotiateTelnetOptionsOnce->isChecked() ? QStringLiteral("1")
			                                                                           : QStringLiteral("0"));

		if (m_useMxp && m_useMxp->currentIndex() >= 0)
			m_runtime->setWorldAttribute(QStringLiteral("use_mxp"), m_useMxp->currentData().toString());
		if (m_mxpDebugLevel && m_mxpDebugLevel->currentIndex() >= 0)
			m_runtime->setWorldAttribute(QStringLiteral("mxp_debug_level"),
			                             m_mxpDebugLevel->currentData().toString());
		if (m_detectPueblo)
			m_runtime->setWorldAttribute(QStringLiteral("detect_pueblo"), m_detectPueblo->isChecked()
			                                                                  ? QStringLiteral("1")
			                                                                  : QStringLiteral("0"));
		if (m_hyperlinkColour)
			m_runtime->setWorldAttribute(QStringLiteral("hyperlink_colour"),
			                             readSwatchValue(m_hyperlinkColour));
		if (m_useCustomLinkColour)
			m_runtime->setWorldAttribute(QStringLiteral("use_custom_link_colour"),
			                             m_useCustomLinkColour->isChecked() ? QStringLiteral("1")
			                                                                : QStringLiteral("0"));
		if (m_mudCanChangeLinkColour)
			m_runtime->setWorldAttribute(QStringLiteral("mud_can_change_link_colour"),
			                             m_mudCanChangeLinkColour->isChecked() ? QStringLiteral("1")
			                                                                   : QStringLiteral("0"));
		if (m_underlineHyperlinks)
			m_runtime->setWorldAttribute(QStringLiteral("underline_hyperlinks"),
			                             m_underlineHyperlinks->isChecked() ? QStringLiteral("1")
			                                                                : QStringLiteral("0"));
		if (m_mudCanRemoveUnderline)
			m_runtime->setWorldAttribute(QStringLiteral("mud_can_remove_underline"),
			                             m_mudCanRemoveUnderline->isChecked() ? QStringLiteral("1")
			                                                                  : QStringLiteral("0"));
		if (m_hyperlinkAddsToCommandHistory)
			m_runtime->setWorldAttribute(QStringLiteral("hyperlink_adds_to_command_history"),
			                             m_hyperlinkAddsToCommandHistory->isChecked() ? QStringLiteral("1")
			                                                                          : QStringLiteral("0"));
		if (m_echoHyperlinkInOutput)
			m_runtime->setWorldAttribute(QStringLiteral("echo_hyperlink_in_output_window"),
			                             m_echoHyperlinkInOutput->isChecked() ? QStringLiteral("1")
			                                                                  : QStringLiteral("0"));
		if (m_ignoreMxpColourChanges)
			m_runtime->setWorldAttribute(QStringLiteral("ignore_mxp_colour_changes"),
			                             m_ignoreMxpColourChanges->isChecked() ? QStringLiteral("1")
			                                                                   : QStringLiteral("0"));
		if (m_sendMxpAfkResponse)
			m_runtime->setWorldAttribute(QStringLiteral("send_mxp_afk_response"),
			                             m_sendMxpAfkResponse->isChecked() ? QStringLiteral("1")
			                                                               : QStringLiteral("0"));
		if (m_mudCanChangeOptions)
			m_runtime->setWorldAttribute(QStringLiteral("mud_can_change_options"),
			                             m_mudCanChangeOptions->isChecked() ? QStringLiteral("1")
			                                                                : QStringLiteral("0"));

		if (m_chatName)
			m_runtime->setWorldAttribute(QStringLiteral("chat_name"), m_chatName->text());
		if (m_autoAllowSnooping)
			m_runtime->setWorldAttribute(QStringLiteral("auto_allow_snooping"),
			                             m_autoAllowSnooping->isChecked() ? QStringLiteral("1")
			                                                              : QStringLiteral("0"));
		if (m_acceptIncomingChatConnections)
			m_runtime->setWorldAttribute(QStringLiteral("accept_chat_connections"),
			                             m_acceptIncomingChatConnections->isChecked() ? QStringLiteral("1")
			                                                                          : QStringLiteral("0"));
		if (m_incomingChatPort)
			m_runtime->setWorldAttribute(QStringLiteral("chat_port"),
			                             QString::number(m_incomingChatPort->value()));
		if (m_validateIncomingCalls)
			m_runtime->setWorldAttribute(QStringLiteral("validate_incoming_chat_calls"),
			                             m_validateIncomingCalls->isChecked() ? QStringLiteral("1")
			                                                                  : QStringLiteral("0"));
		if (m_ignoreChatColours)
			m_runtime->setWorldAttribute(QStringLiteral("ignore_chat_colours"),
			                             m_ignoreChatColours->isChecked() ? QStringLiteral("1")
			                                                              : QStringLiteral("0"));
		if (m_chatMessagePrefix)
			m_runtime->setWorldAttribute(QStringLiteral("chat_message_prefix"), m_chatMessagePrefix->text());
		if (m_maxChatLines)
			m_runtime->setWorldAttribute(QStringLiteral("chat_max_lines_per_message"),
			                             QString::number(m_maxChatLines->value()));
		if (m_maxChatBytes)
			m_runtime->setWorldAttribute(QStringLiteral("chat_max_bytes_per_message"),
			                             QString::number(m_maxChatBytes->value()));
		if (m_chatSaveDirectory)
			m_runtime->setWorldAttribute(QStringLiteral("chat_file_save_directory"),
			                             m_chatSaveDirectory->text());
		if (m_autoAllowFiles)
			m_runtime->setWorldAttribute(QStringLiteral("auto_allow_files"), m_autoAllowFiles->isChecked()
			                                                                     ? QStringLiteral("1")
			                                                                     : QStringLiteral("0"));
		if (m_chatTextColour)
			m_runtime->setWorldAttribute(QStringLiteral("chat_foreground_colour"),
			                             readSwatchValue(m_chatTextColour));
		if (m_chatBackColour)
			m_runtime->setWorldAttribute(QStringLiteral("chat_background_colour"),
			                             readSwatchValue(m_chatBackColour));

		const QString updatedAutoLogFileName =
		    m_runtime->worldAttributes().value(QStringLiteral("auto_log_file_name"));
		const bool shouldReopenLogForUpdatedPath = hadLogOpenBeforeApply &&
		                                           !updatedAutoLogFileName.trimmed().isEmpty() &&
		                                           updatedAutoLogFileName != previousAutoLogFileName;
		if (shouldReopenLogForUpdatedPath)
		{
			if (m_runtime->closeLog() != eOK)
			{
				QMessageBox::warning(this, QStringLiteral("Logging"),
				                     QStringLiteral("Could not close the current log file."));
			}
			else
			{
				(void)m_runtime->openLog(QString(), true);
				if (!m_runtime->isLogOpen())
				{
					QMessageBox::warning(this, QStringLiteral("Logging"),
					                     QStringLiteral("Could not open the new log file \"%1\".")
					                         .arg(updatedAutoLogFileName));
				}
				else
				{
					const QMap<QString, QString> &attrs     = m_runtime->worldAttributes();
					const QMap<QString, QString> &multi     = m_runtime->worldMultilineAttributes();
					const auto                    isEnabled = [](const QString &value)
					{
						return value == QStringLiteral("1") ||
						       value.compare(QStringLiteral("y"), Qt::CaseInsensitive) == 0 ||
						       value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
					};

					const bool      logHtml  = isEnabled(attrs.value(QStringLiteral("log_html")));
					const QDateTime now      = QDateTime::currentDateTime();
					QString         preamble = multi.value(QStringLiteral("log_file_preamble"));
					if (preamble.isEmpty())
						preamble = attrs.value(QStringLiteral("log_file_preamble"));

					if (!preamble.isEmpty())
					{
						preamble.replace(QStringLiteral("%n"), QStringLiteral("\n"));
						preamble = m_runtime->formatTime(now, preamble, logHtml);
						preamble.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
						m_runtime->writeLog(preamble);
						m_runtime->writeLog(QStringLiteral("\n"));
					}

					if (const bool writeWorldName =
					        isEnabled(attrs.value(QStringLiteral("write_world_name_to_log")));
					    writeWorldName)
					{
						const QString strTime =
						    m_runtime->formatTime(now, QStringLiteral("%A, %B %d, %Y, %#I:%M %p"), false);
						QString strPreamble = attrs.value(QStringLiteral("name"));
						strPreamble += QStringLiteral(" - ");
						strPreamble += strTime;

						if (logHtml)
						{
							m_runtime->writeLog(QStringLiteral("<br>\n"));
							m_runtime->writeLog(fixHtmlString(strPreamble));
							m_runtime->writeLog(QStringLiteral("<br>\n"));
						}
						else
						{
							m_runtime->writeLog(QStringLiteral("\n"));
							m_runtime->writeLog(strPreamble);
							m_runtime->writeLog(QStringLiteral("\n"));
						}

						const QString strHyphens(strPreamble.length(), QLatin1Char('-'));
						m_runtime->writeLog(strHyphens);
						if (logHtml)
							m_runtime->writeLog(QStringLiteral("<br><br>"));
						else
							m_runtime->writeLog(QStringLiteral("\n\n"));
					}
				}
			}
		}
	}

	applyListEdits();
	if (AppController *app = AppController::instance())
		app->applyConfiguredWorldDefaults(m_runtime);
	m_runtime->refreshCommandProcessorOptions();
	m_runtime->syncChatAcceptCallsWithPreferences();

	if (m_view)
	{
		const QSet<QString> changedViewAttributeKeys = WorldView::changedRuntimeSettingsAttributeKeys(
		    worldAttributesBeforeApply, m_runtime->worldAttributes());
		const QSet<QString> changedViewMultilineKeys =
		    WorldView::changedRuntimeSettingsMultilineAttributeKeys(
		        worldMultilineBeforeApply, m_runtime->worldMultilineAttributes(), worldAttributesBeforeApply,
		        m_runtime->worldAttributes());
		if (!changedViewAttributeKeys.isEmpty() || !changedViewMultilineKeys.isEmpty())
		{
			const bool needsFullRebuild =
			    WorldView::runtimeSettingsNeedFullRebuild(changedViewAttributeKeys, changedViewMultilineKeys);
			if (needsFullRebuild)
				m_view->applyRuntimeSettings();
			else
				m_view->applyRuntimeSettingsWithoutOutputRebuild();
		}
	}

	QDialog::accept();
}

bool WorldPreferencesDialog::eventFilter(QObject *obj, QEvent *event)
{
	if (event->type() == QEvent::MouseButtonPress)
	{
		if (obj == m_chatTextColour)
		{
			openLineEditColourPicker(m_chatTextColour, QStringLiteral("Chat text colour"));
			return true;
		}
		if (obj == m_chatBackColour)
		{
			openLineEditColourPicker(m_chatBackColour, QStringLiteral("Chat background colour"));
			return true;
		}
		if (obj == m_hyperlinkColour)
		{
			openLineEditColourPicker(m_hyperlinkColour, QStringLiteral("Hyperlink colour"));
			return true;
		}
	}
	return QDialog::eventFilter(obj, event);
}

void WorldPreferencesDialog::setLineEditSwatch(QLineEdit *edit, const QColor &colour) const
{
	if (!edit)
		return;
	if (!colour.isValid())
	{
		edit->setProperty("colour_value", QString());
		edit->setText(QString());
		edit->setPalette(style()->standardPalette());
		return;
	}
	edit->setProperty("colour_value", formatColourValue(colour));
	edit->setText(QString());
	QPalette pal = edit->palette();
	pal.setColor(QPalette::Base, colour);
	pal.setColor(QPalette::Text, colour);
	edit->setPalette(pal);
}

void WorldPreferencesDialog::openLineEditColourPicker(QLineEdit *edit, const QString &title)
{
	if (!edit)
		return;
	const QString stored  = edit->property("colour_value").toString();
	QColor        current = parseColourValue(stored);
	if (!current.isValid())
		current = parseColourValue(edit->text());
	ColourPickerDialog dlg(this);
	dlg.setWindowTitle(title);
	dlg.setPickColour(true);
	if (current.isValid())
		dlg.setInitialColour(current);
	if (dlg.exec() != QDialog::Accepted)
		return;
	const QColor chosen = dlg.selectedColour();
	if (!chosen.isValid())
		return;
	setLineEditSwatch(edit, chosen);
}

void WorldPreferencesDialog::updateAutoCopyHtmlState() const
{
	if (!m_copySelectionToClipboard || !m_autoCopyHtml)
		return;
	m_autoCopyHtml->setEnabled(m_copySelectionToClipboard->isChecked());
}

void WorldPreferencesDialog::updateOutputFontControls() const
{
	if (!m_outputFontButton)
		return;
	const bool hasDefault = hasDefaultOutputFont();
	if (m_useDefaultOutputFont)
		m_useDefaultOutputFont->setEnabled(hasDefault);
	const bool usingDefault = m_useDefaultOutputFont && m_useDefaultOutputFont->isChecked() && hasDefault;
	m_outputFontButton->setEnabled(!usingDefault || !hasDefault);
}

void WorldPreferencesDialog::updateDefaultColoursState()
{
	const bool hasDefaults = hasDefaultColoursFile();
	if (m_useDefaultColours)
		m_useDefaultColours->setEnabled(hasDefaults);
	const bool allowEdits = !m_useDefaultColours || !m_useDefaultColours->isChecked() || !hasDefaults;
	for (QPushButton *swatch : m_customTextSwatches)
		if (swatch)
			swatch->setEnabled(allowEdits);
	for (QPushButton *swatch : m_customBackSwatches)
		if (swatch)
			swatch->setEnabled(allowEdits);
	for (QPushButton *swatch : m_ansiNormalSwatches)
		if (swatch)
			swatch->setEnabled(allowEdits);
	for (QPushButton *swatch : m_ansiBoldSwatches)
		if (swatch)
			swatch->setEnabled(allowEdits);
	if (m_ansiSwap)
		m_ansiSwap->setEnabled(allowEdits);
	if (m_ansiLoad)
		m_ansiLoad->setEnabled(allowEdits);
	if (m_ansiDefaults)
		m_ansiDefaults->setEnabled(allowEdits);
}

void WorldPreferencesDialog::updateInputFontControls() const
{
	if (!m_inputFontButton)
		return;
	const bool hasDefault = hasDefaultInputFont();
	if (m_useDefaultInputFont)
		m_useDefaultInputFont->setEnabled(hasDefault);
	const bool usingDefault = m_useDefaultInputFont && m_useDefaultInputFont->isChecked() && hasDefault;
	m_inputFontButton->setEnabled(!usingDefault || !hasDefault);
}

void WorldPreferencesDialog::updateCommandAutoResizeControls() const
{
	if (!m_autoResizeMinimumLines || !m_autoResizeMaximumLines)
		return;
	const bool enabled = m_autoResizeCommandWindow && m_autoResizeCommandWindow->isChecked();
	m_autoResizeMinimumLines->setEnabled(enabled);
	m_autoResizeMaximumLines->setEnabled(enabled);
}

void WorldPreferencesDialog::updateMacroControls() const
{
	if (!m_useDefaultMacros)
		return;
	const bool hasDefaults = hasDefaultMacrosFile();
	m_useDefaultMacros->setEnabled(hasDefaults);
	const bool allowEdit = !m_useDefaultMacros->isChecked() || !hasDefaults;
	if (m_macrosTable)
		m_macrosTable->setEnabled(allowEdit);
	if (m_editMacroButton)
		m_editMacroButton->setEnabled(allowEdit && m_macrosTable && m_macrosTable->currentRow() >= 0);
	if (m_findMacroButton)
		m_findMacroButton->setEnabled(allowEdit);
	if (m_findNextMacroButton)
		m_findNextMacroButton->setEnabled(allowEdit && !m_macroFindText.isEmpty());
	if (m_loadMacroButton)
		m_loadMacroButton->setEnabled(allowEdit);
	if (m_saveMacroButton)
		m_saveMacroButton->setEnabled(m_macrosTable && m_macrosTable->rowCount() > 0);
}

void WorldPreferencesDialog::updateAliasControls() const
{
	if (!m_aliasesTable)
		return;
	const int  rowCount    = m_aliasesTable->rowCount();
	const int  row         = selectedRow(m_aliasesTable);
	const bool hasDefaults = hasDefaultAliasesFile();
	if (m_useDefaultAliases)
		m_useDefaultAliases->setEnabled(hasDefaults);
	const bool usingDefault = m_useDefaultAliases && m_useDefaultAliases->isChecked() && hasDefaults;
	const bool allowEdit    = !usingDefault;
	if (m_addAliasButton)
		m_addAliasButton->setEnabled(allowEdit);
	if (m_loadAliasesButton)
		m_loadAliasesButton->setEnabled(allowEdit);
	if (m_editAliasButton)
		m_editAliasButton->setEnabled(allowEdit && row >= 0);
	if (m_deleteAliasButton)
		m_deleteAliasButton->setEnabled(allowEdit && row >= 0);
	if (m_moveAliasUpButton)
		m_moveAliasUpButton->setEnabled(allowEdit && row > 0);
	if (m_moveAliasDownButton)
		m_moveAliasDownButton->setEnabled(allowEdit && row >= 0 && row < rowCount - 1);
	if (m_copyAliasButton)
		m_copyAliasButton->setEnabled(allowEdit && row >= 0);
	if (m_pasteAliasButton)
		m_pasteAliasButton->setEnabled(allowEdit);
	if (m_findAliasButton)
		m_findAliasButton->setEnabled(rowCount > 0);
	if (m_findNextAliasButton)
		m_findNextAliasButton->setEnabled(rowCount > 0 && !m_aliasFindText.isEmpty());
	if (m_saveAliasesButton)
		m_saveAliasesButton->setEnabled(rowCount > 0);
}

void WorldPreferencesDialog::updateTriggerControls() const
{
	if (!m_triggersTable)
		return;
	const int  rowCount    = m_triggersTable->rowCount();
	const int  row         = selectedRow(m_triggersTable);
	const bool hasDefaults = hasDefaultTriggersFile();
	if (m_useDefaultTriggers)
		m_useDefaultTriggers->setEnabled(hasDefaults);
	const bool usingDefault = m_useDefaultTriggers && m_useDefaultTriggers->isChecked() && hasDefaults;
	const bool allowEdit    = !usingDefault;
	if (m_addTriggerButton)
		m_addTriggerButton->setEnabled(allowEdit);
	if (m_loadTriggersButton)
		m_loadTriggersButton->setEnabled(allowEdit);
	if (m_editTriggerButton)
		m_editTriggerButton->setEnabled(allowEdit && row >= 0);
	if (m_deleteTriggerButton)
		m_deleteTriggerButton->setEnabled(allowEdit && row >= 0);
	if (m_moveTriggerUpButton)
		m_moveTriggerUpButton->setEnabled(allowEdit && row > 0);
	if (m_moveTriggerDownButton)
		m_moveTriggerDownButton->setEnabled(allowEdit && row >= 0 && row < rowCount - 1);
	if (m_copyTriggerButton)
		m_copyTriggerButton->setEnabled(allowEdit && row >= 0);
	if (m_pasteTriggerButton)
		m_pasteTriggerButton->setEnabled(allowEdit);
	if (m_findTriggerButton)
		m_findTriggerButton->setEnabled(rowCount > 0);
	if (m_findNextTriggerButton)
		m_findNextTriggerButton->setEnabled(rowCount > 0 && !m_triggerFindText.isEmpty());
	if (m_saveTriggersButton)
		m_saveTriggersButton->setEnabled(rowCount > 0);
}

void WorldPreferencesDialog::updateTimerControls() const
{
	if (!m_timersTable)
		return;
	const int  rowCount    = m_timersTable->rowCount();
	const int  row         = selectedRow(m_timersTable);
	const bool hasDefaults = hasDefaultTimersFile();
	if (m_useDefaultTimers)
		m_useDefaultTimers->setEnabled(hasDefaults);
	const bool usingDefault = m_useDefaultTimers && m_useDefaultTimers->isChecked() && hasDefaults;
	const bool allowEdit    = !usingDefault;
	if (m_addTimerButton)
		m_addTimerButton->setEnabled(allowEdit);
	if (m_loadTimersButton)
		m_loadTimersButton->setEnabled(allowEdit);
	if (m_editTimerButton)
		m_editTimerButton->setEnabled(allowEdit && row >= 0);
	if (m_deleteTimerButton)
		m_deleteTimerButton->setEnabled(allowEdit && row >= 0);
	if (m_copyTimerButton)
		m_copyTimerButton->setEnabled(allowEdit && row >= 0);
	if (m_pasteTimerButton)
		m_pasteTimerButton->setEnabled(allowEdit);
	if (m_findTimerButton)
		m_findTimerButton->setEnabled(rowCount > 0);
	if (m_findNextTimerButton)
		m_findNextTimerButton->setEnabled(rowCount > 0 && !m_timerFindText.isEmpty());
	if (m_saveTimersButton)
		m_saveTimersButton->setEnabled(rowCount > 0);
	if (m_resetTimersButton)
		m_resetTimersButton->setEnabled(rowCount > 0);
}

void WorldPreferencesDialog::updateRuleViewModes()
{
	auto applyMode =
	    [this](const QCheckBox *toggle, QStackedWidget *stack, QTableWidget *table, QTreeWidget *tree)
	{
		if (!toggle || !stack || !table || !tree)
			return;

		const bool treeMode = toggle->isChecked();
		stack->setCurrentWidget(treeMode ? static_cast<QWidget *>(tree) : static_cast<QWidget *>(table));

		if (treeMode)
		{
			if (const int index = rowToIndex(table, selectedRow(table)); index >= 0)
			{
				QSignalBlocker block(tree);
				m_syncingRuleSelection = true;
				tree->setCurrentItem(findTreeItemByIndex(tree, index));
				m_syncingRuleSelection = false;
			}
		}
		else
		{
			const int index = currentTreeIndex(tree);
			if (index >= 0)
			{
				QSignalBlocker block(table);
				m_syncingRuleSelection = true;
				selectRowByIndex(table, index);
				m_syncingRuleSelection = false;
			}
		}
	};

	applyMode(m_triggerTreeView, m_triggersViewStack, m_triggersTable, m_triggersTree);
	applyMode(m_aliasTreeView, m_aliasesViewStack, m_aliasesTable, m_aliasesTree);
	applyMode(m_timerTreeView, m_timersViewStack, m_timersTable, m_timersTree);
}

void WorldPreferencesDialog::updateVariableControls() const
{
	if (!m_variablesTable)
		return;
	const int rowCount = m_variablesTable->rowCount();
	const int row      = selectedRow(m_variablesTable);
	if (m_editVariableButton)
		m_editVariableButton->setEnabled(row >= 0);
	if (m_deleteVariableButton)
		m_deleteVariableButton->setEnabled(row >= 0);
	if (m_copyVariableButton)
		m_copyVariableButton->setEnabled(row >= 0);
	if (m_findVariableButton)
		m_findVariableButton->setEnabled(rowCount > 0);
	if (m_findNextVariableButton)
		m_findNextVariableButton->setEnabled(rowCount > 0 && !m_variableFindText.isEmpty());
	if (m_saveVariablesButton)
		m_saveVariablesButton->setEnabled(rowCount > 0);
}

void WorldPreferencesDialog::updateSpellCheckState() const
{
	if (!m_spellCheckOnSend)
		return;
	AppController *app = AppController::instance();
	if (!app)
	{
		m_spellCheckOnSend->setEnabled(false);
		return;
	}
	m_spellCheckOnSend->setEnabled(app->isSpellCheckerAvailable());
}

bool WorldPreferencesDialog::hasDefaultOutputFont()
{
	AppController *app = AppController::instance();
	if (!app)
		return false;
	const QString value = app->getGlobalOption(QStringLiteral("DefaultOutputFont")).toString();
	return !value.isEmpty();
}

bool WorldPreferencesDialog::hasDefaultColoursFile()
{
	AppController *app = AppController::instance();
	if (!app)
		return false;
	const QString value = app->getGlobalOption(QStringLiteral("DefaultColoursFile")).toString();
	return !value.isEmpty();
}

bool WorldPreferencesDialog::hasDefaultInputFont()
{
	AppController *app = AppController::instance();
	if (!app)
		return false;
	const QString value = app->getGlobalOption(QStringLiteral("DefaultInputFont")).toString();
	return !value.isEmpty();
}

bool WorldPreferencesDialog::hasDefaultMacrosFile()
{
	AppController *app = AppController::instance();
	if (!app)
		return false;
	const QString value = app->getGlobalOption(QStringLiteral("DefaultMacrosFile")).toString();
	return !value.isEmpty();
}

bool WorldPreferencesDialog::hasDefaultTriggersFile()
{
	AppController *app = AppController::instance();
	if (!app)
		return false;
	const QString value = app->getGlobalOption(QStringLiteral("DefaultTriggersFile")).toString();
	return !value.isEmpty();
}

bool WorldPreferencesDialog::hasDefaultAliasesFile()
{
	AppController *app = AppController::instance();
	if (!app)
		return false;
	const QString value = app->getGlobalOption(QStringLiteral("DefaultAliasesFile")).toString();
	return !value.isEmpty();
}

bool WorldPreferencesDialog::hasDefaultTimersFile()
{
	AppController *app = AppController::instance();
	if (!app)
		return false;
	const QString value = app->getGlobalOption(QStringLiteral("DefaultTimersFile")).toString();
	return !value.isEmpty();
}

QFont WorldPreferencesDialog::outputFontFromDialog() const
{
	QFont font;
	bool  hasValue = false;
	if (m_outputFontName && !m_outputFontName->text().isEmpty())
	{
		font.setFamily(m_outputFontName->text());
		hasValue = true;
	}
	if (m_outputFontHeight && m_outputFontHeight->value() > 0)
	{
		font.setPointSize(m_outputFontHeight->value());
		hasValue = true;
	}
	if (m_outputFontWeight > 0)
	{
		font.setWeight(WorldView::mapFontWeight(m_outputFontWeight));
		hasValue = true;
	}
	if (!hasValue && m_view)
		font = m_view->outputFont();
	return font;
}

QFont WorldPreferencesDialog::outputFontFromView() const
{
	if (m_view)
		return m_view->outputFont();
	return outputFontFromDialog();
}

int WorldPreferencesDialog::averageCharWidth(const QFont &font)
{
	const QFontMetrics metrics(font);
	int                width = metrics.averageCharWidth();
	if (width <= 0)
		width = metrics.horizontalAdvance(QLatin1Char('M'));
	return width > 0 ? width : 1;
}

void WorldPreferencesDialog::storeKeypadFields(const bool ctrlView)
{
	for (auto it = m_keypadFields.constBegin(); it != m_keypadFields.constEnd(); ++it)
	{
		const QString keyName = ctrlView ? QStringLiteral("Ctrl+%1").arg(it.key()) : it.key();
		const QString value   = it.value() ? it.value()->text() : QString();
		m_keypadValues.insert(keyName, value);
	}
}

void WorldPreferencesDialog::loadKeypadFields(const bool ctrlView)
{
	for (auto it = m_keypadFields.begin(); it != m_keypadFields.end(); ++it)
	{
		if (!it.value())
			continue;
		const QString keyName = ctrlView ? QStringLiteral("Ctrl+%1").arg(it.key()) : it.key();
		it.value()->setText(m_keypadValues.value(keyName));
	}
}

void WorldPreferencesDialog::doNotesFind(const bool again)
{
	if (!m_notes)
		return;

	QString findText  = m_notesFindText;
	bool    matchCase = m_notesFindMatchCase;
	bool    forwards  = m_notesFindForwards;

	if (!again || findText.isEmpty())
	{
		QDialog dialog(this);
		dialog.setWindowTitle(QStringLiteral("Find notes"));
		auto *dialogLayout = new QVBoxLayout(&dialog);
		auto *form         = new QFormLayout();
		auto *combo        = new QComboBox(&dialog);
		combo->setEditable(true);
		combo->addItems(m_notesFindHistory);
		if (!findText.isEmpty())
			combo->setCurrentText(findText);
		else if (!m_notesFindHistory.isEmpty())
			combo->setCurrentText(m_notesFindHistory.first());
		form->addRow(QStringLiteral("Find text:"), combo);
		auto *matchCaseBox = new QCheckBox(QStringLiteral("Match case"), &dialog);
		auto *forwardsBox  = new QCheckBox(QStringLiteral("Search forwards"), &dialog);
		auto *regexBox     = new QCheckBox(QStringLiteral("Regular expression"), &dialog);
		matchCaseBox->setChecked(matchCase);
		forwardsBox->setChecked(forwards);
		regexBox->setChecked(false);
		form->addRow(QString(), matchCaseBox);
		form->addRow(QString(), forwardsBox);
		form->addRow(QString(), regexBox);
		dialogLayout->addLayout(form);
		auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
		connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
		connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
		dialogLayout->addWidget(buttons);
		if (dialog.exec() != QDialog::Accepted)
			return;
		findText  = combo->currentText();
		matchCase = matchCaseBox->isChecked();
		forwards  = forwardsBox->isChecked();
		if (regexBox->isChecked())
		{
			QMessageBox::information(this, QStringLiteral("Find notes"),
			                         QStringLiteral("Regular expressions not supported here."));
			return;
		}
		if (findText.isEmpty())
			return;
		m_notesFindHistory.removeAll(findText);
		m_notesFindHistory.prepend(findText);
		m_notesFindText      = findText;
		m_notesFindMatchCase = matchCase;
		m_notesFindForwards  = forwards;
		m_notesFindIndex     = forwards ? 0 : -1;
	}

	const QString content = m_notes->toPlainText();
	if (content.isEmpty() || findText.isEmpty())
		return;
	const Qt::CaseSensitivity cs         = matchCase ? Qt::CaseSensitive : Qt::CaseInsensitive;
	int                       startIndex = 0;
	if (forwards)
	{
		startIndex = (again && m_notesFindIndex >= 0) ? (m_notesFindIndex + 1) : 0;
	}
	else
	{
		const qsizetype maxStartSize = qMax<qsizetype>(0, content.size() - findText.size());
		const int       maxStart     = (maxStartSize > std::numeric_limits<int>::max())
		                                   ? std::numeric_limits<int>::max()
		                                   : static_cast<int>(maxStartSize);
		startIndex                   = (again && m_notesFindIndex >= 0) ? (m_notesFindIndex - 1) : maxStart;
		startIndex                   = qBound(0, startIndex, maxStart);
	}

	const qsizetype index =
	    forwards ? content.indexOf(findText, startIndex, cs) : content.lastIndexOf(findText, startIndex, cs);

	if (index < 0)
	{
		const QString message = QStringLiteral("The text \"%1\" was not found%2")
		                            .arg(findText)
		                            .arg(again ? QStringLiteral(" again.") : QStringLiteral("."));
		QMessageBox::information(this, QStringLiteral("Find notes"), message);
		m_notesFindIndex = -1;
		return;
	}

	const int indexInt =
	    (index > std::numeric_limits<int>::max()) ? std::numeric_limits<int>::max() : static_cast<int>(index);
	const qsizetype endIndex = index + findText.size();
	const int   endIndexInt  = (endIndex > std::numeric_limits<int>::max()) ? std::numeric_limits<int>::max()
	                                                                        : static_cast<int>(endIndex);
	QTextCursor cursor       = m_notes->textCursor();
	cursor.setPosition(indexInt);
	cursor.setPosition(endIndexInt, QTextCursor::KeepAnchor);
	m_notes->setTextCursor(cursor);
	m_notes->setFocus();
	m_notesFindIndex = indexInt;
}

void WorldPreferencesDialog::applyListEdits()
{
	if (!m_runtime)
		return;

	bool                        coloursChanged = false;
	QList<WorldRuntime::Colour> colours        = m_runtime->colours();
	if (!m_customColourNames.isEmpty() && m_customColourNames.size() == m_customTextSwatches.size() &&
	    m_customColourNames.size() == m_customBackSwatches.size())
	{
		for (int i = 0; i < m_customColourNames.size(); ++i)
		{
			const QString seq  = QString::number(i + 1);
			const QString name = m_customColourNames[i] ? m_customColourNames[i]->text() : QString();
			const QString text = formatColourValue(swatchButtonColour(m_customTextSwatches.value(i)));
			const QString back = formatColourValue(swatchButtonColour(m_customBackSwatches.value(i)));
			for (auto &colour : colours)
			{
				if (!colour.group.startsWith(QStringLiteral("custom/")))
					continue;
				if (colour.attributes.value(QStringLiteral("seq")) != seq)
					continue;
				if (colour.attributes.value(QStringLiteral("text")) != text ||
				    colour.attributes.value(QStringLiteral("back")) != back ||
				    colour.attributes.value(QStringLiteral("name")) != name)
				{
					colour.attributes.insert(QStringLiteral("text"), text);
					colour.attributes.insert(QStringLiteral("back"), back);
					colour.attributes.insert(QStringLiteral("name"), name);
					coloursChanged = true;
				}
				break;
			}
		}
	}
	if (!m_ansiNormalSwatches.isEmpty() && !m_ansiBoldSwatches.isEmpty())
	{
		QVector<QString> normalRgb(8);
		QVector<QString> boldRgb(8);
		for (int i = 0; i < 8; ++i)
		{
			normalRgb[i] = formatColourValue(swatchButtonColour(m_ansiNormalSwatches.value(i)));
			boldRgb[i]   = formatColourValue(swatchButtonColour(m_ansiBoldSwatches.value(i)));
		}
		for (auto &colour : colours)
		{
			if (!colour.group.startsWith(QStringLiteral("ansi/")))
				continue;
			bool      ok  = false;
			const int seq = colour.attributes.value(QStringLiteral("seq")).toInt(&ok);
			if (!ok || seq < 1 || seq > 8)
				continue;
			const int index = seq - 1;
			if (colour.group == QStringLiteral("ansi/normal"))
			{
				if (colour.attributes.value(QStringLiteral("rgb")) != normalRgb.value(index))
				{
					colour.attributes.insert(QStringLiteral("rgb"), normalRgb.value(index));
					coloursChanged = true;
				}
			}
			else if (colour.group == QStringLiteral("ansi/bold"))
			{
				if (colour.attributes.value(QStringLiteral("rgb")) != boldRgb.value(index))
				{
					colour.attributes.insert(QStringLiteral("rgb"), boldRgb.value(index));
					coloursChanged = true;
				}
			}
		}
	}
	if (coloursChanged)
		m_runtime->setColours(colours);

	if (m_macrosTable)
	{
		QList<WorldRuntime::Macro> macros        = m_runtime->macros();
		bool                       macrosChanged = false;
		const int                  rowCount      = m_macrosTable->rowCount();
		for (int row = 0; row < rowCount; ++row)
		{
			QTableWidgetItem *nameItem = m_macrosTable->item(row, 0);
			if (!nameItem)
				continue;
			bool      ok         = false;
			const int macroIndex = nameItem->data(Qt::UserRole).toInt(&ok);
			if (!ok || macroIndex < 0 || macroIndex >= macros.size())
				continue;
			if (auto *typeCombo = qobject_cast<QComboBox *>(m_macrosTable->cellWidget(row, 1)); typeCombo)
			{
				if (const QString newType = typeCombo->currentData().toString();
				    macros[macroIndex].attributes.value(QStringLiteral("type")) != newType)
				{
					macros[macroIndex].attributes.insert(QStringLiteral("type"), newType);
					macrosChanged = true;
				}
			}
			if (QTableWidgetItem *sendItem = m_macrosTable->item(row, 2); sendItem)
			{
				if (const QString newSend = sendItem->text();
				    macros[macroIndex].children.value(QStringLiteral("send")) != newSend)
				{
					macros[macroIndex].children.insert(QStringLiteral("send"), newSend);
					macrosChanged = true;
				}
			}
		}
		if (macrosChanged)
			m_runtime->setMacros(macros);
	}

	if (!m_keypadFields.isEmpty())
	{
		if (m_keypadControl)
			storeKeypadFields(m_keypadControl->isChecked());
		QList<WorldRuntime::Keypad> entries;
		entries.reserve(m_keypadValues.size());
		for (auto it = m_keypadValues.constBegin(); it != m_keypadValues.constEnd(); ++it)
		{
			WorldRuntime::Keypad entry;
			entry.attributes.insert(QStringLiteral("name"), it.key());
			entry.content = it.value();
			entries.push_back(entry);
		}
		if (!entries.isEmpty())
			m_runtime->setKeypadEntries(entries);
	}

	if (!m_printingNormalBold.isEmpty() || !m_printingBoldBold.isEmpty())
	{
		QList<WorldRuntime::PrintingStyle> styles          = m_runtime->printingStyles();
		bool                               printingChanged = false;
		for (auto &style : styles)
		{
			const QString group = style.group.toLower();
			bool          ok    = false;
			const int     seq   = style.attributes.value(QStringLiteral("seq")).toInt(&ok);
			if (!ok || seq < 1 || seq > 8)
				continue;
			const int  index    = seq - 1;
			const bool isNormal = group == QStringLiteral("ansi/normal");
			const bool isBold   = group == QStringLiteral("ansi/bold");
			if (!isNormal && !isBold)
				continue;
			const QVector<QCheckBox *> &boldChecks = isNormal ? m_printingNormalBold : m_printingBoldBold;
			const QVector<QCheckBox *> &italicChecks =
			    isNormal ? m_printingNormalItalic : m_printingBoldItalic;
			const QVector<QCheckBox *> &underlineChecks =
			    isNormal ? m_printingNormalUnderline : m_printingBoldUnderline;
			if (index < 0 || index >= boldChecks.size() || index >= italicChecks.size() ||
			    index >= underlineChecks.size())
				continue;
			const QString newBold = boldChecks[index] && boldChecks[index]->isChecked() ? QStringLiteral("1")
			                                                                            : QStringLiteral("0");
			const QString newItalic    = italicChecks[index] && italicChecks[index]->isChecked()
			                                 ? QStringLiteral("1")
			                                 : QStringLiteral("0");
			const QString newUnderline = underlineChecks[index] && underlineChecks[index]->isChecked()
			                                 ? QStringLiteral("1")
			                                 : QStringLiteral("0");
			if (style.attributes.value(QStringLiteral("bold")) != newBold ||
			    style.attributes.value(QStringLiteral("italic")) != newItalic ||
			    style.attributes.value(QStringLiteral("underline")) != newUnderline)
			{
				style.attributes.insert(QStringLiteral("bold"), newBold);
				style.attributes.insert(QStringLiteral("italic"), newItalic);
				style.attributes.insert(QStringLiteral("underline"), newUnderline);
				printingChanged = true;
			}
		}
		if (printingChanged)
			m_runtime->setPrintingStyles(styles);
	}
}

void WorldPreferencesDialog::editColourCell(QTableWidget *table, const int row, const int column)
{
	if (!table)
		return;
	QTableWidgetItem *item = table->item(row, column);
	if (!item)
		return;
	QColor current;
	if (const QVariant stored = item->data(Qt::UserRole); stored.isValid())
		current = QColor(stored.toString());
	if (!current.isValid())
		current = QColor(item->text());
	const QColor chosen = QColorDialog::getColor(current.isValid() ? current : Qt::white, this,
	                                             QStringLiteral("Select colour"));
	if (!chosen.isValid())
		return;
	item->setText(QString());
	item->setData(Qt::UserRole, chosen);
	item->setBackground(chosen);
}

void WorldPreferencesDialog::editMacroAtRow(const int row)
{
	if (!m_macrosTable || row < 0 || row >= m_macrosTable->rowCount())
		return;
	if (m_useDefaultMacros && m_useDefaultMacros->isChecked() && hasDefaultMacrosFile())
		return;
	QTableWidgetItem *nameItem  = m_macrosTable->item(row, 0);
	auto             *typeCombo = qobject_cast<QComboBox *>(m_macrosTable->cellWidget(row, 1));
	QTableWidgetItem *sendItem  = m_macrosTable->item(row, 2);
	if (!nameItem || !typeCombo || !sendItem)
		return;

	QDialog dlg(this);
	dlg.setWindowTitle(QStringLiteral("Edit macro"));
	auto *layout     = new QVBoxLayout(&dlg);
	auto *macroLabel = new QLabel(QStringLiteral("Macro: %1").arg(nameItem->text()), &dlg);
	layout->addWidget(macroLabel);

	auto *sendLabel = new QLabel(QStringLiteral("Send:"), &dlg);
	layout->addWidget(sendLabel);
	auto *sendEdit = new QTextEdit(&dlg);
	sendEdit->setPlainText(sendItem->text());
	layout->addWidget(sendEdit);

	auto *actionBox    = new QGroupBox(QStringLiteral("Send action"), &dlg);
	auto *actionLayout = new QHBoxLayout(actionBox);
	auto *replaceRadio = new QRadioButton(QStringLiteral("Replace"), actionBox);
	auto *sendNowRadio = new QRadioButton(QStringLiteral("Send now"), actionBox);
	auto *insertRadio  = new QRadioButton(QStringLiteral("Insert"), actionBox);
	actionLayout->addWidget(replaceRadio);
	actionLayout->addWidget(sendNowRadio);
	actionLayout->addWidget(insertRadio);
	layout->addWidget(actionBox);

	if (const QString typeValue = typeCombo->currentData().toString();
	    typeValue == QStringLiteral("send_now"))
		sendNowRadio->setChecked(true);
	else if (typeValue == QStringLiteral("insert"))
		insertRadio->setChecked(true);
	else
		replaceRadio->setChecked(true);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
	layout->addWidget(buttons);
	connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

	if (dlg.exec() != QDialog::Accepted)
		return;

	QString newType = QStringLiteral("replace");
	if (sendNowRadio->isChecked())
		newType = QStringLiteral("send_now");
	else if (insertRadio->isChecked())
		newType = QStringLiteral("insert");

	if (const int typeIndex = typeCombo->findData(newType); typeIndex >= 0)
		typeCombo->setCurrentIndex(typeIndex);
	sendItem->setText(sendEdit->toPlainText());
}

void WorldPreferencesDialog::findMacro(const QString &text, const bool continueFromCurrent)
{
	if (!m_macrosTable)
		return;
	const int rowCount = m_macrosTable->rowCount();
	if (rowCount == 0)
		return;

	QString findText  = text;
	bool    matchCase = m_macroFindMatchCase;
	bool    forwards  = m_macroFindForwards;
	bool    useRegex  = m_macroFindRegex;

	if (!continueFromCurrent || findText.isEmpty())
	{
		QDialog dialog(this);
		dialog.setWindowTitle(QStringLiteral("Find macro"));
		auto *dialogLayout = new QVBoxLayout(&dialog);
		auto *form         = new QFormLayout();
		auto *combo        = new QComboBox(&dialog);
		combo->setEditable(true);
		combo->addItems(m_macroFindHistory);
		if (!findText.isEmpty())
			combo->setCurrentText(findText);
		else if (!m_macroFindHistory.isEmpty())
			combo->setCurrentText(m_macroFindHistory.first());
		form->addRow(QStringLiteral("Find text:"), combo);
		auto *matchCaseBox = new QCheckBox(QStringLiteral("Match case"), &dialog);
		auto *forwardsBox  = new QCheckBox(QStringLiteral("Search forwards"), &dialog);
		auto *regexBox     = new QCheckBox(QStringLiteral("Regular expression"), &dialog);
		matchCaseBox->setChecked(matchCase);
		forwardsBox->setChecked(forwards);
		regexBox->setChecked(useRegex);
		form->addRow(QString(), matchCaseBox);
		form->addRow(QString(), forwardsBox);
		form->addRow(QString(), regexBox);
		dialogLayout->addLayout(form);
		auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
		connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
		connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
		dialogLayout->addWidget(buttons);
		if (dialog.exec() != QDialog::Accepted)
			return;
		findText  = combo->currentText();
		matchCase = matchCaseBox->isChecked();
		forwards  = forwardsBox->isChecked();
		useRegex  = regexBox->isChecked();
		if (findText.isEmpty())
			return;
		m_macroFindHistory.removeAll(findText);
		m_macroFindHistory.prepend(findText);
		m_macroFindText      = findText;
		m_macroFindMatchCase = matchCase;
		m_macroFindForwards  = forwards;
		m_macroFindRegex     = useRegex;
		m_macroFindRow       = forwards ? -1 : rowCount;
		updateMacroControls();
	}

	QRegularExpression regex;
	if (useRegex)
	{
		QRegularExpression::PatternOptions options = QRegularExpression::NoPatternOption;
		if (!matchCase)
			options |= QRegularExpression::CaseInsensitiveOption;
		regex = QRegularExpression(findText, options);
		if (!regex.isValid())
		{
			QMessageBox::information(this, QStringLiteral("Find macro"), regex.errorString());
			return;
		}
	}

	auto rowMatches = [&](const int row) -> bool
	{
		QTableWidgetItem *nameItem  = m_macrosTable->item(row, 0);
		QTableWidgetItem *sendItem  = m_macrosTable->item(row, 2);
		auto             *typeCombo = qobject_cast<QComboBox *>(m_macrosTable->cellWidget(row, 1));
		QString           haystack;
		if (nameItem)
			haystack += nameItem->text();
		if (sendItem)
			haystack += QStringLiteral(" ") + sendItem->text();
		if (typeCombo)
			haystack += QStringLiteral(" ") + typeCombo->currentText();
		if (useRegex)
			return regex.match(haystack).hasMatch();
		return haystack.contains(findText, matchCase ? Qt::CaseSensitive : Qt::CaseInsensitive);
	};

	int startRow = 0;
	if (continueFromCurrent && m_macroFindRow >= 0 && m_macroFindRow < rowCount)
		startRow = forwards ? (m_macroFindRow + 1) : (m_macroFindRow - 1);
	else if (!continueFromCurrent && m_macrosTable->currentRow() >= 0)
		startRow = m_macrosTable->currentRow();
	else
		startRow = forwards ? 0 : (rowCount - 1);

	if (forwards)
	{
		for (int row = startRow; row < rowCount; ++row)
		{
			if (rowMatches(row))
			{
				m_macrosTable->setCurrentCell(row, 0);
				m_macroFindRow = row;
				return;
			}
		}
		for (int row = 0; row < startRow; ++row)
		{
			if (rowMatches(row))
			{
				m_macrosTable->setCurrentCell(row, 0);
				m_macroFindRow = row;
				return;
			}
		}
	}
	else
	{
		for (int row = startRow; row >= 0; --row)
		{
			if (rowMatches(row))
			{
				m_macrosTable->setCurrentCell(row, 0);
				m_macroFindRow = row;
				return;
			}
		}
		for (int row = rowCount - 1; row > startRow; --row)
		{
			if (rowMatches(row))
			{
				m_macrosTable->setCurrentCell(row, 0);
				m_macroFindRow = row;
				return;
			}
		}
	}

	QMessageBox::information(this, QStringLiteral("Find macro"), QStringLiteral("No further matches found."));
}

bool WorldPreferencesDialog::saveMacrosToFile(const QString &fileName) const
{
	if (!m_runtime)
		return false;

	const QString outputPath = canonicalSavePath(fileName, QStringLiteral("qdm"));
	QSaveFile     file(outputPath);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
	{
		QMessageBox::warning(const_cast<WorldPreferencesDialog *>(this), QStringLiteral("Save macros"),
		                     QStringLiteral("Unable to create the requested file."));
		return false;
	}

	QTextStream out(&file);
	out.setEncoding(QStringConverter::Utf8);
	const QString   nl = QStringLiteral("\r\n");

	const QDateTime now     = QDateTime::currentDateTime();
	const QString   savedOn = QLocale::system().toString(now, QStringLiteral("dddd, MMMM dd, yyyy, h:mm AP"));

	out << R"(<?xml version="1.0" encoding="utf-8"?>)" << nl;
	out << "<!DOCTYPE qmud>" << nl;
	out << "<!-- Saved on " << fixHtmlString(savedOn) << " -->" << nl;
	out << "<!-- QMud version " << kVersionString << " -->" << nl;
	out << "<!-- Written by Panagiotis Kalogiratos -->" << nl;
	out << "<!-- Home Page: https://github.com/Nodens-/QMud -->" << nl;
	out << "<qmud>" << nl;

	out << nl << "<!-- macros -->" << nl;
	out << nl << "<macros" << nl;
	out << "   qmud_version=\"" << fixHtmlString(QString::fromLatin1(kVersionString)) << "\" " << nl;
	out << "   world_file_version=\"" << m_runtime->worldFileVersion() << "\" " << nl;
	out << "   date_saved=\"" << now.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) << "\" " << nl;
	out << "  >" << nl;

	const QList<WorldRuntime::Macro>           macros = m_runtime->macros();
	QMap<QString, const WorldRuntime::Macro *> macroMap;
	for (const auto &macro : macros)
	{
		if (const QString name = macro.attributes.value(QStringLiteral("name")).trimmed(); !name.isEmpty())
			macroMap.insert(name, &macro);
	}

	for (const QStringList macroNames = macroDescriptionList(); const QString &name : macroNames)
	{
		const WorldRuntime::Macro *macro = macroMap.value(name, nullptr);
		if (!macro)
			continue;
		const QString send = macro->children.value(QStringLiteral("send"));
		if (send.isEmpty())
			continue;
		QString type = macro->attributes.value(QStringLiteral("type")).trimmed();
		if (type.isEmpty())
			type = QStringLiteral("replace");
		if (type != QStringLiteral("replace") && type != QStringLiteral("send_now") &&
		    type != QStringLiteral("insert"))
			type = QStringLiteral("unknown");

		out << nl << "  <macro ";
		out << "name=\"" << fixHtmlString(name) << "\" ";
		out << "type=\"" << fixHtmlString(type) << "\" ";
		out << ">" << nl;
		out << "  <send>" << fixHtmlMultilineString(send) << "</send>";
		out << nl << "  </macro>" << nl;
	}

	out << "</macros>" << nl;
	out << "</qmud>" << nl;

	if (!file.commit())
	{
		QMessageBox::warning(const_cast<WorldPreferencesDialog *>(this), QStringLiteral("Save macros"),
		                     QStringLiteral("Unable to create the requested file."));
		return false;
	}

	return true;
}

bool WorldPreferencesDialog::loadMacrosFromFile(const QString &fileName)
{
	if (!m_runtime)
		return false;

	WorldDocument doc;
	doc.setLoadMask(WorldDocument::XML_MACROS | WorldDocument::XML_NO_PLUGINS |
	                WorldDocument::XML_IMPORT_MAIN_FILE_ONLY);
	if (!doc.loadFromFile(fileName))
	{
		QMessageBox::warning(this, QStringLiteral("Load macros"), doc.errorString());
		return false;
	}

	const QStringList          macroNames = macroDescriptionList();
	QList<WorldRuntime::Macro> macros;
	for (const auto &m : doc.macros())
	{
		WorldRuntime::Macro macro;
		macro.attributes        = m.attributes;
		macro.children          = m.children;
		const QString macroName = macro.attributes.value(QStringLiteral("name")).trimmed();
		if (const qsizetype index = macroNames.indexOf(macroName); index >= 0)
			macro.attributes.insert(QStringLiteral("index"), QString::number(index));
		macros.push_back(macro);
	}

	m_runtime->setMacros(macros);
	return true;
}

bool WorldPreferencesDialog::saveTriggersToFile(const QString &fileName) const
{
	if (!m_runtime)
		return false;

	const QString outputPath = canonicalSavePath(fileName, QStringLiteral("qdt"));
	QSaveFile     file(outputPath);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
	{
		QMessageBox::warning(const_cast<WorldPreferencesDialog *>(this), QStringLiteral("Save triggers"),
		                     QStringLiteral("Unable to create the requested file."));
		return false;
	}

	QTextStream out(&file);
	out.setEncoding(QStringConverter::Utf8);
	const QString   nl = QStringLiteral("\r\n");

	const QDateTime now     = QDateTime::currentDateTime();
	const QString   savedOn = QLocale::system().toString(now, QStringLiteral("dddd, MMMM dd, yyyy, h:mm AP"));

	out << R"(<?xml version="1.0" encoding="utf-8"?>)" << nl;
	out << "<!DOCTYPE qmud>" << nl;
	out << "<!-- Saved on " << fixHtmlString(savedOn) << " -->" << nl;
	out << "<!-- QMud version " << kVersionString << " -->" << nl;
	out << "<!-- Written by Panagiotis Kalogiratos -->" << nl;
	out << "<!-- Home Page: https://github.com/Nodens-/QMud -->" << nl;
	out << "<qmud>" << nl;

	out << nl << "<!-- triggers -->" << nl;
	out << nl << "<triggers" << nl;
	out << "   qmud_version=\"" << fixHtmlString(QString::fromLatin1(kVersionString)) << "\" " << nl;
	out << "   world_file_version=\"" << m_runtime->worldFileVersion() << "\" " << nl;
	out << "   date_saved=\"" << now.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) << "\" " << nl;
	out << "  >" << nl;

	for (const auto &tr : m_runtime->triggers())
	{
		out << nl << "  <trigger ";
		for (auto it = tr.attributes.begin(); it != tr.attributes.end(); ++it)
			out << it.key() << "=\"" << fixHtmlString(it.value()) << "\" ";
		out << ">" << nl;
		for (auto it = tr.children.begin(); it != tr.children.end(); ++it)
		{
			out << "  <" << it.key() << ">" << fixHtmlMultilineString(it.value()) << "</" << it.key() << ">"
			    << nl;
		}
		out << nl << "  </trigger>" << nl;
	}

	out << "</triggers>" << nl;
	out << "</qmud>" << nl;

	if (!file.commit())
	{
		QMessageBox::warning(const_cast<WorldPreferencesDialog *>(this), QStringLiteral("Save triggers"),
		                     QStringLiteral("Unable to create the requested file."));
		return false;
	}

	return true;
}

bool WorldPreferencesDialog::loadTriggersFromFile(const QString &fileName, const bool replace)
{
	if (!m_runtime)
		return false;

	WorldDocument doc;
	doc.setLoadMask(WorldDocument::XML_TRIGGERS | WorldDocument::XML_NO_PLUGINS |
	                WorldDocument::XML_IMPORT_MAIN_FILE_ONLY);
	if (!doc.loadFromFile(fileName))
	{
		QMessageBox::warning(this, QStringLiteral("Load triggers"), doc.errorString());
		return false;
	}

	QList<WorldRuntime::Trigger> combined;
	if (!replace)
		combined = m_runtime->triggers();
	for (const auto &t : doc.triggers())
	{
		WorldRuntime::Trigger rt;
		rt.attributes = t.attributes;
		rt.children   = t.children;
		rt.included   = t.included;
		combined.push_back(rt);
	}
	m_runtime->setTriggers(combined);
	return true;
}

bool WorldPreferencesDialog::saveAliasesToFile(const QString &fileName) const
{
	if (!m_runtime)
		return false;

	const QString outputPath = canonicalSavePath(fileName, QStringLiteral("qda"));
	QSaveFile     file(outputPath);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
	{
		QMessageBox::warning(const_cast<WorldPreferencesDialog *>(this), QStringLiteral("Save aliases"),
		                     QStringLiteral("Unable to create the requested file."));
		return false;
	}

	QTextStream out(&file);
	out.setEncoding(QStringConverter::Utf8);
	const QString   nl = QStringLiteral("\r\n");

	const QDateTime now     = QDateTime::currentDateTime();
	const QString   savedOn = QLocale::system().toString(now, QStringLiteral("dddd, MMMM dd, yyyy, h:mm AP"));

	out << R"(<?xml version="1.0" encoding="utf-8"?>)" << nl;
	out << "<!DOCTYPE qmud>" << nl;
	out << "<!-- Saved on " << fixHtmlString(savedOn) << " -->" << nl;
	out << "<!-- QMud version " << kVersionString << " -->" << nl;
	out << "<!-- Written by Panagiotis Kalogiratos -->" << nl;
	out << "<!-- Home Page: https://github.com/Nodens-/QMud -->" << nl;
	out << "<qmud>" << nl;

	out << nl << "<!-- aliases -->" << nl;
	out << nl << "<aliases" << nl;
	out << "   qmud_version=\"" << fixHtmlString(QString::fromLatin1(kVersionString)) << "\" " << nl;
	out << "   world_file_version=\"" << m_runtime->worldFileVersion() << "\" " << nl;
	out << "   date_saved=\"" << now.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) << "\" " << nl;
	out << "  >" << nl;

	for (const auto &al : m_runtime->aliases())
	{
		out << nl << "  <alias ";
		for (auto it = al.attributes.begin(); it != al.attributes.end(); ++it)
			out << it.key() << "=\"" << fixHtmlString(it.value()) << "\" ";
		out << ">" << nl;
		for (auto it = al.children.begin(); it != al.children.end(); ++it)
		{
			out << "  <" << it.key() << ">" << fixHtmlMultilineString(it.value()) << "</" << it.key() << ">"
			    << nl;
		}
		out << nl << "  </alias>" << nl;
	}

	out << "</aliases>" << nl;
	out << "</qmud>" << nl;

	if (!file.commit())
	{
		QMessageBox::warning(const_cast<WorldPreferencesDialog *>(this), QStringLiteral("Save aliases"),
		                     QStringLiteral("Unable to create the requested file."));
		return false;
	}

	return true;
}

bool WorldPreferencesDialog::loadAliasesFromFile(const QString &fileName, const bool replace)
{
	if (!m_runtime)
		return false;

	WorldDocument doc;
	doc.setLoadMask(WorldDocument::XML_ALIASES | WorldDocument::XML_NO_PLUGINS |
	                WorldDocument::XML_IMPORT_MAIN_FILE_ONLY);
	if (!doc.loadFromFile(fileName))
	{
		QMessageBox::warning(this, QStringLiteral("Load aliases"), doc.errorString());
		return false;
	}

	QList<WorldRuntime::Alias> combined;
	if (!replace)
		combined = m_runtime->aliases();
	for (const auto &a : doc.aliases())
	{
		WorldRuntime::Alias ra;
		ra.attributes = a.attributes;
		ra.children   = a.children;
		ra.included   = a.included;
		combined.push_back(ra);
	}
	m_runtime->setAliases(combined);
	return true;
}

bool WorldPreferencesDialog::saveTimersToFile(const QString &fileName) const
{
	if (!m_runtime)
		return false;

	const QString outputPath = canonicalSavePath(fileName, QStringLiteral("qdi"));
	QSaveFile     file(outputPath);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
	{
		QMessageBox::warning(const_cast<WorldPreferencesDialog *>(this), QStringLiteral("Save timers"),
		                     QStringLiteral("Unable to create the requested file."));
		return false;
	}

	QTextStream out(&file);
	out.setEncoding(QStringConverter::Utf8);
	const QString   nl = QStringLiteral("\r\n");

	const QDateTime now     = QDateTime::currentDateTime();
	const QString   savedOn = QLocale::system().toString(now, QStringLiteral("dddd, MMMM dd, yyyy, h:mm AP"));

	out << R"(<?xml version="1.0" encoding="utf-8"?>)" << nl;
	out << "<!DOCTYPE qmud>" << nl;
	out << "<!-- Saved on " << fixHtmlString(savedOn) << " -->" << nl;
	out << "<!-- QMud version " << kVersionString << " -->" << nl;
	out << "<!-- Written by Panagiotis Kalogiratos -->" << nl;
	out << "<!-- Home Page: https://github.com/Nodens-/QMud -->" << nl;
	out << "<qmud>" << nl;

	out << nl << "<!-- timers -->" << nl;
	out << nl << "<timers" << nl;
	out << "   qmud_version=\"" << fixHtmlString(QString::fromLatin1(kVersionString)) << "\" " << nl;
	out << "   world_file_version=\"" << m_runtime->worldFileVersion() << "\" " << nl;
	out << "   date_saved=\"" << now.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) << "\" " << nl;
	out << "  >" << nl;

	for (const auto &tm : m_runtime->timers())
	{
		out << nl << "  <timer ";
		for (auto it = tm.attributes.begin(); it != tm.attributes.end(); ++it)
			out << it.key() << "=\"" << fixHtmlString(it.value()) << "\" ";
		out << ">" << nl;
		for (auto it = tm.children.begin(); it != tm.children.end(); ++it)
		{
			out << "  <" << it.key() << ">" << fixHtmlMultilineString(it.value()) << "</" << it.key() << ">"
			    << nl;
		}
		out << nl << "  </timer>" << nl;
	}

	out << "</timers>" << nl;
	out << "</qmud>" << nl;

	if (!file.commit())
	{
		QMessageBox::warning(const_cast<WorldPreferencesDialog *>(this), QStringLiteral("Save timers"),
		                     QStringLiteral("Unable to create the requested file."));
		return false;
	}

	return true;
}

bool WorldPreferencesDialog::loadTimersFromFile(const QString &fileName, const bool replace)
{
	if (!m_runtime)
		return false;

	WorldDocument doc;
	doc.setLoadMask(WorldDocument::XML_TIMERS | WorldDocument::XML_NO_PLUGINS |
	                WorldDocument::XML_IMPORT_MAIN_FILE_ONLY);
	if (!doc.loadFromFile(fileName))
	{
		QMessageBox::warning(this, QStringLiteral("Load timers"), doc.errorString());
		return false;
	}

	QList<WorldRuntime::Timer> combined;
	if (!replace)
		combined = m_runtime->timers();
	for (const auto &t : doc.timers())
	{
		WorldRuntime::Timer rt;
		rt.attributes = t.attributes;
		rt.children   = t.children;
		rt.included   = t.included;
		combined.push_back(rt);
	}
	m_runtime->setTimers(combined);
	return true;
}

bool WorldPreferencesDialog::saveVariablesToFile(const QString &fileName) const
{
	if (!m_runtime)
		return false;

	const QString outputPath = canonicalSavePath(fileName, QStringLiteral("qdv"));
	QSaveFile     file(outputPath);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
	{
		QMessageBox::warning(const_cast<WorldPreferencesDialog *>(this), QStringLiteral("Save variables"),
		                     QStringLiteral("Unable to create the requested file."));
		return false;
	}

	QTextStream out(&file);
	out.setEncoding(QStringConverter::Utf8);
	const QString   nl = QStringLiteral("\r\n");

	const QDateTime now     = QDateTime::currentDateTime();
	const QString   savedOn = QLocale::system().toString(now, QStringLiteral("dddd, MMMM dd, yyyy, h:mm AP"));

	out << R"(<?xml version="1.0" encoding="utf-8"?>)" << nl;
	out << "<!DOCTYPE qmud>" << nl;
	out << "<!-- Saved on " << fixHtmlString(savedOn) << " -->" << nl;
	out << "<!-- QMud version " << kVersionString << " -->" << nl;
	out << "<!-- Written by Panagiotis Kalogiratos -->" << nl;
	out << "<!-- Home Page: https://github.com/Nodens-/QMud -->" << nl;
	out << "<qmud>" << nl;
	out << nl << "<!-- variables -->" << nl;
	out << "<variables>" << nl;

	for (const auto &var : m_runtime->variables())
	{
		const QString name = var.attributes.value(QStringLiteral("name"));
		out << "  <variable name=\"" << fixHtmlString(name) << "\">" << fixHtmlMultilineString(var.content)
		    << "</variable>" << nl;
	}

	out << "</variables>" << nl;
	out << "</qmud>" << nl;

	if (!file.commit())
	{
		QMessageBox::warning(const_cast<WorldPreferencesDialog *>(this), QStringLiteral("Save variables"),
		                     QStringLiteral("Unable to create the requested file."));
		return false;
	}

	return true;
}

bool WorldPreferencesDialog::loadVariablesFromFile(const QString &fileName)
{
	if (!m_runtime)
		return false;

	WorldDocument doc;
	doc.setLoadMask(WorldDocument::XML_VARIABLES | WorldDocument::XML_NO_PLUGINS |
	                WorldDocument::XML_IMPORT_MAIN_FILE_ONLY);
	if (!doc.loadFromFile(fileName))
	{
		QMessageBox::warning(this, QStringLiteral("Load variables"), doc.errorString());
		return false;
	}

	QList<WorldRuntime::Variable> vars;
	for (const auto &v : doc.variables())
	{
		WorldRuntime::Variable rv;
		rv.attributes = v.attributes;
		rv.content    = v.content;
		vars.push_back(rv);
	}
	m_runtime->setVariables(vars);
	return true;
}

void WorldPreferencesDialog::buildUi()
{
	auto *layout = new QVBoxLayout(this);
	layout->setSizeConstraint(QLayout::SetNoConstraint);
	auto *header = new GradientHeader(this);
	header->setText(windowTitle());
	if (AppController *app = AppController::instance())
		header->setGradientEnabled(app->getGlobalOption(QStringLiteral("ColourGradientConfig")).toInt() != 0);
	layout->addWidget(header);
	auto *contentLayout = new QHBoxLayout();
	m_pageTree          = new QTreeWidget(this);
	m_pageTree->setHeaderHidden(true);
	m_pageTree->setSelectionMode(QAbstractItemView::SingleSelection);
	m_pageTree->setMinimumWidth(220);
	m_pageTree->setRootIsDecorated(true);
	m_pages = new QStackedWidget(this);

	auto *generalPage       = new QWidget(this);
	auto *soundPage         = new QWidget(this);
	auto *customColoursPage = new QWidget(this);
	auto *loggingPage       = new QWidget(this);
	auto *ansiColoursPage   = new QWidget(this);
	auto *macrosPage        = new QWidget(this);
	auto *aliasesPage       = new QWidget(this);
	auto *triggersPage      = new QWidget(this);
	auto *commandsPage      = new QWidget(this);
	auto *sendToWorldPage   = new QWidget(this);
	auto *notesPage         = new QWidget(this);
	auto *keypadPage        = new QWidget(this);
	auto *pastePage         = new QWidget(this);
	auto *outputPage        = new QWidget(this);
	auto *infoPage          = new QWidget(this);
	auto *timersPage        = new QWidget(this);
	auto *scriptingPage     = new QWidget(this);
	auto *variablesPage     = new QWidget(this);
	auto *autoSayPage       = new QWidget(this);
	auto *printingPage      = new QWidget(this);
	auto *connectingPage    = new QWidget(this);
	auto *mxpPage           = new QWidget(this);
	auto *chatPage          = new QWidget(this);

	auto  addPageItem = [this](QTreeWidgetItem *parent, const QString &label, const Page page)
	{
		auto *item = new QTreeWidgetItem(parent);
		item->setText(0, label);
		item->setData(0, Qt::UserRole, page);
		m_pageItems.insert(page, item);
		return item;
	};
	auto addCategory = [this](const QString &label)
	{
		auto *item = new QTreeWidgetItem(m_pageTree);
		item->setText(0, label);
		item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
		bool expand = true;
		if (AppController *app = AppController::instance())
			expand = app->getGlobalOption(QStringLiteral("AutoExpandConfig")).toInt() != 0;
		item->setExpanded(expand);
		return item;
	};
	QTreeWidgetItem *generalCategory = addCategory(QStringLiteral("General"));
	addPageItem(generalCategory, QStringLiteral("IP address"), PageGeneral);
	addPageItem(generalCategory, QStringLiteral("Connecting"), PageConnecting);
	addPageItem(generalCategory, QStringLiteral("Logging"), PageLogging);
	addPageItem(generalCategory, QStringLiteral("Timers"), PageTimers);
	addPageItem(generalCategory, QStringLiteral("Chat"), PageChat);
	addPageItem(generalCategory, QStringLiteral("Info"), PageInfo);
	addPageItem(generalCategory, QStringLiteral("Notes"), PageNotes);

	QTreeWidgetItem *appearanceCategory = addCategory(QStringLiteral("Output"));
	addPageItem(appearanceCategory, QStringLiteral("Server Output"), PageOutput);
	addPageItem(appearanceCategory, QStringLiteral("MXP / Pueblo"), PageMxp);
	addPageItem(appearanceCategory, QStringLiteral("ANSI Colour"), PageAnsiColours);
	addPageItem(appearanceCategory, QStringLiteral("Custom Colour"), PageCustomColours);
	addPageItem(appearanceCategory, QStringLiteral("Triggers"), PageTriggers);
	addPageItem(appearanceCategory, QStringLiteral("Printing"), PagePrinting);

	QTreeWidgetItem *inputCategory = addCategory(QStringLiteral("Input"));
	addPageItem(inputCategory, QStringLiteral("Commands"), PageCommands);
	addPageItem(inputCategory, QStringLiteral("Aliases"), PageAliases);
	addPageItem(inputCategory, QStringLiteral("Keypad"), PageKeypad);
	addPageItem(inputCategory, QStringLiteral("Macros"), PageMacros);
	addPageItem(inputCategory, QStringLiteral("Auto Say"), PageAutoSay);

	QTreeWidgetItem *pasteCategory = addCategory(QStringLiteral("Paste/Send"));
	addPageItem(pasteCategory, QStringLiteral("Paste"), PagePaste);
	addPageItem(pasteCategory, QStringLiteral("Send"), PageSendToWorld);

	QTreeWidgetItem *scriptingCategory = addCategory(QStringLiteral("Scripting"));
	addPageItem(scriptingCategory, QStringLiteral("Scripts"), PageScripting);
	addPageItem(scriptingCategory, QStringLiteral("Variables"), PageVariables);

	m_pages->addWidget(generalPage);
	m_pages->addWidget(soundPage);
	m_pages->addWidget(customColoursPage);
	m_pages->addWidget(loggingPage);
	m_pages->addWidget(ansiColoursPage);
	m_pages->addWidget(macrosPage);
	m_pages->addWidget(aliasesPage);
	m_pages->addWidget(triggersPage);
	m_pages->addWidget(commandsPage);
	m_pages->addWidget(sendToWorldPage);
	m_pages->addWidget(notesPage);
	m_pages->addWidget(keypadPage);
	m_pages->addWidget(pastePage);
	m_pages->addWidget(outputPage);
	m_pages->addWidget(infoPage);
	m_pages->addWidget(timersPage);
	m_pages->addWidget(scriptingPage);
	m_pages->addWidget(variablesPage);
	m_pages->addWidget(autoSayPage);
	m_pages->addWidget(printingPage);
	m_pages->addWidget(connectingPage);
	m_pages->addWidget(mxpPage);
	m_pages->addWidget(chatPage);

	connect(m_pageTree, &QTreeWidget::currentItemChanged, this,
	        [this](const QTreeWidgetItem *current, QTreeWidgetItem *)
	        {
		        if (!current)
			        return;
		        if (const QVariant data = current->data(0, Qt::UserRole); data.isValid())
		        {
			        const int index = data.toInt();
			        if (index >= 0 && index < m_pages->count())
			        {
				        m_pages->setCurrentIndex(index);
				        if (m_runtime)
					        m_runtime->setLastPreferencesPage(index);
			        }
		        }
	        });
	m_pageTree->expandAll();
	setInitialPage(PageGeneral);

	// General
	auto *generalLayout     = new QGridLayout(generalPage);
	auto *generalFormWidget = new QWidget(generalPage);
	auto *generalForm       = new QHBoxLayout(generalFormWidget);
	auto *worldLabel        = new QLabel(QStringLiteral("World"), generalFormWidget);
	m_worldName             = new QLineEdit(generalFormWidget);
	generalForm->addStretch();
	generalForm->addWidget(worldLabel);
	generalForm->addWidget(m_worldName, 1);
	generalForm->addStretch();
	generalLayout->addWidget(generalFormWidget, 0, 0, 1, 2);

	auto *mudGroup  = new QGroupBox(QStringLiteral("MUD address and port"), generalPage);
	auto *mudLayout = new QGridLayout(mudGroup);
	m_host          = new QLineEdit(mudGroup);
	m_port          = new QSpinBox(mudGroup);
	m_port->setRange(1, 65535);
	m_clearCachedButton = new QPushButton(QStringLiteral("Clear Cached IP"), mudGroup);
	mudLayout->addWidget(new QLabel(QStringLiteral("TCP/IP"), mudGroup), 0, 0);
	mudLayout->addWidget(m_host, 0, 1, 1, 2);
	mudLayout->addWidget(new QLabel(QStringLiteral("Port"), mudGroup), 1, 0);
	mudLayout->addWidget(m_port, 1, 1);
	mudLayout->addWidget(m_clearCachedButton, 2, 1);
	generalLayout->addWidget(mudGroup, 1, 0);

	auto *proxyGroup  = new QGroupBox(QStringLiteral("Proxy"), generalPage);
	auto *proxyLayout = new QFormLayout(proxyGroup);
	m_proxyType       = new QComboBox(proxyGroup);
	m_proxyType->addItem(QStringLiteral("None"), eProxyServerNone);
	m_proxyType->addItem(QStringLiteral("SOCKS4"), eProxyServerSocks4);
	m_proxyType->addItem(QStringLiteral("SOCKS5"), eProxyServerSocks5);
	m_proxyServer = new QLineEdit(proxyGroup);
	m_proxyPort   = new QSpinBox(proxyGroup);
	m_proxyPort->setRange(0, 65535);
	m_proxyAuthButton = new QPushButton(QStringLiteral("Username/Password..."), proxyGroup);
	proxyLayout->addRow(QStringLiteral("Type"), m_proxyType);
	proxyLayout->addRow(QStringLiteral("Server"), m_proxyServer);
	proxyLayout->addRow(QStringLiteral("Port"), m_proxyPort);
	proxyLayout->addRow(QString(), m_proxyAuthButton);
	generalLayout->addWidget(proxyGroup, 1, 1);

	m_saveWorldAutomatically =
	    new QCheckBox(QStringLiteral("Save World Automatically On Close"), generalPage);
	generalLayout->addWidget(m_saveWorldAutomatically, 2, 0, 1, 2, Qt::AlignLeft);

	auto *autosaveWidget = new QWidget(generalPage);
	auto *autosaveLayout = new QHBoxLayout(autosaveWidget);
	autosaveLayout->setContentsMargins(0, 0, 0, 0);
	autosaveLayout->setSpacing(6);
	auto *autosaveLabel = new QLabel(QStringLiteral("Autosave every"), autosaveWidget);
	m_autosaveMinutes   = new QSpinBox(autosaveWidget);
	m_autosaveMinutes->setRange(0, 100000);
	m_autosaveMinutes->setSingleStep(5);
	m_autosaveMinutes->setValue(60);
	auto *autosaveUnits = new QLabel(QStringLiteral("minutes (0 = disabled)"), autosaveWidget);
	autosaveLayout->addWidget(autosaveLabel);
	autosaveLayout->addWidget(m_autosaveMinutes);
	autosaveLayout->addWidget(autosaveUnits);
	autosaveLayout->addStretch();
	generalLayout->addWidget(autosaveWidget, 3, 0, 1, 2, Qt::AlignLeft);

	auto *linkWidget = new QWidget(generalPage);
	auto *linkLayout = new QVBoxLayout(linkWidget);
	auto *bugLabel   = new QLabel(QStringLiteral("Found a bug? Have a suggestion? Report it"), linkWidget);
	bugLabel->setAlignment(Qt::AlignHCenter);
	m_bugReportLink = new QLabel(linkWidget);
	m_bugReportLink->setText(QStringLiteral("<a href=\"%1\">%1</a>")
	                             .arg(QStringLiteral("https://github.com/Nodens-/QMud/issues")));
	m_bugReportLink->setTextInteractionFlags(Qt::TextBrowserInteraction);
	m_bugReportLink->setOpenExternalLinks(true);
	m_bugReportLink->setAlignment(Qt::AlignHCenter);
	linkLayout->addWidget(bugLabel);
	linkLayout->addWidget(m_bugReportLink);
	generalLayout->addWidget(linkWidget, 4, 0, 1, 2);
	generalLayout->setColumnStretch(0, 1);
	generalLayout->setColumnStretch(1, 1);
	generalLayout->setRowStretch(5, 1);

	// Custom colors
	auto *customColoursLayout = new QVBoxLayout(customColoursPage);
	auto *customMainRow       = new QHBoxLayout();
	auto *customLeftRow       = new QHBoxLayout();
	customLeftRow->setContentsMargins(0, 0, 0, 0);
	constexpr int nameToSwatchGap = 16;
	customLeftRow->setSpacing(nameToSwatchGap);
	auto *customGrid = new QGridLayout();
	customGrid->setHorizontalSpacing(8);
	customGrid->setVerticalSpacing(4);
	customGrid->setContentsMargins(0, 0, 0, 0);
	customGrid->setSizeConstraint(QLayout::SetFixedSize);
	constexpr QSize customSwatchSize(28, 20);
	auto           *customNameLabel = new QLabel(QStringLiteral("Custom Colour Name"), customColoursPage);
	auto           *customTextLabel = new QLabel(QStringLiteral("Text"), customColoursPage);
	auto           *customBackLabel = new QLabel(QStringLiteral("Background"), customColoursPage);
	customTextLabel->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
	customBackLabel->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
	customGrid->addWidget(customNameLabel, 0, 0, Qt::AlignLeft);
	customGrid->addWidget(customTextLabel, 0, 1, Qt::AlignHCenter);
	customGrid->addWidget(customBackLabel, 0, 2, Qt::AlignHCenter);
	const int textLabelWidth = qMax(customTextLabel->sizeHint().width(), customSwatchSize.width());
	const int backLabelWidth = qMax(customBackLabel->sizeHint().width(), customSwatchSize.width());
	customGrid->setColumnMinimumWidth(1, textLabelWidth);
	customGrid->setColumnMinimumWidth(2, backLabelWidth);
	customGrid->setColumnStretch(0, 0);
	customGrid->setColumnStretch(1, 0);
	customGrid->setColumnStretch(2, 0);
	m_customColourNames.resize(MAX_CUSTOM);
	m_customTextSwatches.resize(MAX_CUSTOM);
	m_customBackSwatches.resize(MAX_CUSTOM);
	for (int i = 0; i < MAX_CUSTOM; ++i)
	{
		auto *nameEdit = new QLineEdit(customColoursPage);
		nameEdit->setText(QStringLiteral("Custom%1").arg(i + 1));
		nameEdit->setFixedWidth(160);
		auto *textSwatch = new QPushButton(customColoursPage);
		textSwatch->setFixedSize(customSwatchSize);
		textSwatch->setFlat(true);
		textSwatch->setFocusPolicy(Qt::NoFocus);
		auto *backSwatch = new QPushButton(customColoursPage);
		backSwatch->setFixedSize(customSwatchSize);
		backSwatch->setFlat(true);
		backSwatch->setFocusPolicy(Qt::NoFocus);
		customGrid->addWidget(nameEdit, i + 1, 0);
		customGrid->addWidget(textSwatch, i + 1, 1, Qt::AlignHCenter);
		customGrid->addWidget(backSwatch, i + 1, 2, Qt::AlignHCenter);
		m_customColourNames[i]  = nameEdit;
		m_customTextSwatches[i] = textSwatch;
		m_customBackSwatches[i] = backSwatch;
	}
	m_customSwap              = new QPushButton(QStringLiteral("<- Swap ->"), customColoursPage);
	const int swatchSpanWidth = textLabelWidth + backLabelWidth + customGrid->horizontalSpacing();
	const int swapMinWidth    = m_customSwap->sizeHint().width() + 8;
	m_customSwap->setMinimumWidth(swapMinWidth);
	const int    swapRowWidth = qMax(swatchSpanWidth, swapMinWidth);
	const double swatchMid =
	    (0.75 * textLabelWidth) + (0.5 * customGrid->horizontalSpacing()) + (0.25 * backLabelWidth);
	const int swapOffset = qMax(0, static_cast<int>(std::lround(swatchMid - (swapMinWidth / 2.0))));
	auto     *swapRow    = new QWidget(customColoursPage);
	swapRow->setFixedWidth(swapRowWidth);
	auto *swapLayout = new QHBoxLayout(swapRow);
	swapLayout->setContentsMargins(swapOffset, 0, 0, 0);
	swapLayout->addWidget(m_customSwap, 0, Qt::AlignLeft);
	swapLayout->addStretch();
	customGrid->addWidget(swapRow, MAX_CUSTOM + 1, 1, 1, 2, Qt::AlignLeft);
	auto *customGridWidget = new QWidget(customColoursPage);
	customGridWidget->setLayout(customGrid);
	customGridWidget->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	customLeftRow->addWidget(customGridWidget, 0, Qt::AlignTop);
	auto *customLeftWidget = new QWidget(customColoursPage);
	customLeftWidget->setLayout(customLeftRow);
	customLeftWidget->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	customMainRow->addWidget(customLeftWidget);
	customMainRow->addStretch();
	auto *customButtons     = new QGridLayout();
	m_customDefaults        = new QPushButton(QStringLiteral("De&faults..."), customColoursPage);
	m_customInvert          = new QPushButton(QStringLiteral("&Invert"), customColoursPage);
	m_customRandom          = new QPushButton(QStringLiteral("&Random..."), customColoursPage);
	m_customLighter         = new QPushButton(QStringLiteral("&Lighter"), customColoursPage);
	m_customDarker          = new QPushButton(QStringLiteral("&Darker"), customColoursPage);
	m_customMoreColour      = new QPushButton(QStringLiteral("&More colour"), customColoursPage);
	m_customLessColour      = new QPushButton(QStringLiteral("L&ess colour"), customColoursPage);
	auto shrinkCustomButton = [](QPushButton *button)
	{
		if (!button)
			return;
		button->setFixedHeight(28);
		button->setMinimumWidth(150);
		button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	};
	shrinkCustomButton(m_customDefaults);
	shrinkCustomButton(m_customLighter);
	shrinkCustomButton(m_customDarker);
	shrinkCustomButton(m_customMoreColour);
	shrinkCustomButton(m_customLessColour);
	shrinkCustomButton(m_customInvert);
	shrinkCustomButton(m_customRandom);
	customButtons->addWidget(m_customDefaults, 0, 0, 1, 2, Qt::AlignHCenter);
	customButtons->addWidget(m_customLighter, 1, 0, 1, 2, Qt::AlignHCenter);
	customButtons->addWidget(m_customDarker, 2, 0, 1, 2, Qt::AlignHCenter);
	customButtons->addWidget(m_customMoreColour, 3, 0, 1, 2, Qt::AlignHCenter);
	customButtons->addWidget(m_customLessColour, 4, 0, 1, 2, Qt::AlignHCenter);
	customButtons->addWidget(m_customInvert, 5, 0, 1, 2, Qt::AlignHCenter);
	customButtons->addWidget(m_customRandom, 6, 0, 1, 2, Qt::AlignHCenter);
	auto *customButtonsInner = new QWidget(customColoursPage);
	customButtonsInner->setLayout(customButtons);
	customButtonsInner->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	auto *customButtonsColumn = new QVBoxLayout();
	customButtonsColumn->setContentsMargins(0, 0, 0, 0);
	customButtonsColumn->addWidget(customButtonsInner, 0, Qt::AlignHCenter);
	customButtonsColumn->addStretch();
	auto *customButtonsWidget = new QWidget(customColoursPage);
	customButtonsWidget->setLayout(customButtonsColumn);
	customButtonsWidget->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	auto *customRightColumn = new QVBoxLayout();
	customRightColumn->setContentsMargins(0, 0, 0, 0);
	customRightColumn->addStretch();
	customRightColumn->addWidget(customButtonsWidget, 0, Qt::AlignHCenter);
	customRightColumn->addStretch();
	customMainRow->addLayout(customRightColumn);
	customColoursLayout->addLayout(customMainRow);

	auto *ansiCompareBox =
	    new QGroupBox(QStringLiteral("ANSI Colours for this world (for comparison)"), customColoursPage);
	auto *ansiCompareLayout = new QGridLayout(ansiCompareBox);
	m_customAnsiNormal.clear();
	m_customAnsiBold.clear();
	for (int i = 0; i < 8; ++i)
	{
		auto *normal = new QFrame(ansiCompareBox);
		normal->setFrameShape(QFrame::Box);
		normal->setFixedSize(18, 18);
		normal->setAutoFillBackground(true);
		m_customAnsiNormal.push_back(normal);
		ansiCompareLayout->addWidget(normal, 0, i);
		auto *bold = new QFrame(ansiCompareBox);
		bold->setFrameShape(QFrame::Box);
		bold->setFixedSize(18, 18);
		bold->setAutoFillBackground(true);
		m_customAnsiBold.push_back(bold);
		ansiCompareLayout->addWidget(bold, 1, i);
	}
	ansiCompareBox->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	if (customRightColumn)
		customRightColumn->addWidget(ansiCompareBox, 0, Qt::AlignHCenter);
	auto *customAnsiNote =
	    new QLabel(QStringLiteral("Use the ANSI Colour configuration tab to change the ANSI colours"),
	               customColoursPage);
	customAnsiNote->setAlignment(Qt::AlignRight);
	customColoursLayout->addWidget(customAnsiNote);

	if (m_proxyAuthButton)
		connect(m_proxyAuthButton, &QPushButton::clicked, this,
		        [this]
		        {
			        QDialog dialog(this);
			        dialog.setWindowTitle(QStringLiteral("Proxy authentication"));
			        auto *form = new QFormLayout(&dialog);
			        auto *user = new QLineEdit(&dialog);
			        auto *pass = new QLineEdit(&dialog);
			        pass->setEchoMode(QLineEdit::Password);
			        user->setText(m_proxyUsername);
			        pass->setText(m_proxyPassword);
			        form->addRow(QStringLiteral("Username"), user);
			        form->addRow(QStringLiteral("Password"), pass);
			        auto *buttons =
			            new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
			        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
			        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
			        form->addRow(buttons);
			        if (dialog.exec() == QDialog::Accepted)
			        {
				        m_proxyUsername = user->text();
				        m_proxyPassword = pass->text();
			        }
		        });
	if (m_clearCachedButton)
		connect(m_clearCachedButton, &QPushButton::clicked, this,
		        [this]
		        {
			        if (!m_runtime)
				        return;
			        m_runtime->resetIpCache();
			        updateClearCachedButton();
		        });

	// Logging
	auto *loggingLayout   = new QGridLayout(loggingPage);
	m_logOutput           = new QCheckBox(QStringLiteral("Log Output"), loggingPage);
	m_logInput            = new QCheckBox(QStringLiteral("Log Commands"), loggingPage);
	m_logNotes            = new QCheckBox(QStringLiteral("Log Notes"), loggingPage);
	m_logHtml             = new QCheckBox(QStringLiteral("HTML"), loggingPage);
	m_logRaw              = new QCheckBox(QStringLiteral("Raw"), loggingPage);
	m_logInColour         = new QCheckBox(QStringLiteral("Colour"), loggingPage);
	m_writeWorldNameToLog = new QCheckBox(QStringLiteral("Write world name to log file"), loggingPage);
	m_logRotateMb         = new QSpinBox(loggingPage);
	m_logRotateGzip       = new QCheckBox(QStringLiteral("Gzip logs on close"), loggingPage);
	m_autoLogFileName     = new QLineEdit(loggingPage);
	m_browseLogFile       = new QPushButton(QStringLiteral("&Browse..."), loggingPage);
	m_logFilePreamble     = new QTextEdit(loggingPage);
	m_logFilePostamble    = new QTextEdit(loggingPage);
	m_standardPreamble    = new QPushButton(QStringLiteral("Standard HTML preamble/postamble"), loggingPage);
	m_editPreamble        = new QPushButton(QStringLiteral("..."), loggingPage);
	m_editPostamble       = new QPushButton(QStringLiteral("..."), loggingPage);
	m_substitutionHelp    = new QPushButton(QStringLiteral("?"), loggingPage);
	m_editPreamble->setFixedWidth(24);
	m_editPostamble->setFixedWidth(24);
	m_substitutionHelp->setFixedWidth(24);

	loggingLayout->addWidget(new QLabel(QStringLiteral("File"), loggingPage), 0, 0);
	loggingLayout->addWidget(m_logFilePreamble, 0, 1);
	auto *preambleButtons = new QVBoxLayout();
	preambleButtons->addWidget(m_substitutionHelp);
	preambleButtons->addWidget(m_editPreamble);
	preambleButtons->addStretch();
	loggingLayout->addLayout(preambleButtons, 0, 2);

	loggingLayout->addWidget(new QLabel(QStringLiteral("File"), loggingPage), 1, 0);
	loggingLayout->addWidget(m_logFilePostamble, 1, 1);
	auto *postambleButtons = new QVBoxLayout();
	postambleButtons->addWidget(m_editPostamble);
	postambleButtons->addStretch();
	loggingLayout->addLayout(postambleButtons, 1, 2);

	loggingLayout->addWidget(m_standardPreamble, 2, 1, 1, 2, Qt::AlignLeft);

	auto *whatToLogBox    = new QGroupBox(QStringLiteral("What to log"), loggingPage);
	auto *whatToLogLayout = new QVBoxLayout(whatToLogBox);
	whatToLogLayout->addWidget(m_logOutput);
	whatToLogLayout->addWidget(m_logInput);
	whatToLogLayout->addWidget(m_logNotes);
	loggingLayout->addWidget(whatToLogBox, 3, 0);

	auto *formatBox    = new QGroupBox(QStringLiteral("Format of log"), loggingPage);
	auto *formatLayout = new QVBoxLayout(formatBox);
	formatLayout->addWidget(m_logHtml);
	formatLayout->addWidget(m_logInColour);
	formatLayout->addWidget(m_logRaw);
	loggingLayout->addWidget(formatBox, 3, 1);
	loggingLayout->addWidget(m_writeWorldNameToLog, 3, 2, Qt::AlignLeft);

	if (m_logRotateMb)
	{
		m_logRotateMb->setRange(0, 1024 * 1024);
		m_logRotateMb->setValue(100);
		m_logRotateMb->setSuffix(QStringLiteral(" MB"));
		m_logRotateMb->setToolTip(
		    QStringLiteral("Rotate log when file reaches this size in MB. 0 disables rotation."));
	}
	if (m_logRotateGzip)
	{
		m_logRotateGzip->setChecked(true);
		m_logRotateGzip->setToolTip(
		    QStringLiteral("When enabled, compress the current log as .gz whenever it is "
		                   "closed (manual close, disconnect, or rotation)."));
	}
	auto *rotateLabel = new QLabel(QStringLiteral("Log rotate"), loggingPage);
	rotateLabel->setToolTip(
	    QStringLiteral("Rotate log when file reaches this size in MB. 0 disables rotation."));
	auto *rotateLayout = new QHBoxLayout();
	rotateLayout->addWidget(m_logRotateMb);
	rotateLayout->addSpacing(12);
	rotateLayout->addWidget(m_logRotateGzip);
	rotateLayout->addStretch();
	loggingLayout->addWidget(rotateLabel, 4, 0);
	loggingLayout->addLayout(rotateLayout, 4, 1, 1, 2);

	auto *autoLogLayout = new QHBoxLayout();
	autoLogLayout->addWidget(m_autoLogFileName, 1);
	autoLogLayout->addWidget(m_browseLogFile);
	loggingLayout->addWidget(new QLabel(QStringLiteral("Automatically log to this"), loggingPage), 5, 0);
	loggingLayout->addLayout(autoLogLayout, 5, 1, 1, 2);

	m_logLinePreambleOutput  = new QLineEdit(loggingPage);
	m_logLinePreambleInput   = new QLineEdit(loggingPage);
	m_logLinePreambleNotes   = new QLineEdit(loggingPage);
	m_logLinePostambleOutput = new QLineEdit(loggingPage);
	m_logLinePostambleInput  = new QLineEdit(loggingPage);
	m_logLinePostambleNotes  = new QLineEdit(loggingPage);
	loggingLayout->addWidget(new QLabel(QStringLiteral("Preamble"), loggingPage), 6, 1, Qt::AlignHCenter);
	loggingLayout->addWidget(new QLabel(QStringLiteral("Postamble"), loggingPage), 6, 2, Qt::AlignHCenter);
	loggingLayout->addWidget(new QLabel(QStringLiteral("Output"), loggingPage), 7, 0);
	loggingLayout->addWidget(m_logLinePreambleOutput, 7, 1);
	loggingLayout->addWidget(m_logLinePostambleOutput, 7, 2);
	loggingLayout->addWidget(new QLabel(QStringLiteral("Commands"), loggingPage), 8, 0);
	loggingLayout->addWidget(m_logLinePreambleInput, 8, 1);
	loggingLayout->addWidget(m_logLinePostambleInput, 8, 2);
	loggingLayout->addWidget(new QLabel(QStringLiteral("Script"), loggingPage), 9, 0);
	loggingLayout->addWidget(m_logLinePreambleNotes, 9, 1);
	loggingLayout->addWidget(m_logLinePostambleNotes, 9, 2);
	if (auto *label = qobject_cast<QLabel *>(loggingLayout->itemAtPosition(0, 0)->widget()))
		label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	if (auto *label = qobject_cast<QLabel *>(loggingLayout->itemAtPosition(1, 0)->widget()))
		label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	loggingLayout->setHorizontalSpacing(8);
	loggingLayout->setColumnMinimumWidth(0, 40);
	loggingLayout->setColumnStretch(1, 1);
	loggingLayout->setRowStretch(10, 1);
	connect(m_logHtml, &QCheckBox::toggled, this,
	        [this](const bool checked)
	        {
		        if (m_logInColour)
			        m_logInColour->setEnabled(checked);
		        if (m_standardPreamble)
			        m_standardPreamble->setEnabled(checked);
	        });
	connect(m_browseLogFile, &QPushButton::clicked, this,
	        [this]
	        {
		        const QString startDir = m_runtime ? m_runtime->defaultLogDirectory() : QString();
		        const QString fileName = QFileDialog::getSaveFileName(
		            this, QStringLiteral("Log file name"), startDir, QStringLiteral("Text files (*.txt)"));
		        if (!fileName.isEmpty())
		        {
			        if (m_runtime)
				        m_runtime->setFileBrowsingDirectory(QFileInfo(fileName).absolutePath());
			        m_autoLogFileName->setText(fileName);
		        }
	        });
	connect(m_standardPreamble, &QPushButton::clicked, this,
	        [this]
	        {
		        const QString preamble =
		            QStringLiteral("<html>\n"
		                           " <head>\n"
		                           " <title>Log of %N session</title>\n"
		                           " <style type=\"text/css\">\n"
		                           "   body {background-color: black;}\n"
		                           " </style>\n"
		                           " </head>\n"
		                           " <body>\n"
		                           "   <pre><code>\n"
		                           "   <font size=2 face=\"DejaVu Sans Mono, Consolas, Menlo, Monaco, "
		                           "Courier New, Courier\">\n");
		        const QString postamble = QStringLiteral("</font></code></pre>\n"
		                                                 "</body>\n"
		                                                 "</html>\n");
		        m_logFilePreamble->setPlainText(preamble);
		        m_logFilePostamble->setPlainText(postamble);
	        });
	connect(m_editPreamble, &QPushButton::clicked, this, [this]
	        { editPlainTextWithDialog(this, m_logFilePreamble, QStringLiteral("Edit log file preamble")); });
	connect(
	    m_editPostamble, &QPushButton::clicked, this, [this]
	    { editPlainTextWithDialog(this, m_logFilePostamble, QStringLiteral("Edit log file postamble")); });
	connect(m_substitutionHelp, &QPushButton::clicked, this,
	        [this]
	        {
		        QDialog dialog(this);
		        dialog.setWindowTitle(QStringLiteral("Special characters"));
		        auto *dialogLayout = new QVBoxLayout(&dialog);
		        auto *text         = new QTextEdit(&dialog);
		        text->setReadOnly(true);
		        QFile file(QStringLiteral(":/resources/qmud/text/substitutions.txt"));
		        if (file.open(QIODevice::ReadOnly | QIODevice::Text))
		        {
			        QTextStream stream(&file);
			        text->setPlainText(stream.readAll());
		        }
		        else
		        {
			        text->setPlainText(QStringLiteral("Substitution help file is not available."));
		        }
		        dialogLayout->addWidget(text);
		        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok, &dialog);
		        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
		        dialogLayout->addWidget(buttons);
		        dialog.exec();
	        });

	// ANSI colors
	auto *ansiColoursLayout = new QVBoxLayout(ansiColoursPage);
	auto *ansiTop           = new QHBoxLayout();
	auto *ansiSwatchGrid    = new QGridLayout();
	ansiSwatchGrid->setHorizontalSpacing(4);
	ansiSwatchGrid->setVerticalSpacing(0);
	ansiSwatchGrid->setContentsMargins(0, 0, 0, 0);
	const int headerTextWidth =
	    QFontMetrics(ansiColoursPage->font()).horizontalAdvance(QStringLiteral("Normal")) + 6;
	const int   swatchWidth = qMax(28, headerTextWidth);
	const QSize swatchSize(swatchWidth, 20);
	auto       *normalHeader = new QLabel(QStringLiteral("Normal"), ansiColoursPage);
	normalHeader->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
	normalHeader->setContentsMargins(0, 0, 0, 0);
	normalHeader->setFixedWidth(swatchSize.width());
	auto *boldHeader = new QLabel(QStringLiteral("Bold"), ansiColoursPage);
	boldHeader->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
	boldHeader->setContentsMargins(0, 0, 0, 0);
	boldHeader->setFixedWidth(swatchSize.width());
	ansiSwatchGrid->addWidget(normalHeader, 0, 1, Qt::AlignBottom | Qt::AlignHCenter);
	ansiSwatchGrid->addWidget(boldHeader, 0, 2, Qt::AlignBottom | Qt::AlignHCenter);
	static const QStringList colourNames = {
	    QStringLiteral("Black"), QStringLiteral("Red"),     QStringLiteral("Green"), QStringLiteral("Yellow"),
	    QStringLiteral("Blue"),  QStringLiteral("Magenta"), QStringLiteral("Cyan"),  QStringLiteral("White")};
	m_ansiNormalSwatches.resize(8);
	m_ansiBoldSwatches.resize(8);
	for (int i = 0; i < 8; ++i)
	{
		auto *nameLabel = new QLabel(colourNames.value(i), ansiColoursPage);
		nameLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
		ansiSwatchGrid->addWidget(nameLabel, i + 1, 0);
		auto *normalSwatch = new QPushButton(ansiColoursPage);
		normalSwatch->setFixedSize(swatchSize);
		normalSwatch->setFlat(true);
		normalSwatch->setFocusPolicy(Qt::NoFocus);
		if (i == 0)
			normalSwatch->setText(QStringLiteral("B"));
		else if (i == 7)
			normalSwatch->setText(QStringLiteral("T"));
		ansiSwatchGrid->addWidget(normalSwatch, i + 1, 1, Qt::AlignHCenter);
		m_ansiNormalSwatches[i] = normalSwatch;
		auto *boldSwatch        = new QPushButton(ansiColoursPage);
		boldSwatch->setFixedSize(swatchSize);
		boldSwatch->setFlat(true);
		boldSwatch->setFocusPolicy(Qt::NoFocus);
		ansiSwatchGrid->addWidget(boldSwatch, i + 1, 2, Qt::AlignHCenter);
		m_ansiBoldSwatches[i] = boldSwatch;
	}
	const int nameWidth =
	    QFontMetrics(ansiColoursPage->font()).horizontalAdvance(QStringLiteral("Magenta")) + 12;
	ansiSwatchGrid->setColumnMinimumWidth(0, nameWidth);
	ansiSwatchGrid->setColumnMinimumWidth(1, swatchSize.width());
	ansiSwatchGrid->setColumnMinimumWidth(2, swatchSize.width());
	ansiSwatchGrid->setColumnStretch(1, 0);
	ansiSwatchGrid->setColumnStretch(2, 0);
	auto *legendB = new QLabel(QStringLiteral("B = Normal Background"), ansiColoursPage);
	auto *legendT = new QLabel(QStringLiteral("T = Normal Text"), ansiColoursPage);
	legendB->setIndent(0);
	legendT->setIndent(0);
	ansiSwatchGrid->addWidget(legendB, 1, 3, Qt::AlignLeft | Qt::AlignVCenter);
	ansiSwatchGrid->addWidget(legendT, 8, 3, Qt::AlignLeft | Qt::AlignVCenter);
	auto *normalLighter = new QPushButton(QStringLiteral("Lighter"), ansiColoursPage);
	auto *normalDarker  = new QPushButton(QStringLiteral("Darker"), ansiColoursPage);
	auto *boldLighter   = new QPushButton(QStringLiteral("Lighter"), ansiColoursPage);
	auto *boldDarker    = new QPushButton(QStringLiteral("Darker"), ansiColoursPage);
	auto *normalAdjust  = new QVBoxLayout();
	normalAdjust->setContentsMargins(0, 0, 0, 0);
	normalAdjust->addWidget(normalLighter);
	normalAdjust->addWidget(normalDarker);
	auto *boldAdjust = new QVBoxLayout();
	boldAdjust->setContentsMargins(0, 0, 0, 0);
	boldAdjust->addWidget(boldLighter);
	boldAdjust->addWidget(boldDarker);
	ansiSwatchGrid->addLayout(normalAdjust, 9, 1, Qt::AlignHCenter);
	ansiSwatchGrid->addLayout(boldAdjust, 9, 2, Qt::AlignHCenter);
	ansiTop->addLayout(ansiSwatchGrid, 0);
	auto readCustomColours = [this]() -> QVector<QPair<QColor, QColor>>
	{
		QVector<QPair<QColor, QColor>> colours(MAX_CUSTOM);
		if (m_customTextSwatches.size() < MAX_CUSTOM || m_customBackSwatches.size() < MAX_CUSTOM)
			return colours;
		for (int i = 0; i < MAX_CUSTOM; ++i)
		{
			const QColor textColour = swatchButtonColour(m_customTextSwatches.value(i));
			const QColor backColour = swatchButtonColour(m_customBackSwatches.value(i));
			colours[i]              = qMakePair(textColour, backColour);
		}
		return colours;
	};
	auto readCustomNames = [this]() -> QVector<QString>
	{
		QVector<QString> names(MAX_CUSTOM);
		if (m_customColourNames.size() < MAX_CUSTOM)
			return names;
		for (int i = 0; i < MAX_CUSTOM; ++i)
		{
			names[i] = m_customColourNames[i] ? m_customColourNames[i]->text() : QString();
		}
		return names;
	};
	auto writeCustomColours = [this](const QVector<QPair<QColor, QColor>> &colours)
	{
		if (m_customTextSwatches.size() < MAX_CUSTOM || m_customBackSwatches.size() < MAX_CUSTOM)
			return;
		for (int i = 0; i < MAX_CUSTOM && i < colours.size(); ++i)
		{
			setSwatchButtonColour(m_customTextSwatches.value(i), colours.at(i).first);
			setSwatchButtonColour(m_customBackSwatches.value(i), colours.at(i).second);
		}
	};
	auto connectCustomSwatch = [this](QPushButton *swatch)
	{
		if (!swatch)
			return;
		connect(swatch, &QPushButton::clicked, this,
		        [this, swatch]
		        {
			        const QColor       current = swatchButtonColour(swatch);
			        ColourPickerDialog dlg(this);
			        dlg.setWindowTitle(QStringLiteral("Select colour"));
			        dlg.setPickColour(true);
			        if (current.isValid())
				        dlg.setInitialColour(current);
			        if (dlg.exec() != QDialog::Accepted)
				        return;
			        const QColor chosen = dlg.selectedColour();
			        if (!chosen.isValid())
				        return;
			        setSwatchButtonColour(swatch, chosen);
			        updateScriptNoteSwatches();
		        });
	};
	for (auto *swatch : m_customTextSwatches)
		connectCustomSwatch(swatch);
	for (auto *swatch : m_customBackSwatches)
		connectCustomSwatch(swatch);
	auto readAnsiColours = [this](QVector<QColor> &normal, QVector<QColor> &bold)
	{
		normal.fill(QColor(), 8);
		bold.fill(QColor(), 8);
		if (m_ansiNormalSwatches.size() < 8 || m_ansiBoldSwatches.size() < 8)
			return;
		for (int row = 0; row < 8; ++row)
		{
			normal[row] = swatchButtonColour(m_ansiNormalSwatches.value(row));
			bold[row]   = swatchButtonColour(m_ansiBoldSwatches.value(row));
		}
	};
	auto writeAnsiColours = [this](const QVector<QColor> &normal, const QVector<QColor> &bold)
	{
		if (m_ansiNormalSwatches.size() < 8 || m_ansiBoldSwatches.size() < 8)
			return;
		for (int row = 0; row < 8; ++row)
		{
			const QColor normalColour = normal.value(row, Qt::black);
			const QColor boldColour   = bold.value(row, Qt::black);
			setSwatchButtonColour(m_ansiNormalSwatches.value(row), normalColour);
			setSwatchButtonColour(m_ansiBoldSwatches.value(row), boldColour);
		}
		for (int i = 0; i < 8; ++i)
		{
			if (i < m_customAnsiNormal.size() && m_customAnsiNormal[i])
			{
				QPalette pal = m_customAnsiNormal[i]->palette();
				pal.setColor(QPalette::Window, normal.value(i));
				m_customAnsiNormal[i]->setPalette(pal);
			}
			if (i < m_customAnsiBold.size() && m_customAnsiBold[i])
			{
				QPalette pal = m_customAnsiBold[i]->palette();
				pal.setColor(QPalette::Window, bold.value(i));
				m_customAnsiBold[i]->setPalette(pal);
			}
		}
	};
	auto updateAnsiCompare = [readAnsiColours, writeAnsiColours]
	{
		QVector<QColor> normal(8);
		QVector<QColor> bold(8);
		readAnsiColours(normal, bold);
		writeAnsiColours(normal, bold);
	};
	auto connectAnsiSwatch = [this, updateAnsiCompare](QPushButton *swatch)
	{
		if (!swatch)
			return;
		connect(swatch, &QPushButton::clicked, this,
		        [this, swatch, updateAnsiCompare]
		        {
			        const QColor       current = swatchButtonColour(swatch);
			        ColourPickerDialog dlg(this);
			        dlg.setWindowTitle(QStringLiteral("Select colour"));
			        dlg.setPickColour(true);
			        if (current.isValid())
				        dlg.setInitialColour(current);
			        if (dlg.exec() != QDialog::Accepted)
				        return;
			        const QColor chosen = dlg.selectedColour();
			        if (!chosen.isValid())
				        return;
			        setSwatchButtonColour(swatch, chosen);
			        updateAnsiCompare();
		        });
	};
	for (auto *swatch : m_ansiNormalSwatches)
		connectAnsiSwatch(swatch);
	for (auto *swatch : m_ansiBoldSwatches)
		connectAnsiSwatch(swatch);
	auto applyCustomAdjust = [readCustomColours, writeCustomColours](const short method)
	{
		QVector<QPair<QColor, QColor>> colours = readCustomColours();
		for (auto &colour : colours)
		{
			if (colour.first.isValid())
				colour.first = fromColourRef(adjustColourValue(toColourRef(colour.first), method));
			if (colour.second.isValid())
				colour.second = fromColourRef(adjustColourValue(toColourRef(colour.second), method));
		}
		writeCustomColours(colours);
	};
	auto applyAnsiAdjust = [readAnsiColours, writeAnsiColours](const short method)
	{
		QVector<QColor> normal(8);
		QVector<QColor> bold(8);
		readAnsiColours(normal, bold);
		for (int i = 0; i < normal.size(); ++i)
		{
			if (normal[i].isValid())
				normal[i] = fromColourRef(adjustColourValue(toColourRef(normal[i]), method));
			if (bold[i].isValid())
				bold[i] = fromColourRef(adjustColourValue(toColourRef(bold[i]), method));
		}
		writeAnsiColours(normal, bold);
	};
	auto applyAnsiAdjustNormal = [readAnsiColours, writeAnsiColours](const short method)
	{
		QVector<QColor> normal(8);
		QVector<QColor> bold(8);
		readAnsiColours(normal, bold);
		for (auto &colour : normal)
		{
			if (colour.isValid())
				colour = fromColourRef(adjustColourValue(toColourRef(colour), method));
		}
		writeAnsiColours(normal, bold);
	};
	auto applyAnsiAdjustBold = [readAnsiColours, writeAnsiColours](const short method)
	{
		QVector<QColor> normal(8);
		QVector<QColor> bold(8);
		readAnsiColours(normal, bold);
		for (auto &colour : bold)
		{
			if (colour.isValid())
				colour = fromColourRef(adjustColourValue(toColourRef(colour), method));
		}
		writeAnsiColours(normal, bold);
	};
	connect(normalLighter, &QPushButton::clicked, this,
	        [applyAnsiAdjustNormal] { applyAnsiAdjustNormal(ADJUST_COLOUR_LIGHTER); });
	connect(normalDarker, &QPushButton::clicked, this,
	        [applyAnsiAdjustNormal] { applyAnsiAdjustNormal(ADJUST_COLOUR_DARKER); });
	connect(boldLighter, &QPushButton::clicked, this,
	        [applyAnsiAdjustBold] { applyAnsiAdjustBold(ADJUST_COLOUR_LIGHTER); });
	connect(boldDarker, &QPushButton::clicked, this,
	        [applyAnsiAdjustBold] { applyAnsiAdjustBold(ADJUST_COLOUR_DARKER); });
	auto applyCustomRandom = [readCustomColours, writeCustomColours]
	{
		QVector<QPair<QColor, QColor>> colours = readCustomColours();
		for (auto &colour : colours)
		{
			const quint32 text = QRandomGenerator::global()->generate() & 0xFFFFFF;
			const quint32 back = QRandomGenerator::global()->generate() & 0xFFFFFF;
			colour.first       = QColor::fromRgb(text);
			colour.second      = QColor::fromRgb(back);
		}
		writeCustomColours(colours);
	};
	auto applyAnsiRandom = [readAnsiColours, writeAnsiColours]
	{
		QVector<QColor> normal(8);
		QVector<QColor> bold(8);
		readAnsiColours(normal, bold);
		for (int i = 0; i < normal.size(); ++i)
		{
			normal[i] = QColor::fromRgb(QRandomGenerator::global()->generate() & 0xFFFFFF);
			bold[i]   = QColor::fromRgb(QRandomGenerator::global()->generate() & 0xFFFFFF);
		}
		writeAnsiColours(normal, bold);
	};
	if (m_customDefaults)
		connect(m_customDefaults, &QPushButton::clicked, this,
		        [this, writeCustomColours]
		        {
			        if (QMessageBox::question(this, QStringLiteral("Custom colours"),
			                                  QStringLiteral("Reset all custom colours to QMud defaults?"),
			                                  QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
				        return;
			        unsigned long text[MAX_CUSTOM];
			        unsigned long back[MAX_CUSTOM];
			        setDefaultCustomColours(text, back);
			        QVector<QPair<QColor, QColor>> colours(MAX_CUSTOM);
			        for (int i = 0; i < MAX_CUSTOM; ++i)
				        colours[i] = qMakePair(fromColourRef(static_cast<long>(text[i])),
				                               fromColourRef(static_cast<long>(back[i])));
			        writeCustomColours(colours);
		        });
	if (m_customInvert)
		connect(m_customInvert, &QPushButton::clicked, this,
		        [applyCustomAdjust] { applyCustomAdjust(ADJUST_COLOUR_INVERT); });
	if (m_customLighter)
		connect(m_customLighter, &QPushButton::clicked, this,
		        [applyCustomAdjust] { applyCustomAdjust(ADJUST_COLOUR_LIGHTER); });
	if (m_customDarker)
		connect(m_customDarker, &QPushButton::clicked, this,
		        [applyCustomAdjust] { applyCustomAdjust(ADJUST_COLOUR_DARKER); });
	if (m_customMoreColour)
		connect(m_customMoreColour, &QPushButton::clicked, this,
		        [applyCustomAdjust] { applyCustomAdjust(ADJUST_COLOUR_MORE_COLOUR); });
	if (m_customLessColour)
		connect(m_customLessColour, &QPushButton::clicked, this,
		        [applyCustomAdjust] { applyCustomAdjust(ADJUST_COLOUR_LESS_COLOUR); });
	if (m_customSwap)
		connect(m_customSwap, &QPushButton::clicked, this,
		        [readCustomColours, writeCustomColours]
		        {
			        QVector<QPair<QColor, QColor>> colours = readCustomColours();
			        for (auto &colour : colours)
				        qSwap(colour.first, colour.second);
			        writeCustomColours(colours);
		        });
	if (m_customRandom)
		connect(m_customRandom, &QPushButton::clicked, this,
		        [this, applyCustomRandom]
		        {
			        if (QMessageBox::question(this, QStringLiteral("Custom colours"),
			                                  QStringLiteral("Make all colours random?"),
			                                  QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
				        return;
			        applyCustomRandom();
		        });
	auto *ansiButtons  = new QGridLayout();
	m_ansiDefaults     = new QPushButton(QStringLiteral("&ANSI colours..."), ansiColoursPage);
	m_ansiSwap         = new QPushButton(QStringLiteral("<- Swap ->"), ansiColoursPage);
	m_ansiInvert       = new QPushButton(QStringLiteral("Invert"), ansiColoursPage);
	m_ansiRandom       = new QPushButton(QStringLiteral("&Random..."), ansiColoursPage);
	m_ansiLighter      = new QPushButton(QStringLiteral("L&ighter"), ansiColoursPage);
	m_ansiDarker       = new QPushButton(QStringLiteral("&Darker"), ansiColoursPage);
	m_ansiMoreColour   = new QPushButton(QStringLiteral("&More colour"), ansiColoursPage);
	m_ansiLessColour   = new QPushButton(QStringLiteral("L&ess colour"), ansiColoursPage);
	m_ansiLoad         = new QPushButton(QStringLiteral("&Load..."), ansiColoursPage);
	m_ansiSave         = new QPushButton(QStringLiteral("&Save..."), ansiColoursPage);
	m_copyAnsiToCustom = new QPushButton(QStringLiteral("&Copy to custom..."), ansiColoursPage);
	if (ansiSwatchGrid)
		ansiSwatchGrid->addWidget(m_ansiSwap, 10, 1, 1, 2, Qt::AlignHCenter);
	auto shrinkAnsiButton = [](QPushButton *button)
	{
		if (!button)
			return;
		button->setFixedHeight(28);
		button->setMinimumWidth(150);
		button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	};
	shrinkAnsiButton(m_ansiDefaults);
	shrinkAnsiButton(m_ansiLoad);
	shrinkAnsiButton(m_ansiSave);
	shrinkAnsiButton(m_ansiLighter);
	shrinkAnsiButton(m_ansiDarker);
	shrinkAnsiButton(m_ansiMoreColour);
	shrinkAnsiButton(m_ansiLessColour);
	shrinkAnsiButton(m_copyAnsiToCustom);
	shrinkAnsiButton(m_ansiInvert);
	shrinkAnsiButton(m_ansiRandom);
	ansiButtons->addWidget(m_ansiDefaults, 0, 0);
	ansiButtons->addWidget(m_ansiLoad, 1, 0);
	ansiButtons->addWidget(m_ansiSave, 2, 0);
	ansiButtons->addWidget(m_ansiLighter, 3, 0);
	ansiButtons->addWidget(m_ansiDarker, 4, 0);
	ansiButtons->addWidget(m_ansiMoreColour, 5, 0);
	ansiButtons->addWidget(m_ansiLessColour, 6, 0);
	ansiButtons->addWidget(m_copyAnsiToCustom, 7, 0);
	ansiButtons->addWidget(m_ansiInvert, 8, 0);
	ansiButtons->addWidget(m_ansiRandom, 9, 0);
	auto *ansiButtonsColumn = new QVBoxLayout();
	ansiButtonsColumn->addLayout(ansiButtons);
	ansiButtonsColumn->addStretch();
	ansiTop->addLayout(ansiButtonsColumn);
	auto *ansiOptions   = new QVBoxLayout();
	m_useDefaultColours = new QCheckBox(QStringLiteral("Override with default colours"), ansiColoursPage);
	m_custom16IsDefaultColour =
	    new QCheckBox(QStringLiteral("Use Custom Colour 16 as default"), ansiColoursPage);
	if (m_useDefaultColours)
		connect(m_useDefaultColours, &QCheckBox::toggled, this, [this] { updateDefaultColoursState(); });
	ansiOptions->setContentsMargins(0, 0, 0, 0);
	ansiOptions->setAlignment(Qt::AlignLeft);
	ansiOptions->addWidget(m_custom16IsDefaultColour, 0, Qt::AlignLeft);
	ansiOptions->addWidget(m_useDefaultColours, 0, Qt::AlignLeft);
	auto *ansiOptionsWidget = new QWidget(ansiColoursPage);
	ansiOptionsWidget->setLayout(ansiOptions);
	if (ansiSwatchGrid)
	{
		ansiSwatchGrid->addItem(new QSpacerItem(0, 8, QSizePolicy::Minimum, QSizePolicy::Fixed), 11, 1, 1, 2);
		ansiSwatchGrid->addWidget(ansiOptionsWidget, 12, 1, 1, 2, Qt::AlignHCenter);
	}
	auto *ansiContentLayout = new QVBoxLayout();
	ansiContentLayout->addLayout(ansiTop);
	ansiColoursLayout->addStretch(1);
	ansiColoursLayout->addLayout(ansiContentLayout);
	ansiColoursLayout->addStretch(1);
	connect(m_ansiDefaults, &QPushButton::clicked, this,
	        [this, writeAnsiColours]
	        {
		        if (QMessageBox::question(this, QStringLiteral("ANSI colours"),
		                                  QStringLiteral("Reset all colours to the ANSI defaults?"),
		                                  QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
			        return;
		        unsigned long normal[8];
		        unsigned long bold[8];
		        setDefaultAnsiColours(normal, bold);
		        QVector<QColor> normalColours(8);
		        QVector<QColor> boldColours(8);
		        for (int i = 0; i < 8; ++i)
		        {
			        normalColours[i] = fromColourRef(static_cast<long>(normal[i]));
			        boldColours[i]   = fromColourRef(static_cast<long>(bold[i]));
		        }
		        writeAnsiColours(normalColours, boldColours);
	        });
	connect(m_ansiSwap, &QPushButton::clicked, this,
	        [readAnsiColours, writeAnsiColours]
	        {
		        QVector<QColor> normal(8);
		        QVector<QColor> bold(8);
		        readAnsiColours(normal, bold);
		        for (int i = 0; i < normal.size(); ++i)
			        qSwap(normal[i], bold[i]);
		        writeAnsiColours(normal, bold);
	        });
	connect(m_ansiInvert, &QPushButton::clicked, this,
	        [applyAnsiAdjust] { applyAnsiAdjust(ADJUST_COLOUR_INVERT); });
	connect(m_ansiLighter, &QPushButton::clicked, this,
	        [applyAnsiAdjust] { applyAnsiAdjust(ADJUST_COLOUR_LIGHTER); });
	connect(m_ansiDarker, &QPushButton::clicked, this,
	        [applyAnsiAdjust] { applyAnsiAdjust(ADJUST_COLOUR_DARKER); });
	connect(m_ansiMoreColour, &QPushButton::clicked, this,
	        [applyAnsiAdjust] { applyAnsiAdjust(ADJUST_COLOUR_MORE_COLOUR); });
	connect(m_ansiLessColour, &QPushButton::clicked, this,
	        [applyAnsiAdjust] { applyAnsiAdjust(ADJUST_COLOUR_LESS_COLOUR); });
	connect(m_ansiRandom, &QPushButton::clicked, this,
	        [this, applyAnsiRandom]
	        {
		        if (QMessageBox::question(this, QStringLiteral("ANSI colours"),
		                                  QStringLiteral("Make all colours random?"),
		                                  QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
			        return;
		        applyAnsiRandom();
	        });
	connect(m_copyAnsiToCustom, &QPushButton::clicked, this,
	        [this, readAnsiColours, readCustomColours, writeCustomColours]
	        {
		        if (QMessageBox::question(this, QStringLiteral("ANSI colours"),
		                                  QStringLiteral("Copy all 16 colours to the custom colours?"),
		                                  QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
			        return;
		        QVector<QColor> normal(8);
		        QVector<QColor> bold(8);
		        readAnsiColours(normal, bold);
		        QVector<QPair<QColor, QColor>> custom = readCustomColours();
		        for (int i = 0; i < 8 && i < custom.size(); ++i)
			        custom[i].first = normal[i];
		        for (int i = 0; i < 8 && (i + 8) < custom.size(); ++i)
			        custom[i + 8].first = bold[i];
		        writeCustomColours(custom);
	        });
	connect(m_ansiLoad, &QPushButton::clicked, this,
	        [this]
	        {
		        const QString startDir = m_runtime ? m_runtime->fileBrowsingDirectory() : QString();
		        const QString fileName = QFileDialog::getOpenFileName(
		            this, QStringLiteral("Colour file name"), startDir,
		            QStringLiteral("QMud colours (*.qdc *.mcc);;All files (*.*)"));
		        if (fileName.isEmpty())
			        return;
		        if (m_runtime)
			        m_runtime->setFileBrowsingDirectory(QFileInfo(fileName).absolutePath());
		        WorldDocument doc;
		        doc.setLoadMask(WorldDocument::XML_COLOURS | WorldDocument::XML_NO_PLUGINS |
		                        WorldDocument::XML_IMPORT_MAIN_FILE_ONLY);
		        if (!doc.loadFromFile(fileName))
		        {
			        QMessageBox::warning(this, QStringLiteral("Load colours"), doc.errorString());
			        return;
		        }
		        if (m_runtime)
		        {
			        QList<WorldRuntime::Colour> colours;
			        for (const auto &c : doc.colours())
			        {
				        WorldRuntime::Colour rc;
				        rc.group      = c.group;
				        rc.attributes = c.attributes;
				        bool      ok  = false;
				        const int seq = rc.attributes.value(QStringLiteral("seq")).toInt(&ok);
				        if (ok)
					        rc.attributes.insert(QStringLiteral("seq_index"), QString::number(seq - 1));
				        colours.push_back(rc);
			        }
			        m_runtime->setColours(colours);
		        }
		        populateCustomColours();
		        populateAnsiColours();
	        });
	connect(m_ansiSave, &QPushButton::clicked, this,
	        [this, readCustomColours, readCustomNames, readAnsiColours]
	        {
		        const QString startDir = m_runtime ? m_runtime->fileBrowsingDirectory() : QString();
		        const QString fileName = QFileDialog::getSaveFileName(
		            this, QStringLiteral("Colour file name"), startDir,
		            QStringLiteral("QMud colours (*.qdc *.mcc);;All files (*.*)"));
		        if (fileName.isEmpty())
			        return;
		        const QString outputPath = canonicalSavePath(fileName, QStringLiteral("qdc"));
		        if (m_runtime)
			        m_runtime->setFileBrowsingDirectory(QFileInfo(outputPath).absolutePath());
		        QVector<QColor> normal(8);
		        QVector<QColor> bold(8);
		        readAnsiColours(normal, bold);
		        const QVector<QPair<QColor, QColor>> custom = readCustomColours();
		        const QVector<QString>               names  = readCustomNames();
		        QSaveFile                            file(outputPath);
		        if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
		        {
			        QMessageBox::warning(this, QStringLiteral("Save colours"),
			                             QStringLiteral("Unable to write to %1").arg(outputPath));
			        return;
		        }
		        QTextStream stream(&file);
		        stream << "<colours>\n";
		        stream << "\n <ansi>\n";
		        stream << "\n  <normal>\n";
		        for (int i = 0; i < normal.size(); ++i)
			        stream << "   <colour seq=\"" << (i + 1) << "\" rgb=\"" << formatColourValue(normal.at(i))
			               << "\"/>\n";
		        stream << "\n  </normal>\n";
		        stream << "\n  <bold>\n";
		        for (int i = 0; i < bold.size(); ++i)
			        stream << "   <colour seq=\"" << (i + 1) << "\" rgb=\"" << formatColourValue(bold.at(i))
			               << "\"/>\n";
		        stream << "\n  </bold>\n";
		        stream << "\n </ansi>\n";
		        stream << "\n <custom>\n";
		        for (int i = 0; i < custom.size(); ++i)
		        {
			        stream << "  <colour seq=\"" << (i + 1) << "\"";
			        if (!names.value(i).isEmpty())
				        stream << " name=\"" << fixHtmlString(names.value(i)) << "\"";
			        stream << " text=\"" << formatColourValue(custom.at(i).first) << "\""
			               << " back=\"" << formatColourValue(custom.at(i).second) << "\"/>\n";
		        }
		        stream << "\n </custom>\n";
		        stream << "</colours>\n";
		        if (!file.commit())
			        QMessageBox::warning(this, QStringLiteral("Save colours"),
			                             QStringLiteral("Unable to write to %1").arg(outputPath));
	        });

	// Macros
	auto *macrosLayout = new QVBoxLayout(macrosPage);
	m_macrosTable      = new QTableWidget(macrosPage);
	m_macrosTable->setColumnCount(3);
	m_macrosTable->setHorizontalHeaderLabels(
	    {QStringLiteral("Macro Name"), QStringLiteral("Text"), QStringLiteral("Action")});
	m_macrosTable->horizontalHeader()->setStretchLastSection(true);
	m_macrosTable->horizontalHeader()->setSortIndicatorShown(true);
	m_macrosTable->verticalHeader()->setVisible(false);
	m_macrosTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
	m_macrosTable->setSelectionBehavior(QAbstractItemView::SelectRows);
	m_macrosTable->setSelectionMode(QAbstractItemView::SingleSelection);
	macrosLayout->addWidget(m_macrosTable);
	auto *macrosHint =
	    new QLabel(QStringLiteral("Customise output from function keys and \"Game\" menu."), macrosPage);
	macrosLayout->addWidget(macrosHint);
	auto *macroButtons    = new QGridLayout();
	m_useDefaultMacros    = new QCheckBox(QStringLiteral("Override with default macros"), macrosPage);
	m_editMacroButton     = new QPushButton(QStringLiteral("Edit..."), macrosPage);
	m_findMacroButton     = new QPushButton(QStringLiteral("Find..."), macrosPage);
	m_findNextMacroButton = new QPushButton(QStringLiteral("Find Next"), macrosPage);
	m_loadMacroButton     = new QPushButton(QStringLiteral("Load..."), macrosPage);
	m_saveMacroButton     = new QPushButton(QStringLiteral("Save..."), macrosPage);
	macroButtons->addWidget(m_useDefaultMacros, 0, 0);
	macroButtons->addWidget(m_editMacroButton, 1, 0);
	macroButtons->addItem(new QSpacerItem(24, 0, QSizePolicy::Fixed, QSizePolicy::Minimum), 0, 1, 2, 1);
	macroButtons->addWidget(m_findMacroButton, 0, 2);
	macroButtons->addWidget(m_loadMacroButton, 0, 3);
	macroButtons->addWidget(m_findNextMacroButton, 1, 2);
	macroButtons->addWidget(m_saveMacroButton, 1, 3);
	macroButtons->setColumnStretch(4, 1);
	macrosLayout->addLayout(macroButtons);
	connect(m_useDefaultMacros, &QCheckBox::toggled, this, [this] { updateMacroControls(); });
	connect(m_macrosTable, &QTableWidget::cellDoubleClicked, this,
	        [this](const int row, int) { editMacroAtRow(row); });
	connect(m_macrosTable->selectionModel(), &QItemSelectionModel::selectionChanged, this,
	        [this](const QItemSelection &, const QItemSelection &) { updateMacroControls(); });
	connect(m_macrosTable->horizontalHeader(), &QHeaderView::sectionClicked, this,
	        [this](const int section)
	        {
		        if (section == m_macroSortColumn)
			        m_macroSortAscending = !m_macroSortAscending;
		        else
		        {
			        m_macroSortColumn    = section;
			        m_macroSortAscending = true;
		        }
		        populateMacros();
	        });
	connect(m_editMacroButton, &QPushButton::clicked, this,
	        [this]
	        {
		        if (!m_macrosTable)
			        return;
		        if (const int row = m_macrosTable->currentRow(); row >= 0)
			        editMacroAtRow(row);
	        });
	connect(m_findMacroButton, &QPushButton::clicked, this, [this] { findMacro(m_macroFindText, false); });
	connect(m_findNextMacroButton, &QPushButton::clicked, this,
	        [this]
	        {
		        if (m_macroFindText.isEmpty())
			        return;
		        findMacro(m_macroFindText, true);
	        });
	connect(m_loadMacroButton, &QPushButton::clicked, this,
	        [this]
	        {
		        const QString startDir = m_runtime ? m_runtime->fileBrowsingDirectory() : QString();
		        if (const QString fileName =
		                QFileDialog::getOpenFileName(this, QStringLiteral("Macro file name"), startDir,
		                                             QStringLiteral("QMud macros (*.qdm *.mcm)"));
		            !fileName.isEmpty())
		        {
			        if (m_runtime)
				        m_runtime->setFileBrowsingDirectory(QFileInfo(fileName).absolutePath());
			        if (!loadMacrosFromFile(fileName))
				        return;
			        populateMacros();
		        }
	        });
	connect(m_saveMacroButton, &QPushButton::clicked, this,
	        [this]
	        {
		        const QString startDir = m_runtime ? m_runtime->fileBrowsingDirectory() : QString();
		        if (const QString fileName =
		                QFileDialog::getSaveFileName(this, QStringLiteral("Macro file name"), startDir,
		                                             QStringLiteral("QMud macros (*.qdm *.mcm)"));
		            !fileName.isEmpty())
		        {
			        const QString outputPath = canonicalSavePath(fileName, QStringLiteral("qdm"));
			        if (m_runtime)
				        m_runtime->setFileBrowsingDirectory(QFileInfo(outputPath).absolutePath());
			        if (!saveMacrosToFile(outputPath))
				        return;
		        }
	        });

	auto editVariableDialog = [this](QString &name, QString &value, const bool allowRename) -> bool
	{
		QDialog dialog(this);
		dialog.setWindowTitle(QStringLiteral("Variable"));
		auto *dialogLayout = new QVBoxLayout(&dialog);
		auto *form         = new QFormLayout();
		auto *nameEdit     = new QLineEdit(&dialog);
		nameEdit->setText(name);
		nameEdit->setReadOnly(!allowRename);
		auto *valueEdit = new QTextEdit(&dialog);
		valueEdit->setPlainText(value);
		form->addRow(QStringLiteral("Name"), nameEdit);
		form->addRow(QStringLiteral("Contents"), valueEdit);
		dialogLayout->addLayout(form);
		auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
		connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
		connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
		dialogLayout->addWidget(buttons);
		if (dialog.exec() != QDialog::Accepted)
			return false;
		name  = nameEdit->text().trimmed();
		value = valueEdit->toPlainText();
		return !name.isEmpty();
	};
	if (m_addVariableButton)
		connect(m_addVariableButton, &QPushButton::clicked, this,
		        [this, editVariableDialog]
		        {
			        if (!m_runtime)
				        return;
			        QString name;
			        QString value;
			        if (!editVariableDialog(name, value, true))
				        return;
			        const QList<WorldRuntime::Variable> &vars = m_runtime->variables();
			        for (const auto &var : vars)
			        {
				        if (var.attributes.value(QStringLiteral("name")) == name)
				        {
					        QMessageBox::warning(this, QStringLiteral("Variables"),
					                             QStringLiteral("That variable name is already in use."));
					        return;
				        }
			        }
			        WorldRuntime::Variable newVar;
			        newVar.attributes.insert(QStringLiteral("name"), name);
			        newVar.content                        = value;
			        QList<WorldRuntime::Variable> updated = vars;
			        updated.push_back(newVar);
			        m_runtime->setVariables(updated);
			        populateVariables();
		        });
	if (m_editVariableButton)
		connect(m_editVariableButton, &QPushButton::clicked, this,
		        [this, editVariableDialog]
		        {
			        if (!m_runtime || !m_variablesTable)
				        return;
			        const int row   = m_variablesTable->currentRow();
			        const int index = rowToIndex(m_variablesTable, row);
			        if (index < 0)
				        return;
			        QList<WorldRuntime::Variable> vars = m_runtime->variables();
			        if (index >= vars.size())
				        return;
			        QString name  = vars.at(index).attributes.value(QStringLiteral("name"));
			        QString value = vars.at(index).content;
			        if (!editVariableDialog(name, value, true))
				        return;
			        vars[index].attributes.insert(QStringLiteral("name"), name);
			        vars[index].content = value;
			        m_runtime->setVariables(vars);
			        populateVariables();
			        selectRowByIndex(m_variablesTable, index);
			        updateVariableControls();
		        });
	if (m_deleteVariableButton)
		connect(m_deleteVariableButton, &QPushButton::clicked, this,
		        [this]
		        {
			        if (!m_runtime || !m_variablesTable)
				        return;
			        const int row   = m_variablesTable->currentRow();
			        const int index = rowToIndex(m_variablesTable, row);
			        if (index < 0)
				        return;
			        if (!confirmRemoval(this, QStringLiteral("variable")))
				        return;
			        QList<WorldRuntime::Variable> vars = m_runtime->variables();
			        if (index >= vars.size())
				        return;
			        vars.removeAt(index);
			        m_runtime->setVariables(vars);
			        populateVariables();
			        const int lastIndex = saturatingToInt(vars.size()) - 1;
			        selectRowByIndex(m_variablesTable, std::min(index, lastIndex));
			        updateVariableControls();
		        });
	if (m_variablesTable)
		connect(m_variablesTable, &QTableWidget::cellDoubleClicked, this,
		        [this]
		        {
			        if (m_editVariableButton)
				        m_editVariableButton->click();
		        });
	if (m_findVariableButton)
		connect(m_findVariableButton, &QPushButton::clicked, this,
		        [this]
		        {
			        if (!m_runtime)
				        return;
			        const QList<WorldRuntime::Variable> vars    = m_runtime->variables();
			        auto                                rowText = [this, &vars](const int row) -> QString
			        {
				        const int index = rowToIndex(m_variablesTable, row);
				        if (index < 0 || index >= vars.size())
					        return {};
				        const WorldRuntime::Variable &var  = vars.at(index);
				        const QString                 name = var.attributes.value(QStringLiteral("name"));
				        return var.content + QLatin1Char('\t') + name;
			        };
			        doFindAdvanced(this, m_variablesTable, rowText, m_variableFindText, m_variableFindRow,
			                       m_variableFindHistory, m_variableFindMatchCase, m_variableFindRegex,
			                       m_variableFindForwards, QStringLiteral("Find variable"), false);
			        updateVariableControls();
		        });
	if (m_findNextVariableButton)
		connect(m_findNextVariableButton, &QPushButton::clicked, this,
		        [this]
		        {
			        if (!m_runtime)
				        return;
			        const QList<WorldRuntime::Variable> vars    = m_runtime->variables();
			        auto                                rowText = [this, &vars](const int row) -> QString
			        {
				        const int index = rowToIndex(m_variablesTable, row);
				        if (index < 0 || index >= vars.size())
					        return {};
				        const WorldRuntime::Variable &var  = vars.at(index);
				        const QString                 name = var.attributes.value(QStringLiteral("name"));
				        return var.content + QLatin1Char('\t') + name;
			        };
			        doFindAdvanced(this, m_variablesTable, rowText, m_variableFindText, m_variableFindRow,
			                       m_variableFindHistory, m_variableFindMatchCase, m_variableFindRegex,
			                       m_variableFindForwards, QStringLiteral("Find variable"), true);
			        updateVariableControls();
		        });
	if (m_loadVariablesButton)
		connect(m_loadVariablesButton, &QPushButton::clicked, this,
		        [this]
		        {
			        const QString startDir = m_runtime ? m_runtime->fileBrowsingDirectory() : QString();
			        const QString fileName =
			            QFileDialog::getOpenFileName(this, QStringLiteral("Variable file name"), startDir,
			                                         QStringLiteral("QMud variables (*.qdv *.mcv)"));
			        if (fileName.isEmpty())
				        return;
			        if (m_runtime)
				        m_runtime->setFileBrowsingDirectory(QFileInfo(fileName).absolutePath());
			        if (!loadVariablesFromFile(fileName))
				        return;
			        populateVariables();
		        });
	if (m_saveVariablesButton)
		connect(m_saveVariablesButton, &QPushButton::clicked, this,
		        [this]
		        {
			        const QString startDir = m_runtime ? m_runtime->fileBrowsingDirectory() : QString();
			        const QString fileName =
			            QFileDialog::getSaveFileName(this, QStringLiteral("Variable file name"), startDir,
			                                         QStringLiteral("QMud variables (*.qdv *.mcv)"));
			        if (fileName.isEmpty())
				        return;
			        const QString outputPath = canonicalSavePath(fileName, QStringLiteral("qdv"));
			        if (m_runtime)
				        m_runtime->setFileBrowsingDirectory(QFileInfo(outputPath).absolutePath());
			        if (!saveVariablesToFile(outputPath))
				        return;
		        });
	if (m_copyVariableButton)
		connect(m_copyVariableButton, &QPushButton::clicked, this,
		        [this]
		        {
			        if (!m_runtime || !m_variablesTable)
				        return;
			        const int row   = m_variablesTable->currentRow();
			        const int index = rowToIndex(m_variablesTable, row);
			        if (index < 0)
				        return;
			        const QList<WorldRuntime::Variable> &vars = m_runtime->variables();
			        if (index >= vars.size())
				        return;
			        const WorldRuntime::Variable &var = vars.at(index);
			        QString                       xml;
			        QTextStream                   out(&xml);
			        out << "<variables>\n";
			        out << "  <variable name=\""
			            << fixHtmlString(var.attributes.value(QStringLiteral("name"))) << "\">"
			            << fixHtmlMultilineString(var.content) << "</variable>\n";
			        out << "</variables>\n";
			        if (QClipboard *clipboard = QGuiApplication::clipboard())
				        clipboard->setText(xml);
		        });
	if (m_pasteVariableButton)
		connect(m_pasteVariableButton, &QPushButton::clicked, this,
		        [this]
		        {
			        if (!m_runtime)
				        return;
			        const QString text = []() -> QString
			        {
				        if (QClipboard *clipboard = QGuiApplication::clipboard())
					        return clipboard->text();
				        return {};
			        }();
			        if (text.trimmed().isEmpty())
				        return;
			        QTemporaryFile temp;
			        if (!temp.open())
				        return;
			        temp.write(text.toUtf8());
			        temp.flush();
			        WorldDocument doc;
			        doc.setLoadMask(WorldDocument::XML_VARIABLES | WorldDocument::XML_NO_PLUGINS |
			                        WorldDocument::XML_IMPORT_MAIN_FILE_ONLY);
			        if (!doc.loadFromFile(temp.fileName()))
			        {
				        QMessageBox::warning(this, QStringLiteral("Paste variables"), doc.errorString());
				        return;
			        }
			        QList<WorldRuntime::Variable> combined = m_runtime->variables();
			        for (const auto &v : doc.variables())
			        {
				        WorldRuntime::Variable rv;
				        rv.attributes = v.attributes;
				        rv.content    = v.content;
				        combined.push_back(rv);
			        }
			        m_runtime->setVariables(combined);
			        populateVariables();
		        });
	if (m_editVariablesFilter)
		connect(m_editVariablesFilter, &QPushButton::clicked, this,
		        [this]
		        {
			        QDialog dialog(this);
			        dialog.setWindowTitle(QStringLiteral("Edit variable filter"));
			        auto *dialogLayout = new QVBoxLayout(&dialog);
			        auto *edit         = new QTextEdit(&dialog);
			        edit->setPlainText(m_variableFilterText);
			        dialogLayout->addWidget(edit);
			        auto *buttons =
			            new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
			        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
			        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
			        dialogLayout->addWidget(buttons);
			        if (dialog.exec() != QDialog::Accepted)
				        return;
			        m_variableFilterText = edit->toPlainText();
			        if (m_filterVariables)
				        m_filterVariables->setChecked(true);
			        populateVariables();
			        updateVariableControls();
		        });
	if (m_filterVariables)
		connect(m_filterVariables, &QCheckBox::toggled, this,
		        [this]
		        {
			        populateVariables();
			        updateVariableControls();
		        });
	// Output
	auto *outputLayout = new QGridLayout(outputPage);
	auto *outputLeft   = new QVBoxLayout();
	auto *outputRight  = new QVBoxLayout();

	auto *beepGroup =
	    new QGroupBox(QStringLiteral("Sound to play when receiving a \"bell\" character"), outputPage);
	auto *beepLayout  = new QGridLayout(beepGroup);
	m_enableBeeps     = new QCheckBox(QStringLiteral("Enable beeps"), beepGroup);
	m_beepSound       = new QLineEdit(beepGroup);
	m_browseBeepSound = new QPushButton(QStringLiteral("&Browse..."), beepGroup);
	m_testBeepSound   = new QPushButton(QStringLiteral("T&est"), beepGroup);
	beepLayout->addWidget(m_enableBeeps, 0, 0, 1, 3);
	beepLayout->addWidget(new QLabel(QStringLiteral("Beep"), beepGroup), 1, 0);
	beepLayout->addWidget(m_beepSound, 1, 1, 1, 2);
	beepLayout->addWidget(m_browseBeepSound, 2, 1);
	beepLayout->addWidget(m_testBeepSound, 2, 2);
	outputLeft->addWidget(beepGroup);

	auto *spacingGroup  = new QGroupBox(QStringLiteral("Spacing"), outputPage);
	auto *spacingLayout = new QGridLayout(spacingGroup);
	m_pixelOffset       = new QSpinBox(spacingGroup);
	m_pixelOffset->setRange(0, 20);
	m_pixelOffset->setMaximumWidth(70);
	m_lineSpacing = new QSpinBox(spacingGroup);
	m_lineSpacing->setRange(0, 100);
	m_lineSpacing->setMaximumWidth(70);
	spacingLayout->addWidget(new QLabel(QStringLiteral("Text offset from edge"), spacingGroup), 0, 0);
	spacingLayout->addWidget(m_pixelOffset, 0, 1);
	spacingLayout->addWidget(new QLabel(QStringLiteral("Line spacing (pixels)"), spacingGroup), 1, 0);
	spacingLayout->addWidget(m_lineSpacing, 1, 1);
	auto *lineSpacingHint = new QLabel(QStringLiteral("(0 = use font default)"), spacingGroup);
	spacingLayout->addWidget(lineSpacingHint, 2, 0, 1, 2);
	outputLeft->addWidget(spacingGroup);

	auto *outputFontBox    = new QGroupBox(QStringLiteral("Font"), outputPage);
	auto *outputFontLayout = new QGridLayout(outputFontBox);
	m_outputFontButton     = new QPushButton(QStringLiteral("&Font..."), outputFontBox);
	m_outputFontName       = new QLineEdit(outputFontBox);
	m_outputFontName->setReadOnly(true);
	m_outputFontName->setFrame(false);
	m_outputFontHeight = new QSpinBox(outputFontBox);
	m_outputFontHeight->setRange(1, 1000);
	m_outputFontHeight->setReadOnly(true);
	m_outputFontHeight->setButtonSymbols(QAbstractSpinBox::NoButtons);
	m_outputFontHeight->setFrame(false);
	m_outputFontHeight->setSuffix(QStringLiteral(" pt."));
	m_outputFontHeight->setMaximumWidth(70);
	m_useDefaultOutputFont = new QCheckBox(QStringLiteral("Override with default"), outputFontBox);
	m_showBold             = new QCheckBox(QStringLiteral("Bold"), outputFontBox);
	m_showItalic           = new QCheckBox(QStringLiteral("Italic"), outputFontBox);
	m_showUnderline        = new QCheckBox(QStringLiteral("Underline"), outputFontBox);
	outputFontLayout->addWidget(m_outputFontButton, 0, 0);
	outputFontLayout->addWidget(m_outputFontName, 0, 1);
	outputFontLayout->addWidget(m_outputFontHeight, 0, 2);
	outputFontLayout->addWidget(m_useDefaultOutputFont, 1, 0, 1, 3);
	auto *fontStyleLayout = new QHBoxLayout();
	fontStyleLayout->addWidget(m_showBold);
	fontStyleLayout->addWidget(m_showItalic);
	fontStyleLayout->addWidget(m_showUnderline);
	fontStyleLayout->addStretch();
	outputFontLayout->addWidget(new QLabel(QStringLiteral("Show:"), outputFontBox), 2, 0);
	outputFontLayout->addLayout(fontStyleLayout, 2, 1, 1, 2);
	outputFontLayout->setColumnStretch(1, 1);
	outputLeft->addWidget(outputFontBox);
	connect(m_outputFontButton, &QPushButton::clicked, this,
	        [this]
	        {
		        QFont current;
		        if (m_outputFontName && !m_outputFontName->text().isEmpty())
			        current = QFont(m_outputFontName->text());
		        if (m_outputFontHeight && m_outputFontHeight->value() > 0)
			        current.setPointSize(m_outputFontHeight->value());
		        if (m_outputFontWeight > 0)
			        current.setWeight(WorldView::mapFontWeight(m_outputFontWeight));
		        bool        ok   = false;
		        const QFont font = QFontDialog::getFont(&ok, current, this, QStringLiteral("Output font"));
		        if (!ok)
			        return;
		        if (m_outputFontName)
			        m_outputFontName->setText(font.family());
		        if (m_outputFontHeight)
		        {
			        const int size = font.pointSize() > 0 ? font.pointSize() : m_outputFontHeight->value();
			        m_outputFontHeight->setValue(size);
		        }
		        if (font.weight() >= QFont::Bold)
			        m_outputFontWeight = 700;
		        else if (font.weight() >= QFont::DemiBold)
			        m_outputFontWeight = 600;
		        else if (font.weight() >= QFont::Medium)
			        m_outputFontWeight = 500;
		        else
			        m_outputFontWeight = 400;
	        });
	if (m_useDefaultOutputFont)
		connect(m_useDefaultOutputFont, &QCheckBox::toggled, this, [this] { updateOutputFontControls(); });

	auto *activityGroup      = new QGroupBox(QStringLiteral("Sound to play on new activity"), outputPage);
	auto *activityLayout     = new QGridLayout(activityGroup);
	m_newActivitySound       = new QLineEdit(activityGroup);
	m_browseActivitySound    = new QPushButton(QStringLiteral("&Browse..."), activityGroup);
	m_testActivitySound      = new QPushButton(QStringLiteral("T&est"), activityGroup);
	m_noActivitySound        = new QPushButton(QStringLiteral("No soun&d"), activityGroup);
	m_playSoundsInBackground = new QCheckBox(QStringLiteral("Play sounds in background"), activityGroup);
	activityLayout->addWidget(new QLabel(QStringLiteral("New activity"), activityGroup), 0, 0);
	activityLayout->addWidget(m_newActivitySound, 0, 1, 1, 3);
	activityLayout->addWidget(m_browseActivitySound, 1, 1);
	activityLayout->addWidget(m_testActivitySound, 1, 2);
	activityLayout->addWidget(m_noActivitySound, 1, 3);
	activityLayout->addWidget(m_playSoundsInBackground, 2, 0, 1, 4);
	outputLeft->addWidget(activityGroup);
	if (m_browseBeepSound && m_beepSound)
		connect(m_browseBeepSound, &QPushButton::clicked, this,
		        [this] { browseSoundFile(m_runtime, this, m_beepSound); });
	if (m_testBeepSound)
		connect(m_testBeepSound, &QPushButton::clicked, this,
		        [this]
		        {
			        if (!m_runtime || !m_beepSound)
				        return;
			        const QString fileName = m_beepSound->text().trimmed();
			        if (fileName.isEmpty())
				        return;
			        m_runtime->playSound(0, fileName, false, 0.0, 0.0);
		        });
	if (m_browseActivitySound && m_newActivitySound)
		connect(m_browseActivitySound, &QPushButton::clicked, this,
		        [this] { browseSoundFile(m_runtime, this, m_newActivitySound); });
	if (m_testActivitySound)
		connect(m_testActivitySound, &QPushButton::clicked, this,
		        [this]
		        {
			        if (!m_runtime || !m_newActivitySound)
				        return;
			        const QString fileName = m_newActivitySound->text().trimmed();
			        if (fileName.isEmpty() || fileName == QStringLiteral("(No sound)"))
				        return;
			        m_runtime->playSound(0, fileName, false, 0.0, 0.0);
		        });
	if (m_noActivitySound && m_newActivitySound)
		connect(m_noActivitySound, &QPushButton::clicked, this,
		        [this] { m_newActivitySound->setText(QStringLiteral("(No sound)")); });
	if (m_beepSound && m_testBeepSound)
		connect(m_beepSound, &QLineEdit::textChanged, this,
		        [this] { m_testBeepSound->setEnabled(canTestSoundFile(m_beepSound)); });
	if (m_newActivitySound && m_testActivitySound && m_noActivitySound)
		connect(m_newActivitySound, &QLineEdit::textChanged, this,
		        [this]
		        {
			        const bool enabled = canTestSoundFile(m_newActivitySound);
			        m_testActivitySound->setEnabled(enabled);
			        m_noActivitySound->setEnabled(enabled);
		        });

	auto *outputBufferBox    = new QGroupBox(QStringLiteral("Output buffer size"), outputPage);
	auto *outputBufferLayout = new QGridLayout(outputBufferBox);
	m_maxLines               = new QSpinBox(outputBufferBox);
	m_maxLines->setRange(200, 500000);
	m_maxLines->setMaximumWidth(90);
	m_wrapOutput = new QCheckBox(QStringLiteral("Wrap output at column number"), outputBufferBox);
	m_wrapColumn = new QSpinBox(outputBufferBox);
	m_wrapColumn->setRange(20, 500);
	m_wrapColumn->setMaximumWidth(90);
	auto *adjustWidthButton = new QPushButton(QStringLiteral("Adjust width to size"), outputBufferBox);
	auto *adjustSizeButton  = new QPushButton(QStringLiteral("Adjust size to width"), outputBufferBox);
	outputBufferLayout->addWidget(new QLabel(QStringLiteral("Number of lines in output"), outputBufferBox), 0,
	                              0);
	outputBufferLayout->addWidget(m_maxLines, 0, 1);
	outputBufferLayout->addWidget(new QLabel(QStringLiteral("(200 to 500,000)"), outputBufferBox), 0, 2);
	outputBufferLayout->addWidget(m_wrapOutput, 1, 0);
	outputBufferLayout->addWidget(m_wrapColumn, 1, 1);
	outputBufferLayout->addWidget(new QLabel(QStringLiteral("(20 to 500)"), outputBufferBox), 1, 2);
	outputBufferLayout->addWidget(adjustWidthButton, 2, 0);
	outputBufferLayout->addWidget(adjustSizeButton, 2, 1);
	outputLeft->addWidget(outputBufferBox);
	connect(adjustWidthButton, &QPushButton::clicked, this,
	        [this]
	        {
		        if (!m_view || !m_wrapColumn)
			        return;
		        const int outputWidth = m_view->outputClientWidth();
		        if (outputWidth <= 0)
			        return;
		        const int   pixelOffset = m_pixelOffset ? m_pixelOffset->value() : 0;
		        const QFont font        = outputFontFromDialog();
		        const int   charWidth   = averageCharWidth(font);
		        int         column      = (outputWidth - pixelOffset) / charWidth;
		        column                  = qBound(20, column, MAX_LINE_WIDTH);
		        m_wrapColumn->setValue(column);
	        });
	connect(adjustSizeButton, &QPushButton::clicked, this,
	        [this]
	        {
		        if (!m_view || !m_wrapColumn)
			        return;
		        QWidget *window = m_view->window();
		        if (!window)
			        return;
		        int column                     = m_wrapColumn->value();
		        column                         = qBound(20, column, MAX_LINE_WIDTH);
		        const int   pixelOffset        = m_pixelOffset ? m_pixelOffset->value() : 0;
		        const QFont font               = outputFontFromView();
		        const int   charWidth          = averageCharWidth(font);
		        const int   desiredOutputWidth = (charWidth * column) + pixelOffset;
		        const int   currentOutputWidth = m_view->outputClientWidth();
		        if (currentOutputWidth <= 0)
			        return;
		        const int newWidth =
		            qMax(window->minimumWidth(), window->width() + (desiredOutputWidth - currentOutputWidth));
		        QRect geom = window->geometry();
		        geom.setWidth(newWidth);
		        window->setGeometry(geom);
	        });
	outputLeft->addStretch();

	auto *outputOptionsWidget = new QWidget(outputPage);
	auto *outputOptionsLayout = new QVBoxLayout(outputOptionsWidget);
	outputOptionsLayout->setContentsMargins(0, 0, 0, 0);
	m_lineInformation         = new QCheckBox(QStringLiteral("Show Line Information"), outputOptionsWidget);
	m_startPaused             = new QCheckBox(QStringLiteral("Start Paused"), outputOptionsWidget);
	m_autoPause               = new QCheckBox(QStringLiteral("Auto Pause"), outputOptionsWidget);
	m_unpauseOnSend           = new QCheckBox(QStringLiteral("Un-Pause on send"), outputOptionsWidget);
	m_keepPauseAtBottomOption = new QCheckBox(QStringLiteral("Keep pause at bottom"), outputOptionsWidget);
	m_doNotShowOutstandingLines =
	    new QCheckBox(QStringLiteral("Do not show outstanding lines in tab"), outputOptionsWidget);
	m_flashIcon          = new QCheckBox(QStringLiteral("New activity flashes taskbar"), outputOptionsWidget);
	m_disableCompression = new QCheckBox(QStringLiteral("Disable compression"), outputOptionsWidget);
	m_indentParas        = new QCheckBox(QStringLiteral("Indent paragraphs"), outputOptionsWidget);
	m_naws               = new QCheckBox(QStringLiteral("Negotiate About Window Size"), outputOptionsWidget);
	m_carriageReturnClearsLine =
	    new QCheckBox(QStringLiteral("Carriage-return clears line"), outputOptionsWidget);
	m_utf8           = new QCheckBox(QStringLiteral("UTF-8 (Unicode)"), outputOptionsWidget);
	m_autoWrapWindow = new QCheckBox(QStringLiteral("Auto-wrap to window size"), outputOptionsWidget);
	m_alternativeInverse =
	    new QCheckBox(QStringLiteral("Alternative inverse/highlight display"), outputOptionsWidget);
	m_showConnectDisconnect =
	    new QCheckBox(QStringLiteral("Show connect/disconnect message"), outputOptionsWidget);
	m_copySelectionToClipboard =
	    new QCheckBox(QStringLiteral("Copy selection to Clipboard"), outputOptionsWidget);
	m_autoCopyHtml = new QCheckBox(QStringLiteral("HTML"), outputOptionsWidget);
	m_convertGaToNewline =
	    new QCheckBox(QStringLiteral("Convert IAC EOR/GA to new line"), outputOptionsWidget);
	m_sendKeepAlives = new QCheckBox(QStringLiteral("Send keep alives"), outputOptionsWidget);
	m_persistOutputBuffer =
	    new QCheckBox(QStringLiteral("Persist output buffer across reload/restart"), outputOptionsWidget);
	outputOptionsLayout->addWidget(m_lineInformation);
	outputOptionsLayout->addWidget(m_startPaused);
	outputOptionsLayout->addWidget(m_autoPause);
	outputOptionsLayout->addWidget(m_unpauseOnSend);
	outputOptionsLayout->addWidget(m_keepPauseAtBottomOption);
	outputOptionsLayout->addWidget(m_doNotShowOutstandingLines);
	outputOptionsLayout->addWidget(m_flashIcon);
	outputOptionsLayout->addWidget(m_disableCompression);
	outputOptionsLayout->addWidget(m_indentParas);
	outputOptionsLayout->addWidget(m_naws);
	outputOptionsLayout->addWidget(m_carriageReturnClearsLine);
	outputOptionsLayout->addWidget(m_utf8);
	outputOptionsLayout->addWidget(m_autoWrapWindow);
	outputOptionsLayout->addWidget(m_alternativeInverse);
	outputOptionsLayout->addWidget(m_showConnectDisconnect);
	outputOptionsLayout->addWidget(m_copySelectionToClipboard);
	auto *htmlLayout = new QHBoxLayout();
	htmlLayout->setContentsMargins(24, 0, 0, 0);
	htmlLayout->addWidget(m_autoCopyHtml);
	htmlLayout->addStretch();
	outputOptionsLayout->addLayout(htmlLayout);
	outputOptionsLayout->addWidget(m_convertGaToNewline);
	outputOptionsLayout->addWidget(m_sendKeepAlives);
	outputOptionsLayout->addWidget(m_persistOutputBuffer);
	outputRight->addWidget(outputOptionsWidget);
	if (m_copySelectionToClipboard && m_autoCopyHtml)
		connect(m_copySelectionToClipboard, &QCheckBox::toggled, this, [this] { updateAutoCopyHtmlState(); });

	auto *outputTelnetBox    = new QWidget(outputPage);
	auto *outputTelnetLayout = new QGridLayout(outputTelnetBox);
	auto *terminalTypeLabel  = new QLabel(QStringLiteral("Terminal Type:"), outputTelnetBox);
	terminalTypeLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	m_terminalIdentification = new QLineEdit(outputTelnetBox);
	m_terminalIdentification->setMaxLength(20);
	outputTelnetLayout->addWidget(terminalTypeLabel, 0, 0);
	outputTelnetLayout->addWidget(m_terminalIdentification, 0, 1);
	outputTelnetLayout->setColumnStretch(1, 1);
	outputRight->addStretch();
	outputRight->addWidget(outputTelnetBox);

	auto *fadeGroup                = new QGroupBox(QStringLiteral("Fade output buffer"), outputPage);
	auto *fadeLayout               = new QGridLayout(fadeGroup);
	m_fadeOutputBufferAfterSeconds = new QSpinBox(fadeGroup);
	m_fadeOutputBufferAfterSeconds->setRange(0, 3600);
	m_fadeOutputBufferAfterSeconds->setMaximumWidth(80);
	m_fadeOutputOpacityPercent = new QSpinBox(fadeGroup);
	m_fadeOutputOpacityPercent->setRange(0, 100);
	m_fadeOutputOpacityPercent->setMaximumWidth(80);
	m_fadeOutputSeconds = new QSpinBox(fadeGroup);
	m_fadeOutputSeconds->setRange(1, 60);
	m_fadeOutputSeconds->setMaximumWidth(80);
	fadeLayout->addWidget(new QLabel(QStringLiteral("Start after (seconds)"), fadeGroup), 0, 0);
	fadeLayout->addWidget(m_fadeOutputBufferAfterSeconds, 0, 1);
	fadeLayout->addWidget(new QLabel(QStringLiteral("Target opacity (%)"), fadeGroup), 1, 0);
	fadeLayout->addWidget(m_fadeOutputOpacityPercent, 1, 1);
	fadeLayout->addWidget(new QLabel(QStringLiteral("Fade duration (seconds)"), fadeGroup), 2, 0);
	fadeLayout->addWidget(m_fadeOutputSeconds, 2, 1);
	outputRight->addWidget(fadeGroup);

	// Persisted parity options not currently exposed in the Qt preferences UI.
	m_toolTipVisibleTime = new QSpinBox(outputPage);
	m_toolTipVisibleTime->setRange(0, 120000);
	m_toolTipVisibleTime->setVisible(false);
	m_toolTipStartTime = new QSpinBox(outputPage);
	m_toolTipStartTime->setRange(0, 120000);
	m_toolTipStartTime->setVisible(false);

	outputLayout->addLayout(outputLeft, 0, 0);
	outputLayout->addLayout(outputRight, 0, 1);
	outputLayout->setColumnStretch(0, 3);
	outputLayout->setColumnStretch(1, 2);

	// Commands
	auto           *commandsLayout = new QGridLayout(commandsPage);
	auto           *commandsLeft   = new QVBoxLayout();
	auto           *commandsRight  = new QVBoxLayout();
	constexpr QSize commandSwatchSize(16, 16);

	auto           *outputWindowBox    = new QGroupBox(QStringLiteral("Output Window"), commandsPage);
	auto           *outputWindowLayout = new QGridLayout(outputWindowBox);
	m_displayMyInput                   = new QCheckBox(QStringLiteral("Echo My &Input In:"), outputWindowBox);
	m_echoColour                       = new QComboBox(outputWindowBox);
	m_echoColour->addItem(QStringLiteral("(no change)"));
	for (int i = 0; i < MAX_CUSTOM; ++i)
	{
		const QString fallback = QStringLiteral("Custom%1").arg(i + 1);
		const QString name     = (i < m_customColourNames.size() && m_customColourNames[i])
		                             ? m_customColourNames[i]->text()
		                             : fallback;
		m_echoColour->addItem(name.isEmpty() ? fallback : name);
	}
	m_inputEchoSwatch = new QPushButton(outputWindowBox);
	m_inputEchoSwatch->setFixedSize(commandSwatchSize);
	m_inputEchoSwatch->setFlat(true);
	m_inputEchoSwatch->setEnabled(false);
	m_inputEchoSwatch->setFocusPolicy(Qt::NoFocus);
	m_inputEchoSwatch2 = new QPushButton(outputWindowBox);
	m_inputEchoSwatch2->setFixedSize(commandSwatchSize);
	m_inputEchoSwatch2->setFlat(true);
	m_inputEchoSwatch2->setEnabled(false);
	m_inputEchoSwatch2->setFocusPolicy(Qt::NoFocus);
	outputWindowLayout->addWidget(m_displayMyInput, 0, 0);
	outputWindowLayout->addWidget(m_echoColour, 0, 1);
	outputWindowLayout->addWidget(m_inputEchoSwatch, 0, 2);
	outputWindowLayout->addWidget(m_inputEchoSwatch2, 0, 3);
	outputWindowLayout->setColumnStretch(1, 1);
	commandsLeft->addWidget(outputWindowBox);

	auto *speedWalkBox    = new QGroupBox(QStringLiteral("Speed Walking"), commandsPage);
	auto *speedWalkLayout = new QGridLayout(speedWalkBox);
	m_enableSpeedWalk     = new QCheckBox(QStringLiteral("Enable &Speed Walking, prefix is:"), speedWalkBox);
	m_speedWalkPrefix     = new QLineEdit(speedWalkBox);
	m_speedWalkPrefix->setMaxLength(1);
	m_speedWalkPrefix->setMaximumWidth(40);
	m_speedWalkFiller = new QLineEdit(speedWalkBox);
	m_speedWalkDelay  = new QSpinBox(speedWalkBox);
	m_speedWalkDelay->setRange(0, 30000);
	m_speedWalkDelay->setMaximumWidth(60);
	auto *fillerLabel = new QLabel(QStringLiteral("Filler:"), speedWalkBox);
	fillerLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	auto *delayLabel = new QLabel(QStringLiteral("Delay:"), speedWalkBox);
	delayLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	speedWalkLayout->addWidget(m_enableSpeedWalk, 0, 0, 1, 2);
	speedWalkLayout->addWidget(m_speedWalkPrefix, 0, 2);
	speedWalkLayout->addWidget(fillerLabel, 1, 0);
	speedWalkLayout->addWidget(m_speedWalkFiller, 1, 1);
	speedWalkLayout->addWidget(delayLabel, 1, 2);
	speedWalkLayout->addWidget(m_speedWalkDelay, 1, 3);
	speedWalkLayout->addWidget(new QLabel(QStringLiteral("ms."), speedWalkBox), 1, 4);
	speedWalkLayout->setColumnStretch(1, 1);
	commandsLeft->addWidget(speedWalkBox);

	auto *commandStackBox    = new QGroupBox(QStringLiteral("Command Stacking"), commandsPage);
	auto *commandStackLayout = new QGridLayout(commandStackBox);
	m_enableCommandStack     = new QCheckBox(QStringLiteral("&Command Stacking Using:"), commandStackBox);
	m_commandStackCharacter  = new QLineEdit(commandStackBox);
	m_commandStackCharacter->setMaxLength(1);
	m_commandStackCharacter->setMaximumWidth(40);
	commandStackLayout->addWidget(m_enableCommandStack, 0, 0);
	commandStackLayout->addWidget(m_commandStackCharacter, 0, 1);
	commandStackLayout->setColumnStretch(0, 1);
	commandsLeft->addWidget(commandStackBox);

	auto *commandWindowBox     = new QGroupBox(QStringLiteral("Command Window"), commandsPage);
	auto *commandWindowLayout  = new QVBoxLayout(commandWindowBox);
	auto *commandColoursBox    = new QGroupBox(QStringLiteral("Command Colours"), commandWindowBox);
	auto *commandColoursLayout = new QHBoxLayout(commandColoursBox);
	m_inputTextColour          = new QLineEdit(commandWindowBox);
	m_inputTextColour->setVisible(false);
	m_inputBackColour = new QLineEdit(commandWindowBox);
	m_inputBackColour->setVisible(false);
	m_commandTextSwatch = new QPushButton(commandColoursBox);
	m_commandTextSwatch->setFixedSize(commandSwatchSize);
	m_commandTextSwatch->setFlat(true);
	m_commandTextSwatch->setFocusPolicy(Qt::NoFocus);
	auto *commandOnLabel = new QLabel(QStringLiteral("on"), commandColoursBox);
	m_commandBackSwatch  = new QPushButton(commandColoursBox);
	m_commandBackSwatch->setFixedSize(commandSwatchSize);
	m_commandBackSwatch->setFlat(true);
	m_commandBackSwatch->setFocusPolicy(Qt::NoFocus);
	commandColoursLayout->addWidget(m_commandTextSwatch);
	commandColoursLayout->addWidget(commandOnLabel);
	commandColoursLayout->addWidget(m_commandBackSwatch);
	commandColoursLayout->addStretch();
	commandWindowLayout->addWidget(commandColoursBox);

	auto *commandFontBox    = new QGroupBox(QStringLiteral("Font"), commandWindowBox);
	auto *commandFontLayout = new QGridLayout(commandFontBox);
	m_inputFontButton       = new QPushButton(QStringLiteral("Change &Font..."), commandFontBox);
	m_useDefaultInputFont   = new QCheckBox(QStringLiteral("O&verride With Default"), commandFontBox);
	m_inputFontName         = new QLineEdit(commandFontBox);
	m_inputFontName->setReadOnly(true);
	m_inputFontName->setFrame(false);
	m_inputFontStyle  = new QLabel(commandFontBox);
	m_inputFontHeight = new QSpinBox(commandFontBox);
	m_inputFontHeight->setRange(1, 1000);
	m_inputFontHeight->setVisible(false);
	commandFontLayout->addWidget(m_inputFontButton, 0, 0);
	commandFontLayout->addWidget(m_useDefaultInputFont, 0, 1, 1, 2);
	commandFontLayout->addWidget(m_inputFontName, 1, 0, 1, 3);
	commandFontLayout->addWidget(m_inputFontStyle, 2, 0, 1, 3);
	commandFontLayout->setColumnStretch(1, 1);
	commandWindowLayout->addWidget(commandFontBox);

	auto *commandResizeBox    = new QGroupBox(QStringLiteral("Auto-resize"), commandWindowBox);
	auto *commandResizeLayout = new QGridLayout(commandResizeBox);
	m_autoResizeCommandWindow = new QCheckBox(QStringLiteral("Auto-resize command window"), commandResizeBox);
	m_autoResizeMinimumLines  = new QSpinBox(commandResizeBox);
	m_autoResizeMinimumLines->setRange(1, 100);
	m_autoResizeMinimumLines->setMaximumWidth(60);
	m_autoResizeMaximumLines = new QSpinBox(commandResizeBox);
	m_autoResizeMaximumLines->setRange(1, 100);
	m_autoResizeMaximumLines->setMaximumWidth(60);
	commandResizeLayout->addWidget(m_autoResizeCommandWindow, 0, 0, 1, 4);
	commandResizeLayout->addWidget(new QLabel(QStringLiteral("Min lines:"), commandResizeBox), 1, 0);
	commandResizeLayout->addWidget(m_autoResizeMinimumLines, 1, 1);
	commandResizeLayout->addWidget(new QLabel(QStringLiteral("Max lines:"), commandResizeBox), 1, 2);
	commandResizeLayout->addWidget(m_autoResizeMaximumLines, 1, 3);
	commandResizeLayout->setColumnStretch(3, 1);
	commandWindowLayout->addWidget(commandResizeBox);
	commandsLeft->addWidget(commandWindowBox);

	auto *spamBox          = new QGroupBox(QStringLiteral("Spam prevention"), commandsPage);
	auto *spamLayout       = new QGridLayout(spamBox);
	m_enableSpamPrevention = new QCheckBox(QStringLiteral("Enable Spam Prevention"), spamBox);
	m_spamLineCount        = new QSpinBox(spamBox);
	m_spamLineCount->setRange(5, 500);
	m_spamLineCount->setMaximumWidth(60);
	m_spamMessage        = new QLineEdit(spamBox);
	auto *spamEveryLabel = new QLabel(QStringLiteral("Every"), spamBox);
	spamEveryLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	auto *spamSendLabel = new QLabel(QStringLiteral("Send"), spamBox);
	spamSendLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	spamLayout->addWidget(m_enableSpamPrevention, 0, 0, 1, 2);
	spamLayout->addWidget(spamEveryLabel, 1, 0);
	spamLayout->addWidget(m_spamLineCount, 1, 1);
	spamLayout->addWidget(new QLabel(QStringLiteral("identical commands"), spamBox), 1, 2);
	spamLayout->addWidget(spamSendLabel, 2, 0);
	spamLayout->addWidget(m_spamMessage, 2, 1, 1, 2);
	spamLayout->setColumnStretch(2, 1);
	commandsLeft->addWidget(spamBox);
	commandsLeft->addStretch();

	m_autoRepeat             = new QCheckBox(QStringLiteral("Auto-repeat Command"), commandsPage);
	m_lowerCaseTabCompletion = new QCheckBox(QStringLiteral("Tab Completion In Lower Case"), commandsPage);
	m_translateGerman        = new QCheckBox(QStringLiteral("Translate German characters"), commandsPage);
	m_spellCheckOnSend       = new QCheckBox(QStringLiteral("Spell Check On Send"), commandsPage);
	m_translateBackslash     = new QCheckBox(QStringLiteral("&Translate Backslash Sequences"), commandsPage);
	m_keepCommandsOnSameLine = new QCheckBox(QStringLiteral("Keep Commands On Prompt Line"), commandsPage);
	m_noEchoOff              = new QCheckBox(QStringLiteral("Ignore 'Echo Off' messages"), commandsPage);
	commandsRight->addWidget(m_autoRepeat);
	commandsRight->addWidget(m_lowerCaseTabCompletion);
	commandsRight->addWidget(m_translateGerman);
	commandsRight->addWidget(m_spellCheckOnSend);
	commandsRight->addWidget(m_translateBackslash);
	commandsRight->addWidget(m_keepCommandsOnSameLine);
	commandsRight->addWidget(m_noEchoOff);
	commandsRight->addSpacing(12);

	auto *tabCompletionButton = new QPushButton(QStringLiteral("Tab Completion..."), commandsPage);
	auto *keyboardPrefsButton = new QPushButton(QStringLiteral("Keyboard preferences..."), commandsPage);
	commandsRight->addWidget(tabCompletionButton);
	commandsRight->addWidget(keyboardPrefsButton);
	commandsRight->addSpacing(8);

	auto *historyBox       = new QGroupBox(QStringLiteral("Command History"), commandsPage);
	auto *historyLayout    = new QGridLayout(historyBox);
	auto *historyKeepLabel = new QLabel(QStringLiteral("Keep:"), historyBox);
	historyKeepLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	m_historyLines = new QSpinBox(historyBox);
	m_historyLines->setRange(20, 5000);
	m_historyLines->setMaximumWidth(70);
	historyLayout->addWidget(historyKeepLabel, 0, 0);
	historyLayout->addWidget(m_historyLines, 0, 1);
	historyLayout->addWidget(new QLabel(QStringLiteral("lines."), historyBox), 0, 2);
	m_persistCommandHistory =
	    new QCheckBox(QStringLiteral("Persist command history across reload/restart"), historyBox);
	historyLayout->addWidget(m_persistCommandHistory, 1, 0, 1, 3);
	m_alwaysRecordCommandHistory = new QCheckBox(QStringLiteral("Always record command history"), historyBox);
	m_doNotAddMacrosToCommandHistory =
	    new QCheckBox(QStringLiteral("Do not add macros to command history"), historyBox);
	historyLayout->addWidget(m_alwaysRecordCommandHistory, 2, 0, 1, 3);
	historyLayout->addWidget(m_doNotAddMacrosToCommandHistory, 3, 0, 1, 3);
	historyLayout->setColumnStretch(1, 1);
	commandsRight->addWidget(historyBox);
	commandsRight->addStretch();

	commandsLayout->addLayout(commandsLeft, 0, 0);
	commandsLayout->addLayout(commandsRight, 0, 1);
	commandsLayout->setColumnStretch(0, 3);
	commandsLayout->setColumnStretch(1, 2);

	m_tabCompletionDefaults = new QTextEdit(commandsPage);
	m_tabCompletionDefaults->setVisible(false);
	m_tabCompletionLines = new QSpinBox(commandsPage);
	m_tabCompletionLines->setRange(1, 500000);
	m_tabCompletionLines->setVisible(false);
	m_tabCompletionSpace = new QCheckBox(commandsPage);
	m_tabCompletionSpace->setVisible(false);
	m_confirmBeforeReplacingTyping = new QCheckBox(commandsPage);
	m_confirmBeforeReplacingTyping->setVisible(false);
	m_escapeDeletesInput = new QCheckBox(commandsPage);
	m_escapeDeletesInput->setVisible(false);
	m_doubleClickInserts = new QCheckBox(commandsPage);
	m_doubleClickInserts->setVisible(false);
	m_doubleClickSends = new QCheckBox(commandsPage);
	m_doubleClickSends->setVisible(false);
	m_saveDeletedCommand = new QCheckBox(commandsPage);
	m_saveDeletedCommand->setVisible(false);
	m_arrowsChangeHistory = new QCheckBox(commandsPage);
	m_arrowsChangeHistory->setVisible(false);
	m_arrowRecallsPartial = new QCheckBox(commandsPage);
	m_arrowRecallsPartial->setVisible(false);
	m_altArrowRecallsPartial = new QCheckBox(commandsPage);
	m_altArrowRecallsPartial->setVisible(false);
	m_arrowKeysWrap = new QCheckBox(commandsPage);
	m_arrowKeysWrap->setVisible(false);
	m_ctrlZToEnd = new QCheckBox(commandsPage);
	m_ctrlZToEnd->setVisible(false);
	m_ctrlPToPrev = new QCheckBox(commandsPage);
	m_ctrlPToPrev->setVisible(false);
	m_ctrlNToNext = new QCheckBox(commandsPage);
	m_ctrlNToNext->setVisible(false);
	m_ctrlBackspaceDeletesLastWord = new QCheckBox(commandsPage);
	m_ctrlBackspaceDeletesLastWord->setVisible(false);
	m_defaultAliasExpandVariables = new QCheckBox(commandsPage);
	m_defaultAliasExpandVariables->setVisible(false);
	m_defaultAliasIgnoreCase = new QCheckBox(commandsPage);
	m_defaultAliasIgnoreCase->setVisible(false);
	m_defaultAliasKeepEvaluating = new QCheckBox(commandsPage);
	m_defaultAliasKeepEvaluating->setVisible(false);
	m_defaultAliasRegexp = new QCheckBox(commandsPage);
	m_defaultAliasRegexp->setVisible(false);
	m_defaultAliasSendTo = new QSpinBox(commandsPage);
	m_defaultAliasSendTo->setVisible(false);
	m_defaultAliasSendTo->setRange(0, 64);
	m_defaultAliasSequence = new QSpinBox(commandsPage);
	m_defaultAliasSequence->setVisible(false);
	m_defaultAliasSequence->setRange(0, 10000);
	m_defaultTimerSendTo = new QSpinBox(commandsPage);
	m_defaultTimerSendTo->setVisible(false);
	m_defaultTimerSendTo->setRange(0, 64);
	m_defaultTriggerExpandVariables = new QCheckBox(commandsPage);
	m_defaultTriggerExpandVariables->setVisible(false);
	m_defaultTriggerIgnoreCase = new QCheckBox(commandsPage);
	m_defaultTriggerIgnoreCase->setVisible(false);
	m_defaultTriggerKeepEvaluating = new QCheckBox(commandsPage);
	m_defaultTriggerKeepEvaluating->setVisible(false);
	m_defaultTriggerRegexp = new QCheckBox(commandsPage);
	m_defaultTriggerRegexp->setVisible(false);
	m_defaultTriggerSendTo = new QSpinBox(commandsPage);
	m_defaultTriggerSendTo->setVisible(false);
	m_defaultTriggerSendTo->setRange(0, 64);
	m_defaultTriggerSequence = new QSpinBox(commandsPage);
	m_defaultTriggerSequence->setVisible(false);
	m_defaultTriggerSequence->setRange(0, 10000);

	auto updateEchoSwatches = [this]
	{
		if (!m_echoColour || !m_inputEchoSwatch || !m_inputEchoSwatch2)
			return;
		const int  index = m_echoColour->currentIndex();
		const bool show =
		    index > 0 && index <= m_customTextSwatches.size() && index <= m_customBackSwatches.size();
		m_inputEchoSwatch->setEnabled(show);
		m_inputEchoSwatch2->setEnabled(show);
		if (!show)
		{
			setSwatchButtonColour(m_inputEchoSwatch, QColor(Qt::black));
			setSwatchButtonColour(m_inputEchoSwatch2, QColor(Qt::black));
			return;
		}
		setSwatchButtonColour(m_inputEchoSwatch, swatchButtonColour(m_customTextSwatches.value(index - 1)));
		setSwatchButtonColour(m_inputEchoSwatch2, swatchButtonColour(m_customBackSwatches.value(index - 1)));
	};
	connect(m_echoColour, qOverload<int>(&QComboBox::currentIndexChanged), this,
	        [updateEchoSwatches](int) { updateEchoSwatches(); });
	updateEchoSwatches();

	auto updateEchoName = [this](const int index, const QString &text)
	{
		if (!m_echoColour)
			return;
		const int itemIndex = index + 1;
		if (itemIndex < 0 || itemIndex >= m_echoColour->count())
			return;
		const QString fallback = QStringLiteral("Custom%1").arg(itemIndex);
		m_echoColour->setItemText(itemIndex, text.isEmpty() ? fallback : text);
		if (m_scriptTextColour && index >= 0 && index < m_scriptTextColour->count())
		{
			const QString scriptFallback = QStringLiteral("Custom%1").arg(index + 1);
			m_scriptTextColour->setItemText(index, text.isEmpty() ? scriptFallback : text);
		}
	};
	for (int i = 0; i < m_customColourNames.size(); ++i)
	{
		if (!m_customColourNames[i])
			continue;
		connect(m_customColourNames[i], &QLineEdit::textChanged, this,
		        [updateEchoName, i](const QString &text) { updateEchoName(i, text); });
	}

	auto connectCommandSwatch = [this](QPushButton *swatch, QLineEdit *target, const QString &title)
	{
		if (!swatch || !target)
			return;
		connect(swatch, &QPushButton::clicked, this,
		        [this, swatch, target, title]
		        {
			        const QColor       current = parseColourValue(target->text());
			        ColourPickerDialog dlg(this);
			        dlg.setWindowTitle(title);
			        dlg.setPickColour(true);
			        if (current.isValid())
				        dlg.setInitialColour(current);
			        if (dlg.exec() != QDialog::Accepted)
				        return;
			        const QColor chosen = dlg.selectedColour();
			        if (!chosen.isValid())
				        return;
			        setSwatchButtonColour(swatch, chosen);
			        target->setText(formatColourValue(chosen));
		        });
	};
	connectCommandSwatch(m_commandTextSwatch, m_inputTextColour, QStringLiteral("Command text colour"));
	connectCommandSwatch(m_commandBackSwatch, m_inputBackColour, QStringLiteral("Command background colour"));

	connect(m_inputFontButton, &QPushButton::clicked, this,
	        [this]
	        {
		        QFont current;
		        if (m_inputFontName && !m_inputFontName->text().isEmpty())
			        current = QFont(m_inputFontName->text());
		        if (m_inputFontHeight && m_inputFontHeight->value() > 0)
			        current.setPointSize(m_inputFontHeight->value());
		        if (m_inputFontWeight > 0)
			        current.setWeight(WorldView::mapFontWeight(m_inputFontWeight));
		        current.setItalic(m_inputFontItalic);
		        bool        ok   = false;
		        const QFont font = QFontDialog::getFont(&ok, current, this, QStringLiteral("Input font"));
		        if (!ok)
			        return;
		        if (m_inputFontName)
			        m_inputFontName->setText(font.family());
		        if (m_inputFontHeight)
		        {
			        const int size = font.pointSize() > 0 ? font.pointSize() : m_inputFontHeight->value();
			        m_inputFontHeight->setValue(size);
		        }
		        if (font.weight() >= QFont::Bold)
			        m_inputFontWeight = 700;
		        else if (font.weight() >= QFont::DemiBold)
			        m_inputFontWeight = 600;
		        else if (font.weight() >= QFont::Medium)
			        m_inputFontWeight = 500;
		        else
			        m_inputFontWeight = 400;
		        m_inputFontItalic = font.italic();
		        if (m_inputFontStyle && m_inputFontHeight)
			        m_inputFontStyle->setText(formatFontStyleText(m_inputFontHeight->value(),
			                                                      m_inputFontWeight, m_inputFontItalic));
	        });
	if (m_useDefaultInputFont)
		connect(m_useDefaultInputFont, &QCheckBox::toggled, this, [this] { updateInputFontControls(); });
	if (m_autoResizeCommandWindow)
		connect(m_autoResizeCommandWindow, &QCheckBox::toggled, this,
		        [this] { updateCommandAutoResizeControls(); });
	if (m_autoResizeMinimumLines && m_autoResizeMaximumLines)
	{
		connect(m_autoResizeMinimumLines, qOverload<int>(&QSpinBox::valueChanged), this,
		        [this](const int value)
		        {
			        if (m_autoResizeMaximumLines && m_autoResizeMaximumLines->value() < value)
				        m_autoResizeMaximumLines->setValue(value);
		        });
		connect(m_autoResizeMaximumLines, qOverload<int>(&QSpinBox::valueChanged), this,
		        [this](const int value)
		        {
			        if (m_autoResizeMinimumLines && m_autoResizeMinimumLines->value() > value)
				        m_autoResizeMinimumLines->setValue(value);
		        });
	}

	connect(tabCompletionButton, &QPushButton::clicked, this,
	        [this]
	        {
		        QDialog dialog(this);
		        dialog.setWindowTitle(QStringLiteral("Tab completion"));
		        auto *dialogLayout = new QVBoxLayout(&dialog);
		        dialogLayout->addWidget(new QLabel(QStringLiteral("Default Word List:"), &dialog));
		        auto *detail = new QLabel(
		            QStringLiteral(
		                "Words below will be checked for first when you press <tab> to complete a word in "
		                "the command window. Enter each word with a space between them."),
		            &dialog);
		        detail->setWordWrap(true);
		        dialogLayout->addWidget(detail);
		        auto *words = new QTextEdit(&dialog);
		        if (m_tabCompletionDefaults)
			        words->setPlainText(m_tabCompletionDefaults->toPlainText());
		        dialogLayout->addWidget(words, 1);
		        auto *optionsLayout = new QGridLayout();
		        auto *linesLabel    = new QLabel(QStringLiteral("Lines To Check:"), &dialog);
		        auto *lines         = new QSpinBox(&dialog);
		        lines->setRange(1, 500000);
		        if (m_tabCompletionLines)
			        lines->setValue(m_tabCompletionLines->value());
		        auto *addSpace = new QCheckBox(QStringLiteral("Insert Space After Word"), &dialog);
		        if (m_tabCompletionSpace)
			        addSpace->setChecked(m_tabCompletionSpace->isChecked());
		        optionsLayout->addWidget(linesLabel, 0, 0);
		        optionsLayout->addWidget(lines, 0, 1);
		        optionsLayout->addWidget(addSpace, 0, 2);
		        optionsLayout->setColumnStretch(2, 1);
		        dialogLayout->addLayout(optionsLayout);
		        auto *buttons =
		            new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
		        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
		        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
		        dialogLayout->addWidget(buttons);
		        if (dialog.exec() != QDialog::Accepted)
			        return;
		        if (m_tabCompletionDefaults)
			        m_tabCompletionDefaults->setPlainText(words->toPlainText());
		        if (m_tabCompletionLines)
			        m_tabCompletionLines->setValue(lines->value());
		        if (m_tabCompletionSpace)
			        m_tabCompletionSpace->setChecked(addSpace->isChecked());
	        });

	connect(
	    keyboardPrefsButton, &QPushButton::clicked, this,
	    [this]
	    {
		    QDialog dialog(this);
		    dialog.setWindowTitle(QStringLiteral("Keyboard preferences"));
		    auto *dialogLayout      = new QGridLayout(&dialog);
		    auto *doubleClickBox    = new QGroupBox(QStringLiteral("Double click"), &dialog);
		    auto *doubleClickLayout = new QVBoxLayout(doubleClickBox);
		    auto *doubleClickPaste =
		        new QCheckBox(QStringLiteral("&Double-click Pastes Word"), doubleClickBox);
		    auto *doubleClickSend = new QCheckBox(QStringLiteral("Double-click Sends Word"), doubleClickBox);
		    if (m_doubleClickInserts)
			    doubleClickPaste->setChecked(m_doubleClickInserts->isChecked());
		    if (m_doubleClickSends)
			    doubleClickSend->setChecked(m_doubleClickSends->isChecked());
		    if (doubleClickPaste->isChecked() && doubleClickSend->isChecked())
			    doubleClickPaste->setChecked(false);
		    auto syncDoubleClickChoices = [doubleClickPaste, doubleClickSend]
		    {
			    const bool sends  = doubleClickSend->isChecked();
			    const bool pastes = doubleClickPaste->isChecked();
			    doubleClickPaste->setEnabled(!sends);
			    doubleClickSend->setEnabled(!pastes);
		    };
		    connect(doubleClickSend, &QCheckBox::toggled, &dialog,
		            [doubleClickPaste, syncDoubleClickChoices](const bool checked)
		            {
			            if (checked && doubleClickPaste->isChecked())
				            doubleClickPaste->setChecked(false);
			            syncDoubleClickChoices();
		            });
		    connect(doubleClickPaste, &QCheckBox::toggled, &dialog,
		            [doubleClickSend, syncDoubleClickChoices](const bool checked)
		            {
			            if (checked && doubleClickSend->isChecked())
				            doubleClickSend->setChecked(false);
			            syncDoubleClickChoices();
		            });
		    syncDoubleClickChoices();
		    doubleClickLayout->addWidget(doubleClickPaste);
		    doubleClickLayout->addWidget(doubleClickSend);

		    auto *deletingBox    = new QGroupBox(QStringLiteral("Deleting"), &dialog);
		    auto *deletingLayout = new QVBoxLayout(deletingBox);
		    auto *escapeDeletes  = new QCheckBox(QStringLiteral("&Escape Deletes Typing"), deletingBox);
		    auto *saveDeleted    = new QCheckBox(QStringLiteral("Save Deleted Command"), deletingBox);
		    auto *confirmReplace =
		        new QCheckBox(QStringLiteral("Confirm Before &Replacing Typing"), deletingBox);
		    auto *ctrlBackspace =
		        new QCheckBox(QStringLiteral("Ctrl+Backspace Deletes Last Word"), deletingBox);
		    if (m_escapeDeletesInput)
			    escapeDeletes->setChecked(m_escapeDeletesInput->isChecked());
		    if (m_saveDeletedCommand)
			    saveDeleted->setChecked(m_saveDeletedCommand->isChecked());
		    if (m_confirmBeforeReplacingTyping)
			    confirmReplace->setChecked(m_confirmBeforeReplacingTyping->isChecked());
		    if (m_ctrlBackspaceDeletesLastWord)
			    ctrlBackspace->setChecked(m_ctrlBackspaceDeletesLastWord->isChecked());
		    deletingLayout->addWidget(escapeDeletes);
		    deletingLayout->addWidget(saveDeleted);
		    deletingLayout->addWidget(confirmReplace);
		    deletingLayout->addWidget(ctrlBackspace);

		    auto *arrowsBox      = new QGroupBox(QStringLiteral("Arrow keys"), &dialog);
		    auto *arrowsLayout   = new QVBoxLayout(arrowsBox);
		    auto *arrowsWrap     = new QCheckBox(QStringLiteral("Arrow Keys Wrap History"), arrowsBox);
		    auto *arrowsTraverse = new QCheckBox(QStringLiteral("&Arrow Keys Traverse History"), arrowsBox);
		    auto *arrowsPartial =
		        new QCheckBox(QStringLiteral("Arrow Key Recalls Partial Command"), arrowsBox);
		    auto *arrowsAltPartial =
		        new QCheckBox(QStringLiteral("Alt+Arrow Key Recalls &Partial Command"), arrowsBox);
		    if (m_arrowKeysWrap)
			    arrowsWrap->setChecked(m_arrowKeysWrap->isChecked());
		    if (m_arrowsChangeHistory)
			    arrowsTraverse->setChecked(m_arrowsChangeHistory->isChecked());
		    if (m_arrowRecallsPartial)
			    arrowsPartial->setChecked(m_arrowRecallsPartial->isChecked());
		    if (m_altArrowRecallsPartial)
			    arrowsAltPartial->setChecked(m_altArrowRecallsPartial->isChecked());
		    arrowsLayout->addWidget(arrowsWrap);
		    arrowsLayout->addWidget(arrowsTraverse);
		    arrowsLayout->addWidget(arrowsPartial);
		    arrowsLayout->addWidget(arrowsAltPartial);

		    auto *compatBox    = new QGroupBox(QStringLiteral("Compatibility"), &dialog);
		    auto *compatLayout = new QVBoxLayout(compatBox);
		    auto *ctrlZ = new QCheckBox(QStringLiteral("Ctrl+Z Goes To End Of Output Window"), compatBox);
		    auto *ctrlP = new QCheckBox(QStringLiteral("Ctrl+P Recalls Previous Command"), compatBox);
		    auto *ctrlN = new QCheckBox(QStringLiteral("Ctrl+N Recalls Next Command"), compatBox);
		    if (m_ctrlZToEnd)
			    ctrlZ->setChecked(m_ctrlZToEnd->isChecked());
		    if (m_ctrlPToPrev)
			    ctrlP->setChecked(m_ctrlPToPrev->isChecked());
		    if (m_ctrlNToNext)
			    ctrlN->setChecked(m_ctrlNToNext->isChecked());
		    compatLayout->addWidget(ctrlZ);
		    compatLayout->addWidget(ctrlP);
		    compatLayout->addWidget(ctrlN);

		    dialogLayout->addWidget(doubleClickBox, 0, 0);
		    dialogLayout->addWidget(deletingBox, 0, 1);
		    dialogLayout->addWidget(arrowsBox, 1, 0);
		    dialogLayout->addWidget(compatBox, 1, 1);
		    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
		    dialogLayout->addWidget(buttons, 2, 1, Qt::AlignRight);
		    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
		    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
		    if (dialog.exec() != QDialog::Accepted)
			    return;
		    if (m_doubleClickInserts)
			    m_doubleClickInserts->setChecked(doubleClickPaste->isChecked());
		    if (m_doubleClickSends)
			    m_doubleClickSends->setChecked(doubleClickSend->isChecked());
		    if (m_escapeDeletesInput)
			    m_escapeDeletesInput->setChecked(escapeDeletes->isChecked());
		    if (m_saveDeletedCommand)
			    m_saveDeletedCommand->setChecked(saveDeleted->isChecked());
		    if (m_confirmBeforeReplacingTyping)
			    m_confirmBeforeReplacingTyping->setChecked(confirmReplace->isChecked());
		    if (m_ctrlBackspaceDeletesLastWord)
			    m_ctrlBackspaceDeletesLastWord->setChecked(ctrlBackspace->isChecked());
		    if (m_arrowKeysWrap)
			    m_arrowKeysWrap->setChecked(arrowsWrap->isChecked());
		    if (m_arrowsChangeHistory)
			    m_arrowsChangeHistory->setChecked(arrowsTraverse->isChecked());
		    if (m_arrowRecallsPartial)
			    m_arrowRecallsPartial->setChecked(arrowsPartial->isChecked());
		    if (m_altArrowRecallsPartial)
			    m_altArrowRecallsPartial->setChecked(arrowsAltPartial->isChecked());
		    if (m_ctrlZToEnd)
			    m_ctrlZToEnd->setChecked(ctrlZ->isChecked());
		    if (m_ctrlPToPrev)
			    m_ctrlPToPrev->setChecked(ctrlP->isChecked());
		    if (m_ctrlNToNext)
			    m_ctrlNToNext->setChecked(ctrlN->isChecked());
	    });

	// Send to world
	auto *sendLayout           = new QVBoxLayout(sendToWorldPage);
	m_sendToWorldFilePreamble  = new QTextEdit(sendToWorldPage);
	m_sendToWorldFilePostamble = new QTextEdit(sendToWorldPage);
	m_sendToWorldLinePreamble  = new QLineEdit(sendToWorldPage);
	m_sendToWorldLinePostamble = new QLineEdit(sendToWorldPage);
	sendLayout->addWidget(
	    new QLabel(QStringLiteral("1. Send this at the start of a \"send file to\""), sendToWorldPage));
	sendLayout->addWidget(m_sendToWorldFilePreamble);
	sendLayout->addWidget(
	    new QLabel(QStringLiteral("2. Send this at the start of each line:"), sendToWorldPage));
	sendLayout->addWidget(m_sendToWorldLinePreamble);
	sendLayout->addWidget(
	    new QLabel(QStringLiteral("3. Send this at the end of each line:"), sendToWorldPage));
	sendLayout->addWidget(m_sendToWorldLinePostamble);
	sendLayout->addWidget(
	    new QLabel(QStringLiteral("4. Send this at the end of a \"send file to\""), sendToWorldPage));
	sendLayout->addWidget(m_sendToWorldFilePostamble);
	auto *sendDelayLayout = new QHBoxLayout();
	m_sendLineDelay       = new QSpinBox(sendToWorldPage);
	m_sendLineDelay->setRange(0, 10000);
	m_sendDelayPerLines = new QSpinBox(sendToWorldPage);
	m_sendDelayPerLines->setRange(1, 100000);
	sendDelayLayout->addWidget(new QLabel(QStringLiteral("Delay between"), sendToWorldPage));
	sendDelayLayout->addWidget(m_sendLineDelay);
	sendDelayLayout->addWidget(new QLabel(QStringLiteral("milliseconds. Every"), sendToWorldPage));
	sendDelayLayout->addWidget(m_sendDelayPerLines);
	sendDelayLayout->addWidget(new QLabel(QStringLiteral("lines."), sendToWorldPage));
	sendDelayLayout->addStretch();
	sendLayout->addLayout(sendDelayLayout);
	m_sendCommentedSoftcode = new QCheckBox(QStringLiteral("Commented Softcode"), sendToWorldPage);
	m_sendEcho              = new QCheckBox(QStringLiteral("Echo to output window"), sendToWorldPage);
	m_sendConfirm = new QCheckBox(QStringLiteral("Confirm on each \"send file to world\""), sendToWorldPage);
	sendLayout->addWidget(m_sendCommentedSoftcode);
	sendLayout->addWidget(m_sendEcho);
	sendLayout->addWidget(m_sendConfirm);
	sendLayout->addStretch();

	// Scripting
	auto *scriptingLayout = new QVBoxLayout(scriptingPage);
	m_enableScripts       = new QCheckBox(QStringLiteral("Enable scripting"), scriptingPage);
	m_scriptLanguage      = new QComboBox(scriptingPage);
	m_scriptLanguage->addItem(QStringLiteral("Lua"), QStringLiteral("Lua"));
	m_scriptLanguage->setVisible(false);
	m_scriptFile         = new QLineEdit(scriptingPage);
	m_browseScriptFile   = new QPushButton(QStringLiteral("&Browse..."), scriptingPage);
	m_newScriptFile      = new QPushButton(QStringLiteral("&New..."), scriptingPage);
	m_editScriptFile     = new QPushButton(QStringLiteral("Edit &Script"), scriptingPage);
	m_scriptPrefix       = new QLineEdit(scriptingPage);
	m_scriptEditor       = new QLineEdit(scriptingPage);
	m_editorWindowName   = new QLineEdit(scriptingPage);
	m_chooseScriptEditor = new QPushButton(QStringLiteral("Choose &Editor..."), scriptingPage);
	m_editScriptWithNotepad =
	    new QCheckBox(QStringLiteral("Use inbuilt notepad to edit script"), scriptingPage);
	m_warnIfScriptingInactive = new QCheckBox(QStringLiteral("Warn if inactive"), scriptingPage);
	m_scriptErrorsToOutput    = new QCheckBox(QStringLiteral("Note errors"), scriptingPage);
	m_logScriptErrors         = new QCheckBox(QStringLiteral("Log script errors"), scriptingPage);
	m_scriptReloadOption      = new QComboBox(scriptingPage);
	m_scriptReloadOption->addItem(QStringLiteral("Confirm"), eReloadConfirm);
	m_scriptReloadOption->addItem(QStringLiteral("Reload always"), eReloadAlways);
	m_scriptReloadOption->addItem(QStringLiteral("Reload never"), eReloadNever);
	m_scriptTextColour = new QComboBox(scriptingPage);
	m_scriptTextSwatch = new QPushButton(scriptingPage);
	m_scriptTextSwatch->setFixedSize(16, 16);
	m_scriptTextSwatch->setFlat(true);
	m_scriptTextSwatch->setEnabled(false);
	m_scriptTextSwatch->setFocusPolicy(Qt::NoFocus);
	m_scriptBackSwatch = new QPushButton(scriptingPage);
	m_scriptBackSwatch->setFixedSize(16, 16);
	m_scriptBackSwatch->setFlat(true);
	m_scriptBackSwatch->setEnabled(false);
	m_scriptBackSwatch->setFocusPolicy(Qt::NoFocus);
	m_scriptIsActive      = new QLabel(QStringLiteral("(inactive)"), scriptingPage);
	m_scriptExecutionTime = new QLabel(QStringLiteral("-"), scriptingPage);

	auto *scriptPrefixLabel = new QLabel(QStringLiteral("Script"), scriptingPage);
	m_scriptPrefix->setFixedWidth(200);
	auto *scriptingTopLayout = new QGridLayout();
	scriptingTopLayout->addWidget(scriptPrefixLabel, 0, 0, Qt::AlignLeft);
	scriptingTopLayout->addWidget(m_scriptPrefix, 0, 1, 1, 2, Qt::AlignLeft);
	scriptingTopLayout->addWidget(m_enableScripts, 1, 0, Qt::AlignLeft);
	scriptingTopLayout->addWidget(m_warnIfScriptingInactive, 1, 1, Qt::AlignLeft);
	scriptingTopLayout->addWidget(m_scriptIsActive, 1, 2, Qt::AlignLeft);
	scriptingTopLayout->setColumnStretch(3, 1);
	scriptingLayout->addLayout(scriptingTopLayout);

	auto *scriptFileBox    = new QGroupBox(QStringLiteral("External Script file"), scriptingPage);
	auto *scriptFileLayout = new QGridLayout(scriptFileBox);
	scriptFileLayout->addWidget(new QLabel(QStringLiteral("Script"), scriptingPage), 0, 0);
	scriptFileLayout->addWidget(m_scriptFile, 0, 1, 1, 3);
	scriptFileLayout->addWidget(m_browseScriptFile, 1, 1);
	scriptFileLayout->addWidget(m_newScriptFile, 1, 2);
	scriptFileLayout->addWidget(m_editScriptFile, 1, 3);
	scriptFileLayout->addWidget(m_chooseScriptEditor, 2, 0);
	scriptFileLayout->addWidget(m_scriptEditor, 2, 1, 1, 3);
	scriptFileLayout->addWidget(new QLabel(QStringLiteral("Editor window name"), scriptingPage), 3, 0);
	scriptFileLayout->addWidget(m_editorWindowName, 3, 1, 1, 3);
	scriptFileLayout->addWidget(m_editScriptWithNotepad, 1, 4, 1, 2);
	scriptFileLayout->addWidget(new QLabel(QStringLiteral("Recompile when script file"), scriptingPage), 4,
	                            0);
	scriptFileLayout->addWidget(m_scriptReloadOption, 4, 1);
	scriptingLayout->addWidget(scriptFileBox);

	auto *noteLayout = new QHBoxLayout();
	m_scriptTextColour->addItem(QStringLiteral("(default)"));
	constexpr int noteCustomCount = MAX_CUSTOM;
	for (int i = 0; i < noteCustomCount; ++i)
	{
		QString name;
		if (i < m_customColourNames.size() && m_customColourNames[i])
			name = m_customColourNames[i]->text();
		if (name.isEmpty())
			name = QStringLiteral("Custom%1").arg(i + 1);
		m_scriptTextColour->addItem(name);
	}
	m_scriptTextColour->setFixedWidth(120);
	connect(m_scriptTextColour, qOverload<int>(&QComboBox::currentIndexChanged), this,
	        [this](const int) { updateScriptNoteSwatches(); });
	noteLayout->addWidget(m_scriptErrorsToOutput);
	noteLayout->addWidget(m_logScriptErrors);
	noteLayout->addSpacing(8);
	noteLayout->addWidget(new QLabel(QStringLiteral("Note"), scriptingPage));
	noteLayout->addWidget(m_scriptTextColour);
	noteLayout->addWidget(m_scriptTextSwatch);
	noteLayout->addWidget(m_scriptBackSwatch);
	noteLayout->addStretch();
	scriptingLayout->addLayout(noteLayout);

	auto *callbackBox    = new QGroupBox(QStringLiteral("World Events"), scriptingPage);
	auto *callbackLayout = new QGridLayout(callbackBox);
	auto  addCallbackRow =
	    [callbackBox, callbackLayout](const QString &labelText, QLineEdit *edit, const int row)
	{
		auto *label = new QLabel(labelText, callbackBox);
		label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
		callbackLayout->addWidget(label, row, 0);
		callbackLayout->addWidget(edit, row, 1, 1, 2);
	};
	m_onWorldOpen       = new QLineEdit(callbackBox);
	m_onWorldClose      = new QLineEdit(callbackBox);
	m_onWorldConnect    = new QLineEdit(callbackBox);
	m_onWorldDisconnect = new QLineEdit(callbackBox);
	m_onWorldSave       = new QLineEdit(callbackBox);
	m_onWorldGetFocus   = new QLineEdit(callbackBox);
	m_onWorldLoseFocus  = new QLineEdit(callbackBox);
	m_onMxpStart        = new QLineEdit(callbackBox);
	m_onMxpStop         = new QLineEdit(callbackBox);
	m_onMxpOpenTag      = new QLineEdit(callbackBox);
	m_onMxpCloseTag     = new QLineEdit(callbackBox);
	m_onMxpSetVariable  = new QLineEdit(callbackBox);
	m_onMxpError        = new QLineEdit(callbackBox);
	m_onMxpStart->setVisible(false);
	m_onMxpStop->setVisible(false);
	m_onMxpOpenTag->setVisible(false);
	m_onMxpCloseTag->setVisible(false);
	m_onMxpSetVariable->setVisible(false);
	m_onMxpError->setVisible(false);
	addCallbackRow(QStringLiteral("Open"), m_onWorldOpen, 0);
	addCallbackRow(QStringLiteral("Connect"), m_onWorldConnect, 1);
	addCallbackRow(QStringLiteral("Get Focus"), m_onWorldGetFocus, 2);
	addCallbackRow(QStringLiteral("Lose Focus"), m_onWorldLoseFocus, 3);
	addCallbackRow(QStringLiteral("Disconnect"), m_onWorldDisconnect, 4);
	addCallbackRow(QStringLiteral("Close"), m_onWorldClose, 5);
	addCallbackRow(QStringLiteral("Save"), m_onWorldSave, 6);
	auto *mxpScriptsButton = new QPushButton(QStringLiteral("MXP..."), callbackBox);
	auto *mxpHint = new QLabel(QStringLiteral("(Click for MXP-related script handlers)"), callbackBox);
	callbackLayout->addWidget(mxpScriptsButton, 7, 1, Qt::AlignLeft);
	callbackLayout->addWidget(mxpHint, 7, 2, Qt::AlignLeft);
	callbackLayout->setColumnStretch(2, 1);
	connect(mxpScriptsButton, &QPushButton::clicked, this,
	        [this]
	        {
		        QDialog dialog(this);
		        dialog.setWindowTitle(QStringLiteral("MXP Script Handlers"));
		        auto *dialogLayout     = new QVBoxLayout(&dialog);
		        auto *form             = new QFormLayout();
		        auto *onMxpStart       = new QLineEdit(&dialog);
		        auto *onMxpStop        = new QLineEdit(&dialog);
		        auto *onMxpOpenTag     = new QLineEdit(&dialog);
		        auto *onMxpCloseTag    = new QLineEdit(&dialog);
		        auto *onMxpSetVariable = new QLineEdit(&dialog);
		        auto *onMxpError       = new QLineEdit(&dialog);
		        if (m_onMxpStart)
			        onMxpStart->setText(m_onMxpStart->text());
		        if (m_onMxpStop)
			        onMxpStop->setText(m_onMxpStop->text());
		        if (m_onMxpOpenTag)
			        onMxpOpenTag->setText(m_onMxpOpenTag->text());
		        if (m_onMxpCloseTag)
			        onMxpCloseTag->setText(m_onMxpCloseTag->text());
		        if (m_onMxpSetVariable)
			        onMxpSetVariable->setText(m_onMxpSetVariable->text());
		        if (m_onMxpError)
			        onMxpError->setText(m_onMxpError->text());
		        form->addRow(QStringLiteral("On MXP start"), onMxpStart);
		        form->addRow(QStringLiteral("On MXP stop"), onMxpStop);
		        form->addRow(QStringLiteral("On MXP open tag"), onMxpOpenTag);
		        form->addRow(QStringLiteral("On MXP close tag"), onMxpCloseTag);
		        form->addRow(QStringLiteral("On MXP set variable"), onMxpSetVariable);
		        form->addRow(QStringLiteral("On MXP error"), onMxpError);
		        dialogLayout->addLayout(form);
		        auto *buttons =
		            new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
		        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
		        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
		        dialogLayout->addWidget(buttons);
		        if (dialog.exec() != QDialog::Accepted)
			        return;
		        if (m_onMxpStart)
			        m_onMxpStart->setText(onMxpStart->text());
		        if (m_onMxpStop)
			        m_onMxpStop->setText(onMxpStop->text());
		        if (m_onMxpOpenTag)
			        m_onMxpOpenTag->setText(onMxpOpenTag->text());
		        if (m_onMxpCloseTag)
			        m_onMxpCloseTag->setText(onMxpCloseTag->text());
		        if (m_onMxpSetVariable)
			        m_onMxpSetVariable->setText(onMxpSetVariable->text());
		        if (m_onMxpError)
			        m_onMxpError->setText(onMxpError->text());
	        });

	auto *scriptingEvents = new QHBoxLayout();
	scriptingEvents->addWidget(callbackBox);
	auto *scriptingSide    = new QVBoxLayout();
	auto *scriptTimeLayout = new QHBoxLayout();
	auto *timeLabel        = new QLabel(QStringLiteral("Time spent:"), scriptingPage);
	scriptTimeLayout->addWidget(timeLabel);
	scriptTimeLayout->addWidget(m_scriptExecutionTime);
	scriptTimeLayout->addStretch();
	scriptingSide->addStretch();
	scriptingSide->addLayout(scriptTimeLayout);
	scriptingEvents->addLayout(scriptingSide);
	scriptingLayout->addLayout(scriptingEvents);
	scriptingLayout->addStretch();

	if (m_browseScriptFile && m_scriptFile)
		connect(m_browseScriptFile, &QPushButton::clicked, this,
		        [this]
		        {
			        const QString startDir = m_runtime ? m_runtime->fileBrowsingDirectory() : QString();
			        const QString fileName =
			            QFileDialog::getOpenFileName(this, QStringLiteral("Script file name"), startDir,
			                                         QStringLiteral("Lua files (*.lua);;All files (*.*)"));
			        if (!fileName.isEmpty())
			        {
				        m_scriptFile->setText(fileName);
				        if (m_runtime)
					        m_runtime->setFileBrowsingDirectory(QFileInfo(fileName).absolutePath());
			        }
		        });
	if (m_newScriptFile && m_scriptFile)
		connect(m_newScriptFile, &QPushButton::clicked, this,
		        [this]
		        {
			        const QString startDir = m_runtime ? m_runtime->fileBrowsingDirectory() : QString();
			        const QString fileName =
			            QFileDialog::getSaveFileName(this, QStringLiteral("New script file"), startDir,
			                                         QStringLiteral("Lua files (*.lua);;All files (*.*)"));
			        if (fileName.isEmpty())
				        return;
			        QString finalName = fileName;
			        if (QFileInfo(finalName).suffix().isEmpty())
				        finalName += QStringLiteral(".lua");
			        QSaveFile file(finalName);
			        if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
			        {
				        QMessageBox::warning(this, QStringLiteral("New script file"),
				                             QStringLiteral("Unable to create the requested file."));
				        return;
			        }
			        file.commit();
			        m_scriptFile->setText(finalName);
			        if (m_runtime)
				        m_runtime->setFileBrowsingDirectory(QFileInfo(finalName).absolutePath());
		        });
	if (m_editScriptFile && m_scriptFile)
		connect(m_editScriptFile, &QPushButton::clicked, this,
		        [this]
		        {
			        const QString fileName = m_scriptFile->text().trimmed();
			        if (fileName.isEmpty())
				        return;
			        AppController *app              = AppController::instance();
			        const QString  resolvedFileName = app ? app->makeAbsolutePath(fileName) : fileName;
			        const QString  editorWindowName =
                        m_editorWindowName ? m_editorWindowName->text().trimmed() : QString();
			        const auto tryRaiseConfiguredEditorWindow = [&]
			        {
				        if (editorWindowName.isEmpty())
					        return;
				        bringOwnedWindowToFrontByTitle(editorWindowName);
			        };
			        if (m_editScriptWithNotepad && m_editScriptWithNotepad->isChecked())
			        {
				        if (!QFileInfo::exists(resolvedFileName))
				        {
					        QMessageBox::warning(this, QStringLiteral("Edit script"),
					                             QStringLiteral("Unable to open the requested file."));
					        return;
				        }
				        if (app)
				        {
					        (void)app->openTextDocument(resolvedFileName);
					        tryRaiseConfiguredEditorWindow();
					        return;
				        }
			        }
			        QString editorPath;
			        if (m_scriptEditor)
				        editorPath = m_scriptEditor->text().trimmed();
			        QString editorArgs;
			        if (m_runtime)
				        editorArgs = m_runtime->worldAttributes()
				                         .value(QStringLiteral("script_editor_argument"))
				                         .trimmed();
			        if (editorArgs.isEmpty())
				        editorArgs = QStringLiteral("\"%file\"");
			        editorArgs.replace(QStringLiteral("%file"), resolvedFileName);
			        if (!editorPath.isEmpty())
			        {
				        if (const QStringList splitArgs = QProcess::splitCommand(editorArgs);
				            QProcess::startDetached(editorPath, splitArgs))
				        {
					        tryRaiseConfiguredEditorWindow();
					        return;
				        }
			        }
			        if (!QDesktopServices::openUrl(QUrl::fromLocalFile(resolvedFileName)))
			        {
				        QMessageBox::warning(this, QStringLiteral("Edit script"),
				                             QStringLiteral("Unable to open the requested file."));
			        }
			        else
				        tryRaiseConfiguredEditorWindow();
		        });
	if (m_chooseScriptEditor && m_scriptEditor)
		connect(m_chooseScriptEditor, &QPushButton::clicked, this,
		        [this]
		        {
			        const QString startDir = m_runtime ? m_runtime->fileBrowsingDirectory() : QString();
			        if (const QString fileName =
			                QFileDialog::getOpenFileName(this, QStringLiteral("Choose script editor"),
			                                             startDir, QStringLiteral("Programs (*.*)"));
			            !fileName.isEmpty())
			        {
				        if (m_runtime)
					        m_runtime->setFileBrowsingDirectory(QFileInfo(fileName).absolutePath());
				        m_scriptEditor->setText(fileName);
			        }
		        });
	if (m_editScriptWithNotepad && m_chooseScriptEditor)
		connect(m_editScriptWithNotepad, &QCheckBox::toggled, this,
		        [this](const bool checked)
		        {
			        m_chooseScriptEditor->setEnabled(!checked);
			        if (m_scriptEditor)
				        m_scriptEditor->setEnabled(!checked);
		        });
	if (m_editScriptWithNotepad)
	{
		const bool checked = m_editScriptWithNotepad->isChecked();
		if (m_chooseScriptEditor)
			m_chooseScriptEditor->setEnabled(!checked);
		if (m_scriptEditor)
			m_scriptEditor->setEnabled(!checked);
	}
	if (m_scriptFile && m_editScriptFile)
	{
		connect(m_scriptFile, &QLineEdit::textChanged, this,
		        [this](const QString &text) { m_editScriptFile->setEnabled(!text.trimmed().isEmpty()); });
		m_editScriptFile->setEnabled(!m_scriptFile->text().trimmed().isEmpty());
	}

	// Notes
	auto *notesLayout = new QVBoxLayout(notesPage);
	m_notes           = new QTextEdit(notesPage);
	notesLayout->addWidget(m_notes);
	auto *notesButtons    = new QHBoxLayout();
	m_loadNotesButton     = new QPushButton(QStringLiteral("&Load..."), notesPage);
	m_saveNotesButton     = new QPushButton(QStringLiteral("&Save..."), notesPage);
	m_editNotesButton     = new QPushButton(QStringLiteral("&Edit..."), notesPage);
	m_findNotesButton     = new QPushButton(QStringLiteral("&Find..."), notesPage);
	m_findNextNotesButton = new QPushButton(QStringLiteral("Find &Next"), notesPage);
	notesButtons->addWidget(m_loadNotesButton);
	notesButtons->addWidget(m_saveNotesButton);
	notesButtons->addWidget(m_editNotesButton);
	notesButtons->addWidget(m_findNotesButton);
	notesButtons->addWidget(m_findNextNotesButton);
	notesButtons->addStretch();
	notesLayout->addLayout(notesButtons);
	m_saveNotesButton->setEnabled(!m_notes->toPlainText().isEmpty());
	connect(m_notes, &QTextEdit::textChanged, this,
	        [this]
	        {
		        if (m_notesUpdating || !m_notes)
			        return;
		        constexpr int maxLength = 32000;
		        const QString text      = m_notes->toPlainText();
		        if (text.size() > maxLength)
		        {
			        m_notesUpdating         = true;
			        const QString trimmed   = text.left(maxLength);
			        const int     cursorPos = m_notes->textCursor().position();
			        m_notes->setPlainText(trimmed);
			        QTextCursor cursor = m_notes->textCursor();
			        cursor.setPosition(qMin(cursorPos, maxLength));
			        m_notes->setTextCursor(cursor);
			        m_notesUpdating = false;
			        QMessageBox::warning(this, QStringLiteral("Notes"),
			                             QStringLiteral("Notes are limited to 32000 characters."));
		        }
		        if (m_saveNotesButton)
			        m_saveNotesButton->setEnabled(!m_notes->toPlainText().isEmpty());
	        });
	connect(m_loadNotesButton, &QPushButton::clicked, this,
	        [this]
	        {
		        const QString startDir = m_runtime ? m_runtime->fileBrowsingDirectory() : QString();
		        if (const QString fileName =
		                QFileDialog::getOpenFileName(this, QStringLiteral("File to load notes from"),
		                                             startDir, QStringLiteral("Text files (*.txt)"));
		            !fileName.isEmpty())
		        {
			        if (m_runtime)
				        m_runtime->setFileBrowsingDirectory(QFileInfo(fileName).absolutePath());
			        QFile file(fileName);
			        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
			        {
				        QMessageBox::warning(this, QStringLiteral("Load notes"),
				                             QStringLiteral("Unable to open the requested file."));
				        return;
			        }
			        const qint64 length = file.size();
			        if (length > 32000)
			        {
				        QMessageBox::warning(
				            this, QStringLiteral("Load notes"),
				            QStringLiteral("File exceeds 32000 bytes in length, cannot be loaded"));
				        return;
			        }
			        if (length <= 0)
			        {
				        QMessageBox::warning(this, QStringLiteral("Load notes"),
				                             QStringLiteral("File is empty"));
				        return;
			        }
			        QTextStream in(&file);
			        in.setEncoding(QStringConverter::Utf8);
			        m_notes->setPlainText(in.readAll());
		        }
	        });
	connect(m_saveNotesButton, &QPushButton::clicked, this,
	        [this]
	        {
		        const QString startDir = m_runtime ? m_runtime->fileBrowsingDirectory() : QString();
		        QString       suggestedName;
		        if (m_runtime)
			        suggestedName = m_runtime->worldAttributes().value(QStringLiteral("name"));
		        if (!suggestedName.isEmpty())
			        suggestedName += QStringLiteral(" notes");
		        QString initialPath = startDir;
		        if (!suggestedName.isEmpty())
			        initialPath = startDir.isEmpty() ? suggestedName : QDir(startDir).filePath(suggestedName);
		        if (const QString fileName =
		                QFileDialog::getSaveFileName(this, QStringLiteral("File to save notes into"),
		                                             initialPath, QStringLiteral("Text files (*.txt)"));
		            !fileName.isEmpty())
		        {
			        if (m_runtime)
				        m_runtime->setFileBrowsingDirectory(QFileInfo(fileName).absolutePath());
			        QSaveFile file(fileName);
			        if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
			        {
				        QMessageBox::warning(this, QStringLiteral("Save notes"),
				                             QStringLiteral("Unable to create the requested file."));
				        return;
			        }
			        QTextStream out(&file);
			        out.setEncoding(QStringConverter::Utf8);
			        out << m_notes->toPlainText();
			        if (!file.commit())
				        QMessageBox::warning(this, QStringLiteral("Save notes"),
				                             QStringLiteral("Unable to create the requested file."));
		        }
	        });
	connect(m_editNotesButton, &QPushButton::clicked, this,
	        [this]
	        {
		        QDialog dialog(this);
		        dialog.setWindowTitle(QStringLiteral("Edit notes"));
		        auto *dialogLayout = new QVBoxLayout(&dialog);
		        auto *edit         = new QTextEdit(&dialog);
		        edit->setPlainText(m_notes->toPlainText());
		        dialogLayout->addWidget(edit);
		        auto *buttons =
		            new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
		        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
		        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
		        dialogLayout->addWidget(buttons);
		        if (dialog.exec() == QDialog::Accepted)
			        m_notes->setPlainText(edit->toPlainText());
	        });
	connect(m_findNotesButton, &QPushButton::clicked, this, [this] { doNotesFind(false); });
	connect(m_findNextNotesButton, &QPushButton::clicked, this, [this] { doNotesFind(true); });

	// Tables
	auto makeTable = [](QWidget *parent, const QStringList &headers) -> QTableWidget *
	{
		auto *table = new QTableWidget(parent);
		table->setColumnCount(saturatingToInt(headers.size()));
		table->setHorizontalHeaderLabels(headers);
		table->horizontalHeader()->setStretchLastSection(true);
		table->verticalHeader()->setVisible(false);
		table->setEditTriggers(QAbstractItemView::NoEditTriggers);
		table->setSelectionBehavior(QAbstractItemView::SelectRows);
		table->setSelectionMode(QAbstractItemView::SingleSelection);
		table->setShowGrid(gridLinesEnabled());
		return table;
	};

	auto makeRuleTree = [](QWidget *parent) -> QTreeWidget *
	{
		auto *tree = new QTreeWidget(parent);
		tree->setColumnCount(1);
		tree->setHeaderHidden(true);
		tree->setRootIsDecorated(true);
		tree->setUniformRowHeights(true);
		tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
		tree->setSelectionMode(QAbstractItemView::SingleSelection);
		tree->setSelectionBehavior(QAbstractItemView::SelectRows);
		return tree;
	};

	auto *triggersLayout = new QVBoxLayout(triggersPage);
	m_triggersViewStack  = new QStackedWidget(triggersPage);
	m_triggersTable      = makeTable(triggersPage, {QStringLiteral("Trigger"), QStringLiteral("Sequence"),
	                                                QStringLiteral("Contents"), QStringLiteral("Label"),
	                                                QStringLiteral("Group")});
	m_triggersTree       = makeRuleTree(triggersPage);
	m_triggersViewStack->addWidget(m_triggersTable);
	m_triggersViewStack->addWidget(m_triggersTree);
	triggersLayout->addWidget(m_triggersViewStack);
	auto *triggerTreeLayout = new QHBoxLayout();
	m_triggerTreeView       = new QCheckBox(QStringLiteral("Tree View"), triggersPage);
	m_triggersCount         = new QLabel(QStringLiteral("0 triggers."), triggersPage);
	triggerTreeLayout->addWidget(m_triggersCount);
	triggerTreeLayout->addStretch();
	triggerTreeLayout->addWidget(m_triggerTreeView);
	triggersLayout->addLayout(triggerTreeLayout);
	auto *triggerOptions  = new QGridLayout();
	m_enableTriggers      = new QCheckBox(QStringLiteral("Enable &Triggers"), triggersPage);
	m_enableTriggerSounds = new QCheckBox(QStringLiteral("Enable Trigger S&ounds"), triggersPage);
	m_useDefaultTriggers  = new QCheckBox(QStringLiteral("O&verride with default triggers"), triggersPage);
	m_filterTriggers      = new QCheckBox(QStringLiteral("F&ilter by:"), triggersPage);
	m_editTriggersFilter  = new QPushButton(QStringLiteral("..."), triggersPage);
	m_editTriggersFilter->setFixedWidth(24);
	triggerOptions->setContentsMargins(0, 0, 0, 0);
	triggerOptions->setHorizontalSpacing(12);
	triggerOptions->setVerticalSpacing(4);
	triggerOptions->addWidget(m_enableTriggers, 0, 0);
	triggerOptions->addWidget(m_enableTriggerSounds, 0, 1);
	triggerOptions->addWidget(m_useDefaultTriggers, 1, 0);
	auto *triggerFilterLayout = new QHBoxLayout();
	triggerFilterLayout->setContentsMargins(0, 0, 0, 0);
	triggerFilterLayout->addWidget(m_filterTriggers);
	triggerFilterLayout->addWidget(m_editTriggersFilter);
	triggerOptions->addLayout(triggerFilterLayout, 1, 1);

	auto *triggerButtons    = new QGridLayout();
	m_addTriggerButton      = new QPushButton(QStringLiteral("&Add..."), triggersPage);
	m_editTriggerButton     = new QPushButton(QStringLiteral("&Edit..."), triggersPage);
	m_deleteTriggerButton   = new QPushButton(QStringLiteral("&Remove"), triggersPage);
	m_findTriggerButton     = new QPushButton(QStringLiteral("&Find..."), triggersPage);
	m_findNextTriggerButton = new QPushButton(QStringLiteral("Find &Next"), triggersPage);
	m_loadTriggersButton    = new QPushButton(QStringLiteral("&Load..."), triggersPage);
	m_saveTriggersButton    = new QPushButton(QStringLiteral("&Save..."), triggersPage);
	m_moveTriggerUpButton   = new QPushButton(QStringLiteral("Move &Up"), triggersPage);
	m_moveTriggerDownButton = new QPushButton(QStringLiteral("Move &Down"), triggersPage);
	m_copyTriggerButton     = new QPushButton(QStringLiteral("&Copy"), triggersPage);
	m_pasteTriggerButton    = new QPushButton(QStringLiteral("&Paste"), triggersPage);
	triggerButtons->addWidget(m_addTriggerButton, 0, 0);
	triggerButtons->addLayout(triggerOptions, 0, 1, 1, 4, Qt::AlignLeft | Qt::AlignVCenter);
	triggerButtons->addWidget(m_editTriggerButton, 1, 0);
	triggerButtons->addWidget(m_deleteTriggerButton, 2, 0);
	triggerButtons->addWidget(m_findTriggerButton, 1, 1);
	triggerButtons->addWidget(m_findNextTriggerButton, 2, 1);
	triggerButtons->addWidget(m_loadTriggersButton, 1, 2);
	triggerButtons->addWidget(m_saveTriggersButton, 2, 2);
	triggerButtons->addWidget(m_moveTriggerUpButton, 1, 3);
	triggerButtons->addWidget(m_moveTriggerDownButton, 2, 3);
	triggerButtons->addWidget(m_copyTriggerButton, 1, 4);
	triggerButtons->addWidget(m_pasteTriggerButton, 2, 4);
	triggerButtons->setColumnStretch(5, 1);
	triggersLayout->addLayout(triggerButtons);

	auto *aliasesLayout = new QVBoxLayout(aliasesPage);
	m_aliasesViewStack  = new QStackedWidget(aliasesPage);
	m_aliasesTable      = makeTable(aliasesPage, {QStringLiteral("Alias"), QStringLiteral("Sequence"),
	                                              QStringLiteral("Contents"), QStringLiteral("Label"),
	                                              QStringLiteral("Group")});
	m_aliasesTree       = makeRuleTree(aliasesPage);
	m_aliasesViewStack->addWidget(m_aliasesTable);
	m_aliasesViewStack->addWidget(m_aliasesTree);
	aliasesLayout->addWidget(m_aliasesViewStack);
	auto *aliasTreeLayout = new QHBoxLayout();
	m_aliasesCount        = new QLabel(QStringLiteral("0 aliases."), aliasesPage);
	m_aliasTreeView       = new QCheckBox(QStringLiteral("Tree View"), aliasesPage);
	aliasTreeLayout->addWidget(m_aliasesCount);
	aliasTreeLayout->addStretch();
	aliasTreeLayout->addWidget(m_aliasTreeView);
	aliasesLayout->addLayout(aliasTreeLayout);

	auto *aliasOptions  = new QGridLayout();
	m_enableAliases     = new QCheckBox(QStringLiteral("Ena&ble Aliases"), aliasesPage);
	m_useDefaultAliases = new QCheckBox(QStringLiteral("O&verride with default aliases"), aliasesPage);
	m_filterAliases     = new QCheckBox(QStringLiteral("F&ilter by:"), aliasesPage);
	m_editAliasesFilter = new QPushButton(QStringLiteral("..."), aliasesPage);
	m_editAliasesFilter->setFixedWidth(24);
	aliasOptions->setContentsMargins(0, 0, 0, 0);
	aliasOptions->setHorizontalSpacing(12);
	aliasOptions->setVerticalSpacing(4);
	if (m_enableTriggers)
		m_enableAliases->setMinimumWidth(m_enableTriggers->sizeHint().width());
	if (m_useDefaultTriggers)
		m_useDefaultAliases->setMinimumWidth(m_useDefaultTriggers->sizeHint().width());
	if (m_enableTriggerSounds)
		aliasOptions->setColumnMinimumWidth(1, m_enableTriggerSounds->sizeHint().width());
	aliasOptions->addWidget(m_enableAliases, 0, 0);
	aliasOptions->addWidget(m_useDefaultAliases, 1, 0);
	auto *aliasFilterLayout = new QHBoxLayout();
	aliasFilterLayout->setContentsMargins(0, 0, 0, 0);
	aliasFilterLayout->addWidget(m_filterAliases);
	aliasFilterLayout->addStretch();
	aliasFilterLayout->addWidget(m_editAliasesFilter);
	aliasOptions->addLayout(aliasFilterLayout, 1, 1);

	auto *aliasButtons    = new QGridLayout();
	m_addAliasButton      = new QPushButton(QStringLiteral("&Add..."), aliasesPage);
	m_editAliasButton     = new QPushButton(QStringLiteral("&Edit..."), aliasesPage);
	m_deleteAliasButton   = new QPushButton(QStringLiteral("&Remove"), aliasesPage);
	m_findAliasButton     = new QPushButton(QStringLiteral("&Find..."), aliasesPage);
	m_findNextAliasButton = new QPushButton(QStringLiteral("Find &Next"), aliasesPage);
	m_loadAliasesButton   = new QPushButton(QStringLiteral("&Load..."), aliasesPage);
	m_saveAliasesButton   = new QPushButton(QStringLiteral("&Save..."), aliasesPage);
	m_moveAliasUpButton   = new QPushButton(QStringLiteral("Move &Up"), aliasesPage);
	m_moveAliasDownButton = new QPushButton(QStringLiteral("Move &Down"), aliasesPage);
	m_copyAliasButton     = new QPushButton(QStringLiteral("&Copy"), aliasesPage);
	m_pasteAliasButton    = new QPushButton(QStringLiteral("&Paste"), aliasesPage);
	aliasButtons->addWidget(m_addAliasButton, 0, 0);
	aliasButtons->addLayout(aliasOptions, 0, 1, 1, 4, Qt::AlignLeft | Qt::AlignVCenter);
	aliasButtons->addWidget(m_editAliasButton, 1, 0);
	aliasButtons->addWidget(m_deleteAliasButton, 2, 0);
	aliasButtons->addWidget(m_findAliasButton, 1, 1);
	aliasButtons->addWidget(m_findNextAliasButton, 2, 1);
	aliasButtons->addWidget(m_loadAliasesButton, 1, 2);
	aliasButtons->addWidget(m_saveAliasesButton, 2, 2);
	aliasButtons->addWidget(m_moveAliasUpButton, 1, 3);
	aliasButtons->addWidget(m_moveAliasDownButton, 2, 3);
	aliasButtons->addWidget(m_copyAliasButton, 1, 4);
	aliasButtons->addWidget(m_pasteAliasButton, 2, 4);
	aliasButtons->setColumnStretch(5, 1);
	aliasesLayout->addLayout(aliasButtons);

	auto *timersLayout = new QVBoxLayout(timersPage);
	m_timersViewStack  = new QStackedWidget(timersPage);
	m_timersTable =
	    makeTable(timersPage, {QStringLiteral("Type"), QStringLiteral("When"), QStringLiteral("Contents"),
	                           QStringLiteral("Label"), QStringLiteral("Group"), QStringLiteral("Next")});
	m_timersTree = makeRuleTree(timersPage);
	m_timersViewStack->addWidget(m_timersTable);
	m_timersViewStack->addWidget(m_timersTree);
	timersLayout->addWidget(m_timersViewStack);
	auto *timerTreeLayout = new QHBoxLayout();
	m_timerTreeView       = new QCheckBox(QStringLiteral("Tree View"), timersPage);
	timerTreeLayout->addStretch();
	timerTreeLayout->addWidget(m_timerTreeView);
	timersLayout->addLayout(timerTreeLayout);
	auto *timerCountLayout = new QHBoxLayout();
	m_timersCount          = new QLabel(QStringLiteral("0 timers."), timersPage);
	timerCountLayout->addWidget(m_timersCount);
	timerCountLayout->addStretch();
	timersLayout->addLayout(timerCountLayout);

	auto *timerButtons = new QGridLayout();
	m_addTimerButton   = new QPushButton(QStringLiteral("&Add..."), timersPage);
	m_enableTimers     = new QCheckBox(QStringLiteral("Enable &Timers"), timersPage);
	m_useDefaultTimers = new QCheckBox(QStringLiteral("O&verride with default timers"), timersPage);
	m_filterTimers     = new QCheckBox(QStringLiteral("F&ilter by:"), timersPage);
	m_editTimersFilter = new QPushButton(QStringLiteral("..."), timersPage);
	m_editTimersFilter->setFixedWidth(24);
	m_editTimerButton     = new QPushButton(QStringLiteral("&Edit..."), timersPage);
	m_deleteTimerButton   = new QPushButton(QStringLiteral("&Remove"), timersPage);
	m_findTimerButton     = new QPushButton(QStringLiteral("&Find..."), timersPage);
	m_findNextTimerButton = new QPushButton(QStringLiteral("Find &Next"), timersPage);
	m_loadTimersButton    = new QPushButton(QStringLiteral("&Load..."), timersPage);
	m_saveTimersButton    = new QPushButton(QStringLiteral("&Save..."), timersPage);
	m_resetTimersButton   = new QPushButton(QStringLiteral("Reset All Timers"), timersPage);
	m_copyTimerButton     = new QPushButton(QStringLiteral("&Copy"), timersPage);
	m_pasteTimerButton    = new QPushButton(QStringLiteral("&Paste"), timersPage);
	timerButtons->addWidget(m_enableTimers, 0, 1);
	timerButtons->addWidget(m_addTimerButton, 1, 0);
	timerButtons->addWidget(m_useDefaultTimers, 1, 1);
	timerButtons->addWidget(m_filterTimers, 1, 3);
	timerButtons->addWidget(m_editTimersFilter, 1, 4);
	timerButtons->addWidget(m_editTimerButton, 2, 0);
	timerButtons->addWidget(m_findTimerButton, 2, 1);
	timerButtons->addWidget(m_loadTimersButton, 2, 2);
	timerButtons->addWidget(m_resetTimersButton, 2, 3, 1, 2);
	timerButtons->addWidget(m_copyTimerButton, 2, 5);
	timerButtons->addWidget(m_deleteTimerButton, 3, 0);
	timerButtons->addWidget(m_findNextTimerButton, 3, 1);
	timerButtons->addWidget(m_saveTimersButton, 3, 2);
	timerButtons->addWidget(m_pasteTimerButton, 3, 5);
	timerButtons->setVerticalSpacing(4);
	timerButtons->setColumnStretch(6, 1);
	timersLayout->addLayout(timerButtons);
	connect(m_editTriggersFilter, &QPushButton::clicked, this,
	        [this]
	        {
		        if (!editFilterDialog(this, m_triggerFilterText, QStringLiteral("Edit trigger filter")))
			        return;
		        if (m_filterTriggers)
			        m_filterTriggers->setChecked(true);
		        populateTriggers();
		        updateTriggerControls();
	        });
	connect(m_filterTriggers, &QCheckBox::toggled, this,
	        [this]
	        {
		        populateTriggers();
		        updateTriggerControls();
	        });
	connect(m_editAliasesFilter, &QPushButton::clicked, this,
	        [this]
	        {
		        if (!editFilterDialog(this, m_aliasFilterText, QStringLiteral("Edit alias filter")))
			        return;
		        if (m_filterAliases)
			        m_filterAliases->setChecked(true);
		        populateAliases();
		        updateAliasControls();
	        });
	connect(m_filterAliases, &QCheckBox::toggled, this,
	        [this]
	        {
		        populateAliases();
		        updateAliasControls();
	        });
	connect(m_editTimersFilter, &QPushButton::clicked, this,
	        [this]
	        {
		        if (!editFilterDialog(this, m_timerFilterText, QStringLiteral("Edit timer filter")))
			        return;
		        if (m_filterTimers)
			        m_filterTimers->setChecked(true);
		        populateTimers();
		        updateTimerControls();
	        });
	connect(m_filterTimers, &QCheckBox::toggled, this,
	        [this]
	        {
		        populateTimers();
		        updateTimerControls();
	        });

	auto *varsLayout = new QVBoxLayout(variablesPage);
	m_variablesTable = makeTable(variablesPage, {QStringLiteral("Name"), QStringLiteral("Value")});
	varsLayout->addWidget(m_variablesTable);
	auto *variableOptions = new QHBoxLayout();
	m_variablesCount      = new QLabel(QStringLiteral("0 items."), variablesPage);
	m_filterVariables     = new QCheckBox(QStringLiteral("F&ilter by:"), variablesPage);
	m_editVariablesFilter = new QPushButton(QStringLiteral("..."), variablesPage);
	m_editVariablesFilter->setFixedWidth(24);
	variableOptions->addWidget(m_variablesCount);
	variableOptions->addStretch();
	varsLayout->addLayout(variableOptions);
	auto *variableButtons    = new QGridLayout();
	m_addVariableButton      = new QPushButton(QStringLiteral("&Add..."), variablesPage);
	m_editVariableButton     = new QPushButton(QStringLiteral("&Edit..."), variablesPage);
	m_deleteVariableButton   = new QPushButton(QStringLiteral("&Remove"), variablesPage);
	m_findVariableButton     = new QPushButton(QStringLiteral("&Find..."), variablesPage);
	m_findNextVariableButton = new QPushButton(QStringLiteral("Find &Next"), variablesPage);
	m_loadVariablesButton    = new QPushButton(QStringLiteral("&Load..."), variablesPage);
	m_saveVariablesButton    = new QPushButton(QStringLiteral("&Save..."), variablesPage);
	m_copyVariableButton     = new QPushButton(QStringLiteral("&Copy"), variablesPage);
	m_pasteVariableButton    = new QPushButton(QStringLiteral("&Paste"), variablesPage);
	variableButtons->addWidget(m_addVariableButton, 0, 0);
	variableButtons->addWidget(m_editVariableButton, 1, 0);
	variableButtons->addWidget(m_deleteVariableButton, 2, 0);
	variableButtons->addWidget(m_findVariableButton, 1, 1);
	variableButtons->addWidget(m_findNextVariableButton, 2, 1);
	variableButtons->addWidget(m_loadVariablesButton, 1, 2);
	variableButtons->addWidget(m_saveVariablesButton, 2, 2);
	auto *variableFilterLayout = new QHBoxLayout();
	variableFilterLayout->setContentsMargins(0, 0, 0, 0);
	variableFilterLayout->addWidget(m_filterVariables);
	variableFilterLayout->addStretch();
	variableFilterLayout->addWidget(m_editVariablesFilter);
	variableButtons->addLayout(variableFilterLayout, 0, 3);
	variableButtons->addWidget(m_copyVariableButton, 1, 3, Qt::AlignRight);
	variableButtons->addWidget(m_pasteVariableButton, 2, 3, Qt::AlignRight);
	variableButtons->setVerticalSpacing(4);
	variableButtons->setColumnStretch(4, 1);
	varsLayout->addLayout(variableButtons);

	// Keypad
	auto *keypadLayout  = new QVBoxLayout(keypadPage);
	auto *keypadContent = new QWidget(keypadPage);
	auto *keypadBlock   = new QGridLayout(keypadContent);
	keypadBlock->setContentsMargins(0, 0, 0, 0);
	keypadBlock->setColumnStretch(1, 1);
	auto addKeypadField = [this](QWidget *parent, QGridLayout *grid, const QString &key, const int row,
	                             const int col, const int colSpan = 1)
	{
		auto *label = new QLabel(key, parent);
		label->setAlignment(Qt::AlignHCenter);
		auto *edit = new QLineEdit(parent);
		m_keypadFields.insert(key, edit);
		grid->addWidget(label, row, col, 1, colSpan, Qt::AlignHCenter);
		grid->addWidget(edit, row + 1, col, 1, colSpan);
	};
	addKeypadField(keypadContent, keypadBlock, QStringLiteral("/"), 0, 0);
	addKeypadField(keypadContent, keypadBlock, QStringLiteral("*"), 0, 2);
	addKeypadField(keypadContent, keypadBlock, QStringLiteral("-"), 0, 3);
	auto *keypadGroup =
	    new QGroupBox(QStringLiteral("\"NumLock\" must be active for these keys to work"), keypadContent);
	auto *keypadGrid = new QGridLayout(keypadGroup);
	addKeypadField(keypadGroup, keypadGrid, QStringLiteral("7"), 0, 0);
	addKeypadField(keypadGroup, keypadGrid, QStringLiteral("8"), 0, 1);
	addKeypadField(keypadGroup, keypadGrid, QStringLiteral("9"), 0, 2);
	addKeypadField(keypadGroup, keypadGrid, QStringLiteral("4"), 2, 0);
	addKeypadField(keypadGroup, keypadGrid, QStringLiteral("5"), 2, 1);
	addKeypadField(keypadGroup, keypadGrid, QStringLiteral("6"), 2, 2);
	addKeypadField(keypadGroup, keypadGrid, QStringLiteral("1"), 4, 0);
	addKeypadField(keypadGroup, keypadGrid, QStringLiteral("2"), 4, 1);
	addKeypadField(keypadGroup, keypadGrid, QStringLiteral("3"), 4, 2);
	addKeypadField(keypadGroup, keypadGrid, QStringLiteral("0"), 6, 0, 2);
	addKeypadField(keypadGroup, keypadGrid, QStringLiteral("."), 6, 2);
	auto *plusWidget = new QWidget(keypadContent);
	auto *plusGrid   = new QGridLayout(plusWidget);
	plusGrid->setContentsMargins(0, 0, 0, 0);
	addKeypadField(plusWidget, plusGrid, QStringLiteral("+"), 0, 0);
	QStyleOptionGroupBox groupOption;
	groupOption.initFrom(keypadGroup);
	groupOption.text = keypadGroup->title();
	groupOption.subControls =
	    QStyle::SC_GroupBoxFrame | QStyle::SC_GroupBoxContents | QStyle::SC_GroupBoxLabel;
	groupOption.rect = QRect(0, 0, keypadGroup->sizeHint().width(), keypadGroup->sizeHint().height());
	const QRect groupContents = keypadGroup->style()->subControlRect(
	    QStyle::CC_GroupBox, &groupOption, QStyle::SC_GroupBoxContents, keypadGroup);
	const int keypadTopInset = keypadGrid->contentsMargins().top();
	plusGrid->setContentsMargins(0, groupContents.top() + keypadTopInset, 0, 0);
	keypadBlock->addWidget(keypadGroup, 2, 0, 1, 3);
	keypadBlock->addWidget(plusWidget, 2, 3, Qt::AlignTop);
	m_keypadEnabled     = new QCheckBox(QStringLiteral("Enable Keypad Keys"), keypadContent);
	m_keypadControl     = new QCheckBox(QStringLiteral("Show Contents If CTRL Held Down"), keypadContent);
	auto *keypadOptions = new QVBoxLayout();
	keypadOptions->setContentsMargins(0, 0, 0, 0);
	keypadOptions->addWidget(m_keypadEnabled);
	keypadOptions->addWidget(m_keypadControl);
	keypadBlock->addLayout(keypadOptions, 3, 0, 1, 3, Qt::AlignLeft);
	keypadLayout->addWidget(keypadContent, 0, Qt::AlignHCenter);
	keypadLayout->addStretch();
	connect(m_keypadControl, &QCheckBox::toggled, this,
	        [this]
	        {
		        storeKeypadFields(!m_keypadControl->isChecked());
		        loadKeypadFields(m_keypadControl->isChecked());
	        });

	// Paste
	auto *pasteLayout    = new QVBoxLayout(pastePage);
	m_pastePreamble      = new QTextEdit(pastePage);
	m_pastePostamble     = new QTextEdit(pastePage);
	m_pasteLinePreamble  = new QLineEdit(pastePage);
	m_pasteLinePostamble = new QLineEdit(pastePage);
	pasteLayout->addWidget(
	    new QLabel(QStringLiteral("1. Send this at the start of a \"paste to\""), pastePage));
	pasteLayout->addWidget(m_pastePreamble);
	pasteLayout->addWidget(new QLabel(QStringLiteral("2. Send this at the start of each line:"), pastePage));
	pasteLayout->addWidget(m_pasteLinePreamble);
	pasteLayout->addWidget(new QLabel(QStringLiteral("3. Send this at the end of each line:"), pastePage));
	pasteLayout->addWidget(m_pasteLinePostamble);
	pasteLayout->addWidget(
	    new QLabel(QStringLiteral("4. Send this at the end of a \"paste to\""), pastePage));
	pasteLayout->addWidget(m_pastePostamble);
	auto *pasteDelayLayout = new QHBoxLayout();
	m_pasteLineDelay       = new QSpinBox(pastePage);
	m_pasteLineDelay->setRange(0, 10000);
	m_pasteDelayPerLines = new QSpinBox(pastePage);
	m_pasteDelayPerLines->setRange(1, 100000);
	pasteDelayLayout->addWidget(new QLabel(QStringLiteral("Delay between"), pastePage));
	pasteDelayLayout->addWidget(m_pasteLineDelay);
	pasteDelayLayout->addWidget(new QLabel(QStringLiteral("milliseconds. Every"), pastePage));
	pasteDelayLayout->addWidget(m_pasteDelayPerLines);
	pasteDelayLayout->addWidget(new QLabel(QStringLiteral("lines."), pastePage));
	pasteDelayLayout->addStretch();
	pasteLayout->addLayout(pasteDelayLayout);
	m_commentedSoftcodePaste = new QCheckBox(QStringLiteral("Commented Softcode"), pastePage);
	m_pasteEcho              = new QCheckBox(QStringLiteral("Echo to output window"), pastePage);
	m_confirmOnPaste         = new QCheckBox(QStringLiteral("Confirm on each \"paste to world\""), pastePage);
	pasteLayout->addWidget(m_commentedSoftcodePaste);
	pasteLayout->addWidget(m_pasteEcho);
	pasteLayout->addWidget(m_confirmOnPaste);
	pasteLayout->addStretch();

	// Info
	QFont infoFont = infoPage->font();
	if (infoFont.pointSize() > 0)
		infoFont.setPointSize(qMax(8, infoFont.pointSize() - 2));
	infoPage->setFont(infoFont);
	auto *infoLayout           = new QVBoxLayout(infoPage);
	m_infoWorldFile            = new QLabel(infoPage);
	m_infoWorldFileVersion     = new QLabel(infoPage);
	m_infoQmudVersion          = new QLabel(infoPage);
	m_infoWorldId              = new QLineEdit(infoPage);
	m_infoDateSaved            = new QLabel(infoPage);
	m_infoBufferLines          = new QLabel(infoPage);
	m_infoConnectionDuration   = new QLabel(infoPage);
	m_infoConnectionTime       = new QLabel(infoPage);
	m_infoAliases              = new QLabel(infoPage);
	m_infoTriggers             = new QLabel(infoPage);
	m_infoTimers               = new QLabel(infoPage);
	m_infoCompressionRatio     = new QLabel(infoPage);
	m_infoBytesSent            = new QLabel(infoPage);
	m_infoBytesReceived        = new QLabel(infoPage);
	m_infoTriggerTimeTaken     = new QLabel(infoPage);
	m_infoIpAddress            = new QLabel(infoPage);
	m_infoMxpBuiltinElements   = new QLabel(infoPage);
	m_infoMxpBuiltinEntities   = new QLabel(infoPage);
	m_infoMxpEntitiesReceived  = new QLabel(infoPage);
	m_infoMxpErrors            = new QLabel(infoPage);
	m_infoMxpMudElements       = new QLabel(infoPage);
	m_infoMxpMudEntities       = new QLabel(infoPage);
	m_infoMxpTagsReceived      = new QLabel(infoPage);
	m_infoMxpUnclosedTags      = new QLabel(infoPage);
	m_infoCompressedIn         = new QLabel(infoPage);
	m_infoCompressedOut        = new QLabel(infoPage);
	m_infoTimeTakenCompressing = new QLabel(infoPage);
	m_infoMxpActionsCached     = new QLabel(infoPage);
	m_infoMxpReferenceCount    = new QLabel(infoPage);
	m_infoMemoryUsed           = new QLabel(infoPage);
	m_infoCalculateMemory      = new QPushButton(QStringLiteral("Calculate"), infoPage);
	m_infoWorldFile->setVisible(false);
	m_infoWorldFileVersion->setVisible(false);
	m_infoQmudVersion->setVisible(false);
	m_infoDateSaved->setVisible(false);
	m_infoWorldId->setReadOnly(true);
	m_infoWorldId->setFrame(false);

	auto makeLeftLabel = [infoPage](const QString &text) -> QLabel *
	{
		auto *label = new QLabel(text, infoPage);
		label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
		return label;
	};

	auto *ipLayout = new QHBoxLayout();
	auto *ipLabel  = new QLabel(QStringLiteral("World IP Address:"), infoPage);
	ipLabel->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
	m_infoIpAddress->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
	ipLayout->addStretch();
	ipLayout->addWidget(ipLabel);
	ipLayout->addSpacing(8);
	ipLayout->addWidget(m_infoIpAddress);
	ipLayout->addStretch();
	infoLayout->addLayout(ipLayout);

	auto *memoryBox    = new QGroupBox(QStringLiteral("Memory usage"), infoPage);
	auto *memoryLayout = new QGridLayout(memoryBox);
	memoryLayout->setColumnStretch(0, 1);
	memoryLayout->setColumnStretch(1, 0);
	memoryLayout->setColumnStretch(2, 1);
	memoryLayout->setColumnStretch(3, 0);
	memoryLayout->setColumnStretch(4, 1);
	memoryLayout->addWidget(makeLeftLabel(QStringLiteral("Lines in output")), 0, 1);
	memoryLayout->addWidget(m_infoBufferLines, 0, 2, Qt::AlignRight);
	memoryLayout->addWidget(new QWidget(memoryBox), 0, 3);
	memoryLayout->addWidget(makeLeftLabel(QStringLiteral("Memory used by output buffer")), 1, 1);
	memoryLayout->addWidget(m_infoMemoryUsed, 1, 2, Qt::AlignRight);
	memoryLayout->addWidget(m_infoCalculateMemory, 1, 3, Qt::AlignLeft);
	infoLayout->addWidget(memoryBox);
	connect(m_infoCalculateMemory, &QPushButton::clicked, this, [this] { calculateMemoryUsage(true); });

	auto *timeBox    = new QGroupBox(QStringLiteral("Time"), infoPage);
	auto *timeLayout = new QGridLayout(timeBox);
	timeLayout->setColumnStretch(0, 1);
	timeLayout->setColumnStretch(1, 0);
	timeLayout->setColumnStretch(2, 1);
	timeLayout->setColumnStretch(3, 1);
	timeLayout->addWidget(makeLeftLabel(QStringLiteral("Time connection")), 0, 1);
	timeLayout->addWidget(m_infoConnectionTime, 0, 2, Qt::AlignRight);
	timeLayout->addWidget(makeLeftLabel(QStringLiteral("Connection")), 1, 1);
	timeLayout->addWidget(m_infoConnectionDuration, 1, 2, Qt::AlignRight);
	timeLayout->addWidget(makeLeftLabel(QStringLiteral("World ID")), 2, 1);
	timeLayout->addWidget(m_infoWorldId, 2, 2, Qt::AlignRight);
	infoLayout->addWidget(timeBox);

	auto *countsBox     = new QGroupBox(QStringLiteral("Counts"), infoPage);
	auto *countsLayout  = new QGridLayout(countsBox);
	auto *triggersLabel = new QLabel(QStringLiteral("Triggers"), infoPage);
	triggersLabel->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
	triggersLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	countsLayout->addWidget(triggersLabel, 0, 0);
	countsLayout->addWidget(m_infoTriggers, 0, 1);
	auto *triggerTimeLabel = new QLabel(QStringLiteral("Trigger time taken"), infoPage);
	triggerTimeLabel->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
	triggerTimeLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	countsLayout->addWidget(triggerTimeLabel, 0, 2);
	countsLayout->addWidget(m_infoTriggerTimeTaken, 0, 3);
	auto *aliasesLabel = new QLabel(QStringLiteral("Aliases"), infoPage);
	aliasesLabel->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
	aliasesLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	countsLayout->addWidget(aliasesLabel, 1, 0);
	countsLayout->addWidget(m_infoAliases, 1, 1);
	auto *timersLabel = new QLabel(QStringLiteral("Timers"), infoPage);
	timersLabel->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
	timersLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	countsLayout->addWidget(timersLabel, 2, 0);
	countsLayout->addWidget(m_infoTimers, 2, 1);
	infoLayout->addWidget(countsBox);

	auto *compressionBox    = new QGroupBox(QStringLiteral("Compression (MCCP)"), infoPage);
	auto *compressionLayout = new QGridLayout(compressionBox);
	auto *compressionLabel  = new QLabel(QStringLiteral("Compression"), infoPage);
	compressionLabel->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
	compressionLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	compressionLayout->addWidget(compressionLabel, 0, 0);
	compressionLayout->addWidget(m_infoCompressionRatio, 0, 1);
	auto *compressedLabel = new QLabel(QStringLiteral("Compressed"), infoPage);
	compressedLabel->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
	compressedLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	compressionLayout->addWidget(compressedLabel, 1, 0);
	compressionLayout->addWidget(m_infoCompressedIn, 1, 1);
	auto *outLabel = new QLabel(QStringLiteral("out:"), infoPage);
	outLabel->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
	outLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	compressionLayout->addWidget(outLabel, 1, 2);
	compressionLayout->addWidget(m_infoCompressedOut, 1, 3);
	auto *timeTakenLabel = new QLabel(QStringLiteral("Time taken"), infoPage);
	timeTakenLabel->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
	timeTakenLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	compressionLayout->addWidget(timeTakenLabel, 2, 0);
	compressionLayout->addWidget(m_infoTimeTakenCompressing, 2, 1);
	infoLayout->addWidget(compressionBox);

	auto *ioBox    = new QGroupBox(QStringLiteral("Input/Output"), infoPage);
	auto *ioLayout = new QGridLayout(ioBox);
	ioLayout->setColumnStretch(0, 1);
	ioLayout->setColumnStretch(1, 0);
	ioLayout->setColumnStretch(2, 1);
	ioLayout->setColumnStretch(3, 1);
	ioLayout->addWidget(makeLeftLabel(QStringLiteral("Received")), 0, 1);
	ioLayout->addWidget(m_infoBytesReceived, 0, 2, Qt::AlignRight);
	ioLayout->addWidget(makeLeftLabel(QStringLiteral("Sent")), 1, 1);
	ioLayout->addWidget(m_infoBytesSent, 1, 2, Qt::AlignRight);
	infoLayout->addWidget(ioBox);

	auto *mxpBox           = new QGroupBox(QStringLiteral("MXP statistics"), infoPage);
	auto *mxpStatsLayout   = new QGridLayout(mxpBox);
	auto *mxpBuiltElements = new QLabel(QStringLiteral("Built-in elements"), infoPage);
	mxpBuiltElements->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
	mxpBuiltElements->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	mxpStatsLayout->addWidget(mxpBuiltElements, 0, 0);
	mxpStatsLayout->addWidget(m_infoMxpBuiltinElements, 0, 1);
	auto *mxpBuiltEntities = new QLabel(QStringLiteral("Built-in entities"), infoPage);
	mxpBuiltEntities->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
	mxpBuiltEntities->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	mxpStatsLayout->addWidget(mxpBuiltEntities, 1, 0);
	mxpStatsLayout->addWidget(m_infoMxpBuiltinEntities, 1, 1);
	auto *mxpMudElements = new QLabel(QStringLiteral("MUD-defined elements"), infoPage);
	mxpMudElements->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
	mxpMudElements->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	mxpStatsLayout->addWidget(mxpMudElements, 2, 0);
	mxpStatsLayout->addWidget(m_infoMxpMudElements, 2, 1);
	auto *mxpMudEntities = new QLabel(QStringLiteral("MUD-defined entities"), infoPage);
	mxpMudEntities->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
	mxpMudEntities->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	mxpStatsLayout->addWidget(mxpMudEntities, 3, 0);
	mxpStatsLayout->addWidget(m_infoMxpMudEntities, 3, 1);
	auto *mxpUnclosed = new QLabel(QStringLiteral("Unclosed"), infoPage);
	mxpUnclosed->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
	mxpUnclosed->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	mxpStatsLayout->addWidget(mxpUnclosed, 4, 0);
	mxpStatsLayout->addWidget(m_infoMxpUnclosedTags, 4, 1);
	auto *mxpErrors = new QLabel(QStringLiteral("Errors"), infoPage);
	mxpErrors->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
	mxpErrors->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	mxpStatsLayout->addWidget(mxpErrors, 0, 2);
	mxpStatsLayout->addWidget(m_infoMxpErrors, 0, 3);
	auto *mxpTags = new QLabel(QStringLiteral("Tags"), infoPage);
	mxpTags->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
	mxpTags->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	mxpStatsLayout->addWidget(mxpTags, 1, 2);
	mxpStatsLayout->addWidget(m_infoMxpTagsReceived, 1, 3);
	auto *mxpEntities = new QLabel(QStringLiteral("Entities"), infoPage);
	mxpEntities->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
	mxpEntities->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	mxpStatsLayout->addWidget(mxpEntities, 2, 2);
	mxpStatsLayout->addWidget(m_infoMxpEntitiesReceived, 2, 3);
	auto *mxpActions = new QLabel(QStringLiteral("Actions"), infoPage);
	mxpActions->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
	mxpActions->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	mxpStatsLayout->addWidget(mxpActions, 3, 2);
	mxpStatsLayout->addWidget(m_infoMxpActionsCached, 3, 3);
	auto *mxpReference = new QLabel(QStringLiteral("Reference"), infoPage);
	mxpReference->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
	mxpReference->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	mxpStatsLayout->addWidget(mxpReference, 4, 2);
	mxpStatsLayout->addWidget(m_infoMxpReferenceCount, 4, 3);
	infoLayout->addWidget(mxpBox);
	const QList<QLabel *> infoLabels = {m_infoIpAddress,
	                                    m_infoBufferLines,
	                                    m_infoMemoryUsed,
	                                    m_infoConnectionTime,
	                                    m_infoConnectionDuration,
	                                    m_infoTriggers,
	                                    m_infoAliases,
	                                    m_infoTimers,
	                                    m_infoTriggerTimeTaken,
	                                    m_infoCompressionRatio,
	                                    m_infoCompressedIn,
	                                    m_infoCompressedOut,
	                                    m_infoTimeTakenCompressing,
	                                    m_infoBytesReceived,
	                                    m_infoBytesSent,
	                                    m_infoMxpBuiltinElements,
	                                    m_infoMxpBuiltinEntities,
	                                    m_infoMxpMudElements,
	                                    m_infoMxpMudEntities,
	                                    m_infoMxpTagsReceived,
	                                    m_infoMxpEntitiesReceived,
	                                    m_infoMxpErrors,
	                                    m_infoMxpUnclosedTags,
	                                    m_infoMxpActionsCached,
	                                    m_infoMxpReferenceCount};
	for (QLabel *label : infoLabels)
	{
		if (!label)
			continue;
		label->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
		label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
		label->setMinimumHeight(18);
	}
	infoLayout->addStretch();

	// Auto-say
	auto *autoSayLayout = new QVBoxLayout(autoSayPage);
	m_enableAutoSay     = new QCheckBox(QStringLiteral("Enable Auto Say"), autoSayPage);
	autoSayLayout->addWidget(m_enableAutoSay, 0, Qt::AlignHCenter);
	auto *autoSayGroup            = new QGroupBox(QStringLiteral("Auto say configuration"), autoSayPage);
	auto *autoSayGrid             = new QGridLayout(autoSayGroup);
	auto *autoSayExclusions       = new QGroupBox(QStringLiteral("Exclusions"), autoSayGroup);
	auto *autoSayExclusionsLayout = new QVBoxLayout(autoSayExclusions);
	m_autoSayExcludeNonAlpha =
	    new QCheckBox(QStringLiteral("Exclude commands starting with special characters"), autoSayExclusions);
	m_autoSayExcludeMacros = new QCheckBox(QStringLiteral("Exclude macros"), autoSayExclusions);
	autoSayExclusionsLayout->addWidget(m_autoSayExcludeNonAlpha);
	autoSayExclusionsLayout->addWidget(m_autoSayExcludeMacros);
	autoSayGrid->addWidget(autoSayExclusions, 0, 0, 1, 2);
	auto *autoSayOverrideLabel = new QLabel(QStringLiteral("Override"), autoSayGroup);
	m_autoSayOverridePrefix    = new QLineEdit(autoSayGroup);
	m_autoSayOverridePrefix->setFixedWidth(60);
	autoSayGrid->addWidget(autoSayOverrideLabel, 1, 0, Qt::AlignLeft);
	autoSayGrid->addWidget(m_autoSayOverridePrefix, 1, 1, Qt::AlignLeft);
	auto *autoSayLabel = new QLabel(QStringLiteral("Auto Say"), autoSayGroup);
	m_autoSayString    = new QLineEdit(autoSayGroup);
	m_autoSayString->setFixedWidth(200);
	autoSayGrid->addWidget(autoSayLabel, 2, 0, Qt::AlignLeft);
	autoSayGrid->addWidget(m_autoSayString, 2, 1, Qt::AlignLeft);
	autoSayLayout->addWidget(autoSayGroup, 0, Qt::AlignHCenter);
	m_reEvaluateAutoSay = new QCheckBox(
	    QStringLiteral("Send auto-say response to command interpreter (for aliases etc)"), autoSayPage);
	auto *autoSayFooter       = new QWidget(autoSayPage);
	auto *autoSayFooterLayout = new QHBoxLayout(autoSayFooter);
	autoSayFooterLayout->setContentsMargins(0, 0, 0, 0);
	autoSayFooterLayout->addWidget(m_reEvaluateAutoSay);
	autoSayFooterLayout->addStretch();
	QStyleOptionGroupBox autoSayOption;
	autoSayOption.initFrom(autoSayGroup);
	autoSayOption.text = autoSayGroup->title();
	autoSayOption.subControls =
	    QStyle::SC_GroupBoxFrame | QStyle::SC_GroupBoxContents | QStyle::SC_GroupBoxLabel;
	autoSayOption.rect = QRect(0, 0, autoSayGroup->sizeHint().width(), autoSayGroup->sizeHint().height());
	const QRect autoSayContents = autoSayGroup->style()->subControlRect(
	    QStyle::CC_GroupBox, &autoSayOption, QStyle::SC_GroupBoxContents, autoSayGroup);
	autoSayFooterLayout->setContentsMargins(autoSayContents.left(), 0, 0, 0);
	const int autoSayMinimumWidth = m_reEvaluateAutoSay->sizeHint().width() + autoSayContents.left();
	const int autoSayLayoutWidth  = qMax(autoSayGroup->sizeHint().width(), autoSayMinimumWidth);
	autoSayGroup->setMinimumWidth(autoSayLayoutWidth);
	autoSayFooter->setFixedWidth(autoSayLayoutWidth);
	autoSayLayout->addWidget(autoSayFooter, 0, Qt::AlignHCenter);
	autoSayLayout->addStretch();

	// Printing
	auto *printingLayout     = new QVBoxLayout(printingPage);
	auto  buildPrintingGroup = [](const QString &title, QVector<QCheckBox *> &boldChecks,
                                 QVector<QCheckBox *> &italicChecks, QVector<QCheckBox *> &underlineChecks,
                                 QWidget *parent) -> QGroupBox *
	{
		static const QStringList colours = {QStringLiteral("Black"), QStringLiteral("Red"),
		                                    QStringLiteral("Green"), QStringLiteral("Yellow"),
		                                    QStringLiteral("Blue"),  QStringLiteral("Magenta"),
		                                    QStringLiteral("Cyan"),  QStringLiteral("White")};
		auto                    *group   = new QGroupBox(title, parent);
		auto                    *grid    = new QGridLayout(group);
		grid->addWidget(new QLabel(QStringLiteral(""), group), 0, 0);
		grid->addWidget(new QLabel(QStringLiteral("Bold"), group), 0, 1);
		grid->addWidget(new QLabel(QStringLiteral("Italic"), group), 0, 2);
		grid->addWidget(new QLabel(QStringLiteral("Underline"), group), 0, 3);
		boldChecks.resize(colours.size());
		italicChecks.resize(colours.size());
		underlineChecks.resize(colours.size());
		for (int i = 0; i < colours.size(); ++i)
		{
			auto *label        = new QLabel(colours.at(i), group);
			auto *bold         = new QCheckBox(group);
			auto *italic       = new QCheckBox(group);
			auto *underline    = new QCheckBox(group);
			boldChecks[i]      = bold;
			italicChecks[i]    = italic;
			underlineChecks[i] = underline;
			grid->addWidget(label, i + 1, 0);
			grid->addWidget(bold, i + 1, 1, Qt::AlignHCenter);
			grid->addWidget(italic, i + 1, 2, Qt::AlignHCenter);
			grid->addWidget(underline, i + 1, 3, Qt::AlignHCenter);
		}
		return group;
	};
	QGroupBox *normalPrintGroup =
	    buildPrintingGroup(QStringLiteral("Normal"), m_printingNormalBold, m_printingNormalItalic,
	                       m_printingNormalUnderline, printingPage);
	QGroupBox *boldPrintGroup =
	    buildPrintingGroup(QStringLiteral("Bold"), m_printingBoldBold, m_printingBoldItalic,
	                       m_printingBoldUnderline, printingPage);
	printingLayout->addWidget(normalPrintGroup, 0, Qt::AlignHCenter);
	printingLayout->addWidget(boldPrintGroup, 0, Qt::AlignHCenter);
	printingLayout->addStretch();

	// Connecting
	auto *connectingLayout = new QVBoxLayout(connectingPage);
	auto *connectGroup     = new QGroupBox(QStringLiteral("Character name and password"), connectingPage);
	auto *connectForm      = new QFormLayout(connectGroup);
	m_playerName           = new QLineEdit(connectGroup);
	m_password             = new QLineEdit(connectGroup);
	m_password->setEchoMode(QLineEdit::Password);
	m_connectMethod = new QComboBox(connectGroup);
	m_connectMethod->addItem(QStringLiteral("No auto-connect"), eNoAutoConnect);
	m_connectMethod->addItem(QStringLiteral("QMud"), eConnectMUSH);
	m_connectMethod->addItem(QStringLiteral("Diku"), eConnectDiku);
	m_connectMethod->addItem(QStringLiteral("MXP"), eConnectMXP);
	connectForm->addRow(QStringLiteral("Name"), m_playerName);
	connectForm->addRow(QStringLiteral("Password"), m_password);
	connectForm->addRow(QStringLiteral("Connect"), m_connectMethod);
	connectingLayout->addWidget(connectGroup);

	auto *connectTextGroup  = new QGroupBox(QStringLiteral("Connect Text"), connectingPage);
	auto *connectTextLayout = new QVBoxLayout(connectTextGroup);
	m_connectText           = new QTextEdit(connectTextGroup);
	m_connectLineCount      = new QLabel(formatLineCountLabel(0), connectTextGroup);
	m_connectLineCount->setAlignment(Qt::AlignRight);
	connectTextLayout->addWidget(m_connectText);
	connectTextLayout->addWidget(m_connectLineCount);
	connect(m_connectText, &QTextEdit::textChanged, this,
	        [this]
	        {
		        const QString text  = m_connectText->toPlainText();
		        int           lines = 0;
		        if (!text.isEmpty())
			        lines = saturatingToInt(text.count(QLatin1Char('\n')) + 1);
		        m_connectLineCount->setText(formatLineCountLabel(lines));
	        });
	auto *connectNote =
	    new QLabel(QStringLiteral("You can use \"%name%\" or \"%password%\" if you wish the name "
	                              "or password supplied above to be inserted."),
	               connectTextGroup);
	connectNote->setWordWrap(true);
	connectTextLayout->addWidget(connectNote);
	connectingLayout->addWidget(connectTextGroup);
	m_onlyNegotiateTelnetOptionsOnce =
	    new QCheckBox(QStringLiteral("Only negotiate telnet options once"), connectingPage);
	connectingLayout->addWidget(m_onlyNegotiateTelnetOptionsOnce);
	connectingLayout->addStretch();

	// MXP
	auto *mxpLayout = new QVBoxLayout(mxpPage);
	auto *mxpGroup  = new QGroupBox(QStringLiteral("MUD Extension"), mxpPage);
	auto *mxpForm   = new QGridLayout(mxpGroup);
	m_mxpActive     = new QLabel(QStringLiteral("MXP inactive"), mxpPage);
	m_useMxp        = new QComboBox(mxpPage);
	m_useMxp->addItem(QStringLiteral("On command"), QStringLiteral("0"));
	m_useMxp->addItem(QStringLiteral("On request"), QStringLiteral("1"));
	m_useMxp->addItem(QStringLiteral("Always on"), QStringLiteral("2"));
	m_useMxp->addItem(QStringLiteral("Never"), QStringLiteral("3"));
	m_detectPueblo  = new QCheckBox(QStringLiteral("Detect Pueblo initiation string"), mxpPage);
	m_mxpDebugLevel = new QComboBox(mxpPage);
	m_mxpDebugLevel->addItem(QStringLiteral("None"), QStringLiteral("0"));
	m_mxpDebugLevel->addItem(QStringLiteral("Errors"), QStringLiteral("1"));
	m_mxpDebugLevel->addItem(QStringLiteral("Warnings"), QStringLiteral("2"));
	m_mxpDebugLevel->addItem(QStringLiteral("Info"), QStringLiteral("3"));
	m_mxpDebugLevel->addItem(QStringLiteral("All"), QStringLiteral("4"));
	mxpForm->addWidget(new QLabel(QStringLiteral("MXP/Pueblo"), mxpPage), 0, 0);
	mxpForm->addWidget(m_mxpActive, 0, 1);
	mxpForm->addWidget(new QLabel(QStringLiteral("Use MXP/Pueblo"), mxpPage), 1, 0);
	mxpForm->addWidget(m_useMxp, 1, 1);
	mxpForm->addWidget(m_detectPueblo, 2, 1);
	mxpForm->addWidget(new QLabel(QStringLiteral("MXP debug"), mxpPage), 3, 0);
	mxpForm->addWidget(m_mxpDebugLevel, 3, 1);
	mxpGroup->setLayout(mxpForm);
	mxpLayout->addWidget(mxpGroup);

	auto *hyperlinkBox    = new QGroupBox(QStringLiteral("Hyperlinks"), mxpPage);
	auto *hyperlinkLayout = new QFormLayout(hyperlinkBox);
	hyperlinkLayout->setFormAlignment(Qt::AlignHCenter | Qt::AlignTop);
	hyperlinkLayout->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
	hyperlinkLayout->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
	m_hyperlinkColour = new QLineEdit(hyperlinkBox);
	m_hyperlinkColour->setReadOnly(true);
	m_hyperlinkColour->setFixedWidth(36);
	m_hyperlinkColour->installEventFilter(this);
	m_hyperlinkColour->setCursor(Qt::PointingHandCursor);
	m_useCustomLinkColour    = new QCheckBox(QStringLiteral("Use custom link colour"), hyperlinkBox);
	m_mudCanChangeLinkColour = new QCheckBox(QStringLiteral("MUD can change link colour"), hyperlinkBox);
	m_underlineHyperlinks    = new QCheckBox(QStringLiteral("Underline hyperlinks"), hyperlinkBox);
	m_mudCanRemoveUnderline  = new QCheckBox(QStringLiteral("MUD can remove underline"), hyperlinkBox);
	m_hyperlinkAddsToCommandHistory =
	    new QCheckBox(QStringLiteral("Save hyperlink mouse clicks in history"), hyperlinkBox);
	m_echoHyperlinkInOutput =
	    new QCheckBox(QStringLiteral("Echo hyperlink mouse clicks in output"), hyperlinkBox);
	auto *customLinkRow    = new QWidget(hyperlinkBox);
	auto *customLinkLayout = new QHBoxLayout(customLinkRow);
	customLinkLayout->setContentsMargins(0, 0, 0, 0);
	customLinkLayout->addWidget(new QLabel(QStringLiteral("Custom link"), customLinkRow));
	customLinkLayout->addWidget(m_hyperlinkColour);
	customLinkLayout->addStretch(1);
	hyperlinkLayout->addRow(QString(), customLinkRow);
	hyperlinkLayout->addRow(QString(), m_useCustomLinkColour);
	hyperlinkLayout->addRow(QString(), m_mudCanChangeLinkColour);
	hyperlinkLayout->addRow(QString(), m_underlineHyperlinks);
	hyperlinkLayout->addRow(QString(), m_mudCanRemoveUnderline);
	hyperlinkLayout->addRow(QString(), m_hyperlinkAddsToCommandHistory);
	hyperlinkLayout->addRow(QString(), m_echoHyperlinkInOutput);
	mxpLayout->addWidget(hyperlinkBox);

	m_ignoreMxpColourChanges = new QCheckBox(QStringLiteral("Ignore colour changes"), mxpPage);
	m_sendMxpAfkResponse     = new QCheckBox(QStringLiteral("Send AFK response"), mxpPage);
	m_mudCanChangeOptions    = new QCheckBox(QStringLiteral("MUD can change some options"), mxpPage);
	mxpLayout->addWidget(m_ignoreMxpColourChanges);
	mxpLayout->addWidget(m_sendMxpAfkResponse);
	mxpLayout->addWidget(m_mudCanChangeOptions);
	m_resetMxpTagsButton = new QPushButton(QStringLiteral("Reset tags"), mxpPage);
	mxpLayout->addWidget(m_resetMxpTagsButton, 0, Qt::AlignLeft);
	connect(m_resetMxpTagsButton, &QPushButton::clicked, this,
	        [this]
	        {
		        if (!m_runtime)
			        return;
		        m_runtime->resetMxp();
	        });
	mxpLayout->addStretch();

	// Chat
	auto *chatLayout     = new QVBoxLayout(chatPage);
	auto *chatNameLayout = new QHBoxLayout();
	chatNameLayout->addWidget(new QLabel(QStringLiteral("Our chat"), chatPage));
	m_chatName = new QLineEdit(chatPage);
	chatNameLayout->addWidget(m_chatName);
	chatNameLayout->addStretch();
	chatLayout->addLayout(chatNameLayout);
	m_autoAllowSnooping = new QCheckBox(QStringLiteral("Automatically allow snoop"), chatPage);
	chatLayout->addWidget(m_autoAllowSnooping);

	auto *incomingBox    = new QGroupBox(QStringLiteral("Incoming calls"), chatPage);
	auto *incomingLayout = new QGridLayout(incomingBox);
	m_acceptIncomingChatConnections =
	    new QCheckBox(QStringLiteral("Accept incoming calls on port:"), incomingBox);
	m_incomingChatPort = new QSpinBox(incomingBox);
	m_incomingChatPort->setRange(1, 65535);
	m_validateIncomingCalls = new QCheckBox(QStringLiteral("Validate caller"), incomingBox);
	incomingLayout->addWidget(m_acceptIncomingChatConnections, 0, 0);
	incomingLayout->addWidget(m_incomingChatPort, 0, 1);
	incomingLayout->addWidget(m_validateIncomingCalls, 1, 0, 1, 2);
	chatLayout->addWidget(incomingBox);

	auto *appearanceBox    = new QGroupBox(QStringLiteral("Message Appearance"), chatPage);
	auto *appearanceLayout = new QGridLayout(appearanceBox);
	m_chatTextColour       = new QLineEdit(appearanceBox);
	m_chatBackColour       = new QLineEdit(appearanceBox);
	m_chatTextColour->setReadOnly(true);
	m_chatBackColour->setReadOnly(true);
	m_chatTextColour->setFixedWidth(36);
	m_chatBackColour->setFixedWidth(36);
	m_chatTextColour->installEventFilter(this);
	m_chatBackColour->installEventFilter(this);
	m_chatTextColour->setCursor(Qt::PointingHandCursor);
	m_chatBackColour->setCursor(Qt::PointingHandCursor);
	auto *onLabel       = new QLabel(QStringLiteral("on"), appearanceBox);
	m_ignoreChatColours = new QCheckBox(QStringLiteral("Ignore incoming colours"), appearanceBox);
	m_chatMessagePrefix = new QLineEdit(appearanceBox);
	appearanceLayout->addWidget(new QLabel(QStringLiteral("Messages"), appearanceBox), 0, 0);
	appearanceLayout->addWidget(m_chatTextColour, 0, 1);
	appearanceLayout->addWidget(onLabel, 0, 2);
	appearanceLayout->addWidget(m_chatBackColour, 0, 3);
	appearanceLayout->addWidget(m_ignoreChatColours, 1, 0, 1, 3);
	appearanceLayout->addWidget(new QLabel(QStringLiteral("Prefix messages"), appearanceBox), 2, 0);
	appearanceLayout->addWidget(m_chatMessagePrefix, 2, 1, 1, 3);
	chatLayout->addWidget(appearanceBox);

	auto *limitsBox    = new QGroupBox(QStringLiteral("Message size limits"), chatPage);
	auto *limitsLayout = new QGridLayout(limitsBox);
	m_maxChatLines     = new QSpinBox(limitsBox);
	m_maxChatLines->setRange(0, 10000);
	m_maxChatBytes = new QSpinBox(limitsBox);
	m_maxChatBytes->setRange(0, 10000000);
	limitsLayout->addWidget(new QLabel(QStringLiteral("Max. lines per"), limitsBox), 0, 0);
	limitsLayout->addWidget(m_maxChatLines, 0, 1);
	limitsLayout->addWidget(new QLabel(QStringLiteral("(0 = no)"), limitsBox), 0, 2);
	limitsLayout->addWidget(new QLabel(QStringLiteral("Max. bytes per"), limitsBox), 1, 0);
	limitsLayout->addWidget(m_maxChatBytes, 1, 1);
	limitsLayout->addWidget(new QLabel(QStringLiteral("(0 = no)"), limitsBox), 1, 2);
	chatLayout->addWidget(limitsBox);

	auto *filesBox      = new QGroupBox(QStringLiteral("Incoming Files"), chatPage);
	auto *filesLayout   = new QGridLayout(filesBox);
	m_autoAllowFiles    = new QCheckBox(QStringLiteral("Automatically accept files"), filesBox);
	m_chatSaveDirectory = new QLineEdit(filesBox);
	m_chatSaveBrowse    = new QPushButton(QStringLiteral("&Browse..."), filesBox);
	filesLayout->addWidget(m_autoAllowFiles, 0, 0, 1, 2);
	filesLayout->addWidget(new QLabel(QStringLiteral("Save to:"), filesBox), 1, 0);
	filesLayout->addWidget(m_chatSaveDirectory, 1, 1);
	filesLayout->addWidget(m_chatSaveBrowse, 2, 1, 1, 1, Qt::AlignLeft);
	connect(m_chatSaveBrowse, &QPushButton::clicked, this,
	        [this]
	        {
		        const QString startDir = m_runtime ? m_runtime->fileBrowsingDirectory() : QString();
		        const QString dirName  = QFileDialog::getExistingDirectory(
                    this, QStringLiteral("Save chat files folder"), startDir);
		        if (!dirName.isEmpty())
		        {
			        if (m_runtime)
				        m_runtime->setFileBrowsingDirectory(dirName);
			        m_chatSaveDirectory->setText(dirName);
		        }
	        });
	chatLayout->addWidget(filesBox);
	chatLayout->addStretch();

	auto editAliasItem = [this](const int row, const bool isNew) -> bool
	{
		if (!m_runtime)
			return false;
		if (m_useDefaultAliases && m_useDefaultAliases->isChecked() && hasDefaultAliasesFile())
			return false;
		QList<WorldRuntime::Alias> aliases = m_runtime->aliases();
		WorldRuntime::Alias        alias;
		int                        index = row;
		if (!isNew)
		{
			index = rowToIndex(m_aliasesTable, row);
			if (index < 0 || index >= aliases.size())
				return false;
			alias = aliases.at(index);
		}
		else
		{
			const QMap<QString, QString> &attrs = m_runtime->worldAttributes();
			const int defaultAliasSendTo   = attrs.value(QStringLiteral("default_alias_send_to")).toInt();
			const int defaultAliasSequence = attrs.value(QStringLiteral("default_alias_sequence")).toInt();
			alias.attributes.insert(QStringLiteral("enabled"), QStringLiteral("y"));
			alias.attributes.insert(
			    QStringLiteral("expand_variables"),
			    qmudIsEnabledFlag(attrs.value(QStringLiteral("default_alias_expand_variables")))
			        ? QStringLiteral("y")
			        : QStringLiteral("n"));
			alias.attributes.insert(
			    QStringLiteral("ignore_case"),
			    qmudIsEnabledFlag(attrs.value(QStringLiteral("default_alias_ignore_case")))
			        ? QStringLiteral("y")
			        : QStringLiteral("n"));
			alias.attributes.insert(
			    QStringLiteral("keep_evaluating"),
			    qmudIsEnabledFlag(attrs.value(QStringLiteral("default_alias_keep_evaluating")))
			        ? QStringLiteral("y")
			        : QStringLiteral("n"));
			alias.attributes.insert(QStringLiteral("regexp"),
			                        qmudIsEnabledFlag(attrs.value(QStringLiteral("default_alias_regexp")))
			                            ? QStringLiteral("y")
			                            : QStringLiteral("n"));
			alias.attributes.insert(QStringLiteral("send_to"), QString::number(defaultAliasSendTo));
			alias.attributes.insert(QStringLiteral("sequence"), QString::number(defaultAliasSequence));
		}

		WorldAliasDialog dlg(m_runtime, alias, aliases, index, this);
		if (dlg.exec() != QDialog::Accepted)
			return false;
		alias = dlg.alias();

		if (isNew)
		{
			aliases.push_back(alias);
			index = saturatingToInt(aliases.size()) - 1;
		}
		else
		{
			aliases[index] = alias;
		}
		m_runtime->setAliases(aliases);
		populateAliases();
		selectRowByIndex(m_aliasesTable, index);
		updateAliasControls();
		return true;
	};

	auto editTriggerItem = [this](const int row, const bool isNew) -> bool
	{
		if (!m_runtime)
			return false;
		if (m_useDefaultTriggers && m_useDefaultTriggers->isChecked() && hasDefaultTriggersFile())
			return false;
		QList<WorldRuntime::Trigger> triggers = m_runtime->triggers();
		WorldRuntime::Trigger        trigger;
		int                          index = row;
		if (!isNew)
		{
			index = rowToIndex(m_triggersTable, row);
			if (index < 0 || index >= triggers.size())
				return false;
			trigger = triggers.at(index);
		}
		else
		{
			const QMap<QString, QString> &attrs = m_runtime->worldAttributes();
			const int defaultTriggerSendTo = attrs.value(QStringLiteral("default_trigger_send_to")).toInt();
			const int defaultTriggerSequence =
			    attrs.value(QStringLiteral("default_trigger_sequence")).toInt();
			trigger.attributes.insert(QStringLiteral("enabled"), QStringLiteral("y"));
			trigger.attributes.insert(
			    QStringLiteral("expand_variables"),
			    qmudIsEnabledFlag(attrs.value(QStringLiteral("default_trigger_expand_variables")))
			        ? QStringLiteral("y")
			        : QStringLiteral("n"));
			trigger.attributes.insert(
			    QStringLiteral("ignore_case"),
			    qmudIsEnabledFlag(attrs.value(QStringLiteral("default_trigger_ignore_case")))
			        ? QStringLiteral("y")
			        : QStringLiteral("n"));
			trigger.attributes.insert(
			    QStringLiteral("keep_evaluating"),
			    qmudIsEnabledFlag(attrs.value(QStringLiteral("default_trigger_keep_evaluating")))
			        ? QStringLiteral("y")
			        : QStringLiteral("n"));
			trigger.attributes.insert(QStringLiteral("regexp"),
			                          qmudIsEnabledFlag(attrs.value(QStringLiteral("default_trigger_regexp")))
			                              ? QStringLiteral("y")
			                              : QStringLiteral("n"));
			trigger.attributes.insert(QStringLiteral("send_to"), QString::number(defaultTriggerSendTo));
			trigger.attributes.insert(QStringLiteral("sequence"), QString::number(defaultTriggerSequence));
		}

		WorldTriggerDialog dlg(m_runtime, trigger, triggers, index, this);
		if (dlg.exec() != QDialog::Accepted)
			return false;
		trigger = dlg.trigger();

		if (isNew)
		{
			triggers.push_back(trigger);
			index = saturatingToInt(triggers.size()) - 1;
		}
		else
		{
			triggers[index] = trigger;
		}
		m_runtime->setTriggers(triggers);
		populateTriggers();
		selectRowByIndex(m_triggersTable, index);
		updateTriggerControls();
		return true;
	};

	auto editTimerItem = [this](const int row, const bool isNew) -> bool
	{
		if (!m_runtime)
			return false;
		if (m_useDefaultTimers && m_useDefaultTimers->isChecked() && hasDefaultTimersFile())
			return false;
		QList<WorldRuntime::Timer> timers = m_runtime->timers();
		WorldRuntime::Timer        timer;
		int                        index = row;
		if (!isNew)
		{
			index = rowToIndex(m_timersTable, row);
			if (index < 0 || index >= timers.size())
				return false;
			timer = timers.at(index);
		}
		else
		{
			const QMap<QString, QString> &attrs = m_runtime->worldAttributes();
			const int defaultTimerSendTo = attrs.value(QStringLiteral("default_timer_send_to")).toInt();
			timer.attributes.insert(QStringLiteral("send_to"), QString::number(defaultTimerSendTo));
		}

		WorldTimerDialog dlg(m_runtime, timer, timers, index, this);
		if (dlg.exec() != QDialog::Accepted)
			return false;
		timer = dlg.timer();

		if (isNew)
		{
			timers.push_back(timer);
			index = saturatingToInt(timers.size()) - 1;
		}
		else
		{
			timers[index] = timer;
		}
		m_runtime->setTimers(timers);
		populateTimers();
		selectRowByIndex(m_timersTable, index);
		updateTimerControls();
		return true;
	};

	auto copyTriggersToClipboard = [this]
	{
		if (!m_runtime)
			return;
		if (m_useDefaultTriggers && m_useDefaultTriggers->isChecked() && hasDefaultTriggersFile())
			return;
		const QList<int> rows = selectedRows(m_triggersTable);
		if (rows.isEmpty())
			return;
		const QList<WorldRuntime::Trigger> triggers = m_runtime->triggers();
		QList<WorldRuntime::Trigger>       selected;
		for (int row : rows)
		{
			const int index = rowToIndex(m_triggersTable, row);
			if (index >= 0 && index < triggers.size())
				selected.push_back(triggers.at(index));
		}
		if (selected.isEmpty())
			return;
		const QString nl = QStringLiteral("\r\n");
		QString       output;
		QTextStream   out(&output);
		out.setEncoding(QStringConverter::Utf8);
		const QDateTime now = QDateTime::currentDateTime();
		const QString   savedOn =
		    QLocale::system().toString(now, QStringLiteral("dddd, MMMM dd, yyyy, h:mm AP"));
		out << R"(<?xml version="1.0" encoding="utf-8"?>)" << nl;
		out << "<!DOCTYPE qmud>" << nl;
		out << "<!-- Saved on " << fixHtmlString(savedOn) << " -->" << nl;
		out << "<!-- QMud version " << kVersionString << " -->" << nl;
		out << "<!-- Written by Panagiotis Kalogiratos -->" << nl;
		out << "<!-- Home Page: https://github.com/Nodens-/QMud -->" << nl;
		out << "<qmud>" << nl;
		out << nl << "<!-- triggers -->" << nl;
		out << nl << "<triggers" << nl;
		out << "   qmud_version=\"" << fixHtmlString(QString::fromLatin1(kVersionString)) << "\" " << nl;
		out << "   world_file_version=\"" << m_runtime->worldFileVersion() << "\" " << nl;
		out << "   date_saved=\"" << now.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) << "\" " << nl;
		out << "  >" << nl;
		for (const auto &tr : selected)
		{
			out << nl << "  <trigger ";
			for (auto it = tr.attributes.begin(); it != tr.attributes.end(); ++it)
				out << it.key() << "=\"" << fixHtmlString(it.value()) << "\" ";
			out << ">" << nl;
			for (auto it = tr.children.begin(); it != tr.children.end(); ++it)
			{
				out << "  <" << it.key() << ">" << fixHtmlMultilineString(it.value()) << "</" << it.key()
				    << ">" << nl;
			}
			out << nl << "  </trigger>" << nl;
		}
		out << "</triggers>" << nl;
		out << "</qmud>" << nl;
		if (QClipboard *clipboard = QGuiApplication::clipboard())
			clipboard->setText(output);
	};

	auto copyAliasesToClipboard = [this]
	{
		if (!m_runtime)
			return;
		if (m_useDefaultAliases && m_useDefaultAliases->isChecked() && hasDefaultAliasesFile())
			return;
		const QList<int> rows = selectedRows(m_aliasesTable);
		if (rows.isEmpty())
			return;
		const QList<WorldRuntime::Alias> aliases = m_runtime->aliases();
		QList<WorldRuntime::Alias>       selected;
		for (int row : rows)
		{
			if (const int index = rowToIndex(m_aliasesTable, row); index >= 0 && index < aliases.size())
				selected.push_back(aliases.at(index));
		}
		if (selected.isEmpty())
			return;
		const QString nl = QStringLiteral("\r\n");
		QString       output;
		QTextStream   out(&output);
		out.setEncoding(QStringConverter::Utf8);
		const QDateTime now = QDateTime::currentDateTime();
		const QString   savedOn =
		    QLocale::system().toString(now, QStringLiteral("dddd, MMMM dd, yyyy, h:mm AP"));
		out << R"(<?xml version="1.0" encoding="utf-8"?>)" << nl;
		out << "<!DOCTYPE qmud>" << nl;
		out << "<!-- Saved on " << fixHtmlString(savedOn) << " -->" << nl;
		out << "<!-- QMud version " << kVersionString << " -->" << nl;
		out << "<!-- Written by Panagiotis Kalogiratos -->" << nl;
		out << "<!-- Home Page: https://github.com/Nodens-/QMud -->" << nl;
		out << "<qmud>" << nl;
		out << nl << "<!-- aliases -->" << nl;
		out << nl << "<aliases" << nl;
		out << "   qmud_version=\"" << fixHtmlString(QString::fromLatin1(kVersionString)) << "\" " << nl;
		out << "   world_file_version=\"" << m_runtime->worldFileVersion() << "\" " << nl;
		out << "   date_saved=\"" << now.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) << "\" " << nl;
		out << "  >" << nl;
		for (const auto &al : selected)
		{
			out << nl << "  <alias ";
			for (auto it = al.attributes.begin(); it != al.attributes.end(); ++it)
				out << it.key() << "=\"" << fixHtmlString(it.value()) << "\" ";
			out << ">" << nl;
			for (auto it = al.children.begin(); it != al.children.end(); ++it)
			{
				out << "  <" << it.key() << ">" << fixHtmlMultilineString(it.value()) << "</" << it.key()
				    << ">" << nl;
			}
			out << nl << "  </alias>" << nl;
		}
		out << "</aliases>" << nl;
		out << "</qmud>" << nl;
		if (QClipboard *clipboard = QGuiApplication::clipboard())
			clipboard->setText(output);
	};

	auto copyTimersToClipboard = [this]
	{
		if (!m_runtime)
			return;
		if (m_useDefaultTimers && m_useDefaultTimers->isChecked() && hasDefaultTimersFile())
			return;
		const QList<int> rows = selectedRows(m_timersTable);
		if (rows.isEmpty())
			return;
		const QList<WorldRuntime::Timer> timers = m_runtime->timers();
		QList<WorldRuntime::Timer>       selected;
		for (int row : rows)
		{
			if (const int index = rowToIndex(m_timersTable, row); index >= 0 && index < timers.size())
				selected.push_back(timers.at(index));
		}
		if (selected.isEmpty())
			return;
		const QString nl = QStringLiteral("\r\n");
		QString       output;
		QTextStream   out(&output);
		out.setEncoding(QStringConverter::Utf8);
		const QDateTime now = QDateTime::currentDateTime();
		const QString   savedOn =
		    QLocale::system().toString(now, QStringLiteral("dddd, MMMM dd, yyyy, h:mm AP"));
		out << R"(<?xml version="1.0" encoding="utf-8"?>)" << nl;
		out << "<!DOCTYPE qmud>" << nl;
		out << "<!-- Saved on " << fixHtmlString(savedOn) << " -->" << nl;
		out << "<!-- QMud version " << kVersionString << " -->" << nl;
		out << "<!-- Written by Panagiotis Kalogiratos -->" << nl;
		out << "<!-- Home Page: https://github.com/Nodens-/QMud -->" << nl;
		out << "<qmud>" << nl;
		out << nl << "<!-- timers -->" << nl;
		out << nl << "<timers" << nl;
		out << "   qmud_version=\"" << fixHtmlString(QString::fromLatin1(kVersionString)) << "\" " << nl;
		out << "   world_file_version=\"" << m_runtime->worldFileVersion() << "\" " << nl;
		out << "   date_saved=\"" << now.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) << "\" " << nl;
		out << "  >" << nl;
		for (const auto &tm : selected)
		{
			out << nl << "  <timer ";
			for (auto it = tm.attributes.begin(); it != tm.attributes.end(); ++it)
				out << it.key() << "=\"" << fixHtmlString(it.value()) << "\" ";
			out << ">" << nl;
			for (auto it = tm.children.begin(); it != tm.children.end(); ++it)
			{
				out << "  <" << it.key() << ">" << fixHtmlMultilineString(it.value()) << "</" << it.key()
				    << ">" << nl;
			}
			out << nl << "  </timer>" << nl;
		}
		out << "</timers>" << nl;
		out << "</qmud>" << nl;
		if (QClipboard *clipboard = QGuiApplication::clipboard())
			clipboard->setText(output);
	};

	auto pasteTriggersFromClipboard = [this]
	{
		if (!m_runtime)
			return;
		if (m_useDefaultTriggers && m_useDefaultTriggers->isChecked() && hasDefaultTriggersFile())
			return;
		QClipboard *clipboard = QGuiApplication::clipboard();
		if (!clipboard)
			return;
		const QString text = clipboard->text();
		if (text.trimmed().isEmpty())
			return;
		QTemporaryFile temp;
		if (!temp.open())
			return;
		temp.write(text.toUtf8());
		temp.flush();
		WorldDocument doc;
		doc.setLoadMask(WorldDocument::XML_TRIGGERS | WorldDocument::XML_NO_PLUGINS |
		                WorldDocument::XML_IMPORT_MAIN_FILE_ONLY);
		if (!doc.loadFromFile(temp.fileName()))
		{
			QMessageBox::warning(this, QStringLiteral("Paste triggers"), doc.errorString());
			return;
		}
		QList<WorldRuntime::Trigger> combined = m_runtime->triggers();
		for (const auto &t : doc.triggers())
		{
			WorldRuntime::Trigger rt;
			rt.attributes = t.attributes;
			rt.children   = t.children;
			rt.included   = t.included;
			combined.push_back(rt);
		}
		m_runtime->setTriggers(combined);
		populateTriggers();
		updateTriggerControls();
	};

	auto pasteAliasesFromClipboard = [this]
	{
		if (!m_runtime)
			return;
		if (m_useDefaultAliases && m_useDefaultAliases->isChecked() && hasDefaultAliasesFile())
			return;
		QClipboard *clipboard = QGuiApplication::clipboard();
		if (!clipboard)
			return;
		const QString text = clipboard->text();
		if (text.trimmed().isEmpty())
			return;
		QTemporaryFile temp;
		if (!temp.open())
			return;
		temp.write(text.toUtf8());
		temp.flush();
		WorldDocument doc;
		doc.setLoadMask(WorldDocument::XML_ALIASES | WorldDocument::XML_NO_PLUGINS |
		                WorldDocument::XML_IMPORT_MAIN_FILE_ONLY);
		if (!doc.loadFromFile(temp.fileName()))
		{
			QMessageBox::warning(this, QStringLiteral("Paste aliases"), doc.errorString());
			return;
		}
		QList<WorldRuntime::Alias> combined = m_runtime->aliases();
		for (const auto &a : doc.aliases())
		{
			WorldRuntime::Alias ra;
			ra.attributes = a.attributes;
			ra.children   = a.children;
			ra.included   = a.included;
			combined.push_back(ra);
		}
		m_runtime->setAliases(combined);
		populateAliases();
		updateAliasControls();
	};

	auto pasteTimersFromClipboard = [this]
	{
		if (!m_runtime)
			return;
		if (m_useDefaultTimers && m_useDefaultTimers->isChecked() && hasDefaultTimersFile())
			return;
		QClipboard *clipboard = QGuiApplication::clipboard();
		if (!clipboard)
			return;
		const QString text = clipboard->text();
		if (text.trimmed().isEmpty())
			return;
		QTemporaryFile temp;
		if (!temp.open())
			return;
		temp.write(text.toUtf8());
		temp.flush();
		WorldDocument doc;
		doc.setLoadMask(WorldDocument::XML_TIMERS | WorldDocument::XML_NO_PLUGINS |
		                WorldDocument::XML_IMPORT_MAIN_FILE_ONLY);
		if (!doc.loadFromFile(temp.fileName()))
		{
			QMessageBox::warning(this, QStringLiteral("Paste timers"), doc.errorString());
			return;
		}
		QList<WorldRuntime::Timer> combined = m_runtime->timers();
		for (const auto &t : doc.timers())
		{
			WorldRuntime::Timer rt;
			rt.attributes = t.attributes;
			rt.children   = t.children;
			rt.included   = t.included;
			combined.push_back(rt);
		}
		m_runtime->setTimers(combined);
		populateTimers();
		updateTimerControls();
	};

	if (m_loadTriggersButton)
		connect(m_loadTriggersButton, &QPushButton::clicked, this,
		        [this]
		        {
			        if (!m_runtime)
				        return;
			        if (m_useDefaultTriggers && m_useDefaultTriggers->isChecked() && hasDefaultTriggersFile())
				        return;
			        bool replace = true;
			        if (!m_runtime->triggers().isEmpty())
			        {
				        const QString message =
				            QStringLiteral("Replace existing triggers?\n"
				                           "If you reply \"No\", then triggers from the file"
				                           " will be added to existing triggers");
				        replace =
				            (QMessageBox::question(this, QStringLiteral("Load triggers"), message,
				                                   QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes);
			        }
			        const QString startDir = m_runtime->fileBrowsingDirectory();
			        const QString fileName =
			            QFileDialog::getOpenFileName(this, QStringLiteral("Trigger file name"), startDir,
			                                         QStringLiteral("QMud triggers (*.qdt *.mct)"));
			        if (fileName.isEmpty())
				        return;
			        m_runtime->setFileBrowsingDirectory(QFileInfo(fileName).absolutePath());
			        if (!loadTriggersFromFile(fileName, replace))
				        return;
			        populateTriggers();
			        updateTriggerControls();
		        });
	if (m_saveTriggersButton)
		connect(m_saveTriggersButton, &QPushButton::clicked, this,
		        [this]
		        {
			        const QString startDir = m_runtime ? m_runtime->fileBrowsingDirectory() : QString();
			        const QString fileName =
			            QFileDialog::getSaveFileName(this, QStringLiteral("Trigger file name"), startDir,
			                                         QStringLiteral("QMud triggers (*.qdt *.mct)"));
			        if (fileName.isEmpty())
				        return;
			        const QString outputPath = canonicalSavePath(fileName, QStringLiteral("qdt"));
			        if (m_runtime)
				        m_runtime->setFileBrowsingDirectory(QFileInfo(outputPath).absolutePath());
			        if (!saveTriggersToFile(outputPath))
				        return;
		        });
	if (m_loadAliasesButton)
		connect(m_loadAliasesButton, &QPushButton::clicked, this,
		        [this]
		        {
			        if (!m_runtime)
				        return;
			        if (m_useDefaultAliases && m_useDefaultAliases->isChecked() && hasDefaultAliasesFile())
				        return;
			        bool replace = true;
			        if (!m_runtime->aliases().isEmpty())
			        {
				        const QString message =
				            QStringLiteral("Replace existing aliases?\n"
				                           "If you reply \"No\", then aliases from the file"
				                           " will be added to existing aliases");
				        replace =
				            (QMessageBox::question(this, QStringLiteral("Load aliases"), message,
				                                   QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes);
			        }
			        const QString startDir = m_runtime->fileBrowsingDirectory();
			        const QString fileName =
			            QFileDialog::getOpenFileName(this, QStringLiteral("Alias file name"), startDir,
			                                         QStringLiteral("QMud aliases (*.qda *.mca)"));
			        if (fileName.isEmpty())
				        return;
			        m_runtime->setFileBrowsingDirectory(QFileInfo(fileName).absolutePath());
			        if (!loadAliasesFromFile(fileName, replace))
				        return;
			        populateAliases();
			        updateAliasControls();
		        });
	if (m_saveAliasesButton)
		connect(m_saveAliasesButton, &QPushButton::clicked, this,
		        [this]
		        {
			        const QString startDir = m_runtime ? m_runtime->fileBrowsingDirectory() : QString();
			        const QString fileName =
			            QFileDialog::getSaveFileName(this, QStringLiteral("Alias file name"), startDir,
			                                         QStringLiteral("QMud aliases (*.qda *.mca)"));
			        if (fileName.isEmpty())
				        return;
			        const QString outputPath = canonicalSavePath(fileName, QStringLiteral("qda"));
			        if (m_runtime)
				        m_runtime->setFileBrowsingDirectory(QFileInfo(outputPath).absolutePath());
			        if (!saveAliasesToFile(outputPath))
				        return;
		        });
	if (m_loadTimersButton)
		connect(m_loadTimersButton, &QPushButton::clicked, this,
		        [this]
		        {
			        if (!m_runtime)
				        return;
			        if (m_useDefaultTimers && m_useDefaultTimers->isChecked() && hasDefaultTimersFile())
				        return;
			        bool replace = true;
			        if (!m_runtime->timers().isEmpty())
			        {
				        const QString message =
				            QStringLiteral("Replace existing timers?\n"
				                           "If you reply \"No\", then timers from the file"
				                           " will be added to existing timers");
				        replace =
				            (QMessageBox::question(this, QStringLiteral("Load timers"), message,
				                                   QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes);
			        }
			        const QString startDir = m_runtime->fileBrowsingDirectory();
			        const QString fileName =
			            QFileDialog::getOpenFileName(this, QStringLiteral("Timer file name"), startDir,
			                                         QStringLiteral("QMud timers (*.qdi *.mci)"));
			        if (fileName.isEmpty())
				        return;
			        m_runtime->setFileBrowsingDirectory(QFileInfo(fileName).absolutePath());
			        if (!loadTimersFromFile(fileName, replace))
				        return;
			        populateTimers();
			        updateTimerControls();
		        });
	if (m_saveTimersButton)
		connect(m_saveTimersButton, &QPushButton::clicked, this,
		        [this]
		        {
			        const QString startDir = m_runtime ? m_runtime->fileBrowsingDirectory() : QString();
			        const QString fileName =
			            QFileDialog::getSaveFileName(this, QStringLiteral("Timer file name"), startDir,
			                                         QStringLiteral("QMud timers (*.qdi *.mci)"));
			        if (fileName.isEmpty())
				        return;
			        const QString outputPath = canonicalSavePath(fileName, QStringLiteral("qdi"));
			        if (m_runtime)
				        m_runtime->setFileBrowsingDirectory(QFileInfo(outputPath).absolutePath());
			        if (!saveTimersToFile(outputPath))
				        return;
		        });

	connect(m_addAliasButton, &QPushButton::clicked, this, [editAliasItem] { editAliasItem(-1, true); });
	connect(m_editAliasButton, &QPushButton::clicked, this,
	        [this, editAliasItem]
	        {
		        const int row = selectedRow(m_aliasesTable);
		        editAliasItem(row, false);
	        });
	connect(m_deleteAliasButton, &QPushButton::clicked, this,
	        [this]
	        {
		        if (!m_runtime)
			        return;
		        if (m_useDefaultAliases && m_useDefaultAliases->isChecked() && hasDefaultAliasesFile())
			        return;
		        const int row   = selectedRow(m_aliasesTable);
		        const int index = rowToIndex(m_aliasesTable, row);
		        if (index < 0)
			        return;
		        if (!confirmRemoval(this, QStringLiteral("alias")))
			        return;
		        QList<WorldRuntime::Alias> aliases = m_runtime->aliases();
		        if (index >= aliases.size())
			        return;
		        aliases.removeAt(index);
		        m_runtime->setAliases(aliases);
		        populateAliases();
		        const int lastIndex = saturatingToInt(aliases.size()) - 1;
		        selectRowByIndex(m_aliasesTable, std::min(index, lastIndex));
		        updateAliasControls();
	        });
	connect(m_moveAliasUpButton, &QPushButton::clicked, this,
	        [this]
	        {
		        if (!m_runtime)
			        return;
		        if (m_useDefaultAliases && m_useDefaultAliases->isChecked() && hasDefaultAliasesFile())
			        return;
		        const int row   = selectedRow(m_aliasesTable);
		        const int index = rowToIndex(m_aliasesTable, row);
		        if (index <= 0)
			        return;
		        QList<WorldRuntime::Alias> aliases = m_runtime->aliases();
		        if (index >= aliases.size())
			        return;
		        aliases.swapItemsAt(index, index - 1);
		        m_runtime->setAliases(aliases);
		        populateAliases();
		        selectRowByIndex(m_aliasesTable, index - 1);
		        updateAliasControls();
	        });
	connect(m_moveAliasDownButton, &QPushButton::clicked, this,
	        [this]
	        {
		        if (!m_runtime)
			        return;
		        const int                  row     = selectedRow(m_aliasesTable);
		        QList<WorldRuntime::Alias> aliases = m_runtime->aliases();
		        if (m_useDefaultAliases && m_useDefaultAliases->isChecked() && hasDefaultAliasesFile())
			        return;
		        const int index = rowToIndex(m_aliasesTable, row);
		        if (index < 0 || index + 1 >= aliases.size())
			        return;
		        aliases.swapItemsAt(index, index + 1);
		        m_runtime->setAliases(aliases);
		        populateAliases();
		        selectRowByIndex(m_aliasesTable, index + 1);
		        updateAliasControls();
	        });
	connect(m_copyAliasButton, &QPushButton::clicked, this,
	        [copyAliasesToClipboard] { copyAliasesToClipboard(); });
	connect(m_pasteAliasButton, &QPushButton::clicked, this,
	        [pasteAliasesFromClipboard] { pasteAliasesFromClipboard(); });
	connect(m_findAliasButton, &QPushButton::clicked, this,
	        [this]
	        {
		        if (!m_runtime)
			        return;
		        const QList<WorldRuntime::Alias> aliases = m_runtime->aliases();
		        auto                             rowText = [this, &aliases](const int row) -> QString
		        {
			        const int index = rowToIndex(m_aliasesTable, row);
			        if (index < 0 || index >= aliases.size())
				        return {};
			        const WorldRuntime::Alias &al     = aliases.at(index);
			        QString                    result = al.attributes.value(QStringLiteral("match"));
			        result += QLatin1Char('\t') + al.children.value(QStringLiteral("send"));
			        result += QLatin1Char('\t') + al.attributes.value(QStringLiteral("name"));
			        result += QLatin1Char('\t') + al.attributes.value(QStringLiteral("group"));
			        result += QLatin1Char('\t') + al.attributes.value(QStringLiteral("script"));
			        return result;
		        };
		        doFindAdvanced(this, m_aliasesTable, rowText, m_aliasFindText, m_aliasFindIndex,
		                       m_aliasFindHistory, m_aliasFindMatchCase, m_aliasFindRegex,
		                       m_aliasFindForwards, QStringLiteral("Find alias"), false);
		        updateAliasControls();
	        });
	connect(m_findNextAliasButton, &QPushButton::clicked, this,
	        [this]
	        {
		        if (!m_runtime)
			        return;
		        const QList<WorldRuntime::Alias> aliases = m_runtime->aliases();
		        auto                             rowText = [this, &aliases](const int row) -> QString
		        {
			        const int index = rowToIndex(m_aliasesTable, row);
			        if (index < 0 || index >= aliases.size())
				        return {};
			        const WorldRuntime::Alias &al     = aliases.at(index);
			        QString                    result = al.attributes.value(QStringLiteral("match"));
			        result += QLatin1Char('\t') + al.children.value(QStringLiteral("send"));
			        result += QLatin1Char('\t') + al.attributes.value(QStringLiteral("name"));
			        result += QLatin1Char('\t') + al.attributes.value(QStringLiteral("group"));
			        result += QLatin1Char('\t') + al.attributes.value(QStringLiteral("script"));
			        return result;
		        };
		        doFindAdvanced(this, m_aliasesTable, rowText, m_aliasFindText, m_aliasFindIndex,
		                       m_aliasFindHistory, m_aliasFindMatchCase, m_aliasFindRegex,
		                       m_aliasFindForwards, QStringLiteral("Find alias"), true);
		        updateAliasControls();
	        });

	connect(m_addTriggerButton, &QPushButton::clicked, this,
	        [editTriggerItem] { editTriggerItem(-1, true); });
	connect(m_editTriggerButton, &QPushButton::clicked, this,
	        [this, editTriggerItem]
	        {
		        const int row = selectedRow(m_triggersTable);
		        editTriggerItem(row, false);
	        });
	connect(m_deleteTriggerButton, &QPushButton::clicked, this,
	        [this]
	        {
		        if (!m_runtime)
			        return;
		        if (m_useDefaultTriggers && m_useDefaultTriggers->isChecked() && hasDefaultTriggersFile())
			        return;
		        const int row   = selectedRow(m_triggersTable);
		        const int index = rowToIndex(m_triggersTable, row);
		        if (index < 0)
			        return;
		        if (!confirmRemoval(this, QStringLiteral("trigger")))
			        return;
		        QList<WorldRuntime::Trigger> triggers = m_runtime->triggers();
		        if (index >= triggers.size())
			        return;
		        triggers.removeAt(index);
		        m_runtime->setTriggers(triggers);
		        populateTriggers();
		        const int lastIndex = saturatingToInt(triggers.size()) - 1;
		        selectRowByIndex(m_triggersTable, std::min(index, lastIndex));
		        updateTriggerControls();
	        });
	connect(m_moveTriggerUpButton, &QPushButton::clicked, this,
	        [this]
	        {
		        if (!m_runtime)
			        return;
		        if (m_useDefaultTriggers && m_useDefaultTriggers->isChecked() && hasDefaultTriggersFile())
			        return;
		        const int row   = selectedRow(m_triggersTable);
		        const int index = rowToIndex(m_triggersTable, row);
		        if (index <= 0)
			        return;
		        QList<WorldRuntime::Trigger> triggers = m_runtime->triggers();
		        if (index >= triggers.size())
			        return;
		        triggers.swapItemsAt(index, index - 1);
		        m_runtime->setTriggers(triggers);
		        populateTriggers();
		        selectRowByIndex(m_triggersTable, index - 1);
		        updateTriggerControls();
	        });
	connect(m_moveTriggerDownButton, &QPushButton::clicked, this,
	        [this]
	        {
		        if (!m_runtime)
			        return;
		        if (m_useDefaultTriggers && m_useDefaultTriggers->isChecked() && hasDefaultTriggersFile())
			        return;
		        const int                    row      = selectedRow(m_triggersTable);
		        QList<WorldRuntime::Trigger> triggers = m_runtime->triggers();
		        const int                    index    = rowToIndex(m_triggersTable, row);
		        if (index < 0 || index + 1 >= triggers.size())
			        return;
		        triggers.swapItemsAt(index, index + 1);
		        m_runtime->setTriggers(triggers);
		        populateTriggers();
		        selectRowByIndex(m_triggersTable, index + 1);
		        updateTriggerControls();
	        });
	connect(m_copyTriggerButton, &QPushButton::clicked, this,
	        [copyTriggersToClipboard] { copyTriggersToClipboard(); });
	connect(m_pasteTriggerButton, &QPushButton::clicked, this,
	        [pasteTriggersFromClipboard] { pasteTriggersFromClipboard(); });
	connect(m_findTriggerButton, &QPushButton::clicked, this,
	        [this]
	        {
		        if (!m_runtime)
			        return;
		        const QList<WorldRuntime::Trigger> triggers = m_runtime->triggers();
		        auto                               rowText  = [this, &triggers](const int row) -> QString
		        {
			        const int index = rowToIndex(m_triggersTable, row);
			        if (index < 0 || index >= triggers.size())
				        return {};
			        const WorldRuntime::Trigger &tr     = triggers.at(index);
			        QString                      result = tr.attributes.value(QStringLiteral("match"));
			        result += QLatin1Char('\t') + tr.children.value(QStringLiteral("send"));
			        result += QLatin1Char('\t') + tr.attributes.value(QStringLiteral("name"));
			        result += QLatin1Char('\t') + tr.attributes.value(QStringLiteral("group"));
			        result += QLatin1Char('\t') + tr.attributes.value(QStringLiteral("script"));
			        return result;
		        };
		        doFindAdvanced(this, m_triggersTable, rowText, m_triggerFindText, m_triggerFindIndex,
		                       m_triggerFindHistory, m_triggerFindMatchCase, m_triggerFindRegex,
		                       m_triggerFindForwards, QStringLiteral("Find trigger"), false);
		        updateTriggerControls();
	        });
	connect(m_findNextTriggerButton, &QPushButton::clicked, this,
	        [this]
	        {
		        if (!m_runtime)
			        return;
		        const QList<WorldRuntime::Trigger> triggers = m_runtime->triggers();
		        auto                               rowText  = [this, &triggers](const int row) -> QString
		        {
			        const int index = rowToIndex(m_triggersTable, row);
			        if (index < 0 || index >= triggers.size())
				        return {};
			        const WorldRuntime::Trigger &tr     = triggers.at(index);
			        QString                      result = tr.attributes.value(QStringLiteral("match"));
			        result += QLatin1Char('\t') + tr.children.value(QStringLiteral("send"));
			        result += QLatin1Char('\t') + tr.attributes.value(QStringLiteral("name"));
			        result += QLatin1Char('\t') + tr.attributes.value(QStringLiteral("group"));
			        result += QLatin1Char('\t') + tr.attributes.value(QStringLiteral("script"));
			        return result;
		        };
		        doFindAdvanced(this, m_triggersTable, rowText, m_triggerFindText, m_triggerFindIndex,
		                       m_triggerFindHistory, m_triggerFindMatchCase, m_triggerFindRegex,
		                       m_triggerFindForwards, QStringLiteral("Find trigger"), true);
		        updateTriggerControls();
	        });

	connect(m_addTimerButton, &QPushButton::clicked, this, [editTimerItem] { editTimerItem(-1, true); });
	connect(m_editTimerButton, &QPushButton::clicked, this,
	        [this, editTimerItem]
	        {
		        const int row = selectedRow(m_timersTable);
		        editTimerItem(row, false);
	        });
	connect(m_deleteTimerButton, &QPushButton::clicked, this,
	        [this]
	        {
		        if (!m_runtime)
			        return;
		        if (m_useDefaultTimers && m_useDefaultTimers->isChecked() && hasDefaultTimersFile())
			        return;
		        const int row   = selectedRow(m_timersTable);
		        const int index = rowToIndex(m_timersTable, row);
		        if (index < 0)
			        return;
		        if (!confirmRemoval(this, QStringLiteral("timer")))
			        return;
		        QList<WorldRuntime::Timer> timers = m_runtime->timers();
		        if (index >= timers.size())
			        return;
		        timers.removeAt(index);
		        m_runtime->setTimers(timers);
		        populateTimers();
		        const int lastIndex = saturatingToInt(timers.size()) - 1;
		        selectRowByIndex(m_timersTable, std::min(index, lastIndex));
		        updateTimerControls();
	        });
	connect(m_copyTimerButton, &QPushButton::clicked, this,
	        [copyTimersToClipboard] { copyTimersToClipboard(); });
	connect(m_pasteTimerButton, &QPushButton::clicked, this,
	        [pasteTimersFromClipboard] { pasteTimersFromClipboard(); });
	connect(m_findTimerButton, &QPushButton::clicked, this,
	        [this]
	        {
		        if (!m_runtime)
			        return;
		        const QList<WorldRuntime::Timer> timers  = m_runtime->timers();
		        auto                             rowText = [this, &timers](const int row) -> QString
		        {
			        const int index = rowToIndex(m_timersTable, row);
			        if (index < 0 || index >= timers.size())
				        return {};
			        const WorldRuntime::Timer &tm     = timers.at(index);
			        QString                    result = tm.children.value(QStringLiteral("send"));
			        result += QLatin1Char('\t') + tm.attributes.value(QStringLiteral("name"));
			        result += QLatin1Char('\t') + tm.attributes.value(QStringLiteral("group"));
			        result += QLatin1Char('\t') + tm.attributes.value(QStringLiteral("script"));
			        return result;
		        };
		        doFindAdvanced(this, m_timersTable, rowText, m_timerFindText, m_timerFindIndex,
		                       m_timerFindHistory, m_timerFindMatchCase, m_timerFindRegex,
		                       m_timerFindForwards, QStringLiteral("Find timer"), false);
		        updateTimerControls();
	        });
	connect(m_findNextTimerButton, &QPushButton::clicked, this,
	        [this]
	        {
		        if (!m_runtime)
			        return;
		        const QList<WorldRuntime::Timer> timers  = m_runtime->timers();
		        auto                             rowText = [this, &timers](const int row) -> QString
		        {
			        const int index = rowToIndex(m_timersTable, row);
			        if (index < 0 || index >= timers.size())
				        return {};
			        const WorldRuntime::Timer &tm     = timers.at(index);
			        QString                    result = tm.children.value(QStringLiteral("send"));
			        result += QLatin1Char('\t') + tm.attributes.value(QStringLiteral("name"));
			        result += QLatin1Char('\t') + tm.attributes.value(QStringLiteral("group"));
			        result += QLatin1Char('\t') + tm.attributes.value(QStringLiteral("script"));
			        return result;
		        };
		        doFindAdvanced(this, m_timersTable, rowText, m_timerFindText, m_timerFindIndex,
		                       m_timerFindHistory, m_timerFindMatchCase, m_timerFindRegex,
		                       m_timerFindForwards, QStringLiteral("Find timer"), true);
		        updateTimerControls();
	        });
	connect(m_resetTimersButton, &QPushButton::clicked, this,
	        [this]
	        {
		        if (!m_runtime)
			        return;
		        m_runtime->resetAllTimers();
		        populateTimers();
	        });

	connect(m_aliasesTable, &QTableWidget::cellDoubleClicked, this,
	        [editAliasItem](const int row, int) { editAliasItem(row, false); });
	connect(m_triggersTable, &QTableWidget::cellDoubleClicked, this,
	        [editTriggerItem](const int row, int) { editTriggerItem(row, false); });
	connect(m_timersTable, &QTableWidget::cellDoubleClicked, this,
	        [editTimerItem](const int row, int) { editTimerItem(row, false); });
	auto syncTreeFromTable = [this](const QTableWidget *table, QTreeWidget *tree)
	{
		if (!table || !tree || m_syncingRuleSelection)
			return;
		const int      index = rowToIndex(table, selectedRow(table));
		QSignalBlocker block(tree);
		m_syncingRuleSelection = true;
		tree->setCurrentItem(findTreeItemByIndex(tree, index));
		m_syncingRuleSelection = false;
	};
	auto syncTableFromTree = [this](const QTreeWidget *tree, QTableWidget *table)
	{
		if (!table || !tree || m_syncingRuleSelection)
			return;
		const int      index = currentTreeIndex(tree);
		QSignalBlocker block(table);
		m_syncingRuleSelection = true;
		if (index >= 0)
			selectRowByIndex(table, index);
		else
			table->clearSelection();
		m_syncingRuleSelection = false;
	};
	auto rowForTreeItem = [](const QTreeWidgetItem *item, const QTableWidget *table) -> int
	{
		if (!item || !table || item->childCount() > 0)
			return -1;
		bool      ok    = false;
		const int index = item->data(0, Qt::UserRole).toInt(&ok);
		if (!ok)
			return -1;
		return findRowForIndex(table, index);
	};
	auto connectSingleGroupExpansion =
	    [this](QTreeWidget *tree, const std::function<void(const QString &)> &save)
	{
		if (!tree)
		{
			return;
		}

		connect(tree, &QTreeWidget::itemExpanded, this,
		        [this, tree, save](const QTreeWidgetItem *item)
		        {
			        if (!item || item->parent())
				        return;

			        {
				        QSignalBlocker block(tree);
				        for (int i = 0; i < tree->topLevelItemCount(); ++i)
				        {
					        QTreeWidgetItem *topLevel = tree->topLevelItem(i);
					        if (!topLevel || topLevel == item)
						        continue;
					        topLevel->setExpanded(false);
				        }
			        }

			        if (m_runtime)
				        save(item->text(0));
		        });

		connect(tree, &QTreeWidget::itemCollapsed, this,
		        [this, tree, save](const QTreeWidgetItem *item)
		        {
			        if (!item || item->parent())
				        return;

			        QString expandedGroup;
			        for (int i = 0; i < tree->topLevelItemCount(); ++i)
			        {
				        QTreeWidgetItem *topLevel = tree->topLevelItem(i);
				        if (topLevel && topLevel->isExpanded())
				        {
					        expandedGroup = topLevel->text(0);
					        break;
				        }
			        }

			        if (m_runtime)
				        save(expandedGroup);
		        });
	};
	if (m_aliasesTree)
		connect(m_aliasesTree, &QTreeWidget::itemDoubleClicked, this,
		        [this, editAliasItem, rowForTreeItem](const QTreeWidgetItem *item, int)
		        {
			        const int row = rowForTreeItem(item, m_aliasesTable);
			        if (row >= 0)
				        editAliasItem(row, false);
		        });
	if (m_triggersTree)
		connect(m_triggersTree, &QTreeWidget::itemDoubleClicked, this,
		        [this, editTriggerItem, rowForTreeItem](const QTreeWidgetItem *item, int)
		        {
			        const int row = rowForTreeItem(item, m_triggersTable);
			        if (row >= 0)
				        editTriggerItem(row, false);
		        });
	if (m_timersTree)
		connect(m_timersTree, &QTreeWidget::itemDoubleClicked, this,
		        [this, editTimerItem, rowForTreeItem](const QTreeWidgetItem *item, int)
		        {
			        const int row = rowForTreeItem(item, m_timersTable);
			        if (row >= 0)
				        editTimerItem(row, false);
		        });
	connectSingleGroupExpansion(m_triggersTree,
	                            [this](const QString &group)
	                            {
		                            if (m_runtime)
			                            m_runtime->setLastTriggerTreeExpandedGroup(group);
	                            });
	connectSingleGroupExpansion(m_aliasesTree,
	                            [this](const QString &group)
	                            {
		                            if (m_runtime)
			                            m_runtime->setLastAliasTreeExpandedGroup(group);
	                            });
	connectSingleGroupExpansion(m_timersTree,
	                            [this](const QString &group)
	                            {
		                            if (m_runtime)
			                            m_runtime->setLastTimerTreeExpandedGroup(group);
	                            });
	if (m_triggersTable)
		connect(m_triggersTable, &QTableWidget::itemSelectionChanged, this,
		        [this, syncTreeFromTable]
		        {
			        syncTreeFromTable(m_triggersTable, m_triggersTree);
			        updateTriggerControls();
		        });
	if (m_aliasesTable)
		connect(m_aliasesTable, &QTableWidget::itemSelectionChanged, this,
		        [this, syncTreeFromTable]
		        {
			        syncTreeFromTable(m_aliasesTable, m_aliasesTree);
			        updateAliasControls();
		        });
	if (m_triggersTree)
		connect(m_triggersTree, &QTreeWidget::currentItemChanged, this,
		        [this, syncTableFromTree](QTreeWidgetItem *, QTreeWidgetItem *)
		        {
			        syncTableFromTree(m_triggersTree, m_triggersTable);
			        updateTriggerControls();
		        });
	if (m_aliasesTree)
		connect(m_aliasesTree, &QTreeWidget::currentItemChanged, this,
		        [this, syncTableFromTree](QTreeWidgetItem *, QTreeWidgetItem *)
		        {
			        syncTableFromTree(m_aliasesTree, m_aliasesTable);
			        updateAliasControls();
		        });
	if (m_useDefaultAliases)
		connect(m_useDefaultAliases, &QCheckBox::toggled, this, [this] { updateAliasControls(); });
	if (m_useDefaultTriggers)
		connect(m_useDefaultTriggers, &QCheckBox::toggled, this, [this] { updateTriggerControls(); });
	if (m_timersTable)
		connect(m_timersTable, &QTableWidget::itemSelectionChanged, this,
		        [this, syncTreeFromTable]
		        {
			        syncTreeFromTable(m_timersTable, m_timersTree);
			        updateTimerControls();
		        });
	if (m_timersTree)
		connect(m_timersTree, &QTreeWidget::currentItemChanged, this,
		        [this, syncTableFromTree](QTreeWidgetItem *, QTreeWidgetItem *)
		        {
			        syncTableFromTree(m_timersTree, m_timersTable);
			        updateTimerControls();
		        });
	if (m_useDefaultTimers)
		connect(m_useDefaultTimers, &QCheckBox::toggled, this, [this] { updateTimerControls(); });
	if (m_aliasTreeView)
		connect(m_aliasTreeView, &QCheckBox::toggled, this,
		        [this]
		        {
			        updateRuleViewModes();
			        updateAliasControls();
		        });
	if (m_triggerTreeView)
		connect(m_triggerTreeView, &QCheckBox::toggled, this,
		        [this]
		        {
			        updateRuleViewModes();
			        updateTriggerControls();
		        });
	if (m_timerTreeView)
		connect(m_timerTreeView, &QCheckBox::toggled, this,
		        [this]
		        {
			        updateRuleViewModes();
			        updateTimerControls();
		        });
	if (m_variablesTable)
		connect(m_variablesTable, &QTableWidget::itemSelectionChanged, this,
		        [this] { updateVariableControls(); });
	auto editVariableDialogNow = [this](QString &name, QString &value, const bool allowRename,
	                                    const QSet<QString> &disallowedNames) -> bool
	{
		QDialog dialog(this);
		dialog.setWindowTitle(QStringLiteral("Variable"));
		dialog.setMinimumSize(700, 440);
		auto *dialogLayout = new QVBoxLayout(&dialog);
		auto *form         = new QFormLayout();
		auto *nameEdit     = new QLineEdit(&dialog);
		nameEdit->setText(name);
		nameEdit->setReadOnly(!allowRename);
		auto *valueEdit = new QTextEdit(&dialog);
		valueEdit->setPlainText(value);
		form->addRow(QStringLiteral("Name"), nameEdit);
		form->addRow(QStringLiteral("Contents"), valueEdit);
		dialogLayout->addLayout(form);
		auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
		connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
		connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
		dialogLayout->addWidget(buttons);
		while (true)
		{
			if (dialog.exec() != QDialog::Accepted)
				return false;

			const QString candidateName = nameEdit->text().trimmed();
			if (candidateName.isEmpty())
			{
				QMessageBox::warning(&dialog, QStringLiteral("Variables"),
				                     QStringLiteral("Variable name cannot be blank."));
				continue;
			}
			if (disallowedNames.contains(candidateName))
			{
				QMessageBox::warning(&dialog, QStringLiteral("Variables"),
				                     QStringLiteral("That variable name is already in use."));
				continue;
			}

			name  = candidateName;
			value = valueEdit->toPlainText();
			return true;
		}
	};
	if (m_addVariableButton)
		connect(m_addVariableButton, &QPushButton::clicked, this,
		        [this, editVariableDialogNow]
		        {
			        if (!m_runtime)
				        return;
			        QSet<QString>                        namesInUse;
			        const QList<WorldRuntime::Variable> &vars = m_runtime->variables();
			        for (const auto &var : vars)
				        namesInUse.insert(var.attributes.value(QStringLiteral("name")));
			        QString name;
			        QString value;
			        if (!editVariableDialogNow(name, value, true, namesInUse))
				        return;
			        WorldRuntime::Variable newVar;
			        newVar.attributes.insert(QStringLiteral("name"), name);
			        newVar.content                        = value;
			        QList<WorldRuntime::Variable> updated = vars;
			        updated.push_back(newVar);
			        m_runtime->setVariables(updated);
			        populateVariables();
			        updateVariableControls();
		        });
	if (m_editVariableButton)
		connect(m_editVariableButton, &QPushButton::clicked, this,
		        [this, editVariableDialogNow]
		        {
			        if (!m_runtime || !m_variablesTable)
				        return;
			        const int row   = m_variablesTable->currentRow();
			        const int index = rowToIndex(m_variablesTable, row);
			        if (index < 0)
				        return;
			        QList<WorldRuntime::Variable> vars = m_runtime->variables();
			        if (index >= vars.size())
				        return;
			        const QString oldName = vars.at(index).attributes.value(QStringLiteral("name"));
			        QString       name    = oldName;
			        QString       value   = vars.at(index).content;
			        QSet<QString> namesInUse;
			        for (int i = 0; i < vars.size(); ++i)
			        {
				        if (i == index)
					        continue;
				        namesInUse.insert(vars.at(i).attributes.value(QStringLiteral("name")));
			        }
			        if (!editVariableDialogNow(name, value, true, namesInUse))
				        return;
			        vars[index].attributes.insert(QStringLiteral("name"), name);
			        vars[index].content = value;
			        m_runtime->setVariables(vars);
			        populateVariables();
			        selectRowByIndex(m_variablesTable, index);
			        updateVariableControls();
		        });
	if (m_deleteVariableButton)
		connect(m_deleteVariableButton, &QPushButton::clicked, this,
		        [this]
		        {
			        if (!m_runtime || !m_variablesTable)
				        return;
			        const int row   = m_variablesTable->currentRow();
			        const int index = rowToIndex(m_variablesTable, row);
			        if (index < 0)
				        return;
			        if (!confirmRemoval(this, QStringLiteral("variable")))
				        return;
			        QList<WorldRuntime::Variable> vars = m_runtime->variables();
			        if (index >= vars.size())
				        return;
			        vars.removeAt(index);
			        m_runtime->setVariables(vars);
			        populateVariables();
			        const int lastIndex = saturatingToInt(vars.size()) - 1;
			        selectRowByIndex(m_variablesTable, std::min(index, lastIndex));
			        updateVariableControls();
		        });
	if (m_variablesTable)
		connect(m_variablesTable, &QTableWidget::cellDoubleClicked, this,
		        [this]
		        {
			        if (m_editVariableButton)
				        m_editVariableButton->click();
		        });
	if (m_findVariableButton)
		connect(m_findVariableButton, &QPushButton::clicked, this,
		        [this]
		        {
			        if (!m_runtime)
				        return;
			        const QList<WorldRuntime::Variable> vars    = m_runtime->variables();
			        auto                                rowText = [this, &vars](const int row) -> QString
			        {
				        const int index = rowToIndex(m_variablesTable, row);
				        if (index < 0 || index >= vars.size())
					        return {};
				        const WorldRuntime::Variable &var  = vars.at(index);
				        const QString                 name = var.attributes.value(QStringLiteral("name"));
				        return var.content + QLatin1Char('\t') + name;
			        };
			        doFindAdvanced(this, m_variablesTable, rowText, m_variableFindText, m_variableFindRow,
			                       m_variableFindHistory, m_variableFindMatchCase, m_variableFindRegex,
			                       m_variableFindForwards, QStringLiteral("Find variable"), false);
			        updateVariableControls();
		        });
	if (m_findNextVariableButton)
		connect(m_findNextVariableButton, &QPushButton::clicked, this,
		        [this]
		        {
			        if (!m_runtime)
				        return;
			        const QList<WorldRuntime::Variable> vars    = m_runtime->variables();
			        auto                                rowText = [this, &vars](const int row) -> QString
			        {
				        const int index = rowToIndex(m_variablesTable, row);
				        if (index < 0 || index >= vars.size())
					        return {};
				        const WorldRuntime::Variable &var  = vars.at(index);
				        const QString                 name = var.attributes.value(QStringLiteral("name"));
				        return var.content + QLatin1Char('\t') + name;
			        };
			        doFindAdvanced(this, m_variablesTable, rowText, m_variableFindText, m_variableFindRow,
			                       m_variableFindHistory, m_variableFindMatchCase, m_variableFindRegex,
			                       m_variableFindForwards, QStringLiteral("Find variable"), true);
			        updateVariableControls();
		        });
	if (m_loadVariablesButton)
		connect(m_loadVariablesButton, &QPushButton::clicked, this,
		        [this]
		        {
			        const QString startDir = m_runtime ? m_runtime->fileBrowsingDirectory() : QString();
			        const QString fileName =
			            QFileDialog::getOpenFileName(this, QStringLiteral("Variable file name"), startDir,
			                                         QStringLiteral("QMud variables (*.qdv *.mcv)"));
			        if (fileName.isEmpty())
				        return;
			        if (m_runtime)
				        m_runtime->setFileBrowsingDirectory(QFileInfo(fileName).absolutePath());
			        if (!loadVariablesFromFile(fileName))
				        return;
			        populateVariables();
			        updateVariableControls();
		        });
	if (m_saveVariablesButton)
		connect(m_saveVariablesButton, &QPushButton::clicked, this,
		        [this]
		        {
			        const QString startDir = m_runtime ? m_runtime->fileBrowsingDirectory() : QString();
			        const QString fileName =
			            QFileDialog::getSaveFileName(this, QStringLiteral("Variable file name"), startDir,
			                                         QStringLiteral("QMud variables (*.qdv *.mcv)"));
			        if (fileName.isEmpty())
				        return;
			        const QString outputPath = canonicalSavePath(fileName, QStringLiteral("qdv"));
			        if (m_runtime)
				        m_runtime->setFileBrowsingDirectory(QFileInfo(outputPath).absolutePath());
			        if (!saveVariablesToFile(outputPath))
				        return;
			        updateVariableControls();
		        });
	if (m_copyVariableButton)
		connect(m_copyVariableButton, &QPushButton::clicked, this,
		        [this]
		        {
			        if (!m_runtime || !m_variablesTable)
				        return;
			        const int row   = m_variablesTable->currentRow();
			        const int index = rowToIndex(m_variablesTable, row);
			        if (index < 0)
				        return;
			        const QList<WorldRuntime::Variable> &vars = m_runtime->variables();
			        if (index >= vars.size())
				        return;
			        const WorldRuntime::Variable &var = vars.at(index);
			        QString                       xml;
			        QTextStream                   out(&xml);
			        out << "<variables>\n";
			        out << "  <variable name=\""
			            << fixHtmlString(var.attributes.value(QStringLiteral("name"))) << "\">"
			            << fixHtmlMultilineString(var.content) << "</variable>\n";
			        out << "</variables>\n";
			        if (QClipboard *clipboard = QGuiApplication::clipboard())
				        clipboard->setText(xml);
			        updateVariableControls();
		        });
	if (m_pasteVariableButton)
		connect(m_pasteVariableButton, &QPushButton::clicked, this,
		        [this, editVariableDialogNow]
		        {
			        if (!m_runtime)
				        return;
			        const QString text = []() -> QString
			        {
				        if (QClipboard *clipboard = QGuiApplication::clipboard())
					        return clipboard->text();
				        return {};
			        }();
			        if (text.trimmed().isEmpty())
				        return;
			        QTemporaryFile temp;
			        if (!temp.open())
				        return;
			        temp.write(text.toUtf8());
			        temp.flush();
			        WorldDocument doc;
			        doc.setLoadMask(WorldDocument::XML_VARIABLES | WorldDocument::XML_NO_PLUGINS |
			                        WorldDocument::XML_IMPORT_MAIN_FILE_ONLY);
			        if (!doc.loadFromFile(temp.fileName()))
			        {
				        QMessageBox::warning(this, QStringLiteral("Paste variables"), doc.errorString());
				        return;
			        }
			        QList<WorldRuntime::Variable> combined = m_runtime->variables();
			        QSet<QString>                 namesInUse;
			        for (const auto &var : combined)
				        namesInUse.insert(var.attributes.value(QStringLiteral("name")));
			        bool changed = false;
			        for (const auto &v : doc.variables())
			        {
				        QString name  = v.attributes.value(QStringLiteral("name"));
				        QString value = v.content;
				        if (namesInUse.contains(name))
				        {
					        if (!editVariableDialogNow(name, value, true, namesInUse))
						        continue;
				        }
				        WorldRuntime::Variable rv;
				        rv.attributes = v.attributes;
				        rv.attributes.insert(QStringLiteral("name"), name);
				        rv.content = value;
				        combined.push_back(rv);
				        namesInUse.insert(name);
				        changed = true;
			        }
			        if (changed)
			        {
				        m_runtime->setVariables(combined);
				        populateVariables();
			        }
			        updateVariableControls();
		        });
	if (m_editVariablesFilter)
		connect(m_editVariablesFilter, &QPushButton::clicked, this,
		        [this]
		        {
			        QDialog dialog(this);
			        dialog.setWindowTitle(QStringLiteral("Edit variable filter"));
			        auto *dialogLayout = new QVBoxLayout(&dialog);
			        auto *edit         = new QTextEdit(&dialog);
			        edit->setPlainText(m_variableFilterText);
			        dialogLayout->addWidget(edit);
			        auto *buttons =
			            new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
			        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
			        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
			        dialogLayout->addWidget(buttons);
			        if (dialog.exec() != QDialog::Accepted)
				        return;
			        m_variableFilterText = edit->toPlainText();
			        if (m_filterVariables)
				        m_filterVariables->setChecked(true);
			        populateVariables();
			        updateVariableControls();
		        });
	if (m_filterVariables)
		connect(m_filterVariables, &QCheckBox::toggled, this,
		        [this]
		        {
			        populateVariables();
			        updateVariableControls();
		        });

	contentLayout->addWidget(m_pageTree);
	contentLayout->addWidget(m_pages, 1);
	layout->addLayout(contentLayout);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	connect(buttons, &QDialogButtonBox::accepted, this, &WorldPreferencesDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &WorldPreferencesDialog::reject);
	layout->addWidget(buttons);

	auto countTreeItems = [](const QTreeWidgetItem *item, auto &&countRef) -> int
	{
		int total = 1;
		for (int i = 0; i < item->childCount(); ++i)
			total += countRef(item->child(i), countRef);
		return total;
	};
	int treeItems = 0;
	for (int i = 0; i < m_pageTree->topLevelItemCount(); ++i)
		treeItems += countTreeItems(m_pageTree->topLevelItem(i), countTreeItems);
	int rowHeight = m_pageTree->sizeHintForRow(0);
	if (rowHeight <= 0)
		rowHeight = m_pageTree->fontMetrics().height() + 6;
	const int      treeHeight = rowHeight * treeItems + (m_pageTree->frameWidth() * 2) + 12;
	const QMargins margins    = layout->contentsMargins();
	const int      minHeight  = treeHeight + buttons->sizeHint().height() + margins.top() + margins.bottom() +
	                      (layout->spacing() * 2);
	const int minHeightWithPadding = minHeight + ((minHeight * 2) / 10);
	setMinimumHeight(qMax(minHeightWithPadding, minimumHeight()));
	int baseWidth = qMax(minimumWidth(), sizeHint().width());
	baseWidth += (baseWidth * 1) / 20;
	setMinimumWidth(qMax(baseWidth, minimumWidth()));

	populateGeneral();
	populateSound();
	populateCustomColours();
	populateLogging();
	populateAnsiColours();
	populateMacros();
	populateOutput();
	populateCommands();
	populateScripting();
	populateSendToWorld();
	populateNotes();
	populateKeypad();
	populatePaste();
	populateInfo();
	populateTriggers();
	populateAliases();
	populateTimers();
	populateVariables();
	populateAutoSay();
	populatePrinting();
	populateConnecting();
	populateMxp();
	populateChat();
	updateRuleViewModes();
}

void WorldPreferencesDialog::populateGeneral()
{
	if (!m_runtime)
		return;
	const QMap<QString, QString> &attrs = m_runtime->worldAttributes();
	if (m_worldName)
		m_worldName->setText(attrs.value(QStringLiteral("name")));
	if (m_host)
		m_host->setText(attrs.value(QStringLiteral("site")));
	if (m_port)
		m_port->setValue(attrs.value(QStringLiteral("port")).toInt());
	if (m_saveWorldAutomatically)
		m_saveWorldAutomatically->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("save_world_automatically"))));
	if (m_autosaveMinutes)
	{
		bool ok      = false;
		int  minutes = attrs.value(QStringLiteral("autosave_minutes")).toInt(&ok);
		if (!ok)
			minutes = 60;
		if (minutes < 0)
			minutes = 0;
		m_autosaveMinutes->setValue(minutes);
	}
	if (m_proxyType)
	{
		const int proxyType = attrs.value(QStringLiteral("proxy_type")).toInt();
		const int index     = m_proxyType->findData(proxyType);
		if (index >= 0)
			m_proxyType->setCurrentIndex(index);
	}
	if (m_proxyServer)
		m_proxyServer->setText(attrs.value(QStringLiteral("proxy_server")));
	if (m_proxyPort)
		m_proxyPort->setValue(attrs.value(QStringLiteral("proxy_port")).toInt());
	m_proxyUsername = attrs.value(QStringLiteral("proxy_username"));
	m_proxyPassword = attrs.value(QStringLiteral("proxy_password"));
	updateClearCachedButton();
}

void WorldPreferencesDialog::populateSound() const
{
	if (!m_runtime)
		return;
	const QMap<QString, QString> &attrs = m_runtime->worldAttributes();
	if (m_beepSound)
		m_beepSound->setText(attrs.value(QStringLiteral("beep_sound")));
	if (m_newActivitySound)
		m_newActivitySound->setText(attrs.value(QStringLiteral("new_activity_sound")));
	if (m_playSoundsInBackground)
		m_playSoundsInBackground->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("play_sounds_in_background"))));
	if (m_testBeepSound && m_beepSound)
		m_testBeepSound->setEnabled(!m_beepSound->text().isEmpty());
	if (m_testActivitySound && m_noActivitySound && m_newActivitySound)
	{
		const QString value   = m_newActivitySound->text();
		const bool    enabled = !value.isEmpty() && value != QStringLiteral("(No sound)");
		m_testActivitySound->setEnabled(enabled);
		m_noActivitySound->setEnabled(enabled);
	}
}

void WorldPreferencesDialog::populateCustomColours()
{
	if (!m_runtime || m_customColourNames.size() < MAX_CUSTOM || m_customTextSwatches.size() < MAX_CUSTOM ||
	    m_customBackSwatches.size() < MAX_CUSTOM)
		return;
	unsigned long defaultText[MAX_CUSTOM];
	unsigned long defaultBack[MAX_CUSTOM];
	setDefaultCustomColours(defaultText, defaultBack);
	QVector<QColor>                    textColours(MAX_CUSTOM);
	QVector<QColor>                    backColours(MAX_CUSTOM);
	QVector<QString>                   names(MAX_CUSTOM);
	const QList<WorldRuntime::Colour> &colours = m_runtime->colours();
	for (const WorldRuntime::Colour &colour : colours)
	{
		if (!colour.group.startsWith(QStringLiteral("custom/")))
			continue;
		bool      ok  = false;
		const int seq = colour.attributes.value(QStringLiteral("seq")).toInt(&ok);
		if (!ok || seq < 1 || seq > MAX_CUSTOM)
			continue;
		const int index = seq - 1;
		if (const QString nameValue = colour.attributes.value(QStringLiteral("name")); !nameValue.isEmpty())
			names[index] = nameValue;
		if (const QColor textColour = parseColourValue(colour.attributes.value(QStringLiteral("text")));
		    textColour.isValid())
			textColours[index] = textColour;
		if (const QColor backColour = parseColourValue(colour.attributes.value(QStringLiteral("back")));
		    backColour.isValid())
			backColours[index] = backColour;
	}
	for (int i = 0; i < MAX_CUSTOM; ++i)
	{
		if (!textColours[i].isValid())
			textColours[i] = fromColourRef(static_cast<long>(defaultText[i]));
		if (!backColours[i].isValid())
			backColours[i] = fromColourRef(static_cast<long>(defaultBack[i]));
		if (m_customColourNames[i])
		{
			const QString fallback = QStringLiteral("Custom%1").arg(i + 1);
			m_customColourNames[i]->setText(names[i].isEmpty() ? fallback : names[i]);
		}
		setSwatchButtonColour(m_customTextSwatches.value(i), textColours[i]);
		setSwatchButtonColour(m_customBackSwatches.value(i), backColours[i]);
	}
}

void WorldPreferencesDialog::populateLogging() const
{
	if (!m_runtime)
		return;
	const QMap<QString, QString> &attrs = m_runtime->worldAttributes();
	const QMap<QString, QString> &multi = m_runtime->worldMultilineAttributes();
	if (m_logOutput)
		m_logOutput->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("log_output"))));
	if (m_logInput)
		m_logInput->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("log_input"))));
	if (m_logNotes)
		m_logNotes->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("log_notes"))));
	if (m_logHtml)
	{
		const bool enabled = qmudIsEnabledFlag(attrs.value(QStringLiteral("log_html")));
		m_logHtml->setChecked(enabled);
		if (m_logInColour)
			m_logInColour->setEnabled(enabled);
		if (m_standardPreamble)
			m_standardPreamble->setEnabled(enabled);
	}
	if (m_logRaw)
		m_logRaw->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("log_raw"))));
	if (m_logInColour)
		m_logInColour->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("log_in_colour"))));
	if (m_writeWorldNameToLog)
		m_writeWorldNameToLog->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("write_world_name_to_log"))));
	if (m_logRotateMb)
	{
		bool ok    = false;
		int  value = attrs.value(QStringLiteral("log_rotate_mb")).toInt(&ok);
		if (!ok)
			value = 100;
		if (value < 0)
			value = 0;
		m_logRotateMb->setValue(value);
	}
	if (m_logRotateGzip)
		m_logRotateGzip->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("log_rotate_gzip"), QStringLiteral("1"))));
	if (m_autoLogFileName)
		m_autoLogFileName->setText(attrs.value(QStringLiteral("auto_log_file_name")));
	if (m_logFilePreamble)
		m_logFilePreamble->setPlainText(multi.value(QStringLiteral("log_file_preamble")));
	if (m_logFilePostamble)
		m_logFilePostamble->setPlainText(multi.value(QStringLiteral("log_file_postamble")));
	if (m_logLinePreambleOutput)
		m_logLinePreambleOutput->setText(attrs.value(QStringLiteral("log_line_preamble_output")));
	if (m_logLinePreambleInput)
		m_logLinePreambleInput->setText(attrs.value(QStringLiteral("log_line_preamble_input")));
	if (m_logLinePreambleNotes)
		m_logLinePreambleNotes->setText(attrs.value(QStringLiteral("log_line_preamble_notes")));
	if (m_logLinePostambleOutput)
		m_logLinePostambleOutput->setText(attrs.value(QStringLiteral("log_line_postamble_output")));
	if (m_logLinePostambleInput)
		m_logLinePostambleInput->setText(attrs.value(QStringLiteral("log_line_postamble_input")));
	if (m_logLinePostambleNotes)
		m_logLinePostambleNotes->setText(attrs.value(QStringLiteral("log_line_postamble_notes")));
}

void WorldPreferencesDialog::populateAnsiColours()
{
	if (!m_runtime || m_ansiNormalSwatches.size() < 8 || m_ansiBoldSwatches.size() < 8)
		return;
	const QMap<QString, QString> &attrs = m_runtime->worldAttributes();
	if (m_useDefaultColours)
		m_useDefaultColours->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("use_default_colours"))));
	if (m_custom16IsDefaultColour)
		m_custom16IsDefaultColour->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("custom_16_is_default_colour"))));
	QVector<QColor>                    normal(8);
	QVector<QColor>                    bold(8);
	const QList<WorldRuntime::Colour> &colours = m_runtime->colours();
	for (const WorldRuntime::Colour &colour : colours)
	{
		if (!colour.group.startsWith(QStringLiteral("ansi/")))
			continue;
		const QString subGroup = colour.group.section('/', 1, 1).trimmed().toLower();
		bool          ok       = false;
		const int     seq      = colour.attributes.value(QStringLiteral("seq")).toInt(&ok);
		if (!ok || seq < 1 || seq > 8)
			continue;
		const QColor rgb = parseColourValue(colour.attributes.value(QStringLiteral("rgb")));
		if (subGroup == QStringLiteral("normal"))
			normal[seq - 1] = rgb;
		else if (subGroup == QStringLiteral("bold"))
			bold[seq - 1] = rgb;
	}
	unsigned long defaultNormal[8];
	unsigned long defaultBold[8];
	setDefaultAnsiColours(defaultNormal, defaultBold);
	for (int i = 0; i < 8; ++i)
	{
		if (!normal[i].isValid())
			normal[i] = fromColourRef(static_cast<long>(defaultNormal[i]));
		if (!bold[i].isValid())
			bold[i] = fromColourRef(static_cast<long>(defaultBold[i]));
	}
	for (int i = 0; i < 8; ++i)
	{
		setSwatchButtonColour(m_ansiNormalSwatches.value(i), normal.value(i));
		setSwatchButtonColour(m_ansiBoldSwatches.value(i), bold.value(i));
	}
	for (int i = 0; i < 8; ++i)
	{
		if (i < m_customAnsiNormal.size() && m_customAnsiNormal[i])
		{
			QPalette pal = m_customAnsiNormal[i]->palette();
			pal.setColor(QPalette::Window, normal.value(i));
			m_customAnsiNormal[i]->setPalette(pal);
		}
		if (i < m_customAnsiBold.size() && m_customAnsiBold[i])
		{
			QPalette pal = m_customAnsiBold[i]->palette();
			pal.setColor(QPalette::Window, bold.value(i));
			m_customAnsiBold[i]->setPalette(pal);
		}
	}
	if (m_useDefaultColours)
		m_initialUseDefaultColours = m_useDefaultColours->isChecked();
	updateDefaultColoursState();
}

void WorldPreferencesDialog::populateMacros()
{
	if (!m_runtime || !m_macrosTable)
		return;
	m_macrosTable->setShowGrid(gridLinesEnabled());
	m_macrosTable->setRowCount(0);
	const QList<WorldRuntime::Macro> &macros = m_runtime->macros();
	struct MacroRow
	{
			int     index{0};
			QString name;
			QString text;
			QString action;
			QString actionKey;
	};
	QVector<MacroRow> rows;
	rows.reserve(macros.size());
	for (int i = 0; i < macros.size(); ++i)
	{
		const WorldRuntime::Macro &macro = macros.at(i);
		MacroRow                   row;
		row.index     = i;
		row.name      = macro.attributes.value(QStringLiteral("name"));
		row.text      = macro.children.value(QStringLiteral("send"));
		row.actionKey = macro.attributes.value(QStringLiteral("type"));
		if (row.actionKey == QStringLiteral("send_now"))
			row.action = QStringLiteral("Send now");
		else if (row.actionKey == QStringLiteral("insert"))
			row.action = QStringLiteral("Insert");
		else if (row.actionKey == QStringLiteral("replace"))
			row.action = QStringLiteral("Replace");
		else
			row.action = QStringLiteral("Unknown");
		rows.push_back(row);
	}
	std::ranges::sort(rows,
	                  [this](const MacroRow &a, const MacroRow &b)
	                  {
		                  const bool asc = m_macroSortAscending;
		                  QString    left;
		                  QString    right;
		                  switch (m_macroSortColumn)
		                  {
		                  case 1:
			                  left  = a.text;
			                  right = b.text;
			                  break;
		                  case 2:
			                  left  = a.action;
			                  right = b.action;
			                  break;
		                  default:
			                  left  = a.name;
			                  right = b.name;
			                  break;
		                  }
		                  const int cmp = QString::compare(left, right, Qt::CaseInsensitive);
		                  if (cmp == 0)
			                  return asc ? (a.index < b.index) : (a.index > b.index);
		                  return asc ? (cmp < 0) : (cmp > 0);
	                  });
	for (const MacroRow &macro : rows)
	{
		const int row = m_macrosTable->rowCount();
		m_macrosTable->insertRow(row);
		auto *nameItem = new QTableWidgetItem(macro.name);
		nameItem->setData(Qt::UserRole, macro.index);
		nameItem->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
		m_macrosTable->setItem(row, 0, nameItem);
		auto *typeCombo = new QComboBox(m_macrosTable);
		typeCombo->addItem(QStringLiteral("Replace"), QStringLiteral("replace"));
		typeCombo->addItem(QStringLiteral("Send now"), QStringLiteral("send_now"));
		typeCombo->addItem(QStringLiteral("Insert"), QStringLiteral("insert"));
		const int typeIndex = typeCombo->findData(macro.actionKey);
		if (typeIndex >= 0)
			typeCombo->setCurrentIndex(typeIndex);
		m_macrosTable->setCellWidget(row, 1, typeCombo);
		auto *sendItem = new QTableWidgetItem(macro.text);
		sendItem->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
		m_macrosTable->setItem(row, 2, sendItem);
	}
	if (m_macrosTable->rowCount() > 0 && m_macrosTable->currentRow() < 0)
		m_macrosTable->setCurrentCell(0, 0);
	if (QHeaderView *header = m_macrosTable->horizontalHeader())
		header->setSortIndicator(m_macroSortColumn,
		                         m_macroSortAscending ? Qt::AscendingOrder : Qt::DescendingOrder);
	if (m_useDefaultMacros)
	{
		const QMap<QString, QString> &attrs = m_runtime->worldAttributes();
		m_useDefaultMacros->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("use_default_macros"))));
	}
	if (m_useDefaultMacros)
		m_initialUseDefaultMacros = m_useDefaultMacros->isChecked();
	updateMacroControls();
}

void WorldPreferencesDialog::populateOutput()
{
	if (!m_runtime)
		return;
	const QMap<QString, QString> &attrs = m_runtime->worldAttributes();
	if (m_wrapColumn)
		m_wrapColumn->setValue(attrs.value(QStringLiteral("wrap_column")).toInt());
	if (m_maxLines)
		m_maxLines->setValue(worldMaxOutputLines(attrs));
	if (m_wrapOutput)
		m_wrapOutput->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("wrap"))));
	if (m_autoWrapWindow)
		m_autoWrapWindow->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("auto_wrap_window_width"))));
	if (m_lineInformation)
		m_lineInformation->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("line_information"))));
	if (m_startPaused)
		m_startPaused->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("start_paused"))));
	if (m_autoPause)
		m_autoPause->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("auto_pause"))));
	if (m_unpauseOnSend)
		m_unpauseOnSend->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("unpause_on_send"))));
	if (m_keepPauseAtBottomOption)
		m_keepPauseAtBottomOption->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("keep_pause_at_bottom"))));
	if (m_doNotShowOutstandingLines)
		m_doNotShowOutstandingLines->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("do_not_show_outstanding_lines"))));
	if (m_indentParas)
		m_indentParas->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("indent_paras"))));
	if (m_alternativeInverse)
		m_alternativeInverse->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("alternative_inverse"))));
	if (m_enableBeeps)
		m_enableBeeps->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("enable_beeps"))));
	if (m_disableCompression)
		m_disableCompression->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("disable_compression"))));
	if (m_flashIcon)
		m_flashIcon->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("flash_taskbar_icon"))));
	if (m_showBold)
		m_showBold->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("show_bold"))));
	if (m_showItalic)
		m_showItalic->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("show_italic"))));
	if (m_showUnderline)
		m_showUnderline->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("show_underline"))));
	if (m_useDefaultOutputFont)
		m_useDefaultOutputFont->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("use_default_output_font"))));
	if (m_outputFontName)
		m_outputFontName->setText(attrs.value(QStringLiteral("output_font_name")));
	if (m_outputFontHeight)
		m_outputFontHeight->setValue(attrs.value(QStringLiteral("output_font_height")).toInt());
	m_outputFontWeight  = attrs.value(QStringLiteral("output_font_weight")).toInt();
	m_outputFontCharset = attrs.value(QStringLiteral("output_font_charset")).toInt();
	if (m_lineSpacing)
		m_lineSpacing->setValue(attrs.value(QStringLiteral("line_spacing")).toInt());
	if (m_pixelOffset)
		m_pixelOffset->setValue(attrs.value(QStringLiteral("pixel_offset")).toInt());
	if (m_naws)
		m_naws->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("naws"))));
	if (m_terminalIdentification)
		m_terminalIdentification->setText(attrs.value(QStringLiteral("terminal_identification")));
	if (m_showConnectDisconnect)
		m_showConnectDisconnect->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("show_connect_disconnect"))));
	if (m_copySelectionToClipboard)
		m_copySelectionToClipboard->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("copy_selection_to_clipboard"))));
	if (m_autoCopyHtml)
		m_autoCopyHtml->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("auto_copy_to_clipboard_in_html"))));
	if (m_utf8)
		m_utf8->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("utf_8"))));
	if (m_carriageReturnClearsLine)
		m_carriageReturnClearsLine->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("carriage_return_clears_line"))));
	if (m_convertGaToNewline)
		m_convertGaToNewline->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("convert_ga_to_newline"))));
	if (m_sendKeepAlives)
		m_sendKeepAlives->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("send_keep_alives"))));
	if (m_persistOutputBuffer)
		m_persistOutputBuffer->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("persist_output_buffer"))));
	if (m_toolTipVisibleTime)
		m_toolTipVisibleTime->setValue(attrs.value(QStringLiteral("tool_tip_visible_time")).toInt());
	if (m_toolTipStartTime)
		m_toolTipStartTime->setValue(attrs.value(QStringLiteral("tool_tip_start_time")).toInt());
	if (m_fadeOutputBufferAfterSeconds)
		m_fadeOutputBufferAfterSeconds->setValue(
		    attrs.value(QStringLiteral("fade_output_buffer_after_seconds")).toInt());
	if (m_fadeOutputOpacityPercent)
		m_fadeOutputOpacityPercent->setValue(
		    attrs.value(QStringLiteral("fade_output_opacity_percent")).toInt());
	if (m_fadeOutputSeconds)
	{
		bool ok    = false;
		int  value = attrs.value(QStringLiteral("fade_output_seconds")).toInt(&ok);
		if (!ok || value <= 0)
			value = 8;
		m_fadeOutputSeconds->setValue(value);
	}
	updateAutoCopyHtmlState();
	updateOutputFontControls();
}

void WorldPreferencesDialog::populateCommands()
{
	if (!m_runtime)
		return;
	const QMap<QString, QString> &attrs = m_runtime->worldAttributes();
	const QMap<QString, QString> &multi = m_runtime->worldMultilineAttributes();
	if (m_arrowsChangeHistory)
		m_arrowsChangeHistory->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("arrows_change_history"))));
	if (m_arrowKeysWrap)
		m_arrowKeysWrap->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("arrow_keys_wrap"))));
	if (m_arrowRecallsPartial)
		m_arrowRecallsPartial->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("arrow_recalls_partial"))));
	if (m_altArrowRecallsPartial)
		m_altArrowRecallsPartial->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("alt_arrow_recalls_partial"))));
	if (m_keepCommandsOnSameLine)
		m_keepCommandsOnSameLine->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("keep_commands_on_same_line"))));
	if (m_confirmBeforeReplacingTyping)
		m_confirmBeforeReplacingTyping->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("confirm_before_replacing_typing"))));
	if (m_escapeDeletesInput)
		m_escapeDeletesInput->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("escape_deletes_input"))));
	if (m_doubleClickInserts)
		m_doubleClickInserts->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("double_click_inserts"))));
	if (m_doubleClickSends)
		m_doubleClickSends->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("double_click_sends"))));
	if (m_saveDeletedCommand)
		m_saveDeletedCommand->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("save_deleted_command"))));
	if (m_ctrlZToEnd)
		m_ctrlZToEnd->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("ctrl_z_goes_to_end_of_buffer"))));
	if (m_ctrlPToPrev)
		m_ctrlPToPrev->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("ctrl_p_goes_to_previous_command"))));
	if (m_ctrlNToNext)
		m_ctrlNToNext->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("ctrl_n_goes_to_next_command"))));
	if (m_ctrlBackspaceDeletesLastWord)
		m_ctrlBackspaceDeletesLastWord->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("ctrl_backspace_deletes_last_word"))));
	if (m_enableCommandStack)
		m_enableCommandStack->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("enable_command_stack"))));
	if (m_commandStackCharacter)
		m_commandStackCharacter->setText(attrs.value(QStringLiteral("command_stack_character")));
	if (m_enableSpeedWalk)
		m_enableSpeedWalk->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("enable_speed_walk"))));
	if (m_speedWalkPrefix)
		m_speedWalkPrefix->setText(attrs.value(QStringLiteral("speed_walk_prefix")));
	if (m_speedWalkFiller)
		m_speedWalkFiller->setText(attrs.value(QStringLiteral("speed_walk_filler")));
	if (m_speedWalkDelay)
		m_speedWalkDelay->setValue(attrs.value(QStringLiteral("speed_walk_delay")).toInt());
	if (m_displayMyInput)
		m_displayMyInput->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("display_my_input"))));
	if (m_historyLines)
		m_historyLines->setValue(attrs.value(QStringLiteral("history_lines")).toInt());
	if (m_persistCommandHistory)
		m_persistCommandHistory->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("persist_command_history"))));
	if (m_alwaysRecordCommandHistory)
		m_alwaysRecordCommandHistory->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("always_record_command_history"))));
	if (m_doNotAddMacrosToCommandHistory)
		m_doNotAddMacrosToCommandHistory->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("do_not_add_macros_to_command_history"))));
	if (m_autoResizeCommandWindow)
		m_autoResizeCommandWindow->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("auto_resize_command_window"))));
	if (m_autoResizeMinimumLines)
	{
		bool ok    = false;
		int  value = attrs.value(QStringLiteral("auto_resize_minimum_lines")).toInt(&ok);
		if (!ok || value <= 0)
			value = 1;
		m_autoResizeMinimumLines->setValue(value);
	}
	if (m_autoResizeMaximumLines)
	{
		bool ok    = false;
		int  value = attrs.value(QStringLiteral("auto_resize_maximum_lines")).toInt(&ok);
		if (!ok || value <= 0)
			value = 20;
		m_autoResizeMaximumLines->setValue(value);
	}
	if (m_autoRepeat)
		m_autoRepeat->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("auto_repeat"))));
	if (m_translateGerman)
		m_translateGerman->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("translate_german"))));
	if (m_spellCheckOnSend)
		m_spellCheckOnSend->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("spell_check_on_send"))));
	if (m_lowerCaseTabCompletion)
		m_lowerCaseTabCompletion->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("lower_case_tab_completion"))));
	if (m_translateBackslash)
		m_translateBackslash->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("translate_backslash_sequences"))));
	if (m_tabCompletionDefaults)
		m_tabCompletionDefaults->setPlainText(multi.value(QStringLiteral("tab_completion_defaults")));
	if (m_tabCompletionLines)
		m_tabCompletionLines->setValue(attrs.value(QStringLiteral("tab_completion_lines")).toInt());
	if (m_tabCompletionSpace)
		m_tabCompletionSpace->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("tab_completion_space"))));
	if (m_useDefaultInputFont)
		m_useDefaultInputFont->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("use_default_input_font"))));
	if (m_inputFontName)
		m_inputFontName->setText(attrs.value(QStringLiteral("input_font_name")));
	if (m_inputFontHeight)
		m_inputFontHeight->setValue(attrs.value(QStringLiteral("input_font_height")).toInt());
	m_inputFontWeight  = attrs.value(QStringLiteral("input_font_weight")).toInt();
	m_inputFontItalic  = qmudIsEnabledFlag(attrs.value(QStringLiteral("input_font_italic")));
	m_inputFontCharset = attrs.value(QStringLiteral("input_font_charset")).toInt();
	if (m_inputFontStyle && m_inputFontHeight)
		m_inputFontStyle->setText(
		    formatFontStyleText(m_inputFontHeight->value(), m_inputFontWeight, m_inputFontItalic));
	if (m_noEchoOff)
		m_noEchoOff->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("no_echo_off"))));
	if (m_enableSpamPrevention)
		m_enableSpamPrevention->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("enable_spam_prevention"))));
	if (m_spamLineCount)
		m_spamLineCount->setValue(attrs.value(QStringLiteral("spam_line_count")).toInt());
	if (m_spamMessage)
		m_spamMessage->setText(attrs.value(QStringLiteral("spam_message")));
	if (m_inputTextColour)
		m_inputTextColour->setText(attrs.value(QStringLiteral("input_text_colour")));
	if (m_inputBackColour)
		m_inputBackColour->setText(attrs.value(QStringLiteral("input_background_colour")));
	if (m_echoColour)
	{
		bool ok    = false;
		int  index = attrs.value(QStringLiteral("echo_colour")).toInt(&ok);
		if (!ok)
			index = 0;
		m_echoColour->setCurrentIndex(qBound(0, index, MAX_CUSTOM));
	}
	if (m_commandTextSwatch)
		setSwatchButtonColour(m_commandTextSwatch,
		                      parseColourValue(attrs.value(QStringLiteral("input_text_colour"))));
	if (m_commandBackSwatch)
		setSwatchButtonColour(m_commandBackSwatch,
		                      parseColourValue(attrs.value(QStringLiteral("input_background_colour"))));
	updateCommandAutoResizeControls();
	updateInputFontControls();
	updateSpellCheckState();
}

void WorldPreferencesDialog::populateScripting() const
{
	if (!m_runtime)
		return;
	const QMap<QString, QString> &attrs = m_runtime->worldAttributes();
	if (m_enableScripts)
		m_enableScripts->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("enable_scripts"))));
	if (m_scriptLanguage)
	{
		const QString lang  = attrs.value(QStringLiteral("script_language"), QStringLiteral("Lua"));
		const int     index = m_scriptLanguage->findData(lang);
		if (index >= 0)
			m_scriptLanguage->setCurrentIndex(index);
		else
			m_scriptLanguage->setCurrentIndex(0);
	}
	if (m_scriptFile)
		m_scriptFile->setText(attrs.value(QStringLiteral("script_filename")));
	if (m_scriptPrefix)
		m_scriptPrefix->setText(attrs.value(QStringLiteral("script_prefix")));
	if (m_scriptEditor)
		m_scriptEditor->setText(attrs.value(QStringLiteral("script_editor")));
	if (m_editorWindowName)
		m_editorWindowName->setText(attrs.value(QStringLiteral("editor_window_name")));
	if (m_editScriptWithNotepad)
	{
		m_editScriptWithNotepad->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("edit_script_with_notepad"))));
		const bool checked = m_editScriptWithNotepad->isChecked();
		if (m_chooseScriptEditor)
			m_chooseScriptEditor->setEnabled(!checked);
		if (m_scriptEditor)
			m_scriptEditor->setEnabled(!checked);
	}
	if (m_scriptReloadOption)
	{
		const int value = attrs.value(QStringLiteral("script_reload_option")).toInt();
		const int index = m_scriptReloadOption->findData(value);
		if (index >= 0)
			m_scriptReloadOption->setCurrentIndex(index);
	}
	if (m_scriptTextColour)
	{
		const QString raw   = attrs.value(QStringLiteral("note_text_colour"));
		bool          ok    = false;
		int           value = raw.toInt(&ok);
		if (!ok)
		{
			const QColor decoded = parseColourValue(raw);
			if (decoded.isValid())
			{
				value = static_cast<int>(toColourRef(decoded));
				ok    = true;
			}
		}
		int index = 0;
		if (ok)
		{
			if (value == WorldRuntime::kSameColour || value < 0)
				index = 0;
			else
				index = value + 1;
		}
		else
			index = 5;
		const int maxIndex = m_scriptTextColour->count() - 1;
		if (maxIndex >= 0)
		{
			index = qBound(0, index, maxIndex);
			m_scriptTextColour->setCurrentIndex(index);
			updateScriptNoteSwatches();
		}
	}
	if (m_warnIfScriptingInactive)
		m_warnIfScriptingInactive->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("warn_if_scripting_inactive"))));
	if (m_scriptErrorsToOutput)
		m_scriptErrorsToOutput->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("script_errors_to_output_window"))));
	if (m_logScriptErrors)
		m_logScriptErrors->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("log_script_errors"))));
	if (m_scriptIsActive)
	{
		const bool    enabled  = qmudIsEnabledFlag(attrs.value(QStringLiteral("enable_scripts")));
		const QString language = attrs.value(QStringLiteral("script_language"), QStringLiteral("Lua"));
		const bool    lua      = language.compare(QStringLiteral("lua"), Qt::CaseInsensitive) == 0;
		m_scriptIsActive->setText(enabled && lua ? QStringLiteral("(active)")
		                                         : QStringLiteral("(not active)"));
	}
	if (m_scriptExecutionTime)
		m_scriptExecutionTime->setText(
		    QStringLiteral("Time spent: %1 seconds.").arg(m_runtime->scriptTimeSeconds(), 0, 'f', 6));
	if (m_onWorldOpen)
		m_onWorldOpen->setText(attrs.value(QStringLiteral("on_world_open")));
	if (m_onWorldClose)
		m_onWorldClose->setText(attrs.value(QStringLiteral("on_world_close")));
	if (m_onWorldConnect)
		m_onWorldConnect->setText(attrs.value(QStringLiteral("on_world_connect")));
	if (m_onWorldDisconnect)
		m_onWorldDisconnect->setText(attrs.value(QStringLiteral("on_world_disconnect")));
	if (m_onWorldSave)
		m_onWorldSave->setText(attrs.value(QStringLiteral("on_world_save")));
	if (m_onWorldGetFocus)
		m_onWorldGetFocus->setText(attrs.value(QStringLiteral("on_world_get_focus")));
	if (m_onWorldLoseFocus)
		m_onWorldLoseFocus->setText(attrs.value(QStringLiteral("on_world_lose_focus")));
	if (m_onMxpStart)
		m_onMxpStart->setText(attrs.value(QStringLiteral("on_mxp_start")));
	if (m_onMxpStop)
		m_onMxpStop->setText(attrs.value(QStringLiteral("on_mxp_stop")));
	if (m_onMxpOpenTag)
		m_onMxpOpenTag->setText(attrs.value(QStringLiteral("on_mxp_open_tag")));
	if (m_onMxpCloseTag)
		m_onMxpCloseTag->setText(attrs.value(QStringLiteral("on_mxp_close_tag")));
	if (m_onMxpSetVariable)
		m_onMxpSetVariable->setText(attrs.value(QStringLiteral("on_mxp_set_variable")));
	if (m_onMxpError)
		m_onMxpError->setText(attrs.value(QStringLiteral("on_mxp_error")));
	if (m_chooseScriptEditor && m_editScriptWithNotepad)
		m_chooseScriptEditor->setEnabled(!m_editScriptWithNotepad->isChecked());
	if (m_editScriptFile && m_scriptFile)
		m_editScriptFile->setEnabled(!m_scriptFile->text().trimmed().isEmpty());
}

void WorldPreferencesDialog::updateScriptNoteSwatches() const
{
	if (!m_scriptTextColour || !m_scriptTextSwatch || !m_scriptBackSwatch)
		return;
	const int  index = m_scriptTextColour->currentIndex() - 1;
	const bool valid =
	    index >= 0 && index < m_customTextSwatches.size() && index < m_customBackSwatches.size();
	m_scriptTextSwatch->setEnabled(valid);
	m_scriptBackSwatch->setEnabled(valid);
	m_scriptTextSwatch->setVisible(valid);
	m_scriptBackSwatch->setVisible(valid);
	if (!valid)
	{
		return;
	}
	setSwatchButtonColour(m_scriptTextSwatch, swatchButtonColour(m_customTextSwatches.value(index)));
	setSwatchButtonColour(m_scriptBackSwatch, swatchButtonColour(m_customBackSwatches.value(index)));
}

void WorldPreferencesDialog::calculateMemoryUsage(const bool allowProgress)
{
	if (!m_runtime || !m_infoMemoryUsed || !m_infoBufferLines)
		return;

	const QVector<WorldRuntime::LineEntry> &lines     = m_runtime->lines();
	const int                               lineCount = saturatingToInt(lines.size());
	const int                               maxLines  = worldMaxOutputLines(m_runtime->worldAttributes());
	QProgressDialog                        *progress  = nullptr;
	if (allowProgress && lineCount > 1000)
	{
		progress = new QProgressDialog(QStringLiteral("Calculating memory usage..."),
		                               QStringLiteral("Cancel"), 0, lineCount, this);
		progress->setWindowTitle(QStringLiteral("Memory used by output buffer"));
		progress->setMinimumDuration(0);
		progress->setValue(0);
	}

	long long     totalBytes     = 0;
	int           styleCount     = 0;
	int           actionRefCount = 0;
	QSet<QString> actionKeys;

	for (int i = 0; i < lineCount; ++i)
	{
		const WorldRuntime::LineEntry &entry = lines.at(i);
		totalBytes += static_cast<long long>(sizeof(WorldRuntime::LineEntry));
		totalBytes += entry.text.capacity() * static_cast<long long>(sizeof(QChar));
		totalBytes += entry.spans.capacity() * static_cast<long long>(sizeof(WorldRuntime::StyleSpan));
		styleCount += saturatingToInt(entry.spans.size());
		for (const auto &span : entry.spans)
		{
			if (span.actionType == WorldRuntime::ActionNone)
				continue;
			actionRefCount++;
			actionKeys.insert(QStringLiteral("%1:%2").arg(span.actionType).arg(span.action));
		}

		if (progress && (i & 63) == 0)
		{
			progress->setValue(i);
			if (progress->wasCanceled())
			{
				progress->deleteLater();
				return;
			}
		}
	}

	if (progress)
	{
		progress->setValue(lineCount);
		progress->deleteLater();
	}

	QString bufferText;
	if (maxLines > 0)
		bufferText = QStringLiteral("%1 / %2").arg(lineCount).arg(maxLines);
	else
		bufferText = QString::number(lineCount);
	if (styleCount > 0)
		bufferText += QStringLiteral(" (%1 styles)").arg(styleCount);
	m_infoBufferLines->setText(bufferText);

	QString memoryText;
	if (totalBytes < (1024LL * 1024LL))
		memoryText = QStringLiteral("%1 Kb").arg(totalBytes / 1024);
	else
		memoryText =
		    QStringLiteral("%1 Mb").arg(static_cast<double>(totalBytes) / (1024.0 * 1024.0), 0, 'f', 1);
	if (lineCount > 0)
		memoryText = QStringLiteral("%1 (%2 bytes/line)").arg(memoryText).arg(totalBytes / lineCount);
	m_infoMemoryUsed->setText(memoryText);

	if (m_infoMxpActionsCached)
		m_infoMxpActionsCached->setText(QString::number(actionKeys.size()));
	if (m_infoMxpReferenceCount)
		m_infoMxpReferenceCount->setText(QString::number(actionRefCount));
	if (m_infoCalculateMemory)
		m_infoCalculateMemory->setEnabled(false);
}

void WorldPreferencesDialog::updateClearCachedButton() const
{
	if (!m_clearCachedButton)
		return;
	bool enabled = false;
	if (m_runtime)
		enabled = m_runtime->hasCachedIp() || !m_runtime->peerAddressString().trimmed().isEmpty();
	m_clearCachedButton->setEnabled(enabled);
}

void WorldPreferencesDialog::populateSendToWorld() const
{
	if (!m_runtime)
		return;
	const QMap<QString, QString> &attrs = m_runtime->worldAttributes();
	const QMap<QString, QString> &multi = m_runtime->worldMultilineAttributes();
	if (m_sendToWorldFilePreamble)
		m_sendToWorldFilePreamble->setPlainText(multi.value(QStringLiteral("send_to_world_file_preamble")));
	if (m_sendToWorldFilePostamble)
		m_sendToWorldFilePostamble->setPlainText(multi.value(QStringLiteral("send_to_world_file_postamble")));
	if (m_sendToWorldLinePreamble)
		m_sendToWorldLinePreamble->setText(attrs.value(QStringLiteral("send_to_world_line_preamble")));
	if (m_sendToWorldLinePostamble)
		m_sendToWorldLinePostamble->setText(attrs.value(QStringLiteral("send_to_world_line_postamble")));
	if (m_sendConfirm)
		m_sendConfirm->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("confirm_on_send"))));
	if (m_sendCommentedSoftcode)
		m_sendCommentedSoftcode->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("send_file_commented_softcode"))));
	if (m_sendLineDelay)
		m_sendLineDelay->setValue(attrs.value(QStringLiteral("send_file_delay")).toInt());
	if (m_sendDelayPerLines)
		m_sendDelayPerLines->setValue(attrs.value(QStringLiteral("send_file_delay_per_lines")).toInt());
	if (m_sendEcho)
		m_sendEcho->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("send_echo"))));
}

void WorldPreferencesDialog::populateNotes() const
{
	if (!m_runtime || !m_notes)
		return;
	const QMap<QString, QString> &multi = m_runtime->worldMultilineAttributes();
	m_notes->setPlainText(multi.value(QStringLiteral("notes")));
}

void WorldPreferencesDialog::populateKeypad()
{
	if (!m_runtime)
		return;
	const QMap<QString, QString> &attrs = m_runtime->worldAttributes();
	if (m_keypadEnabled)
		m_keypadEnabled->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("keypad_enable"))));
	if (m_keypadControl)
		m_keypadControl->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("keypad_ctrl_view"))));
	static const QMap<QString, QString> keypadDefaults = {
	    {QStringLiteral("/"), QStringLiteral("inv")  },
        {QStringLiteral("*"), QStringLiteral("score")},
	    {QStringLiteral("-"), QStringLiteral("up")   },
        {QStringLiteral("+"), QStringLiteral("down") },
	    {QStringLiteral("7"), QStringLiteral("nw")   },
        {QStringLiteral("8"), QStringLiteral("north")},
	    {QStringLiteral("9"), QStringLiteral("ne")   },
        {QStringLiteral("4"), QStringLiteral("west") },
	    {QStringLiteral("5"), QStringLiteral("look") },
        {QStringLiteral("6"), QStringLiteral("east") },
	    {QStringLiteral("1"), QStringLiteral("sw")   },
        {QStringLiteral("2"), QStringLiteral("south")},
	    {QStringLiteral("3"), QStringLiteral("se")   },
        {QStringLiteral("0"), QStringLiteral("who")  },
	    {QStringLiteral("."), QStringLiteral("hide") }
    };
	m_keypadValues.clear();
	const QList<WorldRuntime::Keypad> &entries = m_runtime->keypadEntries();
	for (const WorldRuntime::Keypad &entry : entries)
	{
		const QString name = entry.attributes.value(QStringLiteral("name"));
		if (name.isEmpty())
			continue;
		m_keypadValues.insert(name, entry.content);
	}
	for (auto it = keypadDefaults.constBegin(); it != keypadDefaults.constEnd(); ++it)
	{
		if (!m_keypadValues.contains(it.key()))
			m_keypadValues.insert(it.key(), it.value());
	}
	loadKeypadFields(m_keypadControl && m_keypadControl->isChecked());
}

void WorldPreferencesDialog::populatePaste() const
{
	if (!m_runtime)
		return;
	const QMap<QString, QString> &attrs = m_runtime->worldAttributes();
	const QMap<QString, QString> &multi = m_runtime->worldMultilineAttributes();
	if (m_pastePreamble)
		m_pastePreamble->setPlainText(multi.value(QStringLiteral("paste_preamble")));
	if (m_pastePostamble)
		m_pastePostamble->setPlainText(multi.value(QStringLiteral("paste_postamble")));
	if (m_pasteLinePreamble)
		m_pasteLinePreamble->setText(attrs.value(QStringLiteral("paste_line_preamble")));
	if (m_pasteLinePostamble)
		m_pasteLinePostamble->setText(attrs.value(QStringLiteral("paste_line_postamble")));
	if (m_confirmOnPaste)
		m_confirmOnPaste->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("confirm_on_paste"))));
	if (m_commentedSoftcodePaste)
		m_commentedSoftcodePaste->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("paste_commented_softcode"))));
	if (m_pasteLineDelay)
		m_pasteLineDelay->setValue(attrs.value(QStringLiteral("paste_delay")).toInt());
	if (m_pasteDelayPerLines)
		m_pasteDelayPerLines->setValue(attrs.value(QStringLiteral("paste_delay_per_lines")).toInt());
	if (m_pasteEcho)
		m_pasteEcho->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("paste_echo"))));
}

void WorldPreferencesDialog::populateInfo()
{
	if (!m_runtime)
		return;
	if (m_infoWorldFile)
		m_infoWorldFile->setText(m_runtime->worldFilePath());
	if (m_infoWorldFileVersion)
		m_infoWorldFileVersion->setText(QString::number(m_runtime->worldFileVersion()));
	if (m_infoQmudVersion)
		m_infoQmudVersion->setText(m_runtime->qmudVersion());
	if (m_infoWorldId)
		m_infoWorldId->setText(m_runtime->worldAttributes().value(QStringLiteral("id")));
	if (m_infoDateSaved)
		m_infoDateSaved->setText(
		    m_runtime->dateSaved().isValid()
		        ? m_runtime->dateSaved().toString(QStringLiteral("dddd, MMMM dd, yyyy, h:mm AP"))
		        : QStringLiteral("-"));

	const QVector<WorldRuntime::LineEntry> &lines     = m_runtime->lines();
	const int                               lineCount = saturatingToInt(lines.size());
	const int                               maxLines  = worldMaxOutputLines(m_runtime->worldAttributes());
	if (m_infoBufferLines)
	{
		if (maxLines > 0)
			m_infoBufferLines->setText(QStringLiteral("%1 / %2").arg(lineCount).arg(maxLines));
		else
			m_infoBufferLines->setText(QString::number(lineCount));
	}
	if (m_infoMemoryUsed)
		m_infoMemoryUsed->setText(QStringLiteral("-"));

	if (m_infoConnectionTime)
	{
		const QDateTime connected = m_runtime->connectTime();
		m_infoConnectionTime->setText(connected.isValid()
		                                  ? connected.toString(QStringLiteral("dddd, MMMM dd, yyyy, h:mm AP"))
		                                  : QStringLiteral("n/a"));
	}
	if (m_infoConnectionDuration)
	{
		const QDateTime connected = m_runtime->connectTime();
		if (connected.isValid())
		{
			qint64       secs = connected.secsTo(QDateTime::currentDateTime());
			const qint64 days = secs / 86400;
			secs %= 86400;
			const qint64 hours = secs / 3600;
			secs %= 3600;
			const qint64 mins = secs / 60;
			secs %= 60;
			m_infoConnectionDuration->setText(
			    QStringLiteral("%1d %2h %3m %4s").arg(days).arg(hours).arg(mins).arg(secs));
		}
		else
			m_infoConnectionDuration->setText(QStringLiteral("n/a"));
	}
	qint64 totalTriggerMatches = 0;
	qint64 totalAliasMatches   = 0;
	qint64 totalTimerFired     = 0;
	for (const auto &triggers = m_runtime->triggers(); const auto &tr : triggers)
		totalTriggerMatches += tr.matched;
	for (const auto &aliases = m_runtime->aliases(); const auto &al : aliases)
		totalAliasMatches += al.matched;
	for (const auto &timers = m_runtime->timers(); const auto &tm : timers)
		totalTimerFired += tm.firedCount;

	if (m_infoAliases)
		m_infoAliases->setText(
		    QStringLiteral("%1   (%2 used)").arg(m_runtime->aliasCount()).arg(totalAliasMatches));
	if (m_infoTriggers)
		m_infoTriggers->setText(
		    QStringLiteral("%1   (%2 matched)").arg(m_runtime->triggerCount()).arg(totalTriggerMatches));
	if (m_infoTimers)
		m_infoTimers->setText(
		    QStringLiteral("%1   (%2 fired)").arg(m_runtime->timerCount()).arg(totalTimerFired));

	if (m_infoBytesSent)
	{
		const qint64 bytes = m_runtime->bytesOut();
		const qint64 kb    = bytes / 1024;
		m_infoBytesSent->setText(QStringLiteral("%1 bytes (%2 Kb)").arg(bytes).arg(kb));
	}
	if (m_infoBytesReceived)
	{
		const qint64 bytes = m_runtime->bytesIn();
		const qint64 kb    = bytes / 1024;
		m_infoBytesReceived->setText(QStringLiteral("%1 bytes (%2 Kb)").arg(bytes).arg(kb));
	}

	if (m_infoTriggerTimeTaken)
		m_infoTriggerTimeTaken->setText(
		    QStringLiteral("%1 seconds.").arg(m_runtime->triggerTimeSeconds(), 0, 'f', 6));
	if (m_infoIpAddress)
	{
		if (const QString ip = m_runtime->peerAddressString().trimmed(); !ip.isEmpty())
			m_infoIpAddress->setText(ip);
		else
			m_infoIpAddress->setText(QStringLiteral("(unknown)"));
	}

	if (m_infoMxpErrors)
		m_infoMxpErrors->setText(QString::number(m_runtime->mxpErrorCount()));
	if (m_infoMxpTagsReceived)
		m_infoMxpTagsReceived->setText(QString::number(m_runtime->mxpTagCount()));
	if (m_infoMxpEntitiesReceived)
		m_infoMxpEntitiesReceived->setText(QString::number(m_runtime->mxpEntityCount()));
	if (m_infoMxpMudElements)
		m_infoMxpMudElements->setText(QString::number(m_runtime->customElementCount()));
	if (m_infoMxpMudEntities)
		m_infoMxpMudEntities->setText(QString::number(m_runtime->customEntityCount()));
	if (m_infoMxpBuiltinElements)
		m_infoMxpBuiltinElements->setText(QString::number(WorldRuntime::mxpBuiltinElementCount()));
	if (m_infoMxpBuiltinEntities)
		m_infoMxpBuiltinEntities->setText(QString::number(WorldRuntime::mxpBuiltinEntityCount()));
	if (m_infoMxpUnclosedTags)
		m_infoMxpUnclosedTags->setText(QString::number(m_runtime->mxpOpenTagCount()));

	int           actionRefCount = 0;
	QSet<QString> actionKeys;
	for (const auto &entry : lines)
	{
		for (const auto &span : entry.spans)
		{
			if (span.actionType == WorldRuntime::ActionNone)
				continue;
			actionRefCount++;
			actionKeys.insert(QStringLiteral("%1:%2").arg(span.actionType).arg(span.action));
		}
	}
	if (m_infoMxpActionsCached)
		m_infoMxpActionsCached->setText(QString::number(actionKeys.size()));
	if (m_infoMxpReferenceCount)
		m_infoMxpReferenceCount->setText(QString::number(actionRefCount));

	const qint64 compressed = m_runtime->totalCompressedBytes();
	if (const qint64 uncompressed = m_runtime->totalUncompressedBytes(); uncompressed > 0)
	{
		const double ratio     = static_cast<double>(compressed) / static_cast<double>(uncompressed) * 100.0;
		QString      ratioText = QStringLiteral("%1% (lower is better)").arg(ratio, 0, 'f', 1);
		if (const int mccpType = m_runtime->mccpType(); mccpType == 1)
			ratioText += QStringLiteral(" MCCP v 1");
		else if (mccpType == 2)
			ratioText += QStringLiteral(" MCCP v 2");
		if (m_infoCompressionRatio)
			m_infoCompressionRatio->setText(ratioText);
		if (m_infoCompressedIn)
			m_infoCompressedIn->setText(QString::number(compressed));
		if (m_infoCompressedOut)
			m_infoCompressedOut->setText(QString::number(uncompressed));
	}
	else
	{
		if (m_infoCompressionRatio)
			m_infoCompressionRatio->setText(QStringLiteral("(world is not using compression)"));
		if (m_infoCompressedIn)
			m_infoCompressedIn->setText(QStringLiteral("n/a"));
		if (m_infoCompressedOut)
			m_infoCompressedOut->setText(QStringLiteral("n/a"));
	}

	if (m_infoTimeTakenCompressing)
	{
		const bool compressActive = m_runtime->mccpType() > 0;
		m_infoTimeTakenCompressing->setText(compressActive ? QStringLiteral("(MCCP active)")
		                                                   : QStringLiteral("(MCCP not active)"));
	}

	if (m_infoCalculateMemory)
	{
		m_infoCalculateMemory->setEnabled(true);
		if (lineCount <= 1000)
			calculateMemoryUsage(false);
	}
}

void WorldPreferencesDialog::populateTriggers()
{
	if (!m_runtime || !m_triggersTable)
		return;
	m_triggersTable->setShowGrid(gridLinesEnabled());
	const QList<WorldRuntime::Trigger> &triggers = m_runtime->triggers();
	QSet<QString>                       pluginOwnedSignatures;
	for (const auto &plugin : m_runtime->plugins())
	{
		for (const auto &trigger : plugin.triggers)
		{
			pluginOwnedSignatures.insert(serializeStringMap(trigger.attributes) + QLatin1Char('\x1d') +
			                             serializeStringMap(trigger.children));
		}
	}
	m_triggersTable->setRowCount(0);
	const QMap<QString, QString> &multi = m_runtime->worldMultilineAttributes();
	if (!m_triggerFilterLoaded)
	{
		m_triggerFilterText   = multi.value(QStringLiteral("filter_triggers"));
		m_triggerFilterLoaded = true;
	}
	const bool filterEnabled = m_filterTriggers && m_filterTriggers->isChecked();
#ifdef QMUD_ENABLE_LUA_SCRIPTING
	LuaFilterRunner filter(filterEnabled ? m_triggerFilterText : QString(), this);
	const bool      useLuaFilter = filterEnabled && filter.isValid();
#endif
	QList<int> visibleIndices;
	for (int i = 0; i < triggers.size(); ++i)
	{
		const WorldRuntime::Trigger &tr = triggers.at(i);
		if (const QString signature =
		        serializeStringMap(tr.attributes) + QLatin1Char('\x1d') + serializeStringMap(tr.children);
		    pluginOwnedSignatures.contains(signature))
			continue;
		bool include = true;
		if (filterEnabled)
		{
#ifdef QMUD_ENABLE_LUA_SCRIPTING
			if (useLuaFilter)
			{
				const QString name     = tr.attributes.value(QStringLiteral("name"));
				auto          pushInfo = [&tr](lua_State *L)
				{
					auto pushString = [L](const char *key, const QString &value)
					{
						lua_pushstring(L, key);
						lua_pushstring(L, value.toUtf8().constData());
						lua_settable(L, -3);
					};
					auto pushBool = [L](const char *key, const bool value)
					{
						lua_pushstring(L, key);
						lua_pushboolean(L, value ? 1 : 0);
						lua_settable(L, -3);
					};
					auto pushNumber = [L](const char *key, const double value)
					{
						lua_pushstring(L, key);
						lua_pushnumber(L, value);
						lua_settable(L, -3);
					};

					const QMap<QString, QString> &attrs    = tr.attributes;
					const QMap<QString, QString> &children = tr.children;
					lua_newtable(L);
					pushString("match", attrs.value(QStringLiteral("match")));
					pushString("name", attrs.value(QStringLiteral("name")));
					pushString("group", attrs.value(QStringLiteral("group")));
					pushString("script", attrs.value(QStringLiteral("script")));
					pushString("send", children.value(QStringLiteral("send")));
					pushString("variable", attrs.value(QStringLiteral("variable")));
					pushNumber("sequence", attrs.value(QStringLiteral("sequence")).toDouble());
					pushNumber("send_to", attrs.value(QStringLiteral("send_to")).toDouble());
					pushBool("enabled", qmudIsEnabledFlag(attrs.value(QStringLiteral("enabled"))));
					pushBool("ignore_case", qmudIsEnabledFlag(attrs.value(QStringLiteral("ignore_case"))));
					pushBool("omit_from_output",
					         qmudIsEnabledFlag(attrs.value(QStringLiteral("omit_from_output"))));
					pushBool("omit_from_log",
					         qmudIsEnabledFlag(attrs.value(QStringLiteral("omit_from_log"))));
					pushBool("keep_evaluating",
					         qmudIsEnabledFlag(attrs.value(QStringLiteral("keep_evaluating"))));
					pushBool("regexp", qmudIsEnabledFlag(attrs.value(QStringLiteral("regexp"))));
					pushBool("repeat", qmudIsEnabledFlag(attrs.value(QStringLiteral("repeat"))));
					pushBool("expand_variables",
					         qmudIsEnabledFlag(attrs.value(QStringLiteral("expand_variables"))));
					pushBool("temporary", qmudIsEnabledFlag(attrs.value(QStringLiteral("temporary"))));
					pushBool("multi_line", qmudIsEnabledFlag(attrs.value(QStringLiteral("multi_line"))));
					pushNumber("lines_to_match", attrs.value(QStringLiteral("lines_to_match")).toDouble());
					pushBool("lowercase_wildcard",
					         qmudIsEnabledFlag(attrs.value(QStringLiteral("lowercase_wildcard"))));
					pushBool("one_shot", qmudIsEnabledFlag(attrs.value(QStringLiteral("one_shot"))));
					pushNumber("clipboard_arg", attrs.value(QStringLiteral("clipboard_arg")).toDouble());
					pushBool("match_text_colour",
					         qmudIsEnabledFlag(attrs.value(QStringLiteral("match_text_colour"))));
					pushBool("match_back_colour",
					         qmudIsEnabledFlag(attrs.value(QStringLiteral("match_back_colour"))));
					pushBool("match_bold", qmudIsEnabledFlag(attrs.value(QStringLiteral("match_bold"))));
					pushBool("match_italic", qmudIsEnabledFlag(attrs.value(QStringLiteral("match_italic"))));
					pushBool("match_inverse",
					         qmudIsEnabledFlag(attrs.value(QStringLiteral("match_inverse"))));
					pushBool("match_underline",
					         qmudIsEnabledFlag(attrs.value(QStringLiteral("match_underline"))));
					pushNumber("text_colour", attrs.value(QStringLiteral("text_colour")).toDouble());
					pushNumber("back_colour", attrs.value(QStringLiteral("back_colour")).toDouble());
					pushNumber("custom_colour", attrs.value(QStringLiteral("custom_colour")).toDouble());
					pushNumber("colour_change_type",
					           attrs.value(QStringLiteral("colour_change_type")).toDouble());
					pushBool("make_bold", qmudIsEnabledFlag(attrs.value(QStringLiteral("make_bold"))));
					pushBool("make_italic", qmudIsEnabledFlag(attrs.value(QStringLiteral("make_italic"))));
					pushBool("make_underline",
					         qmudIsEnabledFlag(attrs.value(QStringLiteral("make_underline"))));
					pushString("sound", attrs.value(QStringLiteral("sound")));
					pushBool("sound_if_inactive",
					         qmudIsEnabledFlag(attrs.value(QStringLiteral("sound_if_inactive"))));
					pushString("other_text_colour", attrs.value(QStringLiteral("other_text_colour")));
					pushString("other_back_colour", attrs.value(QStringLiteral("other_back_colour")));
					pushNumber("user", attrs.value(QStringLiteral("user")).toDouble());

					pushNumber("invocation_count", tr.invocationCount);
					pushNumber("times_matched", tr.matched);
					if (tr.lastMatched.isValid())
						pushString("when_matched", tr.lastMatched.toString(Qt::ISODate));
					pushBool("included", tr.included);
					pushNumber("match_attempts", tr.matchAttempts);
					pushString("last_match", tr.lastMatchTarget);
					pushBool("script_valid", !attrs.value(QStringLiteral("script")).isEmpty());
					pushBool("executing", tr.executingScript);

					lua_pushstring(L, "attributes");
					lua_newtable(L);
					for (auto it = attrs.begin(); it != attrs.end(); ++it)
					{
						lua_pushstring(L, it.key().toUtf8().constData());
						lua_pushstring(L, it.value().toUtf8().constData());
						lua_settable(L, -3);
					}
					lua_settable(L, -3);

					lua_pushstring(L, "children");
					lua_newtable(L);
					for (auto it = children.begin(); it != children.end(); ++it)
					{
						lua_pushstring(L, it.key().toUtf8().constData());
						lua_pushstring(L, it.value().toUtf8().constData());
						lua_settable(L, -3);
					}
					lua_settable(L, -3);
				};
				include = filter.matches(name, pushInfo);
			}
#else
			const QString text = m_triggerFilterText.trimmed();
			if (!text.isEmpty())
			{
				QString haystack = tr.attributes.value(QStringLiteral("match"));
				haystack += QLatin1Char('\t') + tr.children.value(QStringLiteral("send"));
				haystack += QLatin1Char('\t') + tr.attributes.value(QStringLiteral("name"));
				haystack += QLatin1Char('\t') + tr.attributes.value(QStringLiteral("group"));
				haystack += QLatin1Char('\t') + tr.attributes.value(QStringLiteral("script"));
				include = haystack.contains(text, Qt::CaseInsensitive);
			}
#endif
		}
		if (!include)
			continue;
		visibleIndices.append(i);
	}
	std::ranges::sort(visibleIndices,
	                  [&triggers](const int lhs, const int rhs)
	                  {
		                  const QString lhsMatch = triggers.at(lhs).attributes.value(QStringLiteral("match"));
		                  const QString rhsMatch = triggers.at(rhs).attributes.value(QStringLiteral("match"));
		                  const int     cmp      = lhsMatch.compare(rhsMatch, Qt::CaseInsensitive);
		                  if (cmp != 0)
			                  return cmp < 0;
		                  return lhs < rhs;
	                  });
	for (const int i : visibleIndices)
	{
		const WorldRuntime::Trigger &tr  = triggers.at(i);
		const int                    row = m_triggersTable->rowCount();
		m_triggersTable->insertRow(row);
		const QString trigger     = tr.attributes.value(QStringLiteral("match"));
		const QString sequence    = tr.attributes.value(QStringLiteral("sequence"));
		const QString contents    = formatListContents(tr.children.value(QStringLiteral("send")));
		const QString label       = tr.attributes.value(QStringLiteral("name"));
		const QString group       = tr.attributes.value(QStringLiteral("group"));
		auto         *triggerItem = new QTableWidgetItem(trigger);
		triggerItem->setData(Qt::UserRole, i);
		m_triggersTable->setItem(row, 0, triggerItem);
		m_triggersTable->setItem(row, 1, new QTableWidgetItem(sequence));
		m_triggersTable->setItem(row, 2, new QTableWidgetItem(contents));
		m_triggersTable->setItem(row, 3, new QTableWidgetItem(label));
		m_triggersTable->setItem(row, 4, new QTableWidgetItem(group));
	}
	if (m_triggersCount)
		m_triggersCount->setText(QStringLiteral("%1 triggers.").arg(m_triggersTable->rowCount()));
	const QMap<QString, QString> &attrs = m_runtime->worldAttributes();
	if (m_enableTriggers)
		m_enableTriggers->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("enable_triggers"))));
	if (m_enableTriggerSounds)
		m_enableTriggerSounds->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("enable_trigger_sounds"))));
	if (m_useDefaultTriggers)
		m_useDefaultTriggers->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("use_default_triggers"))));
	if (m_triggerTreeView)
		m_triggerTreeView->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("treeview_triggers"))));
	if (m_defaultTriggerExpandVariables)
		m_defaultTriggerExpandVariables->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("default_trigger_expand_variables"))));
	if (m_defaultTriggerIgnoreCase)
		m_defaultTriggerIgnoreCase->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("default_trigger_ignore_case"))));
	if (m_defaultTriggerKeepEvaluating)
		m_defaultTriggerKeepEvaluating->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("default_trigger_keep_evaluating"))));
	if (m_defaultTriggerRegexp)
		m_defaultTriggerRegexp->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("default_trigger_regexp"))));
	if (m_defaultTriggerSendTo)
		m_defaultTriggerSendTo->setValue(attrs.value(QStringLiteral("default_trigger_send_to")).toInt());
	if (m_defaultTriggerSequence)
		m_defaultTriggerSequence->setValue(attrs.value(QStringLiteral("default_trigger_sequence")).toInt());
	if (m_triggersTree)
	{
		rebuildGroupedTree(
		    m_triggersTable, m_triggersTree,
		    [this](const int row) -> QString
		    {
			    if (QTableWidgetItem *item = m_triggersTable->item(row, 0))
				    return item->text();
			    return {};
		    },
		    m_runtime ? m_runtime->lastTriggerTreeExpandedGroup() : QString());
	}
	updateRuleViewModes();
	if (m_useDefaultTriggers && !m_useDefaultTriggersLoaded)
	{
		m_initialUseDefaultTriggers = m_useDefaultTriggers->isChecked();
		m_useDefaultTriggersLoaded  = true;
	}
	updateTriggerControls();
}

void WorldPreferencesDialog::populateAliases()
{
	if (!m_runtime || !m_aliasesTable)
		return;
	m_aliasesTable->setShowGrid(gridLinesEnabled());
	const QList<WorldRuntime::Alias> &aliases = m_runtime->aliases();
	QSet<QString>                     pluginOwnedSignatures;
	for (const auto &plugin : m_runtime->plugins())
	{
		for (const auto &alias : plugin.aliases)
		{
			const QString signature = serializeStringMap(alias.attributes) + QLatin1Char('\x1d') +
			                          serializeStringMap(alias.children);
			pluginOwnedSignatures.insert(signature);
		}
	}
	m_aliasesTable->setRowCount(0);
	const QMap<QString, QString> &multi = m_runtime->worldMultilineAttributes();
	if (!m_aliasFilterLoaded)
	{
		m_aliasFilterText   = multi.value(QStringLiteral("filter_aliases"));
		m_aliasFilterLoaded = true;
	}
	const bool filterEnabled = m_filterAliases && m_filterAliases->isChecked();
#ifdef QMUD_ENABLE_LUA_SCRIPTING
	LuaFilterRunner filter(filterEnabled ? m_aliasFilterText : QString(), this);
	const bool      useLuaFilter = filterEnabled && filter.isValid();
#endif
	QList<int> visibleIndices;
	for (int i = 0; i < aliases.size(); ++i)
	{
		const WorldRuntime::Alias &al = aliases.at(i);
		const QString              signature =
		    serializeStringMap(al.attributes) + QLatin1Char('\x1d') + serializeStringMap(al.children);
		if (pluginOwnedSignatures.contains(signature))
			continue;
		bool include = true;
		if (filterEnabled)
		{
#ifdef QMUD_ENABLE_LUA_SCRIPTING
			if (useLuaFilter)
			{
				const QString name     = al.attributes.value(QStringLiteral("name"));
				auto          pushInfo = [&al](lua_State *L)
				{
					auto pushString = [L](const char *key, const QString &value)
					{
						lua_pushstring(L, key);
						lua_pushstring(L, value.toUtf8().constData());
						lua_settable(L, -3);
					};
					auto pushBool = [L](const char *key, const bool value)
					{
						lua_pushstring(L, key);
						lua_pushboolean(L, value ? 1 : 0);
						lua_settable(L, -3);
					};
					auto pushNumber = [L](const char *key, const double value)
					{
						lua_pushstring(L, key);
						lua_pushnumber(L, value);
						lua_settable(L, -3);
					};

					const QMap<QString, QString> &attrs    = al.attributes;
					const QMap<QString, QString> &children = al.children;
					lua_newtable(L);
					pushBool("echo_alias", qmudIsEnabledFlag(attrs.value(QStringLiteral("echo_alias"))));
					pushBool("enabled", qmudIsEnabledFlag(attrs.value(QStringLiteral("enabled"))));
					pushBool("expand_variables",
					         qmudIsEnabledFlag(attrs.value(QStringLiteral("expand_variables"))));
					pushString("group", attrs.value(QStringLiteral("group")));
					pushBool("ignore_case", qmudIsEnabledFlag(attrs.value(QStringLiteral("ignore_case"))));
					pushBool("keep_evaluating",
					         qmudIsEnabledFlag(attrs.value(QStringLiteral("keep_evaluating"))));
					pushString("match", attrs.value(QStringLiteral("match")));
					pushBool("menu", qmudIsEnabledFlag(attrs.value(QStringLiteral("menu"))));
					pushString("name", attrs.value(QStringLiteral("name")));
					pushBool("omit_from_command_history",
					         qmudIsEnabledFlag(attrs.value(QStringLiteral("omit_from_command_history"))));
					pushBool("omit_from_log",
					         qmudIsEnabledFlag(attrs.value(QStringLiteral("omit_from_log"))));
					pushBool("omit_from_output",
					         qmudIsEnabledFlag(attrs.value(QStringLiteral("omit_from_output"))));
					pushBool("one_shot", qmudIsEnabledFlag(attrs.value(QStringLiteral("one_shot"))));
					pushBool("regexp", qmudIsEnabledFlag(attrs.value(QStringLiteral("regexp"))));
					pushString("script", attrs.value(QStringLiteral("script")));
					pushString("send", children.value(QStringLiteral("send")));
					pushNumber("send_to", attrs.value(QStringLiteral("send_to")).toDouble());
					pushNumber("sequence", attrs.value(QStringLiteral("sequence")).toDouble());
					pushNumber("user", attrs.value(QStringLiteral("user")).toDouble());
					pushString("variable", attrs.value(QStringLiteral("variable")));

					pushNumber("invocation_count", al.invocationCount);
					pushNumber("times_matched", al.matched);
					pushNumber("match_count", al.matched);
					if (al.lastMatched.isValid())
						pushString("when_matched", al.lastMatched.toString(Qt::ISODate));
					pushBool("temporary", qmudIsEnabledFlag(attrs.value(QStringLiteral("temporary"))));
					pushBool("included", al.included);
					if (!al.lastMatchTarget.isEmpty())
						pushString("last_match", al.lastMatchTarget);
					pushBool("script_valid", !attrs.value(QStringLiteral("script")).isEmpty());
					pushNumber("match_attempts", al.matchAttempts);
					pushBool("executing", al.executingScript);

					lua_pushstring(L, "attributes");
					lua_newtable(L);
					for (auto it = attrs.begin(); it != attrs.end(); ++it)
					{
						lua_pushstring(L, it.key().toUtf8().constData());
						lua_pushstring(L, it.value().toUtf8().constData());
						lua_settable(L, -3);
					}
					lua_settable(L, -3);

					lua_pushstring(L, "children");
					lua_newtable(L);
					for (auto it = children.begin(); it != children.end(); ++it)
					{
						lua_pushstring(L, it.key().toUtf8().constData());
						lua_pushstring(L, it.value().toUtf8().constData());
						lua_settable(L, -3);
					}
					lua_settable(L, -3);
				};
				include = filter.matches(name, pushInfo);
			}
#else
			const QString text = m_aliasFilterText.trimmed();
			if (!text.isEmpty())
			{
				QString haystack = al.attributes.value(QStringLiteral("match"));
				haystack += QLatin1Char('\t') + al.children.value(QStringLiteral("send"));
				haystack += QLatin1Char('\t') + al.attributes.value(QStringLiteral("name"));
				haystack += QLatin1Char('\t') + al.attributes.value(QStringLiteral("group"));
				haystack += QLatin1Char('\t') + al.attributes.value(QStringLiteral("script"));
				include = haystack.contains(text, Qt::CaseInsensitive);
			}
#endif
		}
		if (!include)
			continue;
		visibleIndices.append(i);
	}
	std::ranges::sort(visibleIndices,
	                  [&aliases](const int lhs, const int rhs)
	                  {
		                  const QString lhsMatch = aliases.at(lhs).attributes.value(QStringLiteral("match"));
		                  const QString rhsMatch = aliases.at(rhs).attributes.value(QStringLiteral("match"));
		                  const int     cmp      = lhsMatch.compare(rhsMatch, Qt::CaseInsensitive);
		                  if (cmp != 0)
			                  return cmp < 0;
		                  return lhs < rhs;
	                  });
	for (const int i : visibleIndices)
	{
		const WorldRuntime::Alias &al       = aliases.at(i);
		const QString              alias    = al.attributes.value(QStringLiteral("match"));
		const QString              sequence = al.attributes.value(QStringLiteral("sequence"));
		const QString              contents = formatListContents(al.children.value(QStringLiteral("send")));
		const QString              label    = al.attributes.value(QStringLiteral("name"));
		const QString              group    = al.attributes.value(QStringLiteral("group"));
		const int                  row      = m_aliasesTable->rowCount();
		m_aliasesTable->insertRow(row);
		auto *aliasItem = new QTableWidgetItem(alias);
		aliasItem->setData(Qt::UserRole, i);
		m_aliasesTable->setItem(row, 0, aliasItem);
		m_aliasesTable->setItem(row, 1, new QTableWidgetItem(sequence));
		m_aliasesTable->setItem(row, 2, new QTableWidgetItem(contents));
		m_aliasesTable->setItem(row, 3, new QTableWidgetItem(label));
		m_aliasesTable->setItem(row, 4, new QTableWidgetItem(group));
	}
	if (m_aliasesCount)
		m_aliasesCount->setText(QStringLiteral("%1 aliases.").arg(m_aliasesTable->rowCount()));
	const QMap<QString, QString> &attrs = m_runtime->worldAttributes();
	if (m_enableAliases)
		m_enableAliases->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("enable_aliases"))));
	if (m_useDefaultAliases)
		m_useDefaultAliases->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("use_default_aliases"))));
	if (m_aliasTreeView)
		m_aliasTreeView->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("treeview_aliases"))));
	if (m_defaultAliasExpandVariables)
		m_defaultAliasExpandVariables->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("default_alias_expand_variables"))));
	if (m_defaultAliasIgnoreCase)
		m_defaultAliasIgnoreCase->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("default_alias_ignore_case"))));
	if (m_defaultAliasKeepEvaluating)
		m_defaultAliasKeepEvaluating->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("default_alias_keep_evaluating"))));
	if (m_defaultAliasRegexp)
		m_defaultAliasRegexp->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("default_alias_regexp"))));
	if (m_defaultAliasSendTo)
		m_defaultAliasSendTo->setValue(attrs.value(QStringLiteral("default_alias_send_to")).toInt());
	if (m_defaultAliasSequence)
		m_defaultAliasSequence->setValue(attrs.value(QStringLiteral("default_alias_sequence")).toInt());
	if (m_aliasesTree)
	{
		rebuildGroupedTree(
		    m_aliasesTable, m_aliasesTree,
		    [this](const int row) -> QString
		    {
			    if (QTableWidgetItem *item = m_aliasesTable->item(row, 0))
				    return item->text();
			    return {};
		    },
		    m_runtime ? m_runtime->lastAliasTreeExpandedGroup() : QString());
	}
	updateRuleViewModes();
	if (m_useDefaultAliases && !m_useDefaultAliasesLoaded)
	{
		m_initialUseDefaultAliases = m_useDefaultAliases->isChecked();
		m_useDefaultAliasesLoaded  = true;
	}
	updateAliasControls();
}

void WorldPreferencesDialog::populateTimers()
{
	if (!m_runtime || !m_timersTable)
		return;
	m_timersTable->setShowGrid(gridLinesEnabled());
	const QList<WorldRuntime::Timer> &timers = m_runtime->timers();
	QSet<QString>                     pluginOwnedSignatures;
	for (const auto &plugin : m_runtime->plugins())
	{
		for (const auto &timer : plugin.timers)
		{
			const QString signature = serializeStringMap(timer.attributes) + QLatin1Char('\x1d') +
			                          serializeStringMap(timer.children);
			pluginOwnedSignatures.insert(signature);
		}
	}
	m_timersTable->setRowCount(0);
	const QMap<QString, QString> &multi = m_runtime->worldMultilineAttributes();
	if (!m_timerFilterLoaded)
	{
		m_timerFilterText   = multi.value(QStringLiteral("filter_timers"));
		m_timerFilterLoaded = true;
	}
	const bool filterEnabled = m_filterTimers && m_filterTimers->isChecked();
#ifdef QMUD_ENABLE_LUA_SCRIPTING
	LuaFilterRunner filter(filterEnabled ? m_timerFilterText : QString(), this);
	const bool      useLuaFilter = filterEnabled && filter.isValid();
#endif
	QList<int> visibleIndices;
	for (int i = 0; i < timers.size(); ++i)
	{
		const WorldRuntime::Timer &tm = timers.at(i);
		const QString              signature =
		    serializeStringMap(tm.attributes) + QLatin1Char('\x1d') + serializeStringMap(tm.children);
		if (pluginOwnedSignatures.contains(signature))
			continue;
		bool include = true;
		if (filterEnabled)
		{
#ifdef QMUD_ENABLE_LUA_SCRIPTING
			if (useLuaFilter)
			{
				const QString name     = tm.attributes.value(QStringLiteral("name"));
				auto          pushInfo = [&tm](lua_State *L)
				{
					auto pushString = [L](const char *key, const QString &value)
					{
						lua_pushstring(L, key);
						lua_pushstring(L, value.toUtf8().constData());
						lua_settable(L, -3);
					};
					auto pushBool = [L](const char *key, const bool value)
					{
						lua_pushstring(L, key);
						lua_pushboolean(L, value ? 1 : 0);
						lua_settable(L, -3);
					};
					auto pushNumber = [L](const char *key, const double value)
					{
						lua_pushstring(L, key);
						lua_pushnumber(L, value);
						lua_settable(L, -3);
					};

					const QMap<QString, QString> &attrs    = tm.attributes;
					const QMap<QString, QString> &children = tm.children;
					lua_newtable(L);
					pushBool("at_time", qmudIsEnabledFlag(attrs.value(QStringLiteral("at_time"))));
					pushNumber("hour", attrs.value(QStringLiteral("hour")).toDouble());
					pushNumber("minute", attrs.value(QStringLiteral("minute")).toDouble());
					pushNumber("second", attrs.value(QStringLiteral("second")).toDouble());
					pushNumber("offset_hour", attrs.value(QStringLiteral("offset_hour")).toDouble());
					pushNumber("offset_minute", attrs.value(QStringLiteral("offset_minute")).toDouble());
					pushNumber("offset_second", attrs.value(QStringLiteral("offset_second")).toDouble());
					pushBool("enabled", qmudIsEnabledFlag(attrs.value(QStringLiteral("enabled"))));
					pushBool("one_shot", qmudIsEnabledFlag(attrs.value(QStringLiteral("one_shot"))));
					pushBool("temporary", qmudIsEnabledFlag(attrs.value(QStringLiteral("temporary"))));
					pushBool("active_closed",
					         qmudIsEnabledFlag(attrs.value(QStringLiteral("active_closed"))));
					pushBool("omit_from_output",
					         qmudIsEnabledFlag(attrs.value(QStringLiteral("omit_from_output"))));
					pushBool("omit_from_log",
					         qmudIsEnabledFlag(attrs.value(QStringLiteral("omit_from_log"))));
					pushNumber("send_to", attrs.value(QStringLiteral("send_to")).toDouble());
					pushString("group", attrs.value(QStringLiteral("group")));
					pushString("name", attrs.value(QStringLiteral("name")));
					pushString("script", attrs.value(QStringLiteral("script")));
					pushString("variable", attrs.value(QStringLiteral("variable")));
					pushString("send", children.value(QStringLiteral("send")));
					pushNumber("user", attrs.value(QStringLiteral("user")).toDouble());

					pushNumber("invocation_count", tm.invocationCount);
					pushNumber("times_fired", tm.firedCount);
					if (tm.lastFired.isValid())
						pushString("when_fired", tm.lastFired.toString(Qt::ISODate));
					if (tm.nextFireTime.isValid())
						pushString("fire_time", tm.nextFireTime.toString(Qt::ISODate));
					pushBool("included", tm.included);
					pushBool("script_valid", !attrs.value(QStringLiteral("script")).isEmpty());
					pushBool("executing", tm.executingScript);

					lua_pushstring(L, "attributes");
					lua_newtable(L);
					for (auto it = attrs.begin(); it != attrs.end(); ++it)
					{
						lua_pushstring(L, it.key().toUtf8().constData());
						lua_pushstring(L, it.value().toUtf8().constData());
						lua_settable(L, -3);
					}
					lua_settable(L, -3);

					lua_pushstring(L, "children");
					lua_newtable(L);
					for (auto it = children.begin(); it != children.end(); ++it)
					{
						lua_pushstring(L, it.key().toUtf8().constData());
						lua_pushstring(L, it.value().toUtf8().constData());
						lua_settable(L, -3);
					}
					lua_settable(L, -3);
				};
				include = filter.matches(name, pushInfo);
			}
#else
			const QString text = m_timerFilterText.trimmed();
			if (!text.isEmpty())
			{
				QString haystack = tm.children.value(QStringLiteral("send"));
				haystack += QLatin1Char('\t') + tm.attributes.value(QStringLiteral("name"));
				haystack += QLatin1Char('\t') + tm.attributes.value(QStringLiteral("group"));
				haystack += QLatin1Char('\t') + tm.attributes.value(QStringLiteral("script"));
				include = haystack.contains(text, Qt::CaseInsensitive);
			}
#endif
		}
		if (!include)
			continue;
		visibleIndices.append(i);
	}
	std::ranges::sort(visibleIndices,
	                  [&timers](const int lhs, const int rhs)
	                  {
		                  const QString lhsWhen = timerWhenText(timers.at(lhs));
		                  const QString rhsWhen = timerWhenText(timers.at(rhs));
		                  const int     cmp     = lhsWhen.compare(rhsWhen, Qt::CaseInsensitive);
		                  if (cmp != 0)
			                  return cmp < 0;
		                  return lhs < rhs;
	                  });
	for (const int i : visibleIndices)
	{
		const WorldRuntime::Timer &tm     = timers.at(i);
		const bool                 atTime = qmudIsEnabledFlag(tm.attributes.value(QStringLiteral("at_time")));
		QString                    type   = atTime ? QStringLiteral("At") : QStringLiteral("Every");
		const QString              when   = timerWhenText(tm);
		const QString              contents = formatListContents(tm.children.value(QStringLiteral("send")));
		const QString              label    = tm.attributes.value(QStringLiteral("name"));
		const QString              group    = tm.attributes.value(QStringLiteral("group"));
		QString                    nextText = QStringLiteral("-");
		if (tm.nextFireTime.isValid())
		{
			const QDateTime now = QDateTime::currentDateTime();
			if (tm.nextFireTime > now)
			{
				qint64       secs = now.secsTo(tm.nextFireTime);
				const qint64 days = secs / 86400;
				secs -= days * 86400;
				const qint64 hours = secs / 3600;
				secs -= hours * 3600;
				const qint64 mins = secs / 60;
				secs -= mins * 60;
				if (days > 0)
					nextText = QStringLiteral("%1 d").arg(days);
				else if (hours > 0)
					nextText = QStringLiteral("%1 h").arg(hours);
				else if (mins > 0)
					nextText = QStringLiteral("%1 m").arg(mins);
				else
					nextText = QStringLiteral("%1 s").arg(secs);
			}
		}
		const int row = m_timersTable->rowCount();
		m_timersTable->insertRow(row);
		auto *typeItem = new QTableWidgetItem(type);
		typeItem->setData(Qt::UserRole, i);
		m_timersTable->setItem(row, 0, typeItem);
		m_timersTable->setItem(row, 1, new QTableWidgetItem(when));
		m_timersTable->setItem(row, 2, new QTableWidgetItem(contents));
		m_timersTable->setItem(row, 3, new QTableWidgetItem(label));
		m_timersTable->setItem(row, 4, new QTableWidgetItem(group));
		m_timersTable->setItem(row, 5, new QTableWidgetItem(nextText));
	}
	if (m_timersCount)
		m_timersCount->setText(QStringLiteral("%1 timers.").arg(m_timersTable->rowCount()));
	const QMap<QString, QString> &attrs = m_runtime->worldAttributes();
	if (m_enableTimers)
		m_enableTimers->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("enable_timers"))));
	if (m_useDefaultTimers)
		m_useDefaultTimers->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("use_default_timers"))));
	if (m_timerTreeView)
		m_timerTreeView->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("treeview_timers"))));
	if (m_defaultTimerSendTo)
		m_defaultTimerSendTo->setValue(attrs.value(QStringLiteral("default_timer_send_to")).toInt());
	if (m_timersTree)
	{
		rebuildGroupedTree(
		    m_timersTable, m_timersTree,
		    [this](const int row)
		    {
			    QString type = m_timersTable->item(row, 0) ? m_timersTable->item(row, 0)->text() : QString();
			    QString when = m_timersTable->item(row, 1) ? m_timersTable->item(row, 1)->text() : QString();
			    if (type.isEmpty())
				    return when;
			    if (when.isEmpty())
				    return type;
			    return type + QStringLiteral(": ") + when;
		    },
		    m_runtime ? m_runtime->lastTimerTreeExpandedGroup() : QString());
	}
	updateRuleViewModes();
	if (m_useDefaultTimers && !m_useDefaultTimersLoaded)
	{
		m_initialUseDefaultTimers = m_useDefaultTimers->isChecked();
		m_useDefaultTimersLoaded  = true;
	}
	updateTimerControls();
}

void WorldPreferencesDialog::populateVariables()
{
	if (!m_runtime || !m_variablesTable)
		return;
	m_variablesTable->setShowGrid(gridLinesEnabled());
	const QList<WorldRuntime::Variable> &vars = m_runtime->variables();
	QSet<QString>                        pluginOwnedVariables;
	for (const auto &plugin : m_runtime->plugins())
	{
		for (auto it = plugin.variables.constBegin(); it != plugin.variables.constEnd(); ++it)
			pluginOwnedVariables.insert(normalizeObjectName(it.key()) + QLatin1Char('\x1d') + it.value());
	}
	const QMap<QString, QString> &multi = m_runtime->worldMultilineAttributes();
	if (!m_variableFilterLoaded)
	{
		m_variableFilterText   = multi.value(QStringLiteral("filter_variables"));
		m_variableFilterLoaded = true;
	}
	m_variablesTable->setRowCount(0);
	const bool filterEnabled = m_filterVariables && m_filterVariables->isChecked();
#ifdef QMUD_ENABLE_LUA_SCRIPTING
	LuaFilterRunner filter(filterEnabled ? m_variableFilterText : QString(), this);
	const bool      useLuaFilter = filterEnabled && filter.isValid();
#endif
	for (int i = 0; i < vars.size(); ++i)
	{
		const WorldRuntime::Variable &var     = vars.at(i);
		const QString                 varName = var.attributes.value(QStringLiteral("name"));
		const QString variableKey = normalizeObjectName(varName) + QLatin1Char('\x1d') + var.content;
		if (pluginOwnedVariables.contains(variableKey))
			continue;
		bool include = true;
		if (filterEnabled)
		{
#ifdef QMUD_ENABLE_LUA_SCRIPTING
			if (useLuaFilter)
			{
				const QString name     = var.attributes.value(QStringLiteral("name"));
				auto          pushInfo = [&var](lua_State *L)
				{
					lua_newtable(L);
					lua_pushstring(L, "contents");
					lua_pushstring(L, var.content.toUtf8().constData());
					lua_settable(L, -3);
				};
				include = filter.matches(name, pushInfo);
			}
#else
			const QString text = m_variableFilterText.trimmed();
			if (!text.isEmpty())
			{
				include = varName.contains(text, Qt::CaseInsensitive) ||
				          var.content.contains(text, Qt::CaseInsensitive);
			}
#endif
		}
		if (!include)
			continue;
		const QString value = var.content;
		const int     row   = m_variablesTable->rowCount();
		m_variablesTable->insertRow(row);
		auto *nameItem = new QTableWidgetItem(varName);
		nameItem->setData(Qt::UserRole, i);
		m_variablesTable->setItem(row, 0, nameItem);
		m_variablesTable->setItem(row, 1, new QTableWidgetItem(value));
	}
	if (m_variablesCount)
		m_variablesCount->setText(QStringLiteral("%1 items.").arg(m_variablesTable->rowCount()));
	updateVariableControls();
}

void WorldPreferencesDialog::populateAutoSay() const
{
	if (!m_runtime)
		return;
	const QMap<QString, QString> &attrs = m_runtime->worldAttributes();
	if (m_enableAutoSay)
		m_enableAutoSay->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("enable_auto_say"))));
	if (m_reEvaluateAutoSay)
		m_reEvaluateAutoSay->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("re_evaluate_auto_say"))));
	if (m_autoSayExcludeNonAlpha)
		m_autoSayExcludeNonAlpha->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("autosay_exclude_non_alpha"))));
	if (m_autoSayExcludeMacros)
		m_autoSayExcludeMacros->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("autosay_exclude_macros"))));
	if (m_autoSayString)
		m_autoSayString->setText(attrs.value(QStringLiteral("auto_say_string")));
	if (m_autoSayOverridePrefix)
		m_autoSayOverridePrefix->setText(attrs.value(QStringLiteral("auto_say_override_prefix")));
}

void WorldPreferencesDialog::populatePrinting()
{
	if (!m_runtime)
		return;
	const QList<WorldRuntime::PrintingStyle> &styles = m_runtime->printingStyles();
	for (QCheckBox *cb : m_printingNormalBold)
		if (cb)
			cb->setChecked(false);
	for (QCheckBox *cb : m_printingNormalItalic)
		if (cb)
			cb->setChecked(false);
	for (QCheckBox *cb : m_printingNormalUnderline)
		if (cb)
			cb->setChecked(false);
	for (QCheckBox *cb : m_printingBoldBold)
		if (cb)
			cb->setChecked(false);
	for (QCheckBox *cb : m_printingBoldItalic)
		if (cb)
			cb->setChecked(false);
	for (QCheckBox *cb : m_printingBoldUnderline)
		if (cb)
			cb->setChecked(false);

	for (const WorldRuntime::PrintingStyle &style : styles)
	{
		const QString group = style.group.toLower();
		bool          ok    = false;
		const int     seq   = style.attributes.value(QStringLiteral("seq")).toInt(&ok);
		if (!ok || seq < 1 || seq > 8)
			continue;
		const int  index    = seq - 1;
		const bool isNormal = group == QStringLiteral("ansi/normal");
		const bool isBold   = group == QStringLiteral("ansi/bold");
		if (!isNormal && !isBold)
			continue;
		const bool bold      = qmudIsEnabledFlag(style.attributes.value(QStringLiteral("bold")));
		const bool italic    = qmudIsEnabledFlag(style.attributes.value(QStringLiteral("italic")));
		const bool underline = qmudIsEnabledFlag(style.attributes.value(QStringLiteral("underline")));
		if (isNormal)
		{
			if (index < m_printingNormalBold.size() && m_printingNormalBold[index])
				m_printingNormalBold[index]->setChecked(bold);
			if (index < m_printingNormalItalic.size() && m_printingNormalItalic[index])
				m_printingNormalItalic[index]->setChecked(italic);
			if (index < m_printingNormalUnderline.size() && m_printingNormalUnderline[index])
				m_printingNormalUnderline[index]->setChecked(underline);
		}
		else
		{
			if (index < m_printingBoldBold.size() && m_printingBoldBold[index])
				m_printingBoldBold[index]->setChecked(bold);
			if (index < m_printingBoldItalic.size() && m_printingBoldItalic[index])
				m_printingBoldItalic[index]->setChecked(italic);
			if (index < m_printingBoldUnderline.size() && m_printingBoldUnderline[index])
				m_printingBoldUnderline[index]->setChecked(underline);
		}
	}
}

void WorldPreferencesDialog::populateConnecting() const
{
	if (!m_runtime)
		return;
	const QMap<QString, QString> &attrs = m_runtime->worldAttributes();
	const QMap<QString, QString> &multi = m_runtime->worldMultilineAttributes();
	if (m_playerName)
		m_playerName->setText(attrs.value(QStringLiteral("player")));
	if (m_password)
		m_password->setText(attrs.value(QStringLiteral("password")));
	if (m_connectText)
		m_connectText->setPlainText(multi.value(QStringLiteral("connect_text")));
	if (m_connectMethod)
	{
		const int method = attrs.value(QStringLiteral("connect_method")).toInt();
		if (const int index = m_connectMethod->findData(method); index >= 0)
			m_connectMethod->setCurrentIndex(index);
	}
	if (m_onlyNegotiateTelnetOptionsOnce)
	{
		m_onlyNegotiateTelnetOptionsOnce->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("only_negotiate_telnet_options_once"))));
	}
	if (m_connectLineCount && m_connectText)
	{
		const QString text  = m_connectText->toPlainText();
		const int     lines = text.isEmpty() ? 0 : saturatingToInt(text.count(QLatin1Char('\n')) + 1);
		m_connectLineCount->setText(formatLineCountLabel(lines));
	}
}

void WorldPreferencesDialog::populateMxp() const
{
	if (!m_runtime)
		return;
	const QMap<QString, QString> &attrs       = m_runtime->worldAttributes();
	auto                          applySwatch = [this](QLineEdit *edit, const QString &value)
	{
		if (!edit)
			return;
		setLineEditSwatch(edit, parseColourValue(value));
		if (!value.isEmpty())
			edit->setProperty("colour_value", value);
	};
	if (m_mxpActive)
		m_mxpActive->setText(m_runtime->isMxpActive() ? QStringLiteral("MXP active")
		                                              : QStringLiteral("MXP inactive"));
	if (m_useMxp)
	{
		if (const int index = m_useMxp->findData(attrs.value(QStringLiteral("use_mxp"))); index >= 0)
			m_useMxp->setCurrentIndex(index);
	}
	if (m_detectPueblo)
		m_detectPueblo->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("detect_pueblo"))));
	if (m_mxpDebugLevel)
	{
		if (const int index = m_mxpDebugLevel->findData(attrs.value(QStringLiteral("mxp_debug_level")));
		    index >= 0)
			m_mxpDebugLevel->setCurrentIndex(index);
	}
	applySwatch(m_hyperlinkColour, attrs.value(QStringLiteral("hyperlink_colour")));
	if (m_useCustomLinkColour)
		m_useCustomLinkColour->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("use_custom_link_colour"))));
	if (m_mudCanChangeLinkColour)
		m_mudCanChangeLinkColour->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("mud_can_change_link_colour"))));
	if (m_underlineHyperlinks)
		m_underlineHyperlinks->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("underline_hyperlinks"))));
	if (m_mudCanRemoveUnderline)
		m_mudCanRemoveUnderline->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("mud_can_remove_underline"))));
	if (m_hyperlinkAddsToCommandHistory)
		m_hyperlinkAddsToCommandHistory->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("hyperlink_adds_to_command_history"))));
	if (m_echoHyperlinkInOutput)
		m_echoHyperlinkInOutput->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("echo_hyperlink_in_output_window"))));
	if (m_ignoreMxpColourChanges)
		m_ignoreMxpColourChanges->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("ignore_mxp_colour_changes"))));
	if (m_sendMxpAfkResponse)
		m_sendMxpAfkResponse->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("send_mxp_afk_response"))));
	if (m_mudCanChangeOptions)
		m_mudCanChangeOptions->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("mud_can_change_options"))));
}

void WorldPreferencesDialog::populateChat() const
{
	if (!m_runtime)
		return;
	const QMap<QString, QString> &attrs       = m_runtime->worldAttributes();
	auto                          applySwatch = [this](QLineEdit *edit, const QString &value)
	{
		if (!edit)
			return;
		setLineEditSwatch(edit, parseColourValue(value));
		if (!value.isEmpty())
			edit->setProperty("colour_value", value);
	};
	if (m_chatName)
		m_chatName->setText(attrs.value(QStringLiteral("chat_name")));
	if (m_autoAllowSnooping)
		m_autoAllowSnooping->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("auto_allow_snooping"))));
	if (m_acceptIncomingChatConnections)
		m_acceptIncomingChatConnections->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("accept_chat_connections"))));
	if (m_incomingChatPort)
		m_incomingChatPort->setValue(attrs.value(QStringLiteral("chat_port")).toInt());
	if (m_validateIncomingCalls)
		m_validateIncomingCalls->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("validate_incoming_chat_calls"))));
	if (m_ignoreChatColours)
		m_ignoreChatColours->setChecked(
		    qmudIsEnabledFlag(attrs.value(QStringLiteral("ignore_chat_colours"))));
	if (m_chatMessagePrefix)
		m_chatMessagePrefix->setText(attrs.value(QStringLiteral("chat_message_prefix")));
	if (m_maxChatLines)
		m_maxChatLines->setValue(attrs.value(QStringLiteral("chat_max_lines_per_message")).toInt());
	if (m_maxChatBytes)
		m_maxChatBytes->setValue(attrs.value(QStringLiteral("chat_max_bytes_per_message")).toInt());
	if (m_chatSaveDirectory)
		m_chatSaveDirectory->setText(attrs.value(QStringLiteral("chat_file_save_directory")));
	if (m_autoAllowFiles)
		m_autoAllowFiles->setChecked(qmudIsEnabledFlag(attrs.value(QStringLiteral("auto_allow_files"))));
	applySwatch(m_chatTextColour, attrs.value(QStringLiteral("chat_foreground_colour")));
	applySwatch(m_chatBackColour, attrs.value(QStringLiteral("chat_background_colour")));
}

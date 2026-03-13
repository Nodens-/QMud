/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: ColourPickerDialog.cpp
 * Role: Color picker dialog behavior that captures user color choices for world preferences and styling workflows.
 */

#include "ColourPickerDialog.h"

#include "ColorPacking.h"

#include <QApplication>
#include <QBrush>
#include <QClipboard>
#include <QColorDialog>
#include <QComboBox>
#include <QGridLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSlider>
#include <QTableWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <limits>
#include <memory>

namespace
{
	struct NamedColour
	{
			const char  *name;
			unsigned int rgb;
	};

	struct HslComponents
	{
			double hue{0.0};
			double saturation{0.0};
			double luminance{0.0};
	};

	int sizeToInt(const qsizetype value)
	{
		return static_cast<int>(qBound(static_cast<qsizetype>(0), value,
		                               static_cast<qsizetype>(std::numeric_limits<int>::max())));
	}

	void setOwnedItem(QTableWidget *table, const int row, const int col,
	                  std::unique_ptr<QTableWidgetItem> item)
	{
		table->setItem(row, col, item.release());
	}

	HslComponents hslFromColor(const QColor &color)
	{
		const QColor hsl        = color.toHsl();
		float        hue        = 0.0f;
		float        saturation = 0.0f;
		float        luminance  = 0.0f;
		hsl.getHslF(&hue, &saturation, &luminance);
		if (hue < 0.0f)
			hue = 0.0f;
		HslComponents out;
		out.hue        = static_cast<double>(hue) * 360.0;
		out.saturation = static_cast<double>(saturation);
		out.luminance  = static_cast<double>(luminance);
		return out;
	}

	const NamedColour kMxpColours[] = {
	    {"aliceblue",            0xf0f8ff},
	    {"antiquewhite",         0xfaebd7},
	    {"aqua",	             0x00ffff},
	    {"aquamarine",           0x7fffd4},
	    {"azure",                0xf0ffff},
	    {"beige",                0xf5f5dc},
	    {"bisque",               0xffe4c4},
	    {"black",                0x000000},
	    {"blanchedalmond",       0xffebcd},
	    {"blue",	             0x0000ff},
	    {"blueviolet",           0x8a2be2},
	    {"brown",                0xa52a2a},
	    {"burlywood",            0xdeb887},
	    {"cadetblue",            0x5f9ea0},
	    {"chartreuse",           0x7fff00},
	    {"chocolate",            0xd2691e},
	    {"coral",                0xff7f50},
	    {"cornflowerblue",       0x6495ed},
	    {"cornsilk",             0xfff8dc},
	    {"crimson",              0xdc143c},
	    {"cyan",	             0x00ffff},
	    {"darkblue",             0x00008b},
	    {"darkcyan",             0x008b8b},
	    {"darkgoldenrod",        0xb8860b},
	    {"darkgray",             0xa9a9a9},
	    {"darkgrey",             0xa9a9a9},
	    {"darkgreen",            0x006400},
	    {"darkkhaki",            0xbdb76b},
	    {"darkmagenta",          0x8b008b},
	    {"darkolivegreen",       0x556b2f},
	    {"darkorange",           0xff8c00},
	    {"darkorchid",           0x9932cc},
	    {"darkred",              0x8b0000},
	    {"darksalmon",           0xe9967a},
	    {"darkseagreen",         0x8fbc8f},
	    {"darkslateblue",        0x483d8b},
	    {"darkslategray",        0x2f4f4f},
	    {"darkslategrey",        0x2f4f4f},
	    {"darkturquoise",        0x00ced1},
	    {"darkviolet",           0x9400d3},
	    {"deeppink",             0xff1493},
	    {"deepskyblue",          0x00bfff},
	    {"dimgray",              0x696969},
	    {"dimgrey",              0x696969},
	    {"dodgerblue",           0x1e90ff},
	    {"firebrick",            0xb22222},
	    {"floralwhite",          0xfffaf0},
	    {"forestgreen",          0x228b22},
	    {"fuchsia",              0xff00ff},
	    {"gainsboro",            0xdcdcdc},
	    {"ghostwhite",           0xf8f8ff},
	    {"gold",	             0xffd700},
	    {"goldenrod",            0xdaa520},
	    {"gray",	             0x808080},
	    {"grey",	             0x808080},
	    {"green",                0x008000},
	    {"greenyellow",          0xadff2f},
	    {"honeydew",             0xf0fff0},
	    {"hotpink",              0xff69b4},
	    {"indianred",            0xcd5c5c},
	    {"indigo",               0x4b0082},
	    {"ivory",                0xfffff0},
	    {"khaki",                0xf0e68c},
	    {"lavender",             0xe6e6fa},
	    {"lavenderblush",        0xfff0f5},
	    {"lawngreen",            0x7cfc00},
	    {"lemonchiffon",         0xfffacd},
	    {"lightblue",            0xadd8e6},
	    {"lightcoral",           0xf08080},
	    {"lightcyan",            0xe0ffff},
	    {"lightgoldenrodyellow", 0xfafad2},
	    {"lightgreen",           0x90ee90},
	    {"lightgrey",            0xd3d3d3},
	    {"lightgray",            0xd3d3d3},
	    {"lightpink",            0xffb6c1},
	    {"lightsalmon",          0xffa07a},
	    {"lightseagreen",        0x20b2aa},
	    {"lightskyblue",         0x87cefa},
	    {"lightslategray",       0x778899},
	    {"lightslategrey",       0x778899},
	    {"lightsteelblue",       0xb0c4de},
	    {"lightyellow",          0xffffe0},
	    {"lime",	             0x00ff00},
	    {"limegreen",            0x32cd32},
	    {"linen",                0xfaf0e6},
	    {"magenta",              0xff00ff},
	    {"maroon",               0x800000},
	    {"mediumaquamarine",     0x66cdaa},
	    {"mediumblue",           0x0000cd},
	    {"mediumorchid",         0xba55d3},
	    {"mediumpurple",         0x9370db},
	    {"mediumseagreen",       0x3cb371},
	    {"mediumslateblue",      0x7b68ee},
	    {"mediumspringgreen",    0x00fa9a},
	    {"mediumturquoise",      0x48d1cc},
	    {"mediumvioletred",      0xc71585},
	    {"midnightblue",         0x191970},
	    {"mintcream",            0xf5fffa},
	    {"mistyrose",            0xffe4e1},
	    {"moccasin",             0xffe4b5},
	    {"navajowhite",          0xffdead},
	    {"navy",	             0x000080},
	    {"oldlace",              0xfdf5e6},
	    {"olive",                0x808000},
	    {"olivedrab",            0x6b8e23},
	    {"orange",               0xffa500},
	    {"orangered",            0xff4500},
	    {"orchid",               0xda70d6},
	    {"palegoldenrod",        0xeee8aa},
	    {"palegreen",            0x98fb98},
	    {"paleturquoise",        0xafeeee},
	    {"palevioletred",        0xdb7093},
	    {"papayawhip",           0xffefd5},
	    {"peachpuff",            0xffdab9},
	    {"peru",	             0xcd853f},
	    {"pink",	             0xffc0cb},
	    {"plum",	             0xdda0dd},
	    {"powderblue",           0xb0e0e6},
	    {"purple",               0x800080},
	    {"rebeccapurple",        0x663399},
	    {"red",	              0xff0000},
	    {"rosybrown",            0xbc8f8f},
	    {"royalblue",            0x4169e1},
	    {"saddlebrown",          0x8b4513},
	    {"salmon",               0xfa8072},
	    {"sandybrown",           0xf4a460},
	    {"seagreen",             0x2e8b57},
	    {"seashell",             0xfff5ee},
	    {"sienna",               0xa0522d},
	    {"silver",               0xc0c0c0},
	    {"skyblue",              0x87ceeb},
	    {"slateblue",            0x6a5acd},
	    {"slategray",            0x708090},
	    {"slategrey",            0x708090},
	    {"snow",	             0xfffafa},
	    {"springgreen",          0x00ff7f},
	    {"steelblue",            0x4682b4},
	    {"tan",	              0xd2b48c},
	    {"teal",	             0x008080},
	    {"thistle",              0xd8bfd8},
	    {"tomato",               0xff6347},
	    {"turquoise",            0x40e0d0},
	    {"violet",               0xee82ee},
	    {"wheat",                0xf5deb3},
	    {"white",                0xffffff},
	    {"whitesmoke",           0xf5f5f5},
	    {"yellow",               0xffff00},
	    {"yellowgreen",          0x9acd32},
	};

	QColor rgbToQColor(const unsigned int rgb)
	{
		const int r = static_cast<int>(rgb >> 16U) & 0xFF;
		const int g = static_cast<int>(rgb >> 8U) & 0xFF;
		const int b = static_cast<int>(rgb) & 0xFF;
		return {r, g, b};
	}
} // namespace

QColor ColourPickerDialog::s_lastColour = QColor(0, 0, 0);
int    ColourPickerDialog::s_sort       = SortHue;

ColourPickerDialog::ColourPickerDialog(QWidget *parent) : QDialog(parent)
{
	setWindowTitle(QStringLiteral("Colour picker"));
	setMinimumSize(760, 520);
	auto  layout    = std::make_unique<QVBoxLayout>();
	auto *layoutPtr = layout.get();

	auto  top = std::make_unique<QHBoxLayout>();
	m_list    = new QTableWidget(this);
	m_list->setColumnCount(2);
	m_list->horizontalHeader()->setVisible(false);
	m_list->verticalHeader()->setVisible(false);
	m_list->setSelectionBehavior(QAbstractItemView::SelectRows);
	m_list->setSelectionMode(QAbstractItemView::SingleSelection);
	m_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
	m_list->setShowGrid(false);
	m_list->setAlternatingRowColors(false);
	m_list->setColumnWidth(0, 130);
	m_list->setColumnWidth(1, 90);
	top->addWidget(m_list, 1);

	auto right = std::make_unique<QVBoxLayout>();
	m_swatch   = new QPushButton(this);
	m_swatch->setFixedSize(64, 64);
	right->addWidget(m_swatch, 0, Qt::AlignLeft);

	m_ok                 = new QPushButton(QStringLiteral("OK"), this);
	auto random          = std::make_unique<QPushButton>(QStringLiteral("Random"), this);
	auto otherColour     = std::make_unique<QPushButton>(QStringLiteral("Other colour..."), this);
	m_paste              = new QPushButton(QStringLiteral("Paste"), this);
	auto  close          = std::make_unique<QPushButton>(QStringLiteral("Close"), this);
	auto *randomPtr      = random.get();
	auto *otherColourPtr = otherColour.get();
	auto *closePtr       = close.get();

	right->addWidget(m_ok);
	right->addWidget(random.release());
	right->addWidget(otherColour.release());
	right->addWidget(m_paste);
	right->addWidget(close.release());
	right->addStretch(1);
	top->addLayout(right.release());
	layoutPtr->addLayout(top.release());

	auto grid = std::make_unique<QGridLayout>();
	grid->addWidget(new QLabel(QStringLiteral("Name:"), this), 0, 0);
	m_colourName = new QPushButton(QStringLiteral("Colour name"), this);
	m_colourName->setFlat(true);
	grid->addWidget(m_colourName, 0, 1, 1, 3);

	m_redSlider   = new QSlider(Qt::Horizontal, this);
	m_greenSlider = new QSlider(Qt::Horizontal, this);
	m_blueSlider  = new QSlider(Qt::Horizontal, this);
	for (QSlider *sliders[] = {m_redSlider, m_greenSlider, m_blueSlider}; QSlider *slider : sliders)
	{
		slider->setRange(0, 255);
		slider->setPageStep(10);
	}
	m_redValue   = new QLabel(QStringLiteral("255"), this);
	m_greenValue = new QLabel(QStringLiteral("255"), this);
	m_blueValue  = new QLabel(QStringLiteral("255"), this);
	grid->addWidget(new QLabel(QStringLiteral("Red:"), this), 1, 0);
	grid->addWidget(m_redSlider, 1, 1);
	grid->addWidget(m_redValue, 1, 2);
	grid->addWidget(new QLabel(QStringLiteral("Green:"), this), 2, 0);
	grid->addWidget(m_greenSlider, 2, 1);
	grid->addWidget(m_greenValue, 2, 2);
	grid->addWidget(new QLabel(QStringLiteral("Blue:"), this), 3, 0);
	grid->addWidget(m_blueSlider, 3, 1);
	grid->addWidget(m_blueValue, 3, 2);

	grid->addWidget(new QLabel(QStringLiteral("Sort:"), this), 0, 4);
	m_sort = new QComboBox(this);
	m_sort->addItems({QStringLiteral("Names"), QStringLiteral("Hue"), QStringLiteral("Saturation"),
	                  QStringLiteral("Luminance"), QStringLiteral("Red"), QStringLiteral("Green"),
	                  QStringLiteral("Blue")});
	m_sort->setCurrentIndex(s_sort);
	grid->addWidget(m_sort, 0, 5);

	grid->addWidget(new QLabel(QStringLiteral("MXP:"), this), 1, 4);
	m_mxpName = new QPushButton(QStringLiteral("MXP value"), this);
	m_mxpName->setFlat(true);
	grid->addWidget(m_mxpName, 1, 5);
	grid->addWidget(new QLabel(QStringLiteral("Lua:"), this), 2, 4);
	m_luaName = new QPushButton(QStringLiteral("Lua value"), this);
	m_luaName->setFlat(true);
	grid->addWidget(m_luaName, 2, 5);

	m_hueValue = new QLabel(QStringLiteral("Hue: 0.0"), this);
	m_satValue = new QLabel(QStringLiteral("Saturation: 0.000"), this);
	m_lumValue = new QLabel(QStringLiteral("Luminance: 0.000"), this);
	grid->addWidget(m_hueValue, 4, 1, 1, 2);
	grid->addWidget(m_satValue, 4, 3, 1, 2);
	grid->addWidget(m_lumValue, 4, 5, 1, 1);

	layoutPtr->addLayout(grid.release());

	loadEntries();
	rebuildList();
	setColour(s_lastColour);

	connect(m_list, &QTableWidget::itemSelectionChanged, this,
	        [this]
	        {
		        if (m_updating)
			        return;
		        const int row = m_list->currentRow();
		        if (row < 0 || row >= m_list->rowCount())
			        return;
		        const QTableWidgetItem *item = m_list->item(row, 0);
		        if (!item)
			        return;
		        const auto   colorRef = item->data(Qt::UserRole).toUInt();
		        const QColor colour(qmudRed(colorRef), qmudGreen(colorRef), qmudBlue(colorRef));
		        if (colour.isValid())
			        setColour(colour);
	        });
	connect(m_list, &QTableWidget::itemDoubleClicked, this, [this] { accept(); });
	connect(m_sort, &QComboBox::currentIndexChanged, this,
	        [this](const int index)
	        {
		        s_sort = index;
		        rebuildList();
		        selectCurrentColour();
	        });

	connect(m_redSlider, &QSlider::valueChanged, this,
	        [this](const int value)
	        {
		        if (m_updating)
			        return;
		        setColour(QColor(value, m_colour.green(), m_colour.blue()));
	        });
	connect(m_greenSlider, &QSlider::valueChanged, this,
	        [this](const int value)
	        {
		        if (m_updating)
			        return;
		        setColour(QColor(m_colour.red(), value, m_colour.blue()));
	        });
	connect(m_blueSlider, &QSlider::valueChanged, this,
	        [this](const int value)
	        {
		        if (m_updating)
			        return;
		        setColour(QColor(m_colour.red(), m_colour.green(), value));
	        });

	connect(m_swatch, &QPushButton::clicked, this,
	        [this]
	        {
		        QColorDialog dlg(m_colour, this);
		        dlg.setOption(QColorDialog::ShowAlphaChannel, false);
		        if (dlg.exec() != Accepted)
			        return;
		        setColour(dlg.selectedColor());
	        });

	connect(otherColourPtr, &QPushButton::clicked, this,
	        [this]
	        {
		        QColorDialog dlg(m_colour, this);
		        dlg.setOption(QColorDialog::ShowAlphaChannel, false);
		        if (dlg.exec() != Accepted)
			        return;
		        setColour(dlg.selectedColor());
	        });
	connect(randomPtr, &QPushButton::clicked, this,
	        [this]
	        {
		        const int value = QRandomGenerator::global()->bounded(0x1000000);
		        setColour(QColor(value & 0xFF, value >> 8 & 0xFF, value >> 16 & 0xFF));
	        });
	connect(m_paste, &QPushButton::clicked, this,
	        [this]
	        {
		        if (m_clipboardColour.isValid())
			        setColour(m_clipboardColour);
	        });
	connect(m_ok, &QPushButton::clicked, this, &QDialog::accept);
	connect(closePtr, &QPushButton::clicked, this, &QDialog::reject);

	connect(m_mxpName, &QPushButton::clicked, this,
	        [this]
	        {
		        QGuiApplication::clipboard()->setText(m_mxpName->text());
		        accept();
	        });
	connect(m_luaName, &QPushButton::clicked, this,
	        [this]
	        {
		        QGuiApplication::clipboard()->setText(m_luaName->text());
		        accept();
	        });
	connect(m_colourName, &QPushButton::clicked, this,
	        [this]
	        {
		        QGuiApplication::clipboard()->setText(m_colourName->text());
		        accept();
	        });

	connect(qApp->clipboard(), &QClipboard::dataChanged, this, [this] { updateClipboardState(); });

	updateClipboardState();
	setLayout(layout.release());
}

void ColourPickerDialog::setPickColour(const bool pick)
{
	m_pickColour = pick;
	if (m_ok)
		m_ok->setText(QStringLiteral("OK"));
}

void ColourPickerDialog::setInitialColour(const QColor &colour)
{
	setColour(colour);
}

QColor ColourPickerDialog::selectedColour() const
{
	return m_colour;
}

void ColourPickerDialog::loadEntries()
{
	m_entries.clear();
	for (const auto &[name, rgb] : kMxpColours)
	{
		Entry e;
		e.name                                  = QString::fromLatin1(name);
		e.color                                 = rgbToQColor(rgb);
		const auto [hue, saturation, luminance] = hslFromColor(e.color);
		e.hue                                   = hue;
		e.saturation                            = saturation;
		e.luminance                             = luminance;
		m_entries.push_back(e);
	}
}

void ColourPickerDialog::rebuildList()
{
	QVector<Entry> sorted = m_entries;
	std::ranges::sort(sorted,
	                  [](const Entry &a, const Entry &b)
	                  {
		                  switch (s_sort)
		                  {
		                  case SortRed:
			                  if (a.color.red() != b.color.red())
				                  return a.color.red() < b.color.red();
			                  break;
		                  case SortGreen:
			                  if (a.color.green() != b.color.green())
				                  return a.color.green() < b.color.green();
			                  break;
		                  case SortBlue:
			                  if (a.color.blue() != b.color.blue())
				                  return a.color.blue() < b.color.blue();
			                  break;
		                  default:
			                  break;
		                  }

		                  switch (s_sort)
		                  {
		                  case SortName:
			                  return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
		                  case SortHue:
			                  if (a.hue != b.hue)
				                  return a.hue < b.hue;
			                  if (a.saturation != b.saturation)
				                  return a.saturation < b.saturation;
			                  return a.luminance < b.luminance;
		                  case SortSaturation:
			                  if (a.saturation != b.saturation)
				                  return a.saturation < b.saturation;
			                  return a.luminance < b.luminance;
		                  case SortLuminance:
			                  if (a.luminance != b.luminance)
				                  return a.luminance < b.luminance;
			                  return a.saturation < b.saturation;
		                  case SortRed:
		                  case SortGreen:
		                  case SortBlue:
			                  if (a.hue != b.hue)
				                  return a.hue < b.hue;
			                  if (a.saturation != b.saturation)
				                  return a.saturation < b.saturation;
			                  return a.luminance < b.luminance;
		                  default:
			                  return false;
		                  }
	                  });

	m_updating = true;
	m_list->setRowCount(sizeToInt(sorted.size()));
	int row = 0;
	for (const Entry &entry : sorted)
	{
		const int colorRef =
		    static_cast<int>(qmudRgb(entry.color.red(), entry.color.green(), entry.color.blue()));

		auto nameItem = std::make_unique<QTableWidgetItem>(entry.name);
		nameItem->setData(Qt::UserRole, colorRef);
		auto swatchItem = std::make_unique<QTableWidgetItem>();
		swatchItem->setData(Qt::UserRole, colorRef);
		swatchItem->setBackground(QBrush(entry.color));
		swatchItem->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
		nameItem->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
		setOwnedItem(m_list, row, 0, std::move(nameItem));
		setOwnedItem(m_list, row, 1, std::move(swatchItem));
		++row;
	}
	m_updating = false;
}

void ColourPickerDialog::selectCurrentColour() const
{
	if (m_list->rowCount() == 0)
		return;
	for (int row = 0; row < m_list->rowCount(); ++row)
	{
		const QTableWidgetItem *item = m_list->item(row, 0);
		if (!item)
			continue;
		const auto   colorRef = item->data(Qt::UserRole).toUInt();
		const QColor rowColour(qmudRed(colorRef), qmudGreen(colorRef), qmudBlue(colorRef));
		if (rowColour.isValid() && rowColour == m_colour)
		{
			m_list->setCurrentCell(row, 0);
			m_list->scrollToItem(m_list->item(row, 0));
			return;
		}
	}
}

void ColourPickerDialog::updateFromColour()
{
	m_updating = true;
	updateSwatch(m_swatch, m_colour);

	m_redValue->setText(QString::number(m_colour.red()));
	m_greenValue->setText(QString::number(m_colour.green()));
	m_blueValue->setText(QString::number(m_colour.blue()));
	m_redSlider->setValue(m_colour.red());
	m_greenSlider->setValue(m_colour.green());
	m_blueSlider->setValue(m_colour.blue());

	const auto [hue, saturation, luminance] = hslFromColor(m_colour);
	m_hueValue->setText(QStringLiteral("Hue: %1").arg(hue, 0, 'f', 1));
	m_satValue->setText(QStringLiteral("Saturation: %1").arg(saturation, 0, 'f', 3));
	m_lumValue->setText(QStringLiteral("Luminance: %1").arg(luminance, 0, 'f', 3));

	const QString names = colourNamesFor(m_colour);
	m_colourName->setText(names.isEmpty() ? QStringLiteral("Colour name") : names);
	m_colourName->setEnabled(!names.isEmpty());

	m_mxpName->setText(QStringLiteral("#%1%2%3")
	                       .arg(m_colour.red(), 2, 16, QLatin1Char('0'))
	                       .arg(m_colour.green(), 2, 16, QLatin1Char('0'))
	                       .arg(m_colour.blue(), 2, 16, QLatin1Char('0'))
	                       .toUpper());
	const int colourRef = static_cast<int>(qmudRgb(m_colour.red(), m_colour.green(), m_colour.blue()));
	m_luaName->setText(QString::number(colourRef));

	m_updating = false;
}

void ColourPickerDialog::updateSwatch(QPushButton *button, const QColor &color)
{
	if (!button)
		return;
	const QString css = QStringLiteral("background-color: %1;").arg(color.name());
	button->setStyleSheet(css);
}

void ColourPickerDialog::updateClipboardState()
{
	QColor     color;
	const bool ok     = tryParseClipboardColour(color);
	m_clipboardColour = ok ? color : QColor();
	if (m_paste)
		m_paste->setEnabled(ok);
}

void ColourPickerDialog::setColour(const QColor &color)
{
	if (!color.isValid())
		return;
	m_colour     = color;
	s_lastColour = color;
	updateFromColour();
	selectCurrentColour();
}

bool ColourPickerDialog::tryParseClipboardColour(QColor &color)
{
	const QString clip = QGuiApplication::clipboard()->text().trimmed();
	if (clip.isEmpty())
		return false;

	const QString lower = clip.toLower();
	for (const auto &[name, rgb] : kMxpColours)
	{
		if (lower == QLatin1String(name))
		{
			color = rgbToQColor(rgb);
			return color.isValid();
		}
	}

	const QString text = clip.trimmed().toUpper();
	if (!text.startsWith(QStringLiteral("#")))
	{
		bool ok = false;
		if (const int numeric = text.toInt(&ok); ok && numeric >= 0 && numeric <= 0xFFFFFF)
		{
			color = QColor(numeric & 0xFF, numeric >> 8 & 0xFF, numeric >> 16 & 0xFF);
			return color.isValid();
		}
		return false;
	}
	constexpr int index = 1;

	if (index >= text.size() || (!text.at(index).isDigit() && !text.at(index).isLetter()))
		return false;

	bool      ok    = false;
	const int value = text.mid(index).toInt(&ok, 16);
	if (!ok || value < 0 || value > 0xFFFFFF)
		return false;
	color = QColor(value >> 16 & 0xFF, value >> 8 & 0xFF, value & 0xFF);
	return color.isValid();
}

QString ColourPickerDialog::colourNamesFor(const QColor &color)
{
	if (!color.isValid())
		return {};
	QStringList names;
	const int   rgb = color.red() << 16 | color.green() << 8 | color.blue();
	for (const auto &[name, value] : kMxpColours)
	{
		if (static_cast<int>(value) == rgb)
			names.push_back(QString::fromLatin1(name));
	}
	return names.join(QStringLiteral(", "));
}

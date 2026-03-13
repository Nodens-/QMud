/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: AcceleratorUtils.cpp
 * Role: Key-sequence normalization and accelerator helper logic used to keep shortcut handling consistent across input
 * paths.
 */

#include "AcceleratorUtils.h"
#include <QtCore/Qt>
#include <QtGui/QKeySequence>
#include <limits>

namespace
{

	// Windows-compatible virtual-key codes used by persisted/script accelerator APIs.
	constexpr quint16 kVkBack      = 0x08;
	constexpr quint16 kVkTab       = 0x09;
	constexpr quint16 kVkClear     = 0x0C;
	constexpr quint16 kVkReturn    = 0x0D;
	constexpr quint16 kVkShift     = 0x10;
	constexpr quint16 kVkControl   = 0x11;
	constexpr quint16 kVkMenu      = 0x12;
	constexpr quint16 kVkPause     = 0x13;
	constexpr quint16 kVkCapital   = 0x14;
	constexpr quint16 kVkEscape    = 0x1B;
	constexpr quint16 kVkSpace     = 0x20;
	constexpr quint16 kVkPrior     = 0x21;
	constexpr quint16 kVkNext      = 0x22;
	constexpr quint16 kVkEnd       = 0x23;
	constexpr quint16 kVkHome      = 0x24;
	constexpr quint16 kVkLeft      = 0x25;
	constexpr quint16 kVkUp        = 0x26;
	constexpr quint16 kVkRight     = 0x27;
	constexpr quint16 kVkDown      = 0x28;
	constexpr quint16 kVkSelect    = 0x29;
	constexpr quint16 kVkPrint     = 0x2A;
	constexpr quint16 kVkExecute   = 0x2B;
	constexpr quint16 kVkSnapshot  = 0x2C;
	constexpr quint16 kVkInsert    = 0x2D;
	constexpr quint16 kVkDelete    = 0x2E;
	constexpr quint16 kVkHelp      = 0x2F;
	constexpr quint16 kVk0         = 0x30;
	constexpr quint16 kVk9         = 0x39;
	constexpr quint16 kVkA         = 0x41;
	constexpr quint16 kVkZ         = 0x5A;
	constexpr quint16 kVkLWin      = 0x5B;
	constexpr quint16 kVkRWin      = 0x5C;
	constexpr quint16 kVkNumpad0   = 0x60;
	constexpr quint16 kVkNumpad9   = 0x69;
	constexpr quint16 kVkMultiply  = 0x6A;
	constexpr quint16 kVkAdd       = 0x6B;
	constexpr quint16 kVkSeparator = 0x6C;
	constexpr quint16 kVkSubtract  = 0x6D;
	constexpr quint16 kVkDecimal   = 0x6E;
	constexpr quint16 kVkDivide    = 0x6F;
	constexpr quint16 kVkF1        = 0x70;
	constexpr quint16 kVkF24       = 0x87;
	constexpr quint16 kVkNumlock   = 0x90;
	constexpr quint16 kVkScroll    = 0x91;
	constexpr quint16 kVkOem1      = 0xBA;
	constexpr quint16 kVkOemPlus   = 0xBB;
	constexpr quint16 kVkOemComma  = 0xBC;
	constexpr quint16 kVkOemMinus  = 0xBD;
	constexpr quint16 kVkOemPeriod = 0xBE;
	constexpr quint16 kVkOem2      = 0xBF;
	constexpr quint16 kVkOem3      = 0xC0;
	constexpr quint16 kVkOem4      = 0xDB;
	constexpr quint16 kVkOem5      = 0xDC;
	constexpr quint16 kVkOem6      = 0xDD;
	constexpr quint16 kVkOem7      = 0xDE;

} // namespace

namespace AcceleratorUtils
{

	quint16 qtKeyToVirtualKey(Qt::Key key, bool keypad)
	{
		if (key >= Qt::Key_0 && key <= Qt::Key_9)
			return static_cast<quint16>((keypad ? kVkNumpad0 : kVk0) + (key - Qt::Key_0));
		if (key >= Qt::Key_A && key <= Qt::Key_Z)
			return static_cast<quint16>(kVkA + (key - Qt::Key_A));
		if (key >= Qt::Key_F1 && key <= Qt::Key_F24)
			return static_cast<quint16>(kVkF1 + (key - Qt::Key_F1));

		switch (key)
		{
		case Qt::Key_Backspace:
			return kVkBack;
		case Qt::Key_Tab:
		case Qt::Key_Backtab:
			return kVkTab;
		case Qt::Key_Clear:
			return kVkClear;
		case Qt::Key_Enter:
		case Qt::Key_Return:
			return kVkReturn;
		case Qt::Key_Shift:
			return kVkShift;
		case Qt::Key_Control:
			return kVkControl;
		case Qt::Key_Alt:
			return kVkMenu;
		case Qt::Key_Pause:
			return kVkPause;
		case Qt::Key_CapsLock:
			return kVkCapital;
		case Qt::Key_Escape:
			return kVkEscape;
		case Qt::Key_Space:
			return kVkSpace;
		case Qt::Key_PageUp:
			return kVkPrior;
		case Qt::Key_PageDown:
			return kVkNext;
		case Qt::Key_End:
			return kVkEnd;
		case Qt::Key_Home:
			return kVkHome;
		case Qt::Key_Left:
			return kVkLeft;
		case Qt::Key_Up:
			return kVkUp;
		case Qt::Key_Right:
			return kVkRight;
		case Qt::Key_Down:
			return kVkDown;
		case Qt::Key_Select:
			return kVkSelect;
		case Qt::Key_Print:
			return kVkPrint;
		case Qt::Key_Execute:
			return kVkExecute;
		case Qt::Key_Insert:
			return kVkInsert;
		case Qt::Key_Delete:
			return kVkDelete;
		case Qt::Key_Help:
			return kVkHelp;
		case Qt::Key_NumLock:
			return kVkNumlock;
		case Qt::Key_ScrollLock:
			return kVkScroll;
		case Qt::Key_Meta:
			return kVkLWin;
		case Qt::Key_Semicolon:
		case Qt::Key_Colon:
			return kVkOem1;
		case Qt::Key_Equal:
		case Qt::Key_Plus:
			return keypad ? kVkAdd : kVkOemPlus;
		case Qt::Key_Comma:
		case Qt::Key_Less:
			return keypad ? kVkDecimal : kVkOemComma;
		case Qt::Key_Minus:
		case Qt::Key_Underscore:
			return keypad ? kVkSubtract : kVkOemMinus;
		case Qt::Key_Period:
		case Qt::Key_Greater:
			return keypad ? kVkDecimal : kVkOemPeriod;
		case Qt::Key_Slash:
		case Qt::Key_Question:
			return keypad ? kVkDivide : kVkOem2;
		case Qt::Key_QuoteLeft:
		case Qt::Key_AsciiTilde:
			return kVkOem3;
		case Qt::Key_BracketLeft:
		case Qt::Key_BraceLeft:
			return kVkOem4;
		case Qt::Key_Backslash:
		case Qt::Key_Bar:
			return kVkOem5;
		case Qt::Key_BracketRight:
		case Qt::Key_BraceRight:
			return kVkOem6;
		case Qt::Key_Apostrophe:
		case Qt::Key_QuoteDbl:
			return kVkOem7;
		case Qt::Key_Asterisk:
			return keypad ? kVkMultiply : static_cast<quint16>('*');
		default:
			break;
		}

		const int raw = static_cast<int>(key);
		if (raw >= 0x20 && raw <= 0x7E)
			return static_cast<quint16>(raw);
		return 0;
	}

	Qt::Key virtualKeyToQtKey(quint16 key)
	{
		if (key >= kVk0 && key <= kVk9)
			return static_cast<Qt::Key>(Qt::Key_0 + (key - kVk0));
		if (key >= kVkA && key <= kVkZ)
			return static_cast<Qt::Key>(Qt::Key_A + (key - kVkA));
		if (key >= kVkF1 && key <= kVkF24)
			return static_cast<Qt::Key>(Qt::Key_F1 + (key - kVkF1));
		if (key >= kVkNumpad0 && key <= kVkNumpad9)
			return static_cast<Qt::Key>(Qt::Key_0 + (key - kVkNumpad0));

		switch (key)
		{
		case kVkBack:
			return Qt::Key_Backspace;
		case kVkTab:
			return Qt::Key_Tab;
		case kVkClear:
			return Qt::Key_Clear;
		case kVkReturn:
			return Qt::Key_Return;
		case kVkShift:
			return Qt::Key_Shift;
		case kVkControl:
			return Qt::Key_Control;
		case kVkMenu:
			return Qt::Key_Alt;
		case kVkPause:
			return Qt::Key_Pause;
		case kVkCapital:
			return Qt::Key_CapsLock;
		case kVkEscape:
			return Qt::Key_Escape;
		case kVkSpace:
			return Qt::Key_Space;
		case kVkPrior:
			return Qt::Key_PageUp;
		case kVkNext:
			return Qt::Key_PageDown;
		case kVkEnd:
			return Qt::Key_End;
		case kVkHome:
			return Qt::Key_Home;
		case kVkLeft:
			return Qt::Key_Left;
		case kVkUp:
			return Qt::Key_Up;
		case kVkRight:
			return Qt::Key_Right;
		case kVkDown:
			return Qt::Key_Down;
		case kVkSelect:
			return Qt::Key_Select;
		case kVkPrint:
		case kVkSnapshot:
			return Qt::Key_Print;
		case kVkExecute:
			return Qt::Key_Execute;
		case kVkInsert:
			return Qt::Key_Insert;
		case kVkDelete:
			return Qt::Key_Delete;
		case kVkHelp:
			return Qt::Key_Help;
		case kVkLWin:
		case kVkRWin:
			return Qt::Key_Meta;
		case kVkNumlock:
			return Qt::Key_NumLock;
		case kVkScroll:
			return Qt::Key_ScrollLock;
		case kVkMultiply:
			return Qt::Key_Asterisk;
		case kVkAdd:
			return Qt::Key_Plus;
		case kVkSeparator:
			return Qt::Key_Comma;
		case kVkSubtract:
			return Qt::Key_Minus;
		case kVkDecimal:
			return Qt::Key_Period;
		case kVkDivide:
			return Qt::Key_Slash;
		case kVkOem1:
			return Qt::Key_Semicolon;
		case kVkOemPlus:
			return Qt::Key_Plus;
		case kVkOemComma:
			return Qt::Key_Comma;
		case kVkOemMinus:
			return Qt::Key_Minus;
		case kVkOemPeriod:
			return Qt::Key_Period;
		case kVkOem2:
			return Qt::Key_Slash;
		case kVkOem3:
			return Qt::Key_QuoteLeft;
		case kVkOem4:
			return Qt::Key_BracketLeft;
		case kVkOem5:
			return Qt::Key_Backslash;
		case kVkOem6:
			return Qt::Key_BracketRight;
		case kVkOem7:
			return Qt::Key_Apostrophe;
		default:
			break;
		}

		if (key >= 0x20 && key <= 0x7E)
			return static_cast<Qt::Key>(key);
		return static_cast<Qt::Key>(0);
	}

	bool virtualKeyUsesKeypadModifier(quint16 key)
	{
		return (key >= kVkNumpad0 && key <= kVkNumpad9) || key == kVkMultiply || key == kVkAdd ||
		       key == kVkSeparator || key == kVkSubtract || key == kVkDecimal || key == kVkDivide;
	}

	bool stringToAccelerator(const QString &text, quint32 &virt, quint16 &key)
	{
		const QKeySequence seq = QKeySequence::fromString(text, QKeySequence::PortableText);
		if (seq.isEmpty())
			return false;

		const QKeyCombination combo = seq[0];
		if (combo.key() == static_cast<Qt::Key>(0))
			return false;

		Qt::KeyboardModifiers mods   = combo.keyboardModifiers();
		const bool            keypad = (mods & Qt::KeypadModifier) != 0;
		mods &= ~Qt::KeypadModifier;
		if ((mods & Qt::MetaModifier) != 0)
			return false;

		const quint16 mappedKey = qtKeyToVirtualKey(combo.key(), keypad);
		if (mappedKey != 0)
			key = mappedKey;
		else
		{
			const int keyPart = static_cast<int>(combo.key());
			if (keyPart <= 0 || keyPart > std::numeric_limits<quint16>::max())
				return false;
			key = static_cast<quint16>(keyPart);
		}

		virt = kVirtKeyFlag | kNoInvertFlag;
		if (mods & Qt::ShiftModifier)
			virt |= kShiftFlag;
		if (mods & Qt::ControlModifier)
			virt |= kControlFlag;
		if (mods & Qt::AltModifier)
			virt |= kAltFlag;

		return true;
	}

	QString acceleratorToString(quint32 virt, quint16 key)
	{
		Qt::KeyboardModifiers modifiers;
		if (virt & kShiftFlag)
			modifiers |= Qt::ShiftModifier;
		if (virt & kControlFlag)
			modifiers |= Qt::ControlModifier;
		if (virt & kAltFlag)
			modifiers |= Qt::AltModifier;

		Qt::Key qtKey = virtualKeyToQtKey(key);
		if (qtKey == static_cast<Qt::Key>(0))
		{
			// Backward compatibility for legacy malformed entries that stored raw low 16 bits of Qt::Key.
			qtKey = static_cast<Qt::Key>(key);
		}
		if (qtKey == static_cast<Qt::Key>(0))
			return {};

		if (virtualKeyUsesKeypadModifier(key))
			modifiers |= Qt::KeypadModifier;

		const QKeySequence seq(QKeyCombination(modifiers, qtKey));
		if (seq.isEmpty())
			return {};

		return seq.toString(QKeySequence::PortableText);
	}

} // namespace AcceleratorUtils

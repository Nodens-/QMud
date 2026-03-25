/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: LuaCallbackEngine.cpp
 * Role: Primary Lua integration engine implementing callback execution, script API functions, and data flow between Lua
 * and the live world runtime.
 */

#include "LuaCallbackEngine.h"

#include "AcceleratorUtils.h"
#include "AppController.h"
#include "DoubleMetaphone.h"
#include "ErrorDescriptions.h"
#include "Flags.h"
#include "FontUtils.h"
#include "InfoTypesMetadata.h"
#include "LuaFunctionTypes.h"
#include "LuaSupport.h"
#include "MainFrame.h"
#include "MainWindowHost.h"
#include "MainWindowHostResolver.h"
#include "SqliteCompat.h"
#include "StringUtils.h"
#include "Version.h"
#include "WorldChildWindow.h"
#include "WorldDocument.h"
#include "WorldOptionDefaults.h"
#include "WorldOptions.h"
#include "WorldRuntime.h"
#include "WorldView.h"
#include "dialogs/SpellCheckDialog.h"
#include "scripting/ScriptingErrors.h"

#include <QApplication>
#include <QBitmap>
#include <QByteArray>
#include <QClipboard>
#include <QColor>
#include <QColorDialog>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDesktopServices>
#include <QDialog>
// ReSharper disable once CppUnusedIncludeDirective
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontDialog>
#include <QGuiApplication>
#include <QHostInfo>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMdiArea>
#include <QMessageBox>
#include <QMetaType>
#include <QOperatingSystemVersion>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressDialog>
#include <QPushButton>
#include <QRawFont>
#include <QRegularExpression>
#include <QScreen>
#include <QSet>
#include <QStringDecoder>
#include <QStyleHints>
#include <QTextBrowser>
#include <QTextStream>
#include <QTimeZone>
#include <QToolBar>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>
#include <QVector>
#include <QXmlStreamReader>
#include <QtMath>
#include <QtNumeric>
#include <QtSql/QSqlDatabase>
#include <QtSql/QSqlQuery>
#include <QtSql/QSqlRecord>
#include <algorithm>
#include <limits>
#include <memory>
#ifdef Q_OS_WIN
#include <winnls.h>
#ifndef VER_PLATFORM_WIN32_NT
#define VER_PLATFORM_WIN32_NT 2
#endif
#endif

#ifdef HILITE
constexpr int kStyleHilite = HILITE;
#else
constexpr int kStyleHilite = 0x0001;
#endif
#ifdef UNDERLINE
constexpr int kStyleUnderline = UNDERLINE;
#else
constexpr int kStyleUnderline = 0x0002;
#endif
#ifdef BLINK
constexpr int kStyleBlink = BLINK;
#else
constexpr int kStyleBlink = 0x0004;
#endif
#ifdef INVERSE
constexpr int kStyleInverse = INVERSE;
#else
constexpr int kStyleInverse = 0x0008;
#endif
// Keep legacy ShowWindow-compatible values returned by GetInfo(238),
// while deriving state from Qt window visibility/minimized/maximized flags.
constexpr int kWindowShowHide      = 0;
constexpr int kWindowShowNormal    = 1;
constexpr int kWindowShowMinimized = 2;
constexpr int kWindowShowMaximized = 3;

#ifdef TRIGGER_MATCH_TEXT
constexpr int kTriggerMatchText = TRIGGER_MATCH_TEXT;
#else
constexpr int kTriggerMatchText = 0x0080;
#endif
#ifdef TRIGGER_MATCH_BACK
constexpr int kTriggerMatchBack = TRIGGER_MATCH_BACK;
#else
constexpr int kTriggerMatchBack = 0x0800;
#endif
#ifdef TRIGGER_MATCH_HILITE
constexpr int kTriggerMatchHilite = TRIGGER_MATCH_HILITE;
#else
constexpr int kTriggerMatchHilite = 0x1000;
#endif
#ifdef TRIGGER_MATCH_UNDERLINE
constexpr int kTriggerMatchUnderline = TRIGGER_MATCH_UNDERLINE;
#else
constexpr int kTriggerMatchUnderline = 0x2000;
#endif
#ifdef TRIGGER_MATCH_BLINK
constexpr int kTriggerMatchBlink = TRIGGER_MATCH_BLINK;
#else
constexpr int kTriggerMatchBlink = 0x4000;
#endif
#ifdef TRIGGER_MATCH_INVERSE
constexpr int kTriggerMatchInverse = TRIGGER_MATCH_INVERSE;
#else
constexpr int kTriggerMatchInverse = 0x8000;
#endif

// POSIX-style regex flag values exposed by rex.flagsPOSIX / rex.newPOSIX.
// Regex matching is implemented with QRegularExpression on all platforms.
constexpr int kRegexExtended   = 0x01;
constexpr int kRegexIgnoreCase = 0x02;
constexpr int kRegexNoSub      = 0x04;
constexpr int kRegexNewline    = 0x08;
namespace
{
	char asciiToLower(const char c)
	{
		if (c >= 'A' && c <= 'Z')
			return static_cast<char>(c + ('a' - 'A'));
		return c;
	}

	bool isAsciiSpace(const unsigned char c)
	{
		switch (c)
		{
		case ' ':
		case '\t':
		case '\n':
		case '\r':
		case '\f':
		case '\v':
			return true;
		default:
			return false;
		}
	}

	int asciiHexValue(const unsigned char c)
	{
		if (c >= '0' && c <= '9')
			return c - '0';
		if (c >= 'a' && c <= 'f')
			return c - 'a' + 10;
		if (c >= 'A' && c <= 'F')
			return c - 'A' + 10;
		return -1;
	}

	QString qmudPngVersionString()
	{
		static const QString kPngVersion = []() -> QString
		{
			for (const QList<QByteArray> formats = QImageReader::supportedImageFormats();
			     const QByteArray       &format : formats)
			{
				if (format.compare("png", Qt::CaseInsensitive) == 0)
					return QStringLiteral("Qt PNG plugin");
			}
			return {};
		}();
		return kPngVersion;
	}

	QString normalizeGroupName(const QString &name)
	{
		return name.trimmed().toLower();
	}

	bool groupMatches(const QString &candidate, const QString &groupName)
	{
		const QString normalizedGroup = normalizeGroupName(groupName);
		return !normalizedGroup.isEmpty() && normalizeGroupName(candidate) == normalizedGroup;
	}

	bool pushLuaFunctionByName(lua_State *state, const QString &functionName)
	{
		if (!state || functionName.isEmpty())
			return false;

		const QByteArray fullName = functionName.toUtf8();
		lua_getglobal(state, fullName.constData());
		if (lua_isfunction(state, -1))
			return true;
		lua_pop(state, 1);

		const QStringList parts = functionName.split(QLatin1Char('.'), Qt::SkipEmptyParts);
		if (parts.size() < 2)
			return false;

		const QByteArray first = parts.first().toUtf8();
		lua_getglobal(state, first.constData());
		if (!lua_istable(state, -1))
		{
			lua_pop(state, 1);
			return false;
		}

		for (int i = 1; i < parts.size(); ++i)
		{
			const QByteArray part = parts[i].toUtf8();
			lua_getfield(state, -1, part.constData());
			lua_remove(state, -2);
			if (i < parts.size() - 1 && !lua_istable(state, -1))
			{
				lua_pop(state, 1);
				return false;
			}
		}

		if (!lua_isfunction(state, -1))
		{
			lua_pop(state, 1);
			return false;
		}

		return true;
	}
} // namespace
constexpr int kLogPixelsXDeviceCapIndex = 88;
constexpr int kLogPixelsYDeviceCapIndex = 90;
constexpr int kFontFamilyDontCare       = 0;

#ifdef QMUD_ENABLE_LUA_SCRIPTING
extern "C" int luaopen_lsqlite3(lua_State *L);
#ifdef QMUD_BUNDLED_BC
extern "C" int luaopen_bc(lua_State *L);
#endif
static void             extendLuaPackagePath(lua_State *L, const QString &appDir);
static void             refreshLuaFunctionSetForState(lua_State *L, QSet<QString> &out);
static bool             optBool(lua_State *L, int index, bool defaultValue);

static QString          concatLuaArgs(lua_State *L, int startIndex, const QString &delimiter = QString());
static WorldRuntime    *runtimeFromLua(lua_State *L);
static void             pushVariant(lua_State *L, const QVariant &value);
static WorldRuntime    *runtimeFromLuaUpvalue(lua_State *L);
static QString          runtimeWorldIdFromLuaUpvalue(lua_State *L);
static TextChildWindow *findNotepadWindow(const QString &title, WorldRuntime *ownerRuntime = nullptr,
                                          const QString &worldId = QString());
static void             pushWorldProxy(lua_State *L, LuaCallbackEngine *engine, WorldRuntime *runtime,
                                       const QString &worldId);
static int              pushWorldProxyResult(lua_State *L, LuaCallbackEngine *engine, WorldRuntime *runtime);
static WorldRuntime    *findWorldRuntimeById(const QString &id);
static WorldRuntime    *findWorldRuntimeByName(const QString &name);
static void             pushStringList(lua_State *L, const QStringList &list);
static bool             isEnabledValue(const QString &value);
static bool             parseBooleanKeywordValue(const QString &text, bool &out);
static QString          attrFlag(bool value);
static bool             resolvePluginContext(const LuaCallbackEngine *engine, WorldRuntime *runtime,
                                             WorldRuntime::Plugin *&plugin, int &errorCode);

namespace
{
	long colorValue(const QColor &color)
	{
		return static_cast<long>(color.red()) | static_cast<long>(color.green()) << 8 |
		       static_cast<long>(color.blue()) << 16;
	}

	int sizeToInt(const qsizetype value)
	{
		return static_cast<int>(qBound(static_cast<qsizetype>(0), value,
		                               static_cast<qsizetype>(std::numeric_limits<int>::max())));
	}

	long colourNameFallback(const QString &name)
	{
		const QString trimmed = name.trimmed();
		if (trimmed.isEmpty())
			return -1;

		bool       ok      = false;
		const long numeric = trimmed.toLong(&ok);
		if (ok)
			return numeric >= 0 && numeric <= 0xFFFFFF ? numeric : -1;

		QColor colour(trimmed);
		if (!colour.isValid())
		{
			const QColor lowered(trimmed.toLower());
			if (!lowered.isValid())
				return -1;
			colour = lowered;
		}

		return colorValue(colour);
	}

	QColor colorFromValue(const long value)
	{
		const auto packed = static_cast<QMudColorRef>(value);
		return {qmudRed(packed), qmudGreen(packed), qmudBlue(packed)};
	}

	void registerErrorDescriptions(lua_State *L)
	{
		lua_newtable(L);
		for (const IntFlagsPair *p = kErrorDescriptionMappingTable; p->value != nullptr; ++p)
		{
			lua_pushinteger(L, p->key);
			lua_pushstring(L, p->value);
			lua_rawset(L, -3);
		}
		lua_setglobal(L, "error_desc");
	}

	void registerColourNames(lua_State *L)
	{
		lua_newtable(L);
		for (const QString &name : QColor::colorNames())
		{
			const QColor color(name);
			if (!color.isValid())
				continue;
			lua_pushstring(L, name.toLatin1().constData());
			lua_pushinteger(L, colorValue(color));
			lua_rawset(L, -3);
		}
		lua_setglobal(L, "colour_names");
	}

	void registerExtendedColours(lua_State *L)
	{
		lua_newtable(L);
		const AppController *app = AppController::instance();
		for (int i = 0; i < 256; ++i)
		{
			lua_pushinteger(L, i);
			lua_pushinteger(L, app ? app->xtermColorAt(i) : 0);
			lua_rawset(L, -3);
		}
		lua_setglobal(L, "extended_colours");
	}

	struct LuaNamedInt
	{
			const char *name;
			int         value;
	};

	struct LuaBindingEntry
	{
			const char   *name;
			lua_CFunction function;
	};

	void registerNamedIntTable(lua_State *L, const char *tableName, const LuaNamedInt *entries)
	{
		lua_newtable(L);
		for (const LuaNamedInt *entry = entries; entry && entry->name; ++entry)
		{
			lua_pushinteger(L, entry->value);
			lua_setfield(L, -2, entry->name);
		}
		lua_setglobal(L, tableName);
	}
} // namespace
#endif

#ifdef QMUD_ENABLE_LUA_SCRIPTING
void LuaStateDeleter::operator()(lua_State *state) const
{
	if (state)
		lua_close(state);
}
#endif

LuaCallbackEngine::LuaCallbackEngine() = default;

LuaCallbackEngine::~LuaCallbackEngine() = default;

void LuaCallbackEngine::applyPackageRestrictions(bool enablePackage)
{
#ifdef QMUD_ENABLE_LUA_SCRIPTING
	m_allowPackage = enablePackage;
	if (m_state && (!m_packageRestrictionsApplied || m_packageRestrictionsAppliedValue != enablePackage))
	{
		QMudLuaSupport::applyLuaPackageRestrictions(m_state, enablePackage);
		m_packageRestrictionsApplied      = true;
		m_packageRestrictionsAppliedValue = enablePackage;
	}
#else
	Q_UNUSED(enablePackage);
#endif
}

void LuaCallbackEngine::setScriptText(const QString &script)
{
	m_script       = script;
	m_scriptLoaded = false;
}

bool LuaCallbackEngine::loadScript()
{
#ifdef QMUD_ENABLE_LUA_SCRIPTING
	return ensureState();
#else
	return false;
#endif
}

void LuaCallbackEngine::resetState()
{
#ifdef QMUD_ENABLE_LUA_SCRIPTING
	m_ownedState.reset();
	m_state = nullptr;
	m_luaFunctionsSet.clear();
	m_worldBindingsReady         = false;
	m_scriptLoaded               = false;
	m_packageRestrictionsApplied = false;
#endif
}

void LuaCallbackEngine::setWorldRuntime(WorldRuntime *runtime)
{
	m_worldRuntime       = runtime;
	m_worldBindingsReady = false;
}

WorldRuntime *LuaCallbackEngine::swapWorldRuntime(WorldRuntime *runtime)
{
	WorldRuntime *previous = m_worldRuntime;
	m_worldRuntime         = runtime;
	return previous;
}

WorldRuntime *LuaCallbackEngine::worldRuntime() const
{
	return m_worldRuntime;
}

void LuaCallbackEngine::pushScriptExecutionDepth()
{
	++m_scriptExecutionDepth;
}

void LuaCallbackEngine::popScriptExecutionDepth()
{
	if (m_scriptExecutionDepth > 0)
		--m_scriptExecutionDepth;
}

int LuaCallbackEngine::scriptExecutionDepth() const
{
	return m_scriptExecutionDepth;
}

const QSet<QString> &LuaCallbackEngine::luaFunctionsSet() const
{
	return m_luaFunctionsSet;
}

#ifdef QMUD_ENABLE_LUA_SCRIPTING
lua_State *LuaCallbackEngine::luaState() const
{
	return m_state;
}
#endif

void LuaCallbackEngine::setPluginInfo(const QString &id, const QString &name)
{
	m_pluginId   = id;
	m_pluginName = name;
}

QString LuaCallbackEngine::pluginId() const
{
	return m_pluginId;
}

QString LuaCallbackEngine::pluginName() const
{
	return m_pluginName;
}

bool LuaCallbackEngine::hasFunction(const QString &functionName)
{
#ifdef QMUD_ENABLE_LUA_SCRIPTING
	if (functionName.isEmpty())
		return false;
	if (!ensureState())
		return false;
	if (!pushLuaFunctionByName(m_state, functionName))
		return false;
	lua_pop(m_state, 1);
	return true;
#else
	Q_UNUSED(functionName);
	return false;
#endif
}

#ifdef QMUD_ENABLE_LUA_SCRIPTING
static int luaResetStatusTime(lua_State *L)
{
	const auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
		return 0;
	if (WorldRuntime *runtime = engine->worldRuntime())
		runtime->resetStatusTime();
	return 0;
}

static double toOleDate(const QDateTime &time)
{
	if (!time.isValid())
		return 0.0;
	const QDateTime base(QDate(1899, 12, 30), QTime(0, 0), QTimeZone::systemTimeZone());
	const qint64    seconds = base.secsTo(time);
	return static_cast<double>(seconds) / 86400.0;
}

static MainWindow *resolveMainWindow()
{
	MainWindowHost *host = resolveMainWindowHost(nullptr);
	return dynamic_cast<MainWindow *>(host);
}

template <typename MatchFn> static WorldRuntime *findWorldRuntimeWhere(MatchFn &&matches)
{
	MainWindowHost *host = resolveMainWindowHost(nullptr);
	if (!host)
		return nullptr;
	for (const WorldWindowDescriptor &entry : host->worldWindowDescriptors())
	{
		if (!entry.runtime)
			continue;
		if (matches(entry.runtime))
			return entry.runtime;
	}
	return nullptr;
}

static WorldChildWindow *findWorldWindowByOrdinal(const MainWindowHost &host, const WorldRuntime &runtime,
                                                  const int ordinal)
{
	if (ordinal <= 0)
		return nullptr;
	int count = 0;
	for (const WorldWindowDescriptor &entry : host.worldWindowDescriptors())
	{
		if (entry.runtime != &runtime || !entry.window)
			continue;
		++count;
		if (count == ordinal)
			return entry.window;
	}
	return nullptr;
}

static void addScriptTimeForRuntime(WorldRuntime *runtime, const qint64 nanos)
{
	if (!runtime || nanos <= 0)
		return;
	runtime->addScriptTime(nanos);
}

static void addScriptTimeForEngine(const LuaCallbackEngine &engine, const qint64 nanos)
{
	if (nanos <= 0)
		return;
	addScriptTimeForRuntime(engine.worldRuntime(), nanos);
}

class ScriptExecutionDepthGuard
{
	public:
		explicit ScriptExecutionDepthGuard(LuaCallbackEngine *engine) : m_engine(engine)
		{
			if (m_engine)
				m_engine->pushScriptExecutionDepth();
		}

		~ScriptExecutionDepthGuard()
		{
			if (m_engine)
				m_engine->popScriptExecutionDepth();
		}

	private:
		LuaCallbackEngine *m_engine{nullptr};
};

static bool activateWorldForRuntime(WorldRuntime *runtime)
{
	if (!runtime)
		return false;
	MainWindowHost *host = resolveMainWindowHostForRuntime(runtime);
	if (!host)
		host = resolveMainWindowHost(QApplication::activeWindow());
	if (!host)
		return false;
	return host->activateWorldRuntime(runtime);
}

static bool activateMainWindow(MainWindow *mw)
{
	if (!mw)
		return false;
	if (mw->isMinimized())
		mw->showNormal();
	mw->show();
	mw->raise();
	mw->activateWindow();
	mw->setFocus();
	return true;
}

static int luaActivateNotepad(lua_State *L)
{
	const char   *title   = luaL_checkstring(L, 1);
	const auto   *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	MainWindow   *mw      = resolveMainWindow();
	bool          ok      = false;
	if (mw)
		ok = mw->activateNotepad(QString::fromUtf8(title), runtime);
	lua_pushboolean(L, ok);
	return 1;
}

static int luaActivate(lua_State *L)
{
	const auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
		return 0;
	activateWorldForRuntime(engine->worldRuntime());
	return 0;
}

static int luaActivateClient(lua_State *L)
{
	const int stackTop = lua_gettop(L);
	Q_UNUSED(stackTop);
	MainWindow *mw = resolveMainWindow();
	activateMainWindow(mw);
	return 0;
}

static int luaAddFont(lua_State *L)
{
	const auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}
	const char *path   = luaL_checkstring(L, 1);
	const int   result = runtime->addFontFromFile(QString::fromUtf8(path));
	lua_pushnumber(L, result);
	return 1;
}

static int luaAddMapperComment(lua_State *L)
{
	const auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}
	const char *comment = luaL_checkstring(L, 1);
	const int   result  = runtime->addMapperComment(QString::fromUtf8(comment));
	lua_pushnumber(L, result);
	return 1;
}

static int luaAddSpellCheckWord(lua_State *L)
{
	size_t      originalLen    = 0;
	size_t      actionLen      = 0;
	size_t      replacementLen = 0;
	const char *original       = luaL_checklstring(L, 1, &originalLen);
	const char *action         = luaL_checklstring(L, 2, &actionLen);
	const char *replacement    = luaL_optlstring(L, 3, "", &replacementLen);
	if (originalLen == 0 || originalLen > 63 || replacementLen > 63)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	if (!action || actionLen != 1)
	{
		lua_pushnumber(L, eUnknownOption);
		return 1;
	}
	switch (action[0])
	{
	case 'a':
	case 'A':
	case 'c':
	case 'C':
	case 'e':
	case 'i':
		break;
	default:
		lua_pushnumber(L, eUnknownOption);
		return 1;
	}

	AppController *controller = AppController::instance();
	if (!controller || !controller->ensureSpellCheckerLoaded())
	{
		lua_pushnumber(L, eSpellCheckNotActive);
		return 1;
	}

	lua_State *spell = controller->spellCheckerLuaState();
	if (!spell)
	{
		lua_pushnumber(L, eSpellCheckNotActive);
		return 1;
	}

	lua_settop(spell, 0);
	lua_getglobal(spell, "spellcheck_add_word");
	if (!lua_isfunction(spell, -1))
	{
		lua_settop(spell, 0);
		lua_pushnumber(L, eSpellCheckNotActive);
		return 1;
	}
	lua_pushlstring(spell, original, originalLen);
	lua_pushlstring(spell, action, actionLen);
	lua_pushlstring(spell, replacement, replacementLen);
	if (const int error = QMudLuaSupport::callLuaWithTraceback(spell, 3, 1); error)
	{
		QMudLuaSupport::luaError(spell, "Run-time error", "spellcheck_add_word", "world.AddSpellCheckWord");
		controller->closeSpellChecker();
		lua_pushnumber(L, eSpellCheckNotActive);
		return 1;
	}

	if (lua_isboolean(spell, -1))
	{
		const int ok = lua_toboolean(spell, -1);
		lua_settop(spell, 0);
		lua_pushnumber(L, ok ? eOK : eBadParameter);
		return 1;
	}
	lua_settop(spell, 0);
	lua_pushnumber(L, eOK);
	return 1;
}

static int luaAddToMapper(lua_State *L)
{
	const auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}
	const char *direction = luaL_checkstring(L, 1);
	const char *reverse   = luaL_checkstring(L, 2);
	const int   result    = runtime->addToMapper(QString::fromUtf8(direction), QString::fromUtf8(reverse));
	lua_pushnumber(L, result);
	return 1;
}

static int luaAdjustColour(lua_State *L)
{
	const auto   *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	const long colour = luaL_checkinteger(L, 1);
	const auto method = static_cast<short>(luaL_checkinteger(L, 2));
	const long result = WorldRuntime::adjustColour(colour, method);
	lua_pushnumber(L, static_cast<lua_Number>(result));
	return 1;
}

static int luaANSI(lua_State *L)
{
	const int   count = lua_gettop(L);
	QStringList parts;
	parts.reserve(count);
	for (int i = 1; i <= count; ++i)
		parts << QString::number(static_cast<int>(luaL_checkinteger(L, i)));
	const QString seq = QStringLiteral("\x1b[%1m").arg(parts.join(';'));
	lua_pushstring(L, seq.toUtf8().constData());
	return 1;
}

static int luaAnsiNote(lua_State *L)
{
	const auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
		return 0;
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
		return 0;
	if (const QString text = concatLuaArgs(L, 1); text.isEmpty())
		runtime->outputText(QString(), true, true);
	else
		runtime->outputAnsiText(text, true);
	return 0;
}

static int luaBlendPixel(lua_State *L)
{
	const auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	const WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, -1);
		return 1;
	}
	const long   blend   = static_cast<long>(luaL_checknumber(L, 1));
	const long   base    = static_cast<long>(luaL_checknumber(L, 2));
	const auto   mode    = static_cast<short>(luaL_checkinteger(L, 3));
	const double opacity = luaL_checknumber(L, 4);
	lua_pushnumber(L, static_cast<lua_Number>(WorldRuntime::blendPixel(blend, base, mode, opacity)));
	return 1;
}

static int luaBookmark(lua_State *L)
{
	const auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
		return 0;
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
		return 0;

	const int  lineNumber = static_cast<int>(luaL_checkinteger(L, 1));
	const bool set        = optBool(L, 2, true);
	runtime->bookmarkLine(lineNumber, set);
	return 0;
}

static int luaGetBoldColour(lua_State *L)
{
	const auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, 0);
		return 1;
	}

	const int    index = static_cast<int>(luaL_checkinteger(L, 1));
	const QColor color = runtime->ansiColour(true, index);
	if (!color.isValid())
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	lua_pushnumber(L, static_cast<lua_Number>(colorValue(color)));
	return 1;
}

static int luaSetBoldColour(lua_State *L)
{
	const auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
		return 0;
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
		return 0;

	const int    index  = static_cast<int>(luaL_checkinteger(L, 1));
	const long   value  = static_cast<long>(luaL_checknumber(L, 2));
	const auto   packed = static_cast<QMudColorRef>(value);
	const QColor color(qmudRed(packed), qmudGreen(packed), qmudBlue(packed));
	runtime->setAnsiColour(true, index, color);
	return 0;
}

static int luaBroadcastPlugin(lua_State *L)
{
	const auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, 0);
		return 1;
	}

	const long  message = static_cast<long>(luaL_checknumber(L, 1));
	const char *text    = luaL_optstring(L, 2, "");
	const int   count =
	    runtime->broadcastPlugin(message, QString::fromUtf8(text), engine->pluginId(), engine->pluginName());
	lua_pushnumber(L, count);
	return 1;
}

static int luaChangeDir(lua_State *L)
{
	const auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, 0);
		return 1;
	}

	const QString path = QString::fromUtf8(luaL_checkstring(L, 1));
	const bool    ok   = QDir::setCurrent(path);
	if (ok)
	{
		QString current = QDir::currentPath();
		if (!current.endsWith('/'))
			current += '/';
		runtime->setStartupDirectory(current);
	}
	lua_pushnumber(L, ok ? 1 : 0);
	return 1;
}

static int luaChatAcceptCalls(lua_State *L)
{
	const auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eCannotCreateChatSocket);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eCannotCreateChatSocket);
		return 1;
	}
	const auto port = static_cast<short>(luaL_optinteger(L, 1, DEFAULT_CHAT_PORT));
	lua_pushnumber(L, runtime->chatAcceptCalls(port));
	return 1;
}

static int luaChatCall(lua_State *L)
{
	const auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eCannotCreateChatSocket);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eCannotCreateChatSocket);
		return 1;
	}
	const QString server = QString::fromUtf8(luaL_checkstring(L, 1));
	const long    port   = luaL_optinteger(L, 2, DEFAULT_CHAT_PORT);
	lua_pushnumber(L, runtime->chatCall(server, port, false));
	return 1;
}

static int luaChatCallzChat(lua_State *L)
{
	const auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eCannotCreateChatSocket);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eCannotCreateChatSocket);
		return 1;
	}
	const QString server = QString::fromUtf8(luaL_checkstring(L, 1));
	const long    port   = luaL_optinteger(L, 2, DEFAULT_CHAT_PORT);
	lua_pushnumber(L, runtime->chatCall(server, port, true));
	return 1;
}

static int luaChatDisconnect(lua_State *L)
{
	const auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eChatIDNotFound);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eChatIDNotFound);
		return 1;
	}
	const long id = luaL_checkinteger(L, 1);
	lua_pushnumber(L, runtime->chatDisconnect(id));
	return 1;
}

static int luaChatDisconnectAll(lua_State *L)
{
	const auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eOK);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eOK);
		return 1;
	}
	lua_pushnumber(L, runtime->chatDisconnectAll());
	return 1;
}

static int luaChatEverybody(lua_State *L)
{
	const auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eNoChatConnections);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eNoChatConnections);
		return 1;
	}
	const QString message = QString::fromUtf8(luaL_checkstring(L, 1));
	const bool    emote   = optBool(L, 2, false);
	lua_pushnumber(L, runtime->chatEverybody(message, emote));
	return 1;
}

static int luaChatGetID(lua_State *L)
{
	const auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eChatPersonNotFound);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eChatPersonNotFound);
		return 1;
	}
	const QString who = QString::fromUtf8(luaL_checkstring(L, 1));
	lua_pushnumber(L, static_cast<lua_Number>(runtime->chatGetId(who)));
	return 1;
}

static int luaChatGroup(lua_State *L)
{
	const auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	const QString group   = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString message = QString::fromUtf8(luaL_checkstring(L, 2));
	const bool    emote   = optBool(L, 3, false);
	lua_pushnumber(L, runtime->chatGroup(group, message, emote));
	return 1;
}

static int luaChatID(lua_State *L)
{
	const auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eChatIDNotFound);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eChatIDNotFound);
		return 1;
	}
	const long    id      = luaL_checkinteger(L, 1);
	const QString message = QString::fromUtf8(luaL_checkstring(L, 2));
	const bool    emote   = optBool(L, 3, false);
	lua_pushnumber(L, runtime->chatId(id, message, emote));
	return 1;
}

static int luaChatMessage(lua_State *L)
{
	const auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eChatIDNotFound);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eChatIDNotFound);
		return 1;
	}
	const long    id      = luaL_checkinteger(L, 1);
	const auto    message = static_cast<short>(luaL_checkinteger(L, 2));
	const QString text    = QString::fromUtf8(luaL_checkstring(L, 3));
	lua_pushnumber(L, runtime->chatMessage(id, message, text));
	return 1;
}

static int luaChatNameChange(lua_State *L)
{
	const auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	const QString name = QString::fromUtf8(luaL_checkstring(L, 1));
	lua_pushnumber(L, runtime->chatNameChange(name));
	return 1;
}

static int luaChatNote(lua_State *L)
{
	const auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
		return 0;
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
		return 0;
	const auto    noteType = static_cast<short>(luaL_checkinteger(L, 1));
	const QString message  = QString::fromUtf8(luaL_optstring(L, 2, ""));
	runtime->chatNote(noteType, message);
	return 0;
}

static int luaChatPasteEverybody(lua_State *L)
{
	const auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eNoChatConnections);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eNoChatConnections);
		return 1;
	}
	lua_pushnumber(L, runtime->chatPasteEverybody());
	return 1;
}

static int luaChatPasteText(lua_State *L)
{
	const auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eNoChatConnections);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eNoChatConnections);
		return 1;
	}
	const long id = luaL_checkinteger(L, 1);
	lua_pushnumber(L, runtime->chatPasteText(id));
	return 1;
}

static int luaChatPeekConnections(lua_State *L)
{
	const auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eChatIDNotFound);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eChatIDNotFound);
		return 1;
	}
	const long id = luaL_checkinteger(L, 1);
	lua_pushnumber(L, runtime->chatPeekConnections(id));
	return 1;
}

static int luaChatPersonal(lua_State *L)
{
	const auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eChatPersonNotFound);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eChatPersonNotFound);
		return 1;
	}
	const QString who     = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString message = QString::fromUtf8(luaL_checkstring(L, 2));
	const bool    emote   = optBool(L, 3, false);
	lua_pushnumber(L, runtime->chatPersonal(who, message, emote));
	return 1;
}

static int luaChatPing(lua_State *L)
{
	const auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eChatIDNotFound);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eChatIDNotFound);
		return 1;
	}
	const long id = luaL_checkinteger(L, 1);
	lua_pushnumber(L, runtime->chatPing(id));
	return 1;
}

static int luaChatRequestConnections(lua_State *L)
{
	const auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eChatIDNotFound);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eChatIDNotFound);
		return 1;
	}
	const long id = luaL_checkinteger(L, 1);
	lua_pushnumber(L, runtime->chatRequestConnections(id));
	return 1;
}

static int luaChatSendFile(lua_State *L)
{
	const auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eChatIDNotFound);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eChatIDNotFound);
		return 1;
	}
	const long    id   = luaL_checkinteger(L, 1);
	const QString path = QString::fromUtf8(luaL_checkstring(L, 2));
	lua_pushnumber(L, runtime->chatSendFile(id, path));
	return 1;
}

static int luaChatStopAcceptingCalls(lua_State *L)
{
	const auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
		return 0;
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
		return 0;
	runtime->chatStopAcceptingCalls();
	return 0;
}

static int luaChatStopFileTransfer(lua_State *L)
{
	const auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eChatIDNotFound);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eChatIDNotFound);
		return 1;
	}
	const long id = luaL_checkinteger(L, 1);
	lua_pushnumber(L, runtime->chatStopFileTransfer(id));
	return 1;
}

static int luaCloseNotepad(lua_State *L)
{
	const auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	const QString title     = QString::fromUtf8(luaL_checkstring(L, 1));
	const bool    querySave = optBool(L, 2, false);
	lua_pushnumber(L, runtime->closeNotepad(title, querySave) ? 1 : 0);
	return 1;
}

static int luaColourNameToRGB(lua_State *L)
{
	const auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		const QString name = QString::fromUtf8(luaL_checkstring(L, 1));
		lua_pushnumber(L, static_cast<lua_Number>(colourNameFallback(name)));
		return 1;
	}
	const WorldRuntime *runtime = engine->worldRuntime();
	const QString       name    = QString::fromUtf8(luaL_checkstring(L, 1));
	long                value   = runtime ? WorldRuntime::colourNameToRGB(name) : -1;
	if (value == -1)
		value = colourNameFallback(name);
	lua_pushnumber(L, static_cast<lua_Number>(value));
	return 1;
}

static int luaColourNote(lua_State *L)
{
	const auto   *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
		return 0;

	const QString textColour = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString backColour = QString::fromUtf8(luaL_checkstring(L, 2));
	QString       text       = QString::fromUtf8(luaL_checkstring(L, 3));
	if (text.right(2) != QStringLiteral("\r\n"))
		text += QStringLiteral("\r\n");

	const bool           oldNotesInRgb     = runtime->notesInRgb();
	const long           oldFore           = runtime->noteColourFore();
	const long           oldBack           = runtime->noteColourBack();
	const int            oldNoteTextColour = runtime->noteTextColour();
	const unsigned short noteStyle         = runtime->noteStyle();

	if (!runtime->notesInRgb())
	{
		runtime->setNoteColourFore(runtime->noteColourFore());
		runtime->setNoteColourBack(runtime->noteColourBack());
	}

	const QColor fore = WorldView::parseColor(textColour);
	const QColor back = WorldView::parseColor(backColour);
	if (fore.isValid())
		runtime->setNoteColourFore(colorValue(fore));
	if (back.isValid())
		runtime->setNoteColourBack(colorValue(back));

	WorldRuntime::StyleSpan span;
	span.length    = sizeToInt(text.size());
	span.fore      = colorFromValue(runtime->noteColourFore());
	span.back      = colorFromValue(runtime->noteColourBack());
	span.bold      = (noteStyle & kStyleHilite) != 0;
	span.underline = (noteStyle & kStyleUnderline) != 0;
	span.blink     = (noteStyle & kStyleBlink) != 0;
	span.inverse   = (noteStyle & kStyleInverse) != 0;
	span.changed   = true;
	runtime->outputStyledText(text, {span}, true, false);

	if (oldNotesInRgb)
	{
		runtime->setNoteColourFore(oldFore);
		runtime->setNoteColourBack(oldBack);
	}
	else
	{
		runtime->setNoteTextColour(oldNoteTextColour);
	}

	return 0;
}

static int luaColourTell(lua_State *L)
{
	const auto   *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
		return 0;

	const QString textColour = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString backColour = QString::fromUtf8(luaL_checkstring(L, 2));
	const QString text       = QString::fromUtf8(luaL_checkstring(L, 3));
	if (text.isEmpty())
		return 0;

	const bool           oldNotesInRgb     = runtime->notesInRgb();
	const long           oldFore           = runtime->noteColourFore();
	const long           oldBack           = runtime->noteColourBack();
	const int            oldNoteTextColour = runtime->noteTextColour();
	const unsigned short noteStyle         = runtime->noteStyle();

	if (!runtime->notesInRgb())
	{
		runtime->setNoteColourFore(runtime->noteColourFore());
		runtime->setNoteColourBack(runtime->noteColourBack());
	}

	const QColor fore = WorldView::parseColor(textColour);
	const QColor back = WorldView::parseColor(backColour);
	if (fore.isValid())
		runtime->setNoteColourFore(colorValue(fore));
	if (back.isValid())
		runtime->setNoteColourBack(colorValue(back));

	WorldRuntime::StyleSpan span;
	span.length    = sizeToInt(text.size());
	span.fore      = colorFromValue(runtime->noteColourFore());
	span.back      = colorFromValue(runtime->noteColourBack());
	span.bold      = (noteStyle & kStyleHilite) != 0;
	span.underline = (noteStyle & kStyleUnderline) != 0;
	span.blink     = (noteStyle & kStyleBlink) != 0;
	span.inverse   = (noteStyle & kStyleInverse) != 0;
	span.changed   = true;
	runtime->outputStyledText(text, {span}, true, false);

	if (oldNotesInRgb)
	{
		runtime->setNoteColourFore(oldFore);
		runtime->setNoteColourBack(oldBack);
	}
	else
	{
		runtime->setNoteTextColour(oldNoteTextColour);
	}

	return 0;
}

static int luaSetCustomColourBackground(lua_State *L)
{
	const auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
		return 0;
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
		return 0;
	const int    index  = static_cast<int>(luaL_checkinteger(L, 1));
	const long   value  = static_cast<long>(luaL_checknumber(L, 2));
	const auto   packed = static_cast<QMudColorRef>(value);
	const QColor color(qmudRed(packed), qmudGreen(packed), qmudBlue(packed));
	runtime->setCustomColourBackground(index, color);
	return 0;
}

static int luaGetCustomColourBackground(lua_State *L)
{
	const auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	const WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	const int index = static_cast<int>(luaL_checkinteger(L, 1));
	lua_pushnumber(L, static_cast<lua_Number>(runtime->customColourBackground(index)));
	return 1;
}

static int luaSetCustomColourText(lua_State *L)
{
	const auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
		return 0;
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
		return 0;
	const int    index  = static_cast<int>(luaL_checkinteger(L, 1));
	const long   value  = static_cast<long>(luaL_checknumber(L, 2));
	const auto   packed = static_cast<QMudColorRef>(value);
	const QColor color(qmudRed(packed), qmudGreen(packed), qmudBlue(packed));
	runtime->setCustomColourText(index, color);
	return 0;
}

static int luaGetCustomColourText(lua_State *L)
{
	const auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	const WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	const int index = static_cast<int>(luaL_checkinteger(L, 1));
	lua_pushnumber(L, static_cast<lua_Number>(runtime->customColourText(index)));
	return 1;
}

static int luaDebug(lua_State *L)
{
	const auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnil(L);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const QString  command = QString::fromUtf8(luaL_optstring(L, 1, ""));
	const QVariant value   = runtime->debugCommand(command);
	if (!value.isValid())
	{
		lua_pushnil(L);
		return 1;
	}
	pushVariant(L, value);
	return 1;
}

static int luaDeleteAllMapItems(lua_State *L)
{
	const auto   *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}
	lua_pushnumber(L, runtime->deleteAllMapItems());
	return 1;
}

static int luaDeleteLastMapItem(lua_State *L)
{
	const auto   *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}
	lua_pushnumber(L, runtime->deleteLastMapItem());
	return 1;
}

static int luaDeleteCommandHistory(lua_State *L)
{
	const auto   *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
		return 0;
	if (WorldView *view = runtime->view())
		view->clearCommandHistory();
	return 0;
}

static int luaDeleteOutput(lua_State *L)
{
	const auto   *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
		return 0;
	runtime->deleteOutput();
	return 0;
}

static int luaDeleteLines(lua_State *L)
{
	const auto   *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
		return 0;
	const int count = static_cast<int>(luaL_checkinteger(L, 1));
	runtime->deleteLines(count);
	return 0;
}

static int luaDeleteVariable(lua_State *L)
{
	const auto   *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}
	const QString name = QString::fromUtf8(luaL_checkstring(L, 1));
	lua_pushnumber(L, runtime->deleteVariable(name));
	return 1;
}

static int luaDiscardQueue(lua_State *L)
{
	const auto   *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}
	lua_pushnumber(L, runtime->discardQueuedCommands());
	return 1;
}

static int     addTimerInternal(const LuaCallbackEngine *engine, const QString &rawName, int hour, int minute,
                                double second, const QString &responseText, int flags, const QString &scriptName);
static QString makeAutoName(const QString &prefix);
static void    applyTimerDefaults(WorldRuntime::Timer &timer);
static void    resetTimerFields(WorldRuntime::Timer &timer);
static QList<WorldRuntime::Trigger> &mutableTriggerList(WorldRuntime *runtime, WorldRuntime::Plugin *plugin);
static QList<WorldRuntime::Alias>   &mutableAliasList(WorldRuntime *runtime, WorldRuntime::Plugin *plugin);
static QList<WorldRuntime::Timer>   &mutableTimerList(WorldRuntime *runtime, WorldRuntime::Plugin *plugin);
static void commitTriggerListMutation(WorldRuntime *runtime, const WorldRuntime::Plugin *plugin);
static void commitAliasListMutation(WorldRuntime *runtime, const WorldRuntime::Plugin *plugin);
static void commitTimerListMutation(WorldRuntime *runtime, const WorldRuntime::Plugin *plugin,
                                    bool structureChanged = false);

static int  addTempTimer(const LuaCallbackEngine *engine, double seconds, const QString &text,
                         const int sendTo)
{
	if (!engine)
		return eWorldClosed;
	if (seconds < 0.1)
		return eTimeInvalid;
	if (sendTo < 0 || sendTo >= eSendToLast)
		return eOptionOutOfRange;

	const int hours = static_cast<int>(seconds / 3600.0);
	seconds -= static_cast<double>(hours) * 3600.0;
	const int minutes = static_cast<int>(seconds / 60.0);
	seconds -= static_cast<double>(minutes) * 60.0;

	if (hours > 23)
		return eTimeInvalid;
	if (minutes < 0 || minutes > 59)
		return eTimeInvalid;
	if (seconds < 0.0 || seconds > 59.9999)
		return eTimeInvalid;

	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
		return eWorldClosed;

	WorldRuntime::Plugin *plugin = nullptr;
	if (int errorCode = eOK; !resolvePluginContext(engine, runtime, plugin, errorCode))
		return errorCode;

	QList<WorldRuntime::Timer> &timers = mutableTimerList(runtime, plugin);

	WorldRuntime::Timer         timer;
	timer.attributes.insert(QStringLiteral("name"), makeAutoName(QStringLiteral("*timer")));
	timer.children.insert(QStringLiteral("send"), text);
	timer.attributes.insert(QStringLiteral("at_time"), QStringLiteral("0"));
	timer.attributes.insert(QStringLiteral("hour"), QString::number(hours));
	timer.attributes.insert(QStringLiteral("minute"), QString::number(minutes));
	timer.attributes.insert(QStringLiteral("second"), QString::number(seconds, 'f', 4));
	timer.attributes.insert(QStringLiteral("offset_hour"), QStringLiteral("0"));
	timer.attributes.insert(QStringLiteral("offset_minute"), QStringLiteral("0"));
	timer.attributes.insert(QStringLiteral("offset_second"), QStringLiteral("0"));
	timer.attributes.insert(QStringLiteral("send_to"), QString::number(sendTo));
	timer.attributes.insert(QStringLiteral("enabled"), attrFlag(true));
	timer.attributes.insert(QStringLiteral("one_shot"), attrFlag(true));
	timer.attributes.insert(QStringLiteral("temporary"), attrFlag(true));
	timer.attributes.insert(QStringLiteral("active_closed"), attrFlag(true));
	applyTimerDefaults(timer);
	resetTimerFields(timer);

	timers.push_back(timer);
	commitTimerListMutation(runtime, plugin, true);
	return eOK;
}

static int luaDoAfter(lua_State *L)
{
	const auto   *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	const auto    seconds = luaL_checknumber(L, 1);
	const QString text    = QString::fromUtf8(luaL_checkstring(L, 2));
	lua_pushnumber(L, addTempTimer(engine, seconds, text, eSendToWorld));
	return 1;
}

static int luaDoAfterNote(lua_State *L)
{
	const auto   *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	const auto    seconds = luaL_checknumber(L, 1);
	const QString text    = QString::fromUtf8(luaL_checkstring(L, 2));
	lua_pushnumber(L, addTempTimer(engine, seconds, text, eSendToOutput));
	return 1;
}

static int luaDoAfterSpecial(lua_State *L)
{
	const auto   *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	const auto    seconds = luaL_checknumber(L, 1);
	const QString text    = QString::fromUtf8(luaL_checkstring(L, 2));
	const int     sendTo  = static_cast<int>(luaL_checkinteger(L, 3));
	lua_pushnumber(L, addTempTimer(engine, seconds, text, sendTo));
	return 1;
}

static int luaDoAfterSpeedWalk(lua_State *L)
{
	const auto   *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	const auto    seconds = luaL_checknumber(L, 1);
	const QString text    = QString::fromUtf8(luaL_checkstring(L, 2));
	lua_pushnumber(L, addTempTimer(engine, seconds, text, eSendToSpeedwalk));
	return 1;
}

static int luaDoCommand(lua_State *L)
{
	const QString cmd = QString::fromUtf8(luaL_checkstring(L, 1));
	const int     id  = qmudStringToCommandId(cmd);
	if (id == 0)
	{
		lua_pushnumber(L, eNoSuchCommand);
		return 1;
	}
	const QString cmdName = qmudCommandIdToString(id);
	MainWindow   *mw      = resolveMainWindow();
	if (!mw)
	{
		lua_pushnumber(L, eNoSuchCommand);
		return 1;
	}
	if (QAction *action = mw->actionForCommand(cmdName))
	{
		action->trigger();
		lua_pushnumber(L, eOK);
		return 1;
	}
	lua_pushnumber(L, eNoSuchCommand);
	return 1;
}

static int luaEditDistance(lua_State *L)
{
	const QString source   = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString target   = QString::fromUtf8(luaL_checkstring(L, 2));
	const int     distance = qmudEditDistance(source, target);
	lua_pushnumber(L, distance);
	return 1;
}

static int luaEnableGroup(lua_State *L)
{
	const auto   *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	const QString groupName = QString::fromUtf8(luaL_checkstring(L, 1));
	if (groupName.trimmed().isEmpty())
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	const bool            enabled = optBool(L, 2, true);

	WorldRuntime::Plugin *plugin = nullptr;
	if (int errorCode = eOK; !resolvePluginContext(engine, runtime, plugin, errorCode))
	{
		lua_pushnumber(L, 0);
		return 1;
	}

	auto setEnabled = [&](auto &list)
	{
		int changed = 0;
		for (auto &item : list)
		{
			if (groupMatches(item.attributes.value(QStringLiteral("group")), groupName))
			{
				item.attributes.insert(QStringLiteral("enabled"), attrFlag(enabled));
				changed++;
			}
		}
		return changed;
	};

	QList<WorldRuntime::Trigger> &triggers = mutableTriggerList(runtime, plugin);
	QList<WorldRuntime::Alias>   &aliases  = mutableAliasList(runtime, plugin);
	QList<WorldRuntime::Timer>   &timers   = mutableTimerList(runtime, plugin);

	const int                     changed = setEnabled(triggers) + setEnabled(aliases) + setEnabled(timers);

	commitTriggerListMutation(runtime, plugin);
	commitAliasListMutation(runtime, plugin);
	commitTimerListMutation(runtime, plugin);

	lua_pushnumber(L, changed);
	return 1;
}

static int luaEnableMapping(lua_State *L)
{
	const auto   *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
		return 0;
	const bool enabled = optBool(L, 1, true);
	runtime->setMappingEnabled(enabled);
	return 0;
}

static int luaErrorDesc(lua_State *L)
{
	const auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	const WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushstring(L, "");
		return 1;
	}
	const int     key  = static_cast<int>(luaL_checkinteger(L, 1));
	const QString desc = WorldRuntime::errorDesc(key);
	lua_pushstring(L, desc.toLatin1().constData());
	return 1;
}

static int luaEvaluateSpeedwalk(lua_State *L)
{
	const auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	const WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushstring(L, "");
		return 1;
	}
	const QString input  = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString result = runtime->evaluateSpeedwalk(input);
	lua_pushstring(L, result.toLocal8Bit().constData());
	return 1;
}

static int luaExecute(lua_State *L)
{
	const auto   *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}
	const QString input = QString::fromUtf8(luaL_checkstring(L, 1));
	lua_pushnumber(L, runtime->executeCommand(input));
	return 1;
}

static int luaExportXML(lua_State *L)
{
	const auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	const WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushstring(L, "");
		return 1;
	}

	const int     type       = static_cast<int>(luaL_checkinteger(L, 1));
	const QString rawName    = QString::fromUtf8(luaL_checkstring(L, 2));
	const QString targetName = rawName.trimmed().toLower();
	if (targetName.isEmpty())
	{
		lua_pushstring(L, "");
		return 1;
	}

	auto fixXmlString = [](const QString &source) -> QString
	{
		QString out;
		out.reserve(source.size());
		for (QChar ch : source)
		{
			switch (ch.unicode())
			{
			case '<':
				out += QStringLiteral("&lt;");
				break;
			case '>':
				out += QStringLiteral("&gt;");
				break;
			case '&':
				out += QStringLiteral("&amp;");
				break;
			case '\"':
				out += QStringLiteral("&quot;");
				break;
			default:
				out += ch;
				break;
			}
		}
		return out;
	};

	auto fixXmlMultiline = [](const QString &source) -> QString
	{
		QString out;
		out.reserve(source.size());
		for (QChar ch : source)
		{
			switch (ch.unicode())
			{
			case '<':
				out += QStringLiteral("&lt;");
				break;
			case '>':
				out += QStringLiteral("&gt;");
				break;
			case '&':
				out += QStringLiteral("&amp;");
				break;
			case '\t':
				out += QStringLiteral("&#9;");
				break;
			default:
				out += ch;
				break;
			}
		}
		return out;
	};

	auto writeAttributes = [&](QTextStream &out, const QMap<QString, QString> &attrs)
	{
		for (auto it = attrs.begin(); it != attrs.end(); ++it)
			out << it.key() << "=\"" << fixXmlString(it.value()) << "\" ";
	};

	auto writeChildren = [&](QTextStream &out, const QMap<QString, QString> &children, const QString &nl)
	{
		for (auto it = children.begin(); it != children.end(); ++it)
		{
			out << "  <" << it.key() << ">" << fixXmlMultiline(it.value()) << "</" << it.key() << ">" << nl;
		}
	};

	QString     xml;
	QTextStream out(&xml);
	out.setEncoding(QStringConverter::Utf8);
	const auto nl = QStringLiteral("\r\n");

	switch (type)
	{
	case 0: // trigger
	{
		for (const auto &tr : runtime->triggers())
		{
			if (const QString name = tr.attributes.value(QStringLiteral("name")).trimmed().toLower();
			    name != targetName)
				continue;
			out << "<triggers>" << nl;
			out << nl << "  <trigger ";
			writeAttributes(out, tr.attributes);
			out << ">" << nl;
			writeChildren(out, tr.children, nl);
			out << nl << "  </trigger>" << nl;
			out << "</triggers>" << nl;
			lua_pushstring(L, xml.toUtf8().constData());
			return 1;
		}
		break;
	}
	case 1: // alias
	{
		for (const auto &al : runtime->aliases())
		{
			if (const QString name = al.attributes.value(QStringLiteral("name")).trimmed().toLower();
			    name != targetName)
				continue;
			out << "<aliases>" << nl;
			out << nl << "  <alias ";
			writeAttributes(out, al.attributes);
			out << ">" << nl;
			writeChildren(out, al.children, nl);
			out << nl << "  </alias>" << nl;
			out << "</aliases>" << nl;
			lua_pushstring(L, xml.toUtf8().constData());
			return 1;
		}
		break;
	}
	case 2: // timer
	{
		for (const auto &tm : runtime->timers())
		{
			if (const QString name = tm.attributes.value(QStringLiteral("name")).trimmed().toLower();
			    name != targetName)
				continue;
			out << "<timers>" << nl;
			out << nl << "  <timer ";
			writeAttributes(out, tm.attributes);
			out << ">" << nl;
			writeChildren(out, tm.children, nl);
			out << nl << "  </timer>" << nl;
			out << "</timers>" << nl;
			lua_pushstring(L, xml.toUtf8().constData());
			return 1;
		}
		break;
	}
	case 3: // macro
	{
		for (const auto &[attributes, children] : runtime->macros())
		{
			if (const QString name = attributes.value(QStringLiteral("name")).trimmed();
			    name.trimmed().toLower() == targetName)
			{
				QString typeText = attributes.value(QStringLiteral("type")).trimmed();
				if (typeText.isEmpty())
					typeText = QStringLiteral("replace");
				if (typeText != QStringLiteral("replace") && typeText != QStringLiteral("send_now") &&
				    typeText != QStringLiteral("insert"))
					typeText = QStringLiteral("unknown");
				const QString send = children.value(QStringLiteral("send"));
				out << "<macros>" << nl;
				out << nl << "  <macro ";
				out << "name=\"" << fixXmlString(name) << "\" ";
				out << "type=\"" << fixXmlString(typeText) << "\" ";
				out << ">" << nl;
				out << "  <send>" << fixXmlMultiline(send) << "</send>";
				out << nl << "  </macro>" << nl;
				out << "</macros>" << nl;
				lua_pushstring(L, xml.toUtf8().constData());
				return 1;
			}
		}
		break;
	}
	case 4: // variable
	{
		for (const auto &[attributes, content] : runtime->variables())
		{
			if (const QString name = attributes.value(QStringLiteral("name")).trimmed().toLower();
			    name != targetName)
				continue;
			out << "<variables>" << nl;
			out << "  <variable name=\"" << fixXmlString(attributes.value(QStringLiteral("name"))) << "\">"
			    << fixXmlMultiline(content) << "</variable>" << nl;
			out << "</variables>" << nl;
			lua_pushstring(L, xml.toUtf8().constData());
			return 1;
		}
		break;
	}
	case 5: // keypad
	{
		for (const auto &[attributes, content] : runtime->keypadEntries())
		{
			if (const QString name = attributes.value(QStringLiteral("name")).trimmed().toLower();
			    name != targetName)
				continue;
			out << "<keypad>" << nl;
			out << nl << "  <key ";
			out << "name=\"" << fixXmlString(attributes.value(QStringLiteral("name"))) << "\" ";
			out << ">" << nl;
			out << "  <send>" << fixXmlMultiline(content) << "</send>";
			out << nl << "  </key>" << nl;
			out << "</keypad>" << nl;
			lua_pushstring(L, xml.toUtf8().constData());
			return 1;
		}
		break;
	}
	default:
		break;
	}

	lua_pushstring(L, "");
	return 1;
}

static QString fixupHtmlString(const QString &source)
{
	QString out = source;
	out.replace(QStringLiteral("&"), QStringLiteral("&amp;"));
	out.replace(QStringLiteral("<"), QStringLiteral("&lt;"));
	out.replace(QStringLiteral(">"), QStringLiteral("&gt;"));
	out.replace(QStringLiteral("\""), QStringLiteral("&quot;"));
	return out;
}

static int luaFilterPixel(lua_State *L)
{
	const auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	const WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, -1);
		return 1;
	}
	const long pixel     = luaL_checkinteger(L, 1);
	const auto operation = static_cast<short>(luaL_checkinteger(L, 2));
	const auto options   = luaL_checknumber(L, 3);
	lua_pushnumber(L, static_cast<lua_Number>(WorldRuntime::filterPixel(pixel, operation, options)));
	return 1;
}

static int luaFixupEscapeSequences(lua_State *L)
{
	const QString input  = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString result = qmudFixupEscapeSequences(input);
	lua_pushstring(L, result.toUtf8().constData());
	return 1;
}

static int luaFixupHTML(lua_State *L)
{
	const QString input  = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString result = fixupHtmlString(input);
	lua_pushstring(L, result.toLocal8Bit().constData());
	return 1;
}

static int luaFlashIcon(lua_State *L)
{
	const int stackTop = lua_gettop(L);
	Q_UNUSED(stackTop);
	if (MainWindow *mw = resolveMainWindow())
		QApplication::alert(mw, 0);
	return 0;
}

static int luaFlushLog(lua_State *L)
{
	const auto   *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}
	lua_pushnumber(L, runtime->flushLog());
	return 1;
}

static int luaGenerateName(lua_State *L)
{
	const auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	const WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const QString name = WorldRuntime::generateName();
	if (name.isEmpty())
	{
		lua_pushnil(L);
		return 1;
	}
	lua_pushstring(L, name.toUtf8().constData());
	return 1;
}

static int luaGetChatInfo(lua_State *L)
{
	const WorldRuntime *runtime = runtimeFromLua(L);
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const long     id       = luaL_checkinteger(L, 1);
	const int      infoType = static_cast<int>(luaL_checkinteger(L, 2));
	const QVariant value    = runtime->chatInfo(id, infoType);
	pushVariant(L, value);
	return 1;
}

static int luaGetChatList(lua_State *L)
{
	const WorldRuntime *runtime = runtimeFromLua(L);
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const QList<long> ids = runtime->chatList();
	if (ids.isEmpty())
	{
		lua_pushnil(L);
		return 1;
	}
	lua_newtable(L);
	int index = 1;
	for (const long id : ids)
	{
		lua_pushinteger(L, id);
		lua_rawseti(L, -2, index++);
	}
	return 1;
}

static int luaGetChatOption(lua_State *L)
{
	const WorldRuntime *runtime = runtimeFromLua(L);
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const long     id     = luaL_checkinteger(L, 1);
	const QString  option = QString::fromUtf8(luaL_checkstring(L, 2));
	const QVariant value  = runtime->chatOption(id, option);
	pushVariant(L, value);
	return 1;
}

static int luaGetClipboard(lua_State *L)
{
	const QString text = QGuiApplication::clipboard()->text();
	lua_pushstring(L, text.toLocal8Bit().constData());
	return 1;
}

static void pushWindowPositionTable(lua_State *L, const QRect &rect)
{
	lua_newtable(L);
	lua_pushstring(L, "left");
	lua_pushinteger(L, rect.left());
	lua_rawset(L, -3);
	lua_pushstring(L, "top");
	lua_pushinteger(L, rect.top());
	lua_rawset(L, -3);
	lua_pushstring(L, "width");
	lua_pushinteger(L, rect.width());
	lua_rawset(L, -3);
	lua_pushstring(L, "height");
	lua_pushinteger(L, rect.height());
	lua_rawset(L, -3);
}

static int luaGetCommand(lua_State *L)
{
	const auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	const WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime || !runtime->view())
	{
		lua_pushstring(L, "");
		return 1;
	}
	lua_pushstring(L, runtime->view()->inputText().toLocal8Bit().constData());
	return 1;
}

static int luaGetCommandList(lua_State *L)
{
	const auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	const WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime || !runtime->view())
	{
		lua_pushnil(L);
		return 1;
	}
	const int         count       = static_cast<int>(luaL_checkinteger(L, 1));
	const QStringList history     = runtime->view()->commandHistoryList();
	const int         historySize = sizeToInt(history.size());
	const int         takeCount   = count <= 0 ? historySize : qMin(count, historySize);
	if (takeCount <= 0)
	{
		lua_pushnil(L);
		return 1;
	}
	lua_newtable(L);
	int index = 1;
	// Legacy behavior: command history list is returned newest-first.
	for (int i = historySize - 1; i >= 0 && index <= takeCount; --i)
	{
		lua_pushstring(L, history.at(i).toLocal8Bit().constData());
		lua_rawseti(L, -2, index++);
	}
	return 1;
}

static int luaGetCurrentValue(lua_State *L)
{
	const auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	const WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const QString key      = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString pluginId = engine->pluginId();

	if (const WorldNumericOption *numeric = QMudWorldOptions::findWorldNumericOption(key); numeric)
	{
		if (!pluginId.isEmpty() && numeric->flags & OPT_PLUGIN_CANNOT_READ)
		{
			lua_pushnil(L);
			return 1;
		}
		const QString canonical       = QString::fromLatin1(numeric->name);
		const QString value           = runtime->worldAttributes().value(canonical);
		const QString trimmed         = value.trimmed().toLower();
		const bool    isBooleanOption = numeric->minValue == 0 && numeric->maxValue == 0;
		if (numeric->flags & OPT_RGB_COLOUR)
		{
			const long rgb = colourNameFallback(value);
			lua_pushnumber(L, static_cast<lua_Number>(rgb >= 0 ? rgb : 0));
			return 1;
		}
		if (bool boolValue = false; parseBooleanKeywordValue(trimmed, boolValue))
		{
			lua_pushnumber(L, boolValue ? 1 : 0);
			return 1;
		}
		bool         ok     = false;
		const double number = trimmed.toDouble(&ok);
		if (ok && !trimmed.isEmpty())
		{
			if (numeric->flags & OPT_CUSTOM_COLOUR)
			{
				long adjusted = static_cast<long>(number) + 1;
				if (adjusted == 65536)
					adjusted = 0;
				lua_pushnumber(L, static_cast<lua_Number>(adjusted));
				return 1;
			}
			if (isBooleanOption)
				lua_pushnumber(L, number != 0.0 ? 1 : 0);
			else
				lua_pushnumber(L, static_cast<lua_Number>(static_cast<long>(number)));
			return 1;
		}
		lua_pushnumber(L, 0);
		return 1;
	}

	const WorldAlphaOption *alpha = QMudWorldOptionDefaults::findWorldAlphaOption(key);
	if (!alpha)
	{
		lua_pushnil(L);
		return 1;
	}
	if (!pluginId.isEmpty() && alpha->flags & OPT_PLUGIN_CANNOT_READ)
	{
		lua_pushnil(L);
		return 1;
	}
	constexpr int kOptMultiline = 0x000001;
	const QString canonical     = QString::fromLatin1(alpha->name);
	const bool    isMultiline   = (alpha->flags & kOptMultiline) != 0;
	const QString value         = isMultiline ? runtime->worldMultilineAttributes().value(canonical)
	                                          : runtime->worldAttributes().value(canonical);
	lua_pushstring(L, value.toLocal8Bit().constData());
	return 1;
}

static int luaGetCustomColourName(lua_State *L)
{
	const int which = static_cast<int>(luaL_checkinteger(L, 1));
	if (which < 1 || which > 16)
	{
		lua_pushstring(L, "");
		return 1;
	}
	const QString name = QStringLiteral("Custom%1").arg(which);
	lua_pushstring(L, name.toLocal8Bit().constData());
	return 1;
}

static int luaGetDefaultValue(lua_State *L)
{
	auto         *engine   = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	const QString key      = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString pluginId = engine->pluginId();

	if (const WorldNumericOption *numeric = QMudWorldOptions::findWorldNumericOption(key); numeric)
	{
		if (!pluginId.isEmpty() && numeric->flags & OPT_PLUGIN_CANNOT_READ)
		{
			lua_pushnil(L);
			return 1;
		}
		lua_pushnumber(L, static_cast<lua_Number>(static_cast<long>(numeric->defaultValue)));
		return 1;
	}

	const WorldAlphaOption *alpha = QMudWorldOptionDefaults::findWorldAlphaOption(key);
	if (!alpha)
	{
		lua_pushnil(L);
		return 1;
	}
	if (!pluginId.isEmpty() && alpha->flags & OPT_PLUGIN_CANNOT_READ)
	{
		lua_pushnil(L);
		return 1;
	}
	const QString value = QString::fromLatin1(alpha->value ? alpha->value : "");
	lua_pushstring(L, value.toLocal8Bit().constData());
	return 1;
}

static int luaGetDeviceCaps(lua_State *L)
{
	const int index  = static_cast<int>(luaL_checkinteger(L, 1));
	QScreen  *screen = QGuiApplication::primaryScreen();
	if (!screen)
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	QRect virtualGeom;
	for (const QList<QScreen *> screens = QGuiApplication::screens(); QScreen *s : screens)
		virtualGeom = virtualGeom.united(s->geometry());
	if (virtualGeom.isNull())
		virtualGeom = screen->geometry();

	switch (index)
	{
	case 2:                   // TECHNOLOGY
		lua_pushnumber(L, 1); // DT_RASDISPLAY
		break;
	case 8: // HORZRES
		lua_pushnumber(L, screen->geometry().width());
		break;
	case 4: // HORZSIZE (mm)
		lua_pushnumber(L, screen->physicalSize().width());
		break;
	case 10: // VERTRES
		lua_pushnumber(L, screen->geometry().height());
		break;
	case 6: // VERTSIZE (mm)
		lua_pushnumber(L, screen->physicalSize().height());
		break;
	case 12: // BITSPIXEL
		lua_pushnumber(L, screen->depth());
		break;
	case 14: // PLANES
		lua_pushnumber(L, 1);
		break;
	case 24: // NUMCOLORS
	{
		if (const int depth = screen->depth(); depth > 8)
			lua_pushnumber(L, -1);
		else
			lua_pushnumber(L, 1 << qMax(0, depth));
	}
	break;
	case kLogPixelsXDeviceCapIndex:
		lua_pushnumber(L, screen->logicalDotsPerInchX());
		break;
	case kLogPixelsYDeviceCapIndex:
		lua_pushnumber(L, screen->logicalDotsPerInchY());
		break;
	case 40: // ASPECTX
		lua_pushnumber(L, screen->geometry().width());
		break;
	case 42: // ASPECTY
		lua_pushnumber(L, screen->geometry().height());
		break;
	case 44: // ASPECTXY
	{
		const auto w = static_cast<double>(screen->geometry().width());
		const auto h = static_cast<double>(screen->geometry().height());
		lua_pushnumber(L, qSqrt(w * w + h * h));
	}
	break;
	case 116: // VREFRESH
		lua_pushnumber(L, screen->refreshRate());
		break;
	case 117: // DESKTOPVERTRES
		lua_pushnumber(L, virtualGeom.height());
		break;
	case 118: // DESKTOPHORZRES
		lua_pushnumber(L, virtualGeom.width());
		break;
	default:
		lua_pushnumber(L, 0);
		break;
	}
	return 1;
}

static int luaGetEntity(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushstring(L, "");
		return 1;
	}
	const QString name  = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString value = runtime->getEntityValue(name);
	lua_pushstring(L, value.toLocal8Bit().constData());
	return 1;
}

static QString decodeXmlEntity(const QString &name)
{
	if (name.isEmpty())
		return {};

	if (name.startsWith(QLatin1Char('#')))
	{
		QString digits = name.mid(1);
		int     base   = 10;
		if (digits.startsWith(QLatin1Char('x'), Qt::CaseInsensitive))
		{
			base   = 16;
			digits = digits.mid(1);
		}
		bool       ok   = false;
		const uint code = digits.toUInt(&ok, base);
		if (!ok || code > 0x10FFFFu)
			return {};
		const auto ch = static_cast<char32_t>(code);
		return QString::fromUcs4(&ch, 1);
	}

	if (name == QLatin1String("lt"))
		return QStringLiteral("<");
	if (name == QLatin1String("gt"))
		return QStringLiteral(">");
	if (name == QLatin1String("amp"))
		return QStringLiteral("&");
	if (name == QLatin1String("apos"))
		return QStringLiteral("'");
	if (name == QLatin1String("quot"))
		return QStringLiteral("\"");
	return {};
}

static int luaGetXMLEntity(lua_State *L)
{
	const QString name   = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString result = decodeXmlEntity(name);
	lua_pushstring(L, result.toUtf8().constData());
	return 1;
}

static int luaGetFrame(lua_State *L)
{
	MainWindow *mw = resolveMainWindow();
	lua_pushlightuserdata(L, mw);
	return 1;
}

static int luaGetGlobalOption(lua_State *L)
{
	const QString name = QString::fromUtf8(luaL_checkstring(L, 1));
	if (AppController *app = AppController::instance(); !app)
	{
		lua_pushnil(L);
		return 1;
	}
	else
	{
		pushVariant(L, app->getGlobalOption(name));
	}
	return 1;
}

static int luaGetGlobalOptionList(lua_State *L)
{
	if (!AppController::instance())
	{
		lua_pushnil(L);
		return 1;
	}

	const QStringList options = AppController::globalOptionList();
	if (options.isEmpty())
	{
		lua_pushnil(L);
		return 1;
	}
	lua_newtable(L);
	int index = 1;
	for (const QString &name : options)
	{
		lua_pushstring(L, name.toUtf8().constData());
		lua_rawseti(L, -2, index++);
	}
	return 1;
}

static int luaGetHostAddress(lua_State *L)
{
	const QString host = QString::fromUtf8(luaL_checkstring(L, 1));
	if (host.isEmpty())
	{
		lua_pushnil(L);
		return 1;
	}

	const QHostInfo info = QHostInfo::fromName(host);
	QStringList     addresses;
	for (const QHostAddress &address : info.addresses())
	{
		if (address.protocol() != QAbstractSocket::IPv4Protocol)
			continue;
		addresses.append(address.toString());
	}
	if (addresses.isEmpty())
	{
		lua_pushnil(L);
		return 1;
	}

	lua_newtable(L);
	int index = 1;
	for (const QString &address : addresses)
	{
		lua_pushstring(L, address.toLocal8Bit().constData());
		lua_rawseti(L, -2, index++);
	}
	return 1;
}

static int luaGetHostName(lua_State *L)
{
	const QString address = QString::fromUtf8(luaL_checkstring(L, 1));
	if (address.isEmpty())
	{
		lua_pushstring(L, "");
		return 1;
	}
	QHostAddress parsed;
	if (!parsed.setAddress(address) || parsed.protocol() != QAbstractSocket::IPv4Protocol)
	{
		lua_pushstring(L, "");
		return 1;
	}
	// Legacy behavior: inet_addr treats 255.255.255.255 as INADDR_NONE (invalid for reverse lookup path).
	if (parsed.toIPv4Address() == 0xFFFFFFFFu)
	{
		lua_pushstring(L, "");
		return 1;
	}
	const QHostInfo info = QHostInfo::fromName(parsed.toString());
	if (info.error() != QHostInfo::NoError)
	{
		lua_pushstring(L, "");
		return 1;
	}
	const QString hostName = info.hostName().trimmed();
	if (hostName.isEmpty() || hostName == parsed.toString())
	{
		lua_pushstring(L, "");
		return 1;
	}
	lua_pushstring(L, hostName.toLocal8Bit().constData());
	return 1;
}

static int luaGetInternalCommandsList(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const QStringList commands = WorldRuntime::internalCommandsList();
	if (commands.isEmpty())
	{
		lua_pushnil(L);
		return 1;
	}
	lua_newtable(L);
	int index = 1;
	for (const QString &command : commands)
	{
		lua_pushstring(L, command.toLatin1().constData());
		lua_rawseti(L, -2, index++);
	}
	return 1;
}

static int luaGetLineInfo(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const int               lineNumber = static_cast<int>(luaL_checkinteger(L, 1));
	const int               infoType   = static_cast<int>(luaL_optinteger(L, 2, 0));
	WorldRuntime::LineEntry entry;
	const bool              hasLine = runtime->luaContextLineEntry(lineNumber, entry);
	if (infoType == 0)
	{
		if (!hasLine)
			return 0;

		lua_newtable(L);

		lua_pushstring(L, "text");
		{
			const QByteArray textBytes = entry.text.toLocal8Bit();
			lua_pushlstring(L, textBytes.constData(), textBytes.size());
		}
		lua_rawset(L, -3);

		lua_pushstring(L, "length");
		lua_pushinteger(L, entry.text.length());
		lua_rawset(L, -3);

		lua_pushstring(L, "newline");
		lua_pushboolean(L, entry.hardReturn);
		lua_rawset(L, -3);

		lua_pushstring(L, "note");
		lua_pushboolean(L, (entry.flags & WorldRuntime::LineNote) != 0);
		lua_rawset(L, -3);

		lua_pushstring(L, "user");
		lua_pushboolean(L, (entry.flags & WorldRuntime::LineInput) != 0);
		lua_rawset(L, -3);

		lua_pushstring(L, "log");
		lua_pushboolean(L, (entry.flags & WorldRuntime::LineLog) != 0);
		lua_rawset(L, -3);

		lua_pushstring(L, "bookmark");
		lua_pushboolean(L, (entry.flags & WorldRuntime::LineBookmark) != 0);
		lua_rawset(L, -3);

		lua_pushstring(L, "hr");
		lua_pushboolean(L, (entry.flags & WorldRuntime::LineHorizontalRule) != 0);
		lua_rawset(L, -3);

		const qint64 lineTime = entry.time.isValid() ? entry.time.toSecsSinceEpoch() : 0;
		lua_pushstring(L, "time");
		lua_pushnumber(L, static_cast<lua_Number>(lineTime));
		lua_rawset(L, -3);

		lua_pushstring(L, "timestr");
		const QString timeString =
		    entry.time.isValid() ? QLocale::system().toString(entry.time, QLocale::ShortFormat) : QString();
		const QByteArray timeBytes = timeString.toLocal8Bit();
		lua_pushlstring(L, timeBytes.constData(), timeBytes.size());
		lua_rawset(L, -3);

		lua_pushstring(L, "line");
		lua_pushinteger(L, entry.lineNumber > 0 ? entry.lineNumber : lineNumber);
		lua_rawset(L, -3);

		lua_pushstring(L, "styles");
		lua_pushinteger(L, entry.spans.size());
		lua_rawset(L, -3);

		lua_pushstring(L, "ticks");
		lua_pushnumber(L, entry.ticks);
		lua_rawset(L, -3);
		lua_pushstring(L, "elapsed");
		lua_pushnumber(L, entry.elapsed);
		lua_rawset(L, -3);

		return 1;
	}

	if (!hasLine)
	{
		lua_pushnil(L);
		return 1;
	}
	switch (infoType)
	{
	case 1:
	{
		const QByteArray textBytes = entry.text.toLocal8Bit();
		lua_pushlstring(L, textBytes.constData(), textBytes.size());
	}
	break;
	case 2:
		lua_pushnumber(L, static_cast<lua_Number>(entry.text.length()));
		break;
	case 3:
		lua_pushboolean(L, entry.hardReturn);
		break;
	case 4:
		lua_pushboolean(L, (entry.flags & WorldRuntime::LineNote) != 0);
		break;
	case 5:
		lua_pushboolean(L, (entry.flags & WorldRuntime::LineInput) != 0);
		break;
	case 6:
		lua_pushboolean(L, (entry.flags & WorldRuntime::LineLog) != 0);
		break;
	case 7:
		lua_pushboolean(L, (entry.flags & WorldRuntime::LineBookmark) != 0);
		break;
	case 8:
		lua_pushboolean(L, (entry.flags & WorldRuntime::LineHorizontalRule) != 0);
		break;
	case 9:
		lua_pushnumber(L, static_cast<lua_Number>(entry.time.isValid() ? entry.time.toSecsSinceEpoch() : 0));
		break;
	case 10:
		lua_pushnumber(L, static_cast<lua_Number>(entry.lineNumber > 0 ? entry.lineNumber : lineNumber));
		break;
	case 11:
		lua_pushnumber(L, static_cast<lua_Number>(entry.spans.size()));
		break;
	case 12:
		lua_pushnumber(L, entry.ticks);
		break;
	case 13:
		lua_pushnumber(L, entry.elapsed);
		break;
	default:
		lua_pushnil(L);
		break;
	}
	return 1;
}

static int luaGetLoadedValue(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const QString key      = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString pluginId = engine->pluginId();

	if (const WorldNumericOption *numeric = QMudWorldOptions::findWorldNumericOption(key); numeric)
	{
		if (!pluginId.isEmpty() && numeric->flags & OPT_PLUGIN_CANNOT_READ)
		{
			lua_pushnil(L);
			return 1;
		}
		const QString canonical = QString::fromLatin1(numeric->name);
		const QString value     = runtime->worldAttributes().value(canonical);
		const QString trimmed   = value.trimmed();
		if (bool boolValue = false; parseBooleanKeywordValue(trimmed, boolValue))
		{
			lua_pushnumber(L, boolValue ? 1 : 0);
			return 1;
		}
		bool            ok     = false;
		const long long number = trimmed.toLongLong(&ok);
		lua_pushnumber(L, ok ? static_cast<lua_Number>(number) : 0);
		return 1;
	}

	const WorldAlphaOption *alpha = QMudWorldOptionDefaults::findWorldAlphaOption(key);
	if (!alpha)
	{
		lua_pushnil(L);
		return 1;
	}
	if (!pluginId.isEmpty() && alpha->flags & OPT_PLUGIN_CANNOT_READ)
	{
		lua_pushnil(L);
		return 1;
	}
	constexpr int kOptMultiline = 0x000001;
	const QString canonical     = QString::fromLatin1(alpha->name);
	const bool    isMultiline   = (alpha->flags & kOptMultiline) != 0;
	const QString value         = isMultiline ? runtime->worldMultilineAttributes().value(canonical)
	                                          : runtime->worldAttributes().value(canonical);
	lua_pushstring(L, value.toLocal8Bit().constData());
	return 1;
}

static int luaGetMainWindowPosition(lua_State *L)
{
	MainWindow *mw = resolveMainWindow();
	if (!mw)
		return 0;

	const bool useGetWindowRect = optBool(L, 1, false);
	QRect      rect             = useGetWindowRect ? mw->frameGeometry() : mw->normalGeometry();
	if (!rect.isValid())
		rect = mw->geometry();
	pushWindowPositionTable(L, rect);
	return 1;
}

static int luaGetWorldWindowPosition(lua_State *L)
{
	MainWindowHost *host = resolveMainWindowHost(nullptr);
	if (!host)
		return 0;

	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
		return 0;

	const int         which            = static_cast<int>(luaL_optinteger(L, 1, 1));
	const bool        screen           = optBool(L, 2, false);
	const bool        useGetWindowRect = optBool(L, 3, false);

	WorldChildWindow *window = findWorldWindowByOrdinal(*host, *runtime, which);
	if (!window)
		return 0;

	QRect rect = useGetWindowRect ? window->frameGeometry() : window->normalGeometry();
	if (!rect.isValid())
		rect = window->geometry();
	if (screen)
	{
		const QPoint topLeft = window->mapToGlobal(rect.topLeft());
		rect.moveTopLeft(topLeft);
	}
	pushWindowPositionTable(L, rect);
	return 1;
}

static int luaGetNotepadWindowPosition(lua_State *L)
{
	const QString    title = QString::fromUtf8(luaL_checkstring(L, 1));
	TextChildWindow *text =
	    findNotepadWindow(title, runtimeFromLuaUpvalue(L), runtimeWorldIdFromLuaUpvalue(L));
	if (!text)
		return 0;

	QRect rect = text->normalGeometry();
	if (!rect.isValid())
		rect = text->geometry();
	pushWindowPositionTable(L, rect);
	return 1;
}

static int luaGetMapColour(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	const long value = luaL_checkinteger(L, 1);
	lua_pushnumber(L, static_cast<lua_Number>(runtime->getMapColour(value)));
	return 1;
}

static int luaGetMappingItem(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const int     index = static_cast<int>(luaL_checkinteger(L, 1));
	const QString entry = runtime->mappingItem(index);
	if (entry.isEmpty())
	{
		lua_pushnil(L);
		return 1;
	}
	lua_pushstring(L, entry.toLocal8Bit().constData());
	return 1;
}

static int luaGetMappingString(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushstring(L, "");
		return 1;
	}
	const QString mapping = runtime->mappingString(false);
	lua_pushstring(L, mapping.toLocal8Bit().constData());
	return 1;
}

static int luaGetNotepadLength(lua_State *L)
{
	const QString    title = QString::fromUtf8(luaL_checkstring(L, 1));
	TextChildWindow *text =
	    findNotepadWindow(title, runtimeFromLuaUpvalue(L), runtimeWorldIdFromLuaUpvalue(L));
	if (!text || !text->editor())
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	const auto length = text->editor()->toPlainText().length();
	lua_pushnumber(L, static_cast<lua_Number>(length));
	return 1;
}

static int luaGetNotepadList(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const bool    all        = QMudLuaSupport::optBoolean(L, 1, false);
	const auto    ownerToken = reinterpret_cast<quintptr>(runtime);
	const QString worldId    = runtime->worldAttributes().value(QStringLiteral("id")).trimmed();

	MainWindow   *frame = resolveMainWindow();
	if (!frame)
	{
		lua_pushnil(L);
		return 1;
	}
	auto *mdi = frame->findChild<QMdiArea *>();
	if (!mdi)
	{
		lua_pushnil(L);
		return 1;
	}

	lua_newtable(L);
	int index = 1;
	for (const QList<QMdiSubWindow *> windows = mdi->subWindowList(QMdiArea::CreationOrder);
	     QMdiSubWindow *sub : windows)
	{
		auto *text = qobject_cast<TextChildWindow *>(sub);
		if (!text)
			continue;
		if (!all)
		{
			if (const auto relatedToken = text->property("worldRuntimeToken").toULongLong();
			    relatedToken == ownerToken)
			{
				// Matched this runtime instance.
			}
			else
			{
				if (relatedToken != 0)
					continue;
				if (const QString related = text->property("worldId").toString();
				    related.isEmpty() || related.compare(worldId, Qt::CaseInsensitive) != 0)
					continue;
			}
		}
		lua_pushstring(L, text->windowTitle().toLocal8Bit().constData());
		lua_rawseti(L, -2, index++);
	}
	if (index == 1)
	{
		lua_pop(L, 1);
		lua_pushnil(L);
		return 1;
	}
	return 1;
}

static int luaGetNotepadText(lua_State *L)
{
	const QString    title = QString::fromUtf8(luaL_checkstring(L, 1));
	TextChildWindow *text =
	    findNotepadWindow(title, runtimeFromLuaUpvalue(L), runtimeWorldIdFromLuaUpvalue(L));
	if (!text || !text->editor())
	{
		lua_pushstring(L, "");
		return 1;
	}
	const QString contents = text->editor()->toPlainText();
	lua_pushstring(L, contents.toLocal8Bit().constData());
	return 1;
}

static int luaGetNotes(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushstring(L, "");
		return 1;
	}
	const QString notes = runtime->worldMultilineAttributes().value(QStringLiteral("notes"));
	lua_pushstring(L, notes.toLocal8Bit().constData());
	return 1;
}

static int luaGetNoteStyle(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	lua_pushnumber(L, runtime->noteStyle());
	return 1;
}

static int luaGetScriptTime(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, 0.0);
		return 1;
	}
	lua_pushnumber(L, runtime->scriptTimeSeconds());
	return 1;
}

static int luaGetSelectionEndColumn(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime || !runtime->view())
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	lua_pushnumber(L, runtime->view()->outputSelectionEndColumn());
	return 1;
}

static int luaGetSelectionEndLine(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime || !runtime->view())
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	lua_pushnumber(L, runtime->view()->outputSelectionEndLine());
	return 1;
}

static int luaGetSelectionStartColumn(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime || !runtime->view())
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	lua_pushnumber(L, runtime->view()->outputSelectionStartColumn());
	return 1;
}

static int luaGetSelectionStartLine(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime || !runtime->view())
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	lua_pushnumber(L, runtime->view()->outputSelectionStartLine());
	return 1;
}

static int luaGetSoundStatus(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, -3);
		return 1;
	}
	const int buffer = static_cast<int>(luaL_checkinteger(L, 1));
	lua_pushnumber(L, runtime->soundStatus(buffer));
	return 1;
}

static int pushStyleInfoTable(lua_State *L, const WorldRuntime::LineEntry &entry, const int styleNumber)
{
	if (styleNumber <= 0 || styleNumber > entry.spans.size())
		return 0;

	int startColumn = 0;
	for (int i = 0; i < styleNumber - 1; ++i)
		startColumn += entry.spans.at(i).length;
	const WorldRuntime::StyleSpan &span = entry.spans.at(styleNumber - 1);

	auto                           actionType = [&]
	{
		switch (span.actionType)
		{
		case WorldRuntime::ActionSend:
			return 1;
		case WorldRuntime::ActionHyperlink:
			return 2;
		case WorldRuntime::ActionPrompt:
			return 3;
		default:
			return 0;
		}
	};

	lua_newtable(L);
	lua_pushstring(L, "text");
	{
		const QByteArray textBytes = entry.text.mid(startColumn, span.length).toLocal8Bit();
		lua_pushlstring(L, textBytes.constData(), textBytes.size());
	}
	lua_rawset(L, -3);
	lua_pushstring(L, "length");
	lua_pushinteger(L, span.length);
	lua_rawset(L, -3);
	lua_pushstring(L, "column");
	lua_pushinteger(L, startColumn + 1);
	lua_rawset(L, -3);
	lua_pushstring(L, "actiontype");
	lua_pushinteger(L, actionType());
	lua_rawset(L, -3);
	lua_pushstring(L, "action");
	lua_pushstring(L, span.action.toLocal8Bit().constData());
	lua_rawset(L, -3);
	lua_pushstring(L, "hint");
	lua_pushstring(L, span.hint.toLocal8Bit().constData());
	lua_rawset(L, -3);
	lua_pushstring(L, "variable");
	lua_pushstring(L, span.variable.toLocal8Bit().constData());
	lua_rawset(L, -3);
	lua_pushstring(L, "bold");
	lua_pushboolean(L, span.bold);
	lua_rawset(L, -3);
	lua_pushstring(L, "ul");
	lua_pushboolean(L, span.underline);
	lua_rawset(L, -3);
	lua_pushstring(L, "blink");
	lua_pushboolean(L, span.blink);
	lua_rawset(L, -3);
	lua_pushstring(L, "inverse");
	lua_pushboolean(L, span.inverse);
	lua_rawset(L, -3);
	lua_pushstring(L, "changed");
	lua_pushboolean(L, span.changed);
	lua_rawset(L, -3);
	lua_pushstring(L, "starttag");
	lua_pushboolean(L, span.startTag);
	lua_rawset(L, -3);
	lua_pushstring(L, "textcolour");
	lua_pushnumber(L, static_cast<lua_Number>(colorValue(span.fore)));
	lua_rawset(L, -3);
	lua_pushstring(L, "backcolour");
	lua_pushnumber(L, static_cast<lua_Number>(colorValue(span.back)));
	lua_rawset(L, -3);
	return 1;
}

static int luaGetStyleInfo(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const int               lineNumber  = static_cast<int>(luaL_checkinteger(L, 1));
	const int               styleNumber = static_cast<int>(luaL_optinteger(L, 2, 0));
	const int               infoType    = static_cast<int>(luaL_optinteger(L, 3, 0));
	WorldRuntime::LineEntry entry;
	const bool              hasLine = runtime->luaContextLineEntry(lineNumber, entry);
	if (styleNumber == 0 || infoType == 0)
	{
		if (!hasLine)
			return 0;
	}

	if (!hasLine)
	{
		lua_pushnil(L);
		return 1;
	}

	if (styleNumber == 0)
	{
		lua_newtable(L);
		for (int i = 1; i <= entry.spans.size(); ++i)
		{
			if (infoType == 0)
			{
				if (!pushStyleInfoTable(L, entry, i))
					lua_pushnil(L);
			}
			else
			{
				int startColumn = 0;
				for (int j = 0; j < i - 1; ++j)
					startColumn += entry.spans.at(j).length;
				const WorldRuntime::StyleSpan &span = entry.spans.at(i - 1);
				switch (infoType)
				{
				case 1:
				{
					const QByteArray textBytes = entry.text.mid(startColumn, span.length).toLocal8Bit();
					lua_pushlstring(L, textBytes.constData(), textBytes.size());
				}
				break;
				case 2:
					lua_pushnumber(L, span.length);
					break;
				case 3:
					lua_pushnumber(L, startColumn + 1);
					break;
				case 4:
					switch (span.actionType)
					{
					case WorldRuntime::ActionSend:
						lua_pushnumber(L, 1);
						break;
					case WorldRuntime::ActionHyperlink:
						lua_pushnumber(L, 2);
						break;
					case WorldRuntime::ActionPrompt:
						lua_pushnumber(L, 3);
						break;
					default:
						lua_pushnumber(L, 0);
						break;
					}
					break;
				case 5:
					lua_pushstring(L, span.action.toLocal8Bit().constData());
					break;
				case 6:
					lua_pushstring(L, span.hint.toLocal8Bit().constData());
					break;
				case 7:
					lua_pushstring(L, span.variable.toLocal8Bit().constData());
					break;
				case 8:
					lua_pushboolean(L, span.bold);
					break;
				case 9:
					lua_pushboolean(L, span.underline);
					break;
				case 10:
					lua_pushboolean(L, span.blink);
					break;
				case 11:
					lua_pushboolean(L, span.inverse);
					break;
				case 12:
					lua_pushboolean(L, span.changed);
					break;
				case 13:
					lua_pushboolean(L, span.startTag);
					break;
				case 14:
					lua_pushnumber(L, static_cast<lua_Number>(colorValue(span.fore)));
					break;
				case 15:
					lua_pushnumber(L, static_cast<lua_Number>(colorValue(span.back)));
					break;
				default:
					lua_pushnil(L);
					break;
				}
			}
			lua_rawseti(L, -2, i);
		}
		return 1;
	}

	if (infoType == 0)
		return pushStyleInfoTable(L, entry, styleNumber);

	if (styleNumber <= 0 || styleNumber > entry.spans.size())
	{
		lua_pushnil(L);
		return 1;
	}

	int startColumn = 0;
	for (int i = 0; i < styleNumber - 1; ++i)
		startColumn += entry.spans.at(i).length;
	const WorldRuntime::StyleSpan &span = entry.spans.at(styleNumber - 1);

	switch (infoType)
	{
	case 1:
	{
		const QByteArray textBytes = entry.text.mid(startColumn, span.length).toLocal8Bit();
		lua_pushlstring(L, textBytes.constData(), textBytes.size());
	}
	break;
	case 2:
		lua_pushnumber(L, span.length);
		break;
	case 3:
		lua_pushnumber(L, startColumn + 1);
		break;
	case 4:
		switch (span.actionType)
		{
		case WorldRuntime::ActionSend:
			lua_pushnumber(L, 1);
			break;
		case WorldRuntime::ActionHyperlink:
			lua_pushnumber(L, 2);
			break;
		case WorldRuntime::ActionPrompt:
			lua_pushnumber(L, 3);
			break;
		default:
			lua_pushnumber(L, 0);
			break;
		}
		break;
	case 5:
		lua_pushstring(L, span.action.toLocal8Bit().constData());
		break;
	case 6:
		lua_pushstring(L, span.hint.toLocal8Bit().constData());
		break;
	case 7:
		lua_pushstring(L, span.variable.toLocal8Bit().constData());
		break;
	case 8:
		lua_pushboolean(L, span.bold);
		break;
	case 9:
		lua_pushboolean(L, span.underline);
		break;
	case 10:
		lua_pushboolean(L, span.blink);
		break;
	case 11:
		lua_pushboolean(L, span.inverse);
		break;
	case 12:
		lua_pushboolean(L, span.changed);
		break;
	case 13:
		lua_pushboolean(L, span.startTag);
		break;
	case 14:
		lua_pushnumber(L, static_cast<lua_Number>(colorValue(span.fore)));
		break;
	case 15:
		lua_pushnumber(L, static_cast<lua_Number>(colorValue(span.back)));
		break;
	default:
		lua_pushnil(L);
		break;
	}
	return 1;
}

static int luaGetSysColor(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	const int index = static_cast<int>(luaL_checkinteger(L, 1));
	lua_pushnumber(L, static_cast<lua_Number>(WorldRuntime::getSysColor(index)));
	return 1;
}

static int luaGetSystemMetrics(lua_State *L)
{
	const int index  = static_cast<int>(luaL_checkinteger(L, 1));
	QScreen  *screen = QGuiApplication::primaryScreen();
	if (!screen)
	{
		lua_pushnumber(L, 0);
		return 1;
	}

	QRect                  virtualGeom;
	const QList<QScreen *> screens = QGuiApplication::screens();
	for (QScreen *s : screens)
		virtualGeom = virtualGeom.united(s->geometry());
	if (virtualGeom.isNull())
		virtualGeom = screen->geometry();

	const QRect geom  = screen->geometry();
	const QRect avail = screen->availableGeometry();

	switch (index)
	{
	case 0: // SM_CXSCREEN
		lua_pushnumber(L, geom.width());
		break;
	case 1: // SM_CYSCREEN
		lua_pushnumber(L, geom.height());
		break;
	case 76: // SM_XVIRTUALSCREEN
		lua_pushnumber(L, virtualGeom.x());
		break;
	case 77: // SM_YVIRTUALSCREEN
		lua_pushnumber(L, virtualGeom.y());
		break;
	case 78: // SM_CXVIRTUALSCREEN
		lua_pushnumber(L, virtualGeom.width());
		break;
	case 79: // SM_CYVIRTUALSCREEN
		lua_pushnumber(L, virtualGeom.height());
		break;
	case 16: // SM_CXFULLSCREEN
		lua_pushnumber(L, avail.width());
		break;
	case 17: // SM_CYFULLSCREEN
		lua_pushnumber(L, avail.height());
		break;
	case 68: // SM_CXDRAG
	case 69: // SM_CYDRAG
	{
		const QStyleHints *hints = QGuiApplication::styleHints();
		const int          drag  = hints ? hints->startDragDistance() : 4;
		lua_pushnumber(L, drag > 0 ? drag : 4);
	}
	break;
	case 75: // SM_MOUSEWHEELPRESENT
		lua_pushnumber(L, 1);
		break;
	case 80: // SM_CMONITORS
		lua_pushnumber(L, static_cast<lua_Number>(screens.isEmpty() ? 1 : screens.size()));
		break;
	case 81: // SM_SAMEDISPLAYFORMAT
	{
		bool same = true;
		if (!screens.isEmpty())
		{
			const int depth = screens.first()->depth();
			for (int i = 1; i < screens.size(); ++i)
			{
				if (screens.at(i)->depth() != depth)
				{
					same = false;
					break;
				}
			}
		}
		lua_pushnumber(L, same ? 1 : 0);
	}
	break;
	default:
		lua_pushnumber(L, 0);
		break;
	}
	return 1;
}

static int luaGetUdpPort(lua_State *L)
{
	const int first = static_cast<int>(luaL_checkinteger(L, 1));
	const int last  = static_cast<int>(luaL_checkinteger(L, 2));
	if (first > last || first < 1 || last > 65535)
	{
		lua_pushnumber(L, 0);
		return 1;
	}

	MainWindowHost *host = resolveMainWindowHost(nullptr);
	if (!host)
	{
		lua_pushnumber(L, 0);
		return 1;
	}

	QSet<int> usedPorts;
	for (const WorldWindowDescriptor &entry : host->worldWindowDescriptors())
	{
		if (!entry.runtime)
		{
			continue;
		}
		for (const QList<int> ports = entry.runtime->udpPortList(); int port : ports)
			usedPorts.insert(port);
	}

	for (int port = first; port < last; ++port)
	{
		if (!usedPorts.contains(port))
		{
			lua_pushnumber(L, port);
			return 1;
		}
	}

	lua_pushnumber(L, 0);
	return 1;
}

static int luaUdpListen(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}
	const QString pluginId = engine->pluginId();
	if (pluginId.isEmpty())
	{
		lua_pushnumber(L, eNotAPlugin);
		return 1;
	}
	const QString ip     = QString::fromUtf8(luaL_checkstring(L, 1));
	const int     port   = static_cast<int>(luaL_checkinteger(L, 2));
	const QString script = QString::fromUtf8(luaL_checkstring(L, 3));
	const int     result = runtime->udpListen(pluginId, ip, port, script);
	lua_pushnumber(L, result);
	return 1;
}

static int luaUdpPortList(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const QList<int> ports = runtime->udpPortList();
	if (ports.isEmpty())
	{
		lua_pushnil(L);
		return 1;
	}
	lua_newtable(L);
	int index = 1;
	for (int port : ports)
	{
		lua_pushinteger(L, port);
		lua_rawseti(L, -2, index++);
	}
	return 1;
}

static int luaUdpSend(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}
	const QString ip      = QString::fromUtf8(luaL_checkstring(L, 1));
	const int     port    = static_cast<int>(luaL_checkinteger(L, 2));
	size_t        length  = 0;
	const auto   *data    = luaL_checklstring(L, 3, &length);
	const auto    payload = QByteArray(data, static_cast<int>(length));
	const int     result  = WorldRuntime::udpSend(ip, port, payload);
	lua_pushnumber(L, result);
	return 1;
}

static int luaGetUniqueNumber(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	lua_pushnumber(L, static_cast<lua_Number>(WorldRuntime::getUniqueNumber()));
	return 1;
}

static int luaGetWorld(lua_State *L)
{
	const QString name   = QString::fromUtf8(luaL_checkstring(L, 1));
	auto         *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *target = findWorldRuntimeByName(name);
	return pushWorldProxyResult(L, engine, target);
}

static int luaGetWorldById(lua_State *L)
{
	const QString id     = QString::fromUtf8(luaL_checkstring(L, 1));
	auto         *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *target = findWorldRuntimeById(id);
	return pushWorldProxyResult(L, engine, target);
}

static int pushWorldAttributeList(lua_State *L, const QString &attributeName)
{
	MainWindowHost *host = resolveMainWindowHost(nullptr);
	if (!host)
	{
		lua_pushnil(L);
		return 1;
	}

	const QVector<WorldWindowDescriptor> worlds = host->worldWindowDescriptors();
	if (worlds.isEmpty())
	{
		lua_pushnil(L);
		return 1;
	}

	lua_newtable(L);
	int index = 1;
	for (const WorldWindowDescriptor &entry : worlds)
	{
		if (!entry.runtime)
			continue;
		const QMap<QString, QString> &attributes = entry.runtime->worldAttributes();
		const auto                    attrIt     = attributes.constFind(attributeName);
		const QString                 value      = attrIt != attributes.cend() ? attrIt.value() : QString{};
		lua_pushstring(L, value.toLocal8Bit().constData());
		lua_rawseti(L, -2, index++);
	}
	if (index == 1)
	{
		lua_pop(L, 1);
		lua_pushnil(L);
	}
	return 1;
}

static int pushWorldProxyResult(lua_State *L, LuaCallbackEngine *engine, WorldRuntime *runtime)
{
	if (!engine || !runtime)
		return 0;
	const QMap<QString, QString> &attributes = runtime->worldAttributes();
	const auto                    idIt       = attributes.constFind(QStringLiteral("id"));
	const QString                 worldId    = idIt != attributes.cend() ? idIt.value() : QString{};
	pushWorldProxy(L, engine, runtime, worldId);
	return 1;
}

static int luaGetWorldIdList(lua_State *L)
{
	return pushWorldAttributeList(L, QStringLiteral("id"));
}

static int luaGetWorldList(lua_State *L)
{
	return pushWorldAttributeList(L, QStringLiteral("name"));
}

static int luaHash(lua_State *L)
{
	const QString    text = QString::fromUtf8(luaL_checkstring(L, 1));
	const QByteArray hash = QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha1);
	lua_pushstring(L, hash.toHex().constData());
	return 1;
}

static QString sqliteVersionString()
{
	static QString cached;
	if (!cached.isEmpty())
		return cached;
	static int    connectionCounter = 0;
	const QString connectionName    = QStringLiteral("sqlite_version_%1").arg(++connectionCounter);
	{
		QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
		db.setDatabaseName(QStringLiteral(":memory:"));
		if (db.open())
		{
			if (QSqlQuery query(db); query.exec(QStringLiteral("SELECT sqlite_version()")) && query.next())
				cached = query.value(0).toString();
		}
		db.close();
	}
	QSqlDatabase::removeDatabase(connectionName);
	if (cached.isEmpty())
		cached = QStringLiteral("unknown");
	return cached;
}

static int sqliteVersionNumber()
{
	const QString     version = sqliteVersionString();
	const QStringList parts   = version.split(QLatin1Char('.'));
	if (parts.size() < 2)
		return 0;
	bool      okMajor = false;
	bool      okMinor = false;
	bool      okPatch = false;
	const int major   = parts.value(0).toInt(&okMajor);
	const int minor   = parts.value(1).toInt(&okMinor);
	const int patch   = parts.value(2).toInt(&okPatch);
	if (!okMajor || !okMinor)
		return 0;
	return major * 1000000 + minor * 1000 + (okPatch ? patch : 0);
}

static bool sqliteThreadsafe()
{
	static bool cached      = true;
	static bool initialized = false;
	if (initialized)
		return cached;
	initialized = true;

	static int    connectionCounter = 0;
	const QString connectionName    = QStringLiteral("sqlite_threadsafe_%1").arg(++connectionCounter);
	{
		QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
		db.setDatabaseName(QStringLiteral(":memory:"));
		if (db.open())
		{
			if (QSqlQuery query(db); query.exec(QStringLiteral("PRAGMA compile_options")))
			{
				while (query.next())
				{
					const QString option = query.value(0).toString().trimmed().toUpper();
					if (!option.startsWith(QStringLiteral("THREADSAFE=")))
						continue;
					const int value = option.mid(QStringLiteral("THREADSAFE=").size()).toInt();
					cached          = value != 0;
					break;
				}
			}
		}
		db.close();
	}
	QSqlDatabase::removeDatabase(connectionName);
	return cached;
}

static bool queryHelpRow(const QSqlDatabase &db, const QString &sql, const QString &key,
                         QMap<QString, QString> &row)
{
	QSqlQuery query(db);
	if (!query.prepare(sql))
		return false;
	query.addBindValue(key);
	if (!query.exec())
		return false;
	if (!query.next())
		return false;
	const QSqlRecord record = query.record();
	for (int i = 0; i < record.count(); ++i)
	{
		const QString name = record.fieldName(i);
		if (name.isEmpty())
			continue;
		row.insert(name, query.value(i).toString());
	}
	return true;
}

static bool showHelpDialog(const QString &title, const QString &html)
{
	QDialog dialog(resolveMainWindow());
	dialog.setWindowTitle(title);
	dialog.setWindowFlags(dialog.windowFlags() | Qt::Tool);
	QVBoxLayout  layout(&dialog);
	QTextBrowser browser;
	browser.setOpenExternalLinks(true);
	browser.setHtml(html);
	layout.addWidget(&browser);
	QDialogButtonBox buttons(QDialogButtonBox::Close);
	QObject::connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	QObject::connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	layout.addWidget(&buttons);
	dialog.exec();
	return true;
}

static bool showHelpTopic(const QString &prefix, const QString &topic)
{
	const QString dbPath = AppController::resolveHelpDatabasePath();
	if (dbPath.isEmpty())
	{
		QMessageBox::information(resolveMainWindow(), QStringLiteral("Help"),
		                         QStringLiteral("Help database (help.db) not found."));
		return false;
	}
	static int    connectionCounter = 0;
	const QString connectionName    = QStringLiteral("help_db_%1").arg(++connectionCounter);
	const auto    toHtml = [](const QString &text) -> QString { return Qt::convertFromPlainText(text); };

	bool          handled = false;
	QString       title;
	QString       html;
	bool          opened = false;
	{
		QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
		db.setDatabaseName(dbPath);
		db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
		opened = db.open();
		if (opened && prefix == QStringLiteral("FNC_"))
		{
			if (QMap<QString, QString> row; queryHelpRow(
			        db,
			        QStringLiteral(
			            "SELECT name, prototype, summary, description, return_value, lua_example, lua_notes, "
			            "see_also, version, type_of_object FROM functions WHERE name = ?"),
			        topic, row))
			{
				title = row.value(QStringLiteral("name"));
				html += QStringLiteral("<h2>%1</h2>").arg(title.toHtmlEscaped());
				html += QStringLiteral("<p><b>Prototype:</b><br>%1</p>")
				            .arg(toHtml(row.value(QStringLiteral("prototype"))));
				html += QStringLiteral("<p><b>Summary:</b> %1</p>")
				            .arg(row.value(QStringLiteral("summary")).toHtmlEscaped());
				html += QStringLiteral("<p>%1</p>").arg(toHtml(row.value(QStringLiteral("description"))));
				if (const QString returnValue = row.value(QStringLiteral("return_value"));
				    !returnValue.isEmpty())
					html += QStringLiteral("<p><b>Returns:</b><br>%1</p>").arg(toHtml(returnValue));
				if (const QString luaExample = row.value(QStringLiteral("lua_example"));
				    !luaExample.isEmpty())
					html += QStringLiteral("<p><b>Lua example:</b><br>%1</p>").arg(toHtml(luaExample));
				if (const QString luaNotes = row.value(QStringLiteral("lua_notes")); !luaNotes.isEmpty())
					html += QStringLiteral("<p><b>Lua notes:</b><br>%1</p>").arg(toHtml(luaNotes));
				if (const QString seeAlso = row.value(QStringLiteral("see_also")); !seeAlso.isEmpty())
					html += QStringLiteral("<p><b>See also:</b> %1</p>").arg(seeAlso.toHtmlEscaped());
				handled = true;
			}
		}
		else if (opened && prefix == QStringLiteral("LUA_"))
		{
			if (QMap<QString, QString> row; queryHelpRow(
			        db,
			        QStringLiteral(
			            "SELECT name, prototype, summary, description FROM lua_functions WHERE name = ?"),
			        topic, row))
			{
				title = row.value(QStringLiteral("name"));
				html += QStringLiteral("<h2>%1</h2>").arg(title.toHtmlEscaped());
				html += QStringLiteral("<p><b>Prototype:</b><br>%1</p>")
				            .arg(toHtml(row.value(QStringLiteral("prototype"))));
				html += QStringLiteral("<p><b>Summary:</b> %1</p>")
				            .arg(row.value(QStringLiteral("summary")).toHtmlEscaped());
				html += QStringLiteral("<p>%1</p>").arg(toHtml(row.value(QStringLiteral("description"))));
				handled = true;
			}
		}
		else if (opened && prefix == QStringLiteral("DOC_"))
		{
			if (QMap<QString, QString> row; queryHelpRow(
			        db, QStringLiteral("SELECT title, description FROM general_doc WHERE doc_name = ?"),
			        topic, row))
			{
				title = row.value(QStringLiteral("title"));
				html += QStringLiteral("<h2>%1</h2>").arg(title.toHtmlEscaped());
				html += QStringLiteral("<p>%1</p>").arg(toHtml(row.value(QStringLiteral("description"))));
				handled = true;
			}
		}
		db.close();
	}
	QSqlDatabase::removeDatabase(connectionName);
	if (!opened)
	{
		QMessageBox::warning(resolveMainWindow(), QStringLiteral("Help"),
		                     QStringLiteral("Unable to open help database."));
		return false;
	}
	if (!handled)
		return false;
	return showHelpDialog(title.isEmpty() ? QStringLiteral("Help") : title, html);
}

static QString chooseHelpFromList(const QString &filter, const bool includeLua,
                                  const QSet<QString> &luaFunctions)
{
	QDialog dialog(resolveMainWindow());
	dialog.setWindowTitle(QStringLiteral("Functions"));
	dialog.setWindowFlags(dialog.windowFlags() | Qt::Tool);
	QVBoxLayout layout(&dialog);

	QLabel      filterLabel(QStringLiteral("Filter"));
	QLineEdit   filterEdit;
	filterEdit.setText(filter);
	layout.addWidget(&filterLabel);
	layout.addWidget(&filterEdit);

	QListWidget list;
	layout.addWidget(&list);

	QPushButton     *luaButton = nullptr;
	QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	if (includeLua)
	{
		luaButton = buttons.addButton(QStringLiteral("Lua functions"), QDialogButtonBox::ActionRole);
	}
	layout.addWidget(&buttons);

	QObject::connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	QObject::connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	QObject::connect(&list, &QListWidget::itemDoubleClicked, &dialog, &QDialog::accept);

	auto populateList = [&]
	{
		const QString needle = filterEdit.text().trimmed();
		list.clear();
		const auto matches = [&](const QString &value)
		{ return needle.isEmpty() || value.contains(needle, Qt::CaseInsensitive); };
		for (int i = 0; kInternalFunctionMetadataTable[i].functionName[0]; ++i)
		{
			if (const QString name = QString::fromUtf8(kInternalFunctionMetadataTable[i].functionName);
			    matches(name))
				list.addItem(name);
		}
		if (includeLua)
		{
			for (const QString &entry : luaFunctions)
			{
				if (matches(entry))
					list.addItem(entry);
			}
		}
		list.sortItems();
		if (list.count() == 1)
			list.setCurrentRow(0);
	};

	QObject::connect(&filterEdit, &QLineEdit::textChanged, &dialog, [&](const QString &) { populateList(); });
	if (luaButton)
	{
		QObject::connect(luaButton, &QPushButton::clicked, &dialog,
		                 [&] { dialog.done(QDialog::Accepted + 1); });
	}

	populateList();

	const int result = dialog.exec();
	if (result == QDialog::Accepted + 1)
		return QStringLiteral("DOC_lua");
	if (result != QDialog::Accepted)
		return {};
	QListWidgetItem *item = list.currentItem();
	return item ? item->text() : QString();
}

static int luaHelp(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
		return 0;

	QString filter                      = QString::fromUtf8(luaL_optstring(L, 1, ""));
	filter                              = filter.trimmed().toLower();
	const QSet<QString>   &luaFunctions = engine->luaFunctionsSet();

	QMap<QString, QString> luaSpecials;
	luaSpecials.insert(QStringLiteral("lua"), QStringLiteral("lua"));
	luaSpecials.insert(QStringLiteral("lua b"), QStringLiteral("lua_base"));
	luaSpecials.insert(QStringLiteral("lua c"), QStringLiteral("lua_coroutines"));
	luaSpecials.insert(QStringLiteral("lua d"), QStringLiteral("lua_debug"));
	luaSpecials.insert(QStringLiteral("lua i"), QStringLiteral("lua_io"));
	luaSpecials.insert(QStringLiteral("lua m"), QStringLiteral("lua_math"));
	luaSpecials.insert(QStringLiteral("lua o"), QStringLiteral("lua_os"));
	luaSpecials.insert(QStringLiteral("lua p"), QStringLiteral("lua_package"));
	luaSpecials.insert(QStringLiteral("lua r"), QStringLiteral("lua_rex"));
	luaSpecials.insert(QStringLiteral("lua s"), QStringLiteral("lua_string"));
	luaSpecials.insert(QStringLiteral("lua t"), QStringLiteral("lua_tables"));
	luaSpecials.insert(QStringLiteral("lua u"), QStringLiteral("lua_utils"));

	if (filter == QStringLiteral("lua bc"))
	{
		showHelpTopic(QStringLiteral("DOC_"), QStringLiteral("lua_bc"));
		return 0;
	}

	if (const QString key = filter.left(5); luaSpecials.contains(key))
	{
		showHelpTopic(QStringLiteral("DOC_"), luaSpecials.value(key));
		return 0;
	}

	if (!filter.isEmpty())
	{
		for (const QString &entry : luaFunctions)
		{
			if (entry.toLower() == filter)
			{
				showHelpTopic(QStringLiteral("LUA_"), entry);
				return 0;
			}
		}
		for (int i = 0; kInternalFunctionMetadataTable[i].functionName[0]; ++i)
		{
			if (const QString name = QString::fromUtf8(kInternalFunctionMetadataTable[i].functionName);
			    name.toLower() == filter)
			{
				showHelpTopic(QStringLiteral("FNC_"), name);
				return 0;
			}
		}
	}

	bool includeLua = !luaFunctions.isEmpty();
	if (includeLua)
	{
		const QMap<QString, QString> &attrs = runtime->worldAttributes();
		const bool    scriptsEnabled        = isEnabledValue(attrs.value(QStringLiteral("enable_scripts")));
		const QString language              = attrs.value(QStringLiteral("script_language"));
		includeLua                          = scriptsEnabled && (language.isEmpty() ||
                                        language.compare(QStringLiteral("lua"), Qt::CaseInsensitive) == 0);
	}
	const QString selection = chooseHelpFromList(filter, includeLua, luaFunctions);
	if (selection.isEmpty())
		return 0;
	if (selection == QStringLiteral("DOC_lua"))
	{
		showHelpTopic(QStringLiteral("DOC_"), QStringLiteral("lua"));
		return 0;
	}
	const QString lower = selection.toLower();
	for (const QString &entry : luaFunctions)
	{
		if (entry.toLower() == lower)
		{
			showHelpTopic(QStringLiteral("LUA_"), entry);
			return 0;
		}
	}
	showHelpTopic(QStringLiteral("FNC_"), selection);
	return 0;
}

static int luaHyperlink(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
		return 0;

	const QString action      = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString text        = QString::fromUtf8(luaL_checkstring(L, 2));
	const QString hint        = QString::fromUtf8(luaL_checkstring(L, 3));
	const QString textColour  = QString::fromUtf8(luaL_checkstring(L, 4));
	const QString backColour  = QString::fromUtf8(luaL_checkstring(L, 5));
	const bool    url         = optBool(L, 6, false);
	const bool    noUnderline = optBool(L, 7, false);

	if (action.isEmpty())
		return 0;

	const QString           outputText = text.isEmpty() ? action : text;
	WorldRuntime::StyleSpan span;
	span.length     = sizeToInt(outputText.size());
	span.actionType = url ? WorldRuntime::ActionHyperlink : WorldRuntime::ActionSend;
	span.action     = action;
	span.hint       = hint.isEmpty() ? action : hint;
	span.underline  = !noUnderline;
	span.fore       = WorldView::parseColor(textColour);
	span.back       = WorldView::parseColor(backColour);

	if (!span.fore.isValid())
		span.fore =
		    WorldView::parseColor(runtime->worldAttributes().value(QStringLiteral("hyperlink_colour")));
	if (!span.back.isValid())
		span.back = colorFromValue(runtime->noteColourBack());

	QVector<WorldRuntime::StyleSpan> spans;
	spans.push_back(span);
	runtime->outputStyledText(outputText, spans, true, false);
	return 0;
}

static int luaImportXML(lua_State *L)
{
	const QString  xml = QString::fromUtf8(luaL_checkstring(L, 1));
	AppController *app = AppController::instance();
	if (!app)
	{
		lua_pushnumber(L, -1);
		return 1;
	}

	constexpr auto mask =
	    ~(WorldDocument::XML_PLUGINS | WorldDocument::XML_NO_PLUGINS | WorldDocument::XML_GENERAL |
	      WorldDocument::XML_PASTE_DUPLICATE | WorldDocument::XML_IMPORT_MAIN_FILE_ONLY);

	const AppController::ImportResult result = app->importXmlFromText(xml, mask);
	if (!result.ok)
	{
		lua_pushnumber(L, -1);
		return 1;
	}

	const int count = result.triggers + result.aliases + result.timers + result.macros + result.variables +
	                  result.colours + result.keypad + result.printing;
	lua_pushnumber(L, count);
	return 1;
}

static int luaInfo(lua_State *L)
{
	const QString message = concatLuaArgs(L, 1);
	if (MainWindow *frame = resolveMainWindow())
		frame->infoBarAppend(message);
	return 0;
}

static int luaInfoBackground(lua_State *L)
{
	MainWindow *frame = resolveMainWindow();
	if (!frame)
		return 0;
	const QString name = QString::fromUtf8(luaL_checkstring(L, 1));
	frame->infoBarSetBackground(WorldView::parseColor(name));
	return 0;
}

static int luaInfoClear(lua_State *L)
{
	const int stackTop = lua_gettop(L);
	Q_UNUSED(stackTop);
	if (MainWindow *frame = resolveMainWindow())
		frame->infoBarClear();
	return 0;
}

static int luaInfoColour(lua_State *L)
{
	MainWindow *frame = resolveMainWindow();
	if (!frame)
		return 0;
	const QString name = QString::fromUtf8(luaL_checkstring(L, 1));
	frame->infoBarSetColour(WorldView::parseColor(name));
	return 0;
}

static int luaInfoFont(lua_State *L)
{
	const QString fontName = QString::fromUtf8(luaL_checkstring(L, 1));
	const int     size     = static_cast<int>(luaL_checkinteger(L, 2));
	const int     style    = static_cast<int>(luaL_checkinteger(L, 3));
	if (MainWindow *frame = resolveMainWindow())
		frame->infoBarSetFont(fontName, size, style);
	return 0;
}

static int luaLogSend(lua_State *L)
{
	const auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}
	const QString text = concatLuaArgs(L, 1);
	const bool    echo = isEnabledValue(runtime->worldAttributes().value(QStringLiteral("display_my_input")));
	const int     result = runtime->sendCommand(text, echo, false, false, false, false);
	if (result == eOK)
		runtime->logInputCommand(text);
	lua_pushnumber(L, result);
	return 1;
}

static int luaMakeRegularExpression(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushstring(L, "");
		return 1;
	}
	const QString input = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString regex = WorldRuntime::makeRegularExpression(input);
	lua_pushstring(L, regex.toLatin1().constData());
	return 1;
}

static int luaMapColour(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
		return 0;
	const long original    = luaL_checkinteger(L, 1);
	const long replacement = luaL_checkinteger(L, 2);
	runtime->mapColour(original, replacement);
	return 0;
}

static int luaMapColourList(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const QMap<long, long> map = runtime->mapColourList();
	if (map.isEmpty())
	{
		lua_pushnil(L);
		return 1;
	}
	lua_newtable(L);
	int index = 1;
	for (auto it = map.constBegin(); it != map.constEnd(); ++it)
	{
		const QString lhs     = WorldRuntime::rgbColourToName(it.key());
		const QString rhs     = WorldRuntime::rgbColourToName(it.value());
		const QString mapping = lhs + QStringLiteral(" = ") + rhs;
		lua_pushinteger(L, index++);
		lua_pushstring(L, mapping.toLocal8Bit().constData());
		lua_rawset(L, -3);
	}
	return 1;
}

static QString showMenuDialog(const WorldView *view, const QStringList &items, const QString &def)
{
	if (items.isEmpty())
		return {};

	QDialog dialog(resolveMainWindow());
	dialog.setWindowTitle(QStringLiteral("Menu"));
	dialog.setWindowFlags(dialog.windowFlags() | Qt::Tool);
	QVBoxLayout layout(&dialog);
	QListWidget list;
	list.addItems(items);
	layout.addWidget(&list);

	QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	layout.addWidget(&buttons);

	QObject::connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	QObject::connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	QObject::connect(&list, &QListWidget::itemDoubleClicked, &dialog, &QDialog::accept);

	if (!def.isEmpty())
	{
		if (const QList<QListWidgetItem *> matches = list.findItems(def, Qt::MatchExactly);
		    !matches.isEmpty())
			list.setCurrentItem(matches.first());
	}
	if (view && view->inputEditor())
	{
		QPlainTextEdit *input  = view->inputEditor();
		QTextCursor     cursor = input->textCursor();
		cursor.setPosition(cursor.selectionEnd());
		const QRect  rect   = input->cursorRect(cursor);
		const QPoint global = input->mapToGlobal(rect.bottomLeft());
		dialog.move(global);
	}

	if (dialog.exec() != QDialog::Accepted)
		return {};

	QListWidgetItem *selected = list.currentItem();
	if (!selected)
		return {};
	return selected->text();
}

static int luaMenu(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	WorldView    *view    = runtime ? runtime->view() : nullptr;
	// Legacy behavior: if there is no command-input view, return empty selection.
	if (!view || !view->inputEditor())
	{
		lua_pushstring(L, "");
		return 1;
	}
	const QString items = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString def   = QString::fromUtf8(luaL_optstring(L, 2, ""));
	QStringList   parts = items.split(QLatin1Char('|'), Qt::KeepEmptyParts);
	parts.sort(Qt::CaseSensitive);
	parts.removeDuplicates();
	const QString result = showMenuDialog(view, parts, def);
	lua_pushstring(L, result.toLocal8Bit().constData());
	return 1;
}

static int luaMetaphone(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushstring(L, "");
		return 1;
	}
	const int     length = static_cast<int>(luaL_optinteger(L, 2, 4));
	const QString input  = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString result = WorldRuntime::metaphone(input, length);
	lua_pushstring(L, result.toUtf8().constData());
	return 1;
}

static int luaGetMapping(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	lua_pushboolean(L, runtime && runtime->isMapping());
	return 1;
}

static int luaSetMapping(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
		return 0;
	const bool enabled = optBool(L, 1, true);
	runtime->setMappingEnabled(enabled);
	return 0;
}

static int luaGetNormalColour(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	const int which = static_cast<int>(luaL_checkinteger(L, 1));
	lua_pushnumber(L, static_cast<lua_Number>(runtime->normalColour(which)));
	return 1;
}

static int luaSetNormalColour(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
		return 0;
	const int  which = static_cast<int>(luaL_checkinteger(L, 1));
	const long value = luaL_checkinteger(L, 2);
	runtime->setNormalColour(which, value);
	return 0;
}

static int luaMoveMainWindow(lua_State *L)
{
	MainWindow *mw = resolveMainWindow();
	if (!mw)
		return 0;
	const int left   = static_cast<int>(luaL_checkinteger(L, 1));
	const int top    = static_cast<int>(luaL_checkinteger(L, 2));
	const int width  = static_cast<int>(luaL_checkinteger(L, 3));
	const int height = static_cast<int>(luaL_checkinteger(L, 4));
	mw->setGeometry(left, top, width, height);
	return 0;
}

static QString runtimeWorldIdFromLuaUpvalue(lua_State *L)
{
	WorldRuntime *runtime = runtimeFromLuaUpvalue(L);
	if (!runtime)
		return {};
	return runtime->worldAttributes().value(QStringLiteral("id")).trimmed();
}

static WorldRuntime *runtimeFromLuaUpvalue(lua_State *L)
{
	const auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	return engine ? engine->worldRuntime() : nullptr;
}

static TextChildWindow *findNotepadWindow(const QString &title, WorldRuntime *ownerRuntime,
                                          const QString &worldId)
{
	MainWindow *frame = resolveMainWindow();
	if (!frame)
		return nullptr;
	auto *mdi = frame->findChild<QMdiArea *>();
	if (!mdi)
		return nullptr;
	const qulonglong ownerToken           = ownerRuntime ? reinterpret_cast<quintptr>(ownerRuntime) : 0;
	TextChildWindow *unnamedOwnerFallback = nullptr;
	for (const QList<QMdiSubWindow *> windows = mdi->subWindowList(QMdiArea::CreationOrder);
	     QMdiSubWindow *sub : windows)
	{
		auto *text = qobject_cast<TextChildWindow *>(sub);
		if (!text)
			continue;
		if (text->windowTitle().compare(title, Qt::CaseInsensitive) == 0)
		{
			const qulonglong relatedToken = text->property("worldRuntimeToken").toULongLong();
			if (ownerToken != 0 && relatedToken == ownerToken)
				return text;
			if (ownerToken != 0 && relatedToken != 0 && relatedToken != ownerToken)
				continue;
			if (worldId.isEmpty())
				return text;
			const QString related = text->property("worldId").toString().trimmed();
			if (related.compare(worldId, Qt::CaseInsensitive) == 0)
				return text;
			if (related.isEmpty() && !unnamedOwnerFallback)
				unnamedOwnerFallback = text;
		}
	}
	return unnamedOwnerFallback;
}

static int luaMoveWorldWindow(lua_State *L)
{
	MainWindowHost *host = resolveMainWindowHost(nullptr);
	if (!host)
		return 0;
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
		return 0;
	const int left   = static_cast<int>(luaL_checkinteger(L, 1));
	const int top    = static_cast<int>(luaL_checkinteger(L, 2));
	const int width  = static_cast<int>(luaL_checkinteger(L, 3));
	const int height = static_cast<int>(luaL_checkinteger(L, 4));
	const int which  = static_cast<int>(luaL_optinteger(L, 5, 1));

	if (WorldChildWindow *window = findWorldWindowByOrdinal(*host, *runtime, which))
		window->setGeometry(left, top, width, height);
	return 0;
}

static int luaMoveNotepadWindow(lua_State *L)
{
	const QString    title  = QString::fromUtf8(luaL_checkstring(L, 1));
	const int        left   = static_cast<int>(luaL_checkinteger(L, 2));
	const int        top    = static_cast<int>(luaL_checkinteger(L, 3));
	const int        width  = static_cast<int>(luaL_checkinteger(L, 4));
	const int        height = static_cast<int>(luaL_checkinteger(L, 5));

	TextChildWindow *text =
	    findNotepadWindow(title, runtimeFromLuaUpvalue(L), runtimeWorldIdFromLuaUpvalue(L));
	if (text)
		text->setGeometry(left, top, width, height);
	lua_pushboolean(L, text != nullptr);
	return 1;
}

static int luaMtSrand(lua_State *L)
{
	AppController *app = AppController::instance();
	if (!app)
		return 0;
	if (constexpr int table = 1; lua_istable(L, table))
	{
		QVector<quint32> values;
		for (lua_pushnil(L); lua_next(L, table) != 0; lua_pop(L, 1))
		{
			if (!lua_isnumber(L, -1))
				luaL_error(L, "MtSrand table must consist of numbers");
			values.push_back(static_cast<quint32>(lua_tonumber(L, -1)));
		}
		if (values.isEmpty())
			luaL_error(L, "MtSrand table must not be empty");
		app->seedRandomFromArray(values);
		return 0;
	}

	const auto seed = static_cast<quint32>(luaL_checknumber(L, 1));
	app->seedRandom(seed);
	return 0;
}

static int luaMtRand(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	lua_pushnumber(L, WorldRuntime::mtRand());
	return 1;
}

static int luaNoteHr(lua_State *L)
{
	const auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
		return 0;
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
		return 0;
	runtime->outputHtml(QStringLiteral("<hr/>"));
	int flags = WorldRuntime::LineHorizontalRule;
	if (isEnabledValue(runtime->worldAttributes().value(QStringLiteral("log_notes"))))
		flags |= WorldRuntime::LineLog;
	runtime->addLine(QString(), flags, true);
	return 0;
}

static int luaNoteColourName(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
		return 0;

	const QString foreName = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString backName = QString::fromUtf8(luaL_checkstring(L, 2));

	if (!runtime->notesInRgb())
	{
		runtime->setNoteColourFore(runtime->noteColourFore());
		runtime->setNoteColourBack(runtime->noteColourBack());
	}

	const QColor fore = WorldView::parseColor(foreName);
	const QColor back = WorldView::parseColor(backName);
	if (fore.isValid())
		runtime->setNoteColourFore(colorValue(fore));
	if (back.isValid())
		runtime->setNoteColourBack(colorValue(back));
	return 0;
}

static int luaNoteColourRGB(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
		return 0;
	const long fore = luaL_checkinteger(L, 1);
	const long back = luaL_checkinteger(L, 2);
	runtime->setNoteColourFore(fore);
	runtime->setNoteColourBack(back);
	return 0;
}

static int luaGetNoteColour(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, -1);
		return 1;
	}

	if (runtime->notesInRgb())
	{
		lua_pushnumber(L, -1);
		return 1;
	}
	if (const int noteColour = runtime->noteTextColour();
	    noteColour == WorldRuntime::kSameColour || noteColour < 0)
		lua_pushnumber(L, 0);
	else
		lua_pushnumber(L, noteColour + 1);
	return 1;
}

static int luaSetNoteColour(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
		return 0;
	const int value = static_cast<int>(luaL_checkinteger(L, 1));
	runtime->setNoteTextColour(value);
	return 0;
}

static int luaGetNoteColourBack(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	lua_pushnumber(L, static_cast<lua_Number>(runtime->noteColourBack()));
	return 1;
}

static int luaSetNoteColourBack(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
		return 0;
	const long value = luaL_checkinteger(L, 1);
	runtime->setNoteColourBack(value);
	return 0;
}

static int luaGetNoteColourFore(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	lua_pushnumber(L, static_cast<lua_Number>(runtime->noteColourFore()));
	return 1;
}

static int luaSetNoteColourFore(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
		return 0;
	const long value = luaL_checkinteger(L, 1);
	runtime->setNoteColourFore(value);
	return 0;
}

static int luaNotepadColour(lua_State *L)
{
	const QString    title      = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString    textColour = QString::fromUtf8(luaL_checkstring(L, 2));
	const QString    backColour = QString::fromUtf8(luaL_checkstring(L, 3));
	TextChildWindow *text =
	    findNotepadWindow(title, runtimeFromLuaUpvalue(L), runtimeWorldIdFromLuaUpvalue(L));
	if (!text || !text->editor())
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	const QColor fore = WorldView::parseColor(textColour);
	const QColor back = WorldView::parseColor(backColour);
	if (!fore.isValid() || !back.isValid())
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	QPalette pal = text->editor()->palette();
	pal.setColor(QPalette::Text, fore);
	pal.setColor(QPalette::Base, back);
	text->editor()->setPalette(pal);
	lua_pushnumber(L, 1);
	return 1;
}

static int luaNotepadFont(lua_State *L)
{
	const QString    title    = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString    fontName = QString::fromUtf8(luaL_checkstring(L, 2));
	const int        size     = static_cast<int>(luaL_checkinteger(L, 3));
	const int        style    = static_cast<int>(luaL_checkinteger(L, 4));
	const int        charset  = static_cast<int>(luaL_optinteger(L, 5, 0));

	TextChildWindow *text =
	    findNotepadWindow(title, runtimeFromLuaUpvalue(L), runtimeWorldIdFromLuaUpvalue(L));
	if (!text || !text->editor())
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	QFont font = text->editor()->font();
	if (!fontName.isEmpty())
		qmudApplyMonospaceFallback(font, fontName);
	if (const QString charsetFamily = qmudFamilyForCharset(font.family(), charset);
	    !charsetFamily.isEmpty() && charsetFamily != font.family())
	{
		qmudApplyMonospaceFallback(font, charsetFamily);
	}
	if (size > 0)
		font.setPointSize(size);
	font.setBold(style & 0x01);
	font.setItalic(style & 0x02);
	font.setUnderline(style & 0x04);
	font.setStrikeOut(style & 0x08);
	text->editor()->setFont(font);
	lua_pushnumber(L, 1);
	return 1;
}

static int luaNotepadReadOnly(lua_State *L)
{
	const QString    title    = QString::fromUtf8(luaL_checkstring(L, 1));
	const bool       readOnly = optBool(L, 2, true);
	TextChildWindow *text =
	    findNotepadWindow(title, runtimeFromLuaUpvalue(L), runtimeWorldIdFromLuaUpvalue(L));
	if (!text || !text->editor())
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	text->editor()->setReadOnly(readOnly);
	lua_pushnumber(L, 1);
	return 1;
}

static int luaNotepadSaveMethod(lua_State *L)
{
	const QString title  = QString::fromUtf8(luaL_checkstring(L, 1));
	const int     method = static_cast<int>(luaL_checkinteger(L, 2));
	if (method < 0 || method > 2)
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	TextChildWindow *text =
	    findNotepadWindow(title, runtimeFromLuaUpvalue(L), runtimeWorldIdFromLuaUpvalue(L));
	if (!text)
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	text->setProperty("save_method", method);
	lua_pushnumber(L, 1);
	return 1;
}

static int luaNoteStyle(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
		return 0;
	const auto style = static_cast<unsigned short>(luaL_checkinteger(L, 1));
	runtime->setNoteStyle(style & 0x002F);
	return 0;
}

static int luaOpen(lua_State *L)
{
	const QString  path       = QString::fromUtf8(luaL_checkstring(L, 1));
	AppController *controller = AppController::instance();
	if (!controller)
	{
		lua_pushboolean(L, false);
		return 1;
	}

	if (!path.isEmpty())
	{
		if (const QFileInfo info(path); !info.exists())
		{
			lua_pushboolean(L, false);
			return 1;
		}
	}

	const bool opened = controller->openDocumentFile(path);
	lua_pushboolean(L, opened);
	return 1;
}

static int luaOpenBrowser(lua_State *L)
{
	const QString urlText = QString::fromUtf8(luaL_checkstring(L, 1));
	if (urlText.isEmpty())
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	if (!urlText.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive) &&
	    !urlText.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive) &&
	    !urlText.startsWith(QStringLiteral("mailto:"), Qt::CaseInsensitive))
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	const bool ok = QDesktopServices::openUrl(QUrl(urlText));
	lua_pushnumber(L, ok ? eOK : eCouldNotOpenFile);
	return 1;
}

static int luaPasteCommand(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushstring(L, "");
		return 1;
	}
	WorldView *view = runtime->view();
	if (!view)
	{
		lua_pushstring(L, "");
		return 1;
	}
	const QString text     = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString replaced = view->pasteCommand(text);
	lua_pushstring(L, replaced.toLocal8Bit().constData());
	return 1;
}

static int luaPause(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
		return 0;
	WorldView *view = runtime->view();
	if (!view)
		return 0;
	const bool frozen = optBool(L, 1, true);
	view->setFrozen(frozen);
	return 0;
}

static int luaPickColour(lua_State *L)
{
	const long suggested = luaL_checkinteger(L, 1);
	QColor     initial;
	if (suggested != -1)
	{
		const auto packed = static_cast<QMudColorRef>(suggested);
		initial           = QColor(qmudRed(packed), qmudGreen(packed), qmudBlue(packed));
	}
	QColor chosen = QColorDialog::getColor(initial, resolveMainWindow(), QStringLiteral("Pick Colour"));
	if (!chosen.isValid())
	{
		lua_pushnumber(L, -1);
		return 1;
	}
	lua_pushnumber(L, static_cast<lua_Number>(colorValue(chosen)));
	return 1;
}

static int luaPlaySound(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}
	const int     buffer   = static_cast<int>(luaL_checkinteger(L, 1));
	const QString fileName = QString::fromUtf8(luaL_checkstring(L, 2));
	const bool    loop     = optBool(L, 3, false);
	const double  volume   = luaL_optnumber(L, 4, 0.0);
	const double  pan      = luaL_optnumber(L, 5, 0.0);
	const int     result   = runtime->playSound(buffer, fileName, loop, volume, pan);
	lua_pushnumber(L, result);
	return 1;
}

static int luaPlaySoundMemory(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}
	const int        buffer = static_cast<int>(luaL_checkinteger(L, 1));
	size_t           length = 0;
	const char      *data   = luaL_checklstring(L, 2, &length);
	const bool       loop   = optBool(L, 3, false);
	const double     volume = luaL_optnumber(L, 4, 0.0);
	const double     pan    = luaL_optnumber(L, 5, 0.0);
	const QByteArray payload(data, static_cast<int>(length));
	const int        result = runtime->playSoundMemory(buffer, payload, loop, volume, pan);
	lua_pushnumber(L, result);
	return 1;
}

static int luaStopSound(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}
	const int buffer = static_cast<int>(luaL_checkinteger(L, 1));
	const int result = runtime->stopSound(buffer);
	lua_pushnumber(L, result);
	return 1;
}

static int luaPushCommand(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushstring(L, "");
		return 1;
	}
	WorldView *view = runtime->view();
	if (!view)
	{
		lua_pushstring(L, "");
		return 1;
	}
	const QString command = view->pushCommand();
	lua_pushstring(L, command.toLocal8Bit().constData());
	return 1;
}

static int luaQueue(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}
	if (runtime->connectPhase() != WorldRuntime::eConnectConnectedToMud)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}
	const QString text   = QString::fromUtf8(luaL_checkstring(L, 1));
	const bool    echo   = optBool(L, 2, true);
	const int     result = runtime->sendCommand(text, echo, true, false, false, false);
	lua_pushnumber(L, result);
	return 1;
}

static int luaReadNamesFile(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}
	const QString fileName = QString::fromUtf8(luaL_checkstring(L, 1));
	lua_pushnumber(L, WorldRuntime::readNamesFile(fileName));
	return 1;
}

static int luaRedraw(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
		return 0;
	WorldView *view = runtime->view();
	if (!view)
		return 0;
	view->update();
	view->refreshMiniWindows();
	return 0;
}

static QString trimDirection(const QString &text)
{
	QString trimmed = text;
	trimmed         = trimmed.trimmed();
	return trimmed;
}

static int luaRemoveBacktracks(lua_State *L)
{
	auto          *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime  *runtime = engine ? engine->worldRuntime() : nullptr;
	AppController *app     = AppController::instance();
	if (!runtime)
	{
		lua_pushstring(L, "");
		return 1;
	}
	const QString path = QString::fromUtf8(luaL_checkstring(L, 1));
	QString       walk = runtime->evaluateSpeedwalk(path);
	if (walk.isEmpty() || walk.startsWith(QLatin1Char('*')))
	{
		lua_pushstring(L, walk.toLocal8Bit().constData());
		return 1;
	}

	const QStringList items = walk.split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
	if (items.isEmpty())
	{
		lua_pushstring(L, "");
		return 1;
	}

	QVector<QString> output;
	for (const QString &item : items)
	{
		QString current = trimDirection(item);
		if (current.isEmpty())
		{
			continue;
		}
		if (const QString mapped = app ? app->mapDirectionToLog(current) : QString(); !mapped.isEmpty())
			current = mapped;

		if (output.isEmpty())
		{
			output.push_back(current);
			continue;
		}

		QString top = output.back();
		if (const QString topReverse = app ? app->mapDirectionReverse(top) : QString();
		    !topReverse.isEmpty() && topReverse == current)
			output.pop_back();
		else
			output.push_back(current);
	}

	QString result;
	QString prev;
	int     count = 0;
	for (const QString &entry : output)
	{
		QString dir = trimDirection(entry);
		if (dir.isEmpty())
			continue;
		if (dir.size() > 1)
			dir = QStringLiteral("(%1)").arg(dir);
		if (dir == prev && count < 99)
		{
			count++;
			continue;
		}
		if (!prev.isEmpty())
		{
			if (count > 1)
				result += QString::number(count) + prev;
			else
				result += prev;
			result += QLatin1Char(' ');
		}
		prev  = dir;
		count = 1;
	}

	if (!prev.isEmpty())
	{
		if (count > 1)
			result += QString::number(count) + prev;
		else
			result += prev;
		result += QLatin1Char(' ');
	}

	lua_pushstring(L, result.toLocal8Bit().constData());
	return 1;
}

static int luaGetRemoveMapReverses(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	lua_pushboolean(L, runtime && runtime->removeMapReverses());
	return 1;
}

static int luaSetRemoveMapReverses(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
		return 0;
	const bool enabled = luaL_checknumber(L, 1) != 0;
	runtime->setRemoveMapReverses(enabled);
	return 0;
}

static int luaRepaint(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
		return 0;
	WorldView *view = runtime->view();
	if (!view)
		return 0;
	view->update();
	view->refreshMiniWindows();
	return 0;
}

static int luaReset(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
		return 0;
	runtime->resetMxp();
	return 0;
}

static int luaReplace(lua_State *L)
{
	const QString source      = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString searchFor   = QString::fromUtf8(luaL_checkstring(L, 2));
	const QString replaceWith = QString::fromUtf8(luaL_checkstring(L, 3));
	const bool    multiple    = optBool(L, 4, true);
	const QString result      = qmudReplaceString(source, searchFor, replaceWith, multiple);
	lua_pushstring(L, result.toUtf8().constData());
	return 1;
}

static QString stripAnsiText(const QString &message)
{
	QString result;
	result.reserve(message.size());
	int i = 0;
	while (i < message.size())
	{
		const QChar c = message.at(i);
		if (c.unicode() == 0x1b)
		{
			++i;
			if (i < message.size() && message.at(i) == QLatin1Char('['))
			{
				++i;
				while (i < message.size() && message.at(i) != QLatin1Char('m'))
					++i;
				if (i < message.size())
					++i;
			}
			else if (i < message.size())
			{
				++i;
			}
			continue;
		}
		result.append(c);
		++i;
	}
	return result;
}

static int luaStripANSI(lua_State *L)
{
	const QString message  = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString stripped = stripAnsiText(message);
	lua_pushstring(L, stripped.toLocal8Bit().constData());
	return 1;
}

static int luaTrim(lua_State *L)
{
	const QString source  = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString trimmed = source.trimmed();
	lua_pushstring(L, trimmed.toLocal8Bit().constData());
	return 1;
}

static int luaTranslateGerman(lua_State *L)
{
	const QString source = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString result = qmudFixUpGerman(source);
	lua_pushstring(L, result.toUtf8().constData());
	return 1;
}

static int luaTranslateDebug(lua_State *L)
{
	AppController *controller = AppController::instance();
	if (!controller)
	{
		lua_pushnumber(L, 1);
		return 1;
	}
	lua_State *translator = controller->translatorLuaState();
	if (!translator)
	{
		lua_pushnumber(L, 1);
		return 1;
	}

	lua_settop(translator, 0);
	lua_getglobal(translator, "Debug");
	if (!lua_isfunction(translator, -1))
	{
		lua_pop(translator, 1);
		lua_pushnumber(L, 2);
		return 1;
	}

	const QString    message  = QString::fromUtf8(luaL_optstring(L, 1, ""));
	const QByteArray msgBytes = message.toLocal8Bit();
	lua_pushlstring(translator, msgBytes.constData(), msgBytes.size());
	if (lua_pcall(translator, 1, 0, 0))
	{
		lua_pop(translator, 1);
		lua_pushnumber(L, 3);
		return 1;
	}

	lua_pushnumber(L, 0);
	return 1;
}

static int luaResetIP(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
		return 0;
	runtime->resetIpCache();
	return 0;
}

static int luaReverseSpeedwalk(lua_State *L)
{
	auto          *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime  *runtime = engine ? engine->worldRuntime() : nullptr;
	AppController *app     = AppController::instance();
	if (!runtime)
	{
		lua_pushstring(L, "");
		return 1;
	}
	const QString input = QString::fromUtf8(luaL_checkstring(L, 1));
	QString       walk  = runtime->evaluateSpeedwalk(input);
	if (walk.isEmpty() || walk.startsWith(QLatin1Char('*')))
	{
		lua_pushstring(L, walk.toLocal8Bit().constData());
		return 1;
	}

	const QStringList items = walk.split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
	if (items.isEmpty())
	{
		lua_pushstring(L, "");
		return 1;
	}

	QString result;
	QString lastDir;
	int     count = 0;
	bool    first = true;

	for (const QString &item : items)
	{
		QString current = trimDirection(item);
		if (current.isEmpty())
		{
			continue;
		}
		if (const QString reverse = app ? app->mapDirectionReverse(current) : QString(); !reverse.isEmpty())
			current = reverse;

		if (current == lastDir && count < 99)
		{
			count++;
			continue;
		}

		if (!first)
		{
			if (lastDir.size() > 1)
				lastDir = QStringLiteral("(%1)").arg(lastDir);
			if (count > 1)
				result += QString::number(count) + lastDir + QLatin1Char(' ');
			else
				result += lastDir + QLatin1Char(' ');
		}

		first   = false;
		lastDir = current;
		count   = 1;
	}

	if (!lastDir.isEmpty())
	{
		if (lastDir.size() > 1)
			lastDir = QStringLiteral("(%1)").arg(lastDir);
		if (count > 1)
			result += QString::number(count) + lastDir + QLatin1Char(' ');
		else
			result += lastDir + QLatin1Char(' ');
	}

	lua_pushstring(L, result.toLocal8Bit().constData());
	return 1;
}

static int luaRGBColourToName(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushstring(L, "");
		return 1;
	}
	const long    colour = luaL_checkinteger(L, 1);
	const QString result = WorldRuntime::rgbColourToName(colour);
	lua_pushstring(L, result.toUtf8().constData());
	return 1;
}

static int luaSave(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushboolean(L, false);
		return 1;
	}
	const bool  saveAs   = optBool(L, 2, false);
	const char *nameArg  = lua_gettop(L) >= 1 && !lua_isnil(L, 1) ? lua_tostring(L, 1) : "";
	QString     fileName = QString::fromUtf8(nameArg ? nameArg : "");
	if (saveAs)
	{
		if (fileName.isEmpty())
		{
			lua_pushboolean(L, false);
			return 1;
		}
	}
	else if (fileName.isEmpty())
	{
		fileName = runtime->worldFilePath();
	}

	if (fileName.isEmpty())
	{
		lua_pushboolean(L, false);
		return 1;
	}

	QString    error;
	const bool ok = runtime->saveWorldFile(fileName, &error);
	lua_pushboolean(L, ok ? 1 : 0);
	return 1;
}

static int luaSaveNotepad(lua_State *L)
{
	const QString    title    = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString    fileName = QString::fromUtf8(luaL_checkstring(L, 2));
	const bool       replace  = optBool(L, 3, false);
	TextChildWindow *text =
	    findNotepadWindow(title, runtimeFromLuaUpvalue(L), runtimeWorldIdFromLuaUpvalue(L));
	if (!text || !text->editor())
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	if (fileName.isEmpty())
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	if (!replace && QFileInfo::exists(fileName))
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	QString    error;
	const bool ok = text->saveToFile(fileName, &error);
	lua_pushnumber(L, ok ? 1 : 0);
	return 1;
}

static int luaSelectCommand(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
		return 0;
	WorldView *view = runtime->view();
	if (!view)
		return 0;
	view->focusInput();
	if (QPlainTextEdit *input = view->inputEditor())
		input->selectAll();
	return 0;
}

static int luaSendPkt(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}
	if (runtime->connectPhase() != WorldRuntime::eConnectConnectedToMud)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}
	size_t      packetLength = 0;
	const char *packetData   = luaL_checklstring(L, 1, &packetLength);
	runtime->sendToWorld(QByteArray(packetData, static_cast<int>(packetLength)));
	lua_pushnumber(L, eOK);
	return 1;
}

static int luaSetBackgroundColour(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	const long oldColour = runtime->backgroundColour();
	const long newColour = luaL_checkinteger(L, 1);
	runtime->setBackgroundColour(newColour);
	lua_pushnumber(L, static_cast<lua_Number>(oldColour));
	return 1;
}

static int luaSetBackgroundImage(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	const QString fileName = QString::fromUtf8(luaL_checkstring(L, 1));
	const int     mode     = static_cast<int>(luaL_checkinteger(L, 2));
	lua_pushnumber(L, runtime->setBackgroundImage(fileName, mode));
	return 1;
}

static int luaTransparency(lua_State *L)
{
	QWidget *window = resolveMainWindow();
	if (!window)
	{
		lua_pushboolean(L, 0);
		return 1;
	}
	const long key     = luaL_checkinteger(L, 1);
	const int  amount  = static_cast<int>(luaL_checkinteger(L, 2));
	const int  clamped = qBound(0, amount, 255);

	// Use the same Qt window-mask/opacity path on all platforms.
	if (key != -1)
	{
		const auto    packed = static_cast<QMudColorRef>(key);
		const QColor  keyColor(qmudRed(packed), qmudGreen(packed), qmudBlue(packed));
		const QPixmap snapshot = window->grab();
		const QImage  image    = snapshot.toImage();
		QBitmap       mask = QBitmap::fromImage(image.createMaskFromColor(keyColor.rgb(), Qt::MaskOutColor));
		window->setMask(mask);
		window->setAttribute(Qt::WA_TranslucentBackground, true);
	}
	else
	{
		window->clearMask();
		window->setAttribute(Qt::WA_TranslucentBackground, false);
	}
	window->setWindowOpacity(static_cast<qreal>(clamped) / 255.0);
	lua_pushboolean(L, 1);
	return 1;
}

static int luaTextRectangle(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}

	const long left              = luaL_checkinteger(L, 1);
	const long top               = luaL_checkinteger(L, 2);
	const long right             = luaL_checkinteger(L, 3);
	const long bottom            = luaL_checkinteger(L, 4);
	const long borderOffset      = luaL_checkinteger(L, 5);
	const long borderColour      = luaL_checkinteger(L, 6);
	const long borderWidth       = luaL_checkinteger(L, 7);
	const long outsideFillColour = luaL_checkinteger(L, 8);
	const long outsideFillStyle  = luaL_checkinteger(L, 9);

	if (qmudValidateBrushStyle(outsideFillStyle, borderColour, outsideFillColour) != eOK)
	{
		lua_pushnumber(L, eBrushStyleNotValid);
		return 1;
	}

	WorldRuntime::TextRectangleSettings settings;
	settings.left              = static_cast<int>(left);
	settings.top               = static_cast<int>(top);
	settings.right             = static_cast<int>(right);
	settings.bottom            = static_cast<int>(bottom);
	settings.borderOffset      = static_cast<int>(borderOffset);
	settings.borderColour      = static_cast<int>(borderColour);
	settings.borderWidth       = static_cast<int>(borderWidth);
	settings.outsideFillColour = static_cast<int>(outsideFillColour);
	settings.outsideFillStyle  = static_cast<int>(outsideFillStyle);
	runtime->setTextRectangle(settings);

	lua_pushnumber(L, eOK);
	return 1;
}

static int luaSetEntity(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
		return 0;
	const QString name     = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString contents = QString::fromUtf8(luaL_checkstring(L, 2));
	runtime->setEntityValue(name, contents);
	return 0;
}

static int luaSetForegroundImage(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	const QString fileName = QString::fromUtf8(luaL_checkstring(L, 1));
	const int     mode     = static_cast<int>(luaL_checkinteger(L, 2));
	lua_pushnumber(L, runtime->setForegroundImage(fileName, mode));
	return 1;
}

static int luaSetFrameBackgroundColour(lua_State *L)
{
	MainWindow *frame = resolveMainWindow();
	if (!frame)
		return 0;
	const long colour = luaL_checkinteger(L, 1);
	frame->setFrameBackgroundColour(colour);
	return 0;
}

static int luaSetToolBarPosition(lua_State *L)
{
	struct ToolBarDockState
	{
			Qt::ToolBarArea area{Qt::TopToolBarArea};
			QPoint          pos;
			bool            floating{false};
	};
	static QHash<QToolBar *, ToolBarDockState> s_toolbarStates;
	static QSet<QToolBar *>                    s_toolbarStateHooks;
	MainWindow                                *frame = resolveMainWindow();
	if (!frame)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	const int    which   = static_cast<int>(luaL_checkinteger(L, 1));
	const bool   doFloat = optBool(L, 2, false);
	const int    side    = static_cast<int>(luaL_checkinteger(L, 3));
	const int    top     = static_cast<int>(luaL_checkinteger(L, 4));
	const int    left    = static_cast<int>(luaL_checkinteger(L, 5));

	QToolBar    *toolBar = nullptr;
	QDockWidget *dock    = nullptr;
	switch (which)
	{
	case 1:
		toolBar = frame->mainToolbar();
		break;
	case 2:
		toolBar = frame->worldToolbar();
		break;
	case 3:
		toolBar = frame->activityToolbar();
		break;
	case 4:
		dock = frame->infoDock();
		break;
	default:
		lua_pushnumber(L, eBadParameter);
		return 1;
	}

	if (toolBar && !s_toolbarStateHooks.contains(toolBar))
	{
		s_toolbarStateHooks.insert(toolBar);
		auto *toolbarStates     = &s_toolbarStates;
		auto *toolbarStateHooks = &s_toolbarStateHooks;
		QObject::connect(toolBar, &QObject::destroyed, toolBar,
		                 [toolbarStates, toolbarStateHooks](QObject *destroyed)
		                 {
			                 auto *removed = qobject_cast<QToolBar *>(destroyed);
			                 if (!removed)
				                 return;
			                 toolbarStates->remove(removed);
			                 toolbarStateHooks->remove(removed);
		                 });
	}

	if (dock)
	{
		if (doFloat)
		{
			if (side != 1 && side != 3)
			{
				lua_pushnumber(L, eBadParameter);
				return 1;
			}
			dock->setFloating(true);
			const QPoint pos = frame->mapToGlobal(QPoint(left, top));
			dock->move(pos);
		}
		else
		{
			Qt::DockWidgetArea area;
			switch (side)
			{
			case 1:
			case 0:
				area = Qt::TopDockWidgetArea;
				break;
			case 2:
				area = Qt::BottomDockWidgetArea;
				break;
			case 3:
				area = Qt::LeftDockWidgetArea;
				break;
			case 4:
				area = Qt::RightDockWidgetArea;
				break;
			default:
				lua_pushnumber(L, eBadParameter);
				return 1;
			}
			frame->addDockWidget(area, dock);
		}
		lua_pushnumber(L, eOK);
		return 1;
	}

	if (!toolBar)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}

	Qt::ToolBarArea area = Qt::TopToolBarArea;
	switch (side)
	{
	case 1:
	case 0:
		area = Qt::TopToolBarArea;
		break;
	case 2:
		area = Qt::BottomToolBarArea;
		break;
	case 3:
		area = Qt::LeftToolBarArea;
		break;
	case 4:
		area = Qt::RightToolBarArea;
		break;
	default:
		lua_pushnumber(L, eBadParameter);
		return 1;
	}

	if (doFloat)
	{
		if (side != 1 && side != 3)
		{
			lua_pushnumber(L, eBadParameter);
			return 1;
		}
		frame->removeToolBar(toolBar);
		toolBar->setFloatable(true);
		toolBar->setMovable(true);
		toolBar->setParent(frame);
		toolBar->setWindowFlags(Qt::Tool);
		toolBar->setOrientation(side == 3 ? Qt::Vertical : Qt::Horizontal);
		toolBar->show();
		const QPoint pos = frame->mapToGlobal(QPoint(left, top));
		toolBar->move(pos);
		ToolBarDockState state;
		state.area     = area;
		state.pos      = QPoint(left, top);
		state.floating = true;
		s_toolbarStates.insert(toolBar, state);
	}
	else
	{
		toolBar->setWindowFlags(Qt::Widget);
		frame->addToolBar(area, toolBar);
		toolBar->setOrientation(area == Qt::LeftToolBarArea || area == Qt::RightToolBarArea ? Qt::Vertical
		                                                                                    : Qt::Horizontal);

		ToolBarDockState state;
		state.area     = area;
		state.pos      = QPoint(left, top);
		state.floating = false;
		s_toolbarStates.insert(toolBar, state);

		QVector<QToolBar *> areaToolbars;
		for (const QVector known = {frame->mainToolbar(), frame->worldToolbar(), frame->activityToolbar()};
		     QToolBar *candidate : known)
		{
			if (!candidate)
				continue;
			const auto it = s_toolbarStates.constFind(candidate);
			if (it == s_toolbarStates.constEnd())
				continue;
			if (it->floating || it->area != area)
				continue;
			areaToolbars.append(candidate);
		}

		if (!areaToolbars.isEmpty())
		{
			std::ranges::sort(
			    areaToolbars,
			    [&](QToolBar *a, QToolBar *b)
			    {
				    const ToolBarDockState &sa = s_toolbarStates.value(a);
				    const ToolBarDockState &sb = s_toolbarStates.value(b);
				    const int               primaryA =
                        area == Qt::LeftToolBarArea || area == Qt::RightToolBarArea ? sa.pos.y() : sa.pos.x();
				    const int primaryB =
				        area == Qt::LeftToolBarArea || area == Qt::RightToolBarArea ? sb.pos.y() : sb.pos.x();
				    return primaryA < primaryB;
			    });
			for (QToolBar *candidate : areaToolbars)
				frame->removeToolBar(candidate);
			for (QToolBar *candidate : areaToolbars)
				frame->addToolBar(area, candidate);
		}
	}

	lua_pushnumber(L, eOK);
	return 1;
}

static int luaSetChatOption(lua_State *L)
{
	WorldRuntime *runtime = runtimeFromLua(L);
	if (!runtime)
	{
		lua_pushnumber(L, eChatIDNotFound);
		return 1;
	}
	const long    id     = luaL_checkinteger(L, 1);
	const QString option = QString::fromUtf8(luaL_checkstring(L, 2));
	const QString value  = QString::fromUtf8(luaL_checkstring(L, 3));
	lua_pushnumber(L, runtime->chatSetOption(id, option, value));
	return 1;
}

static int luaSetChanged(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
		return 0;
	const bool changed = optBool(L, 1, true);
	runtime->setWorldFileModified(changed);
	return 0;
}

static int luaSetClipboard(lua_State *L)
{
	const QString text = concatLuaArgs(L, 1);
	QGuiApplication::clipboard()->setText(text);
	return 0;
}

static int luaSetCommand(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	WorldView *view = runtime->view();
	if (!view)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	if (!view->inputText().isEmpty())
	{
		lua_pushnumber(L, eCommandNotEmpty);
		return 1;
	}
	const QString message = QString::fromUtf8(luaL_checkstring(L, 1));
	view->setInputText(message, true);
	lua_pushnumber(L, eOK);
	return 1;
}

static int luaSetCommandSelection(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	WorldView *view = runtime->view();
	if (!view)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	const int first = static_cast<int>(luaL_checkinteger(L, 1));
	const int last  = static_cast<int>(luaL_checkinteger(L, 2));
	lua_pushnumber(L, view->setCommandSelection(first, last));
	return 1;
}

static int luaSetCommandWindowHeight(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	WorldView *view = runtime->view();
	if (!view)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	const int height = static_cast<int>(luaL_checkinteger(L, 1));
	lua_pushnumber(L, view->setCommandWindowHeight(height));
	return 1;
}

static int luaSetUnseenLines(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
		return 0;
	const int count = static_cast<int>(luaL_checkinteger(L, 1));
	runtime->setNewLines(count);
	return 0;
}

static int luaSetWorldWindowStatus(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
		return 0;
	MainWindowHost *host = resolveMainWindowHostForRuntime(runtime);
	if (!host)
		return 0;
	WorldChildWindow *child = host->findWorldChildWindow(runtime);
	if (!child)
		return 0;
	switch (static_cast<int>(luaL_checkinteger(L, 1)))
	{
	case 1:
		child->showMaximized();
		break;
	case 2:
		child->showMinimized();
		break;
	case 4:
	case 3:
		child->showNormal();
		break;
	default:
		break;
	}
	return 0;
}

static int luaShiftTabCompleteItem(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	const QString item = QString::fromUtf8(luaL_checkstring(L, 1));
	lua_pushnumber(L, runtime->shiftTabCompleteItem(item));
	return 1;
}

static int luaShowInfoBar(lua_State *L)
{
	const bool visible = optBool(L, 1, true);
	if (MainWindow *frame = resolveMainWindow())
		frame->setInfoBarVisible(visible);
	return 0;
}

static int luaSimulate(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
		return 0;
	const QString text = concatLuaArgs(L, 1);
	runtime->setDoingSimulate(true);
	runtime->receiveRawData(text.toUtf8());
	runtime->setDoingSimulate(false);
	return 0;
}

static int luaSound(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}
	const QString fileName = QString::fromUtf8(luaL_checkstring(L, 1));
	if (fileName.isEmpty())
	{
		lua_pushnumber(L, eNoNameSpecified);
		return 1;
	}
	const int result = runtime->playSound(0, fileName, false, 0.0, 0.0);
	lua_pushnumber(L, result);
	return 1;
}

static int luaGetSpeedWalkDelay(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	const QString value = runtime->worldAttributes().value(QStringLiteral("speed_walk_delay"));
	lua_pushnumber(L, value.toInt());
	return 1;
}

static int luaSetSpeedWalkDelay(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
		return 0;
	const int delay = static_cast<int>(luaL_checkinteger(L, 1));
	runtime->setWorldAttribute(QStringLiteral("speed_walk_delay"), QString::number(delay));
	return 0;
}

static int luaSpellCheck(lua_State *L)
{
	const char    *text       = luaL_checkstring(L, 1);
	AppController *controller = AppController::instance();
	if (!controller || !controller->ensureSpellCheckerLoaded())
	{
		lua_pushnil(L);
		return 1;
	}
	lua_State *spell = controller->spellCheckerLuaState();
	if (!spell)
	{
		lua_pushnil(L);
		return 1;
	}

	lua_settop(spell, 0);
	lua_getglobal(spell, "spellcheck_string");
	if (!lua_isfunction(spell, -1))
	{
		lua_settop(spell, 0);
		lua_pushnil(L);
		return 1;
	}
	lua_pushstring(spell, text);
	if (const int error = QMudLuaSupport::callLuaWithTraceback(spell, 1, 1); error)
	{
		QMudLuaSupport::luaError(spell, "Run-time error", "spellcheck_string", "world.SpellCheck");
		lua_settop(spell, 0);
		lua_pushnil(L);
		return 1;
	}

	if (lua_isnumber(spell, -1))
	{
		const double result = lua_tonumber(spell, -1);
		lua_settop(spell, 0);
		lua_pushnumber(L, result);
		return 1;
	}

	if (!lua_istable(spell, -1))
	{
		lua_settop(spell, 0);
		lua_pushnil(L);
		return 1;
	}

	QStringList errors;
	for (int i = 1;; ++i)
	{
		lua_rawgeti(spell, -1, i);
		if (lua_isnil(spell, -1))
		{
			lua_pop(spell, 1);
			break;
		}
		if (lua_isstring(spell, -1))
		{
			errors.append(QString::fromUtf8(lua_tostring(spell, -1)));
		}
		lua_pop(spell, 1);
	}
	lua_settop(spell, 0);
	if (errors.isEmpty())
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	errors.removeDuplicates();
	errors.sort();
	pushStringList(L, errors);
	return 1;
}

static int luaSpellCheckCommand(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime || !runtime->view())
	{
		lua_pushnumber(L, -1);
		return 1;
	}

	AppController *controller = AppController::instance();
	if (!controller || !controller->ensureSpellCheckerLoaded())
	{
		lua_pushnumber(L, -1);
		return 1;
	}
	lua_State *spell = controller->spellCheckerLuaState();
	if (!spell)
	{
		lua_pushnumber(L, -1);
		return 1;
	}

	QPlainTextEdit *input = runtime->view()->inputEditor();
	if (!input)
	{
		lua_pushnumber(L, -1);
		return 1;
	}

	QTextCursor cursor    = input->textCursor();
	const int   origStart = cursor.selectionStart();
	const int   origEnd   = cursor.selectionEnd();

	int         startCol = static_cast<int>(luaL_optinteger(L, 1, -1));
	int         endCol   = static_cast<int>(luaL_optinteger(L, 2, -1));
	if (startCol > 0)
		startCol -= 1;
	if (endCol > startCol && startCol >= 0 && endCol >= 0)
	{
		QTextCursor selection = input->textCursor();
		selection.setPosition(startCol);
		selection.setPosition(endCol, QTextCursor::KeepAnchor);
		input->setTextCursor(selection);
	}

	QTextCursor activeCursor = input->textCursor();
	QString     selected     = activeCursor.selectedText();
	bool        all          = false;
	if (selected.isEmpty())
	{
		all      = true;
		selected = input->toPlainText();
	}

	lua_settop(spell, 0);
	lua_getglobal(spell, "spellcheck");
	if (!lua_isfunction(spell, -1))
	{
		lua_settop(spell, 0);
		lua_pushnumber(L, -1);
		return 1;
	}
	const QByteArray textBytes = selected.toUtf8();
	lua_pushlstring(spell, textBytes.constData(), textBytes.size());
	lua_pushboolean(spell, all);
	if (const int error = QMudLuaSupport::callLuaWithTraceback(spell, 2, 1); error)
	{
		QMudLuaSupport::luaError(spell, "Run-time error", "spellcheck", "Command-line spell-check");
		controller->closeSpellChecker();
		lua_settop(spell, 0);
		lua_pushnumber(L, -1);
		return 1;
	}

	int result = 0;
	if (lua_isstring(spell, -1))
	{
		const QString replacement = QString::fromUtf8(lua_tostring(spell, -1));
		if (all)
			input->selectAll();
		QTextCursor replaceCursor = input->textCursor();
		replaceCursor.insertText(replacement);
		result = 1;
	}
	lua_settop(spell, 0);

	QTextCursor restore = input->textCursor();
	restore.setPosition(origStart);
	restore.setPosition(origEnd, QTextCursor::KeepAnchor);
	input->setTextCursor(restore);

	lua_pushnumber(L, result);
	return 1;
}

static int luaSpellCheckDlg(lua_State *L)
{
	const char    *text       = luaL_checkstring(L, 1);
	AppController *controller = AppController::instance();
	if (!controller || !controller->ensureSpellCheckerLoaded())
	{
		lua_pushnil(L);
		return 1;
	}
	lua_State *spell = controller->spellCheckerLuaState();
	if (!spell)
	{
		lua_pushnil(L);
		return 1;
	}

	lua_settop(spell, 0);
	lua_getglobal(spell, "spellcheck_string");
	if (!lua_isfunction(spell, -1))
	{
		lua_settop(spell, 0);
		lua_pushnil(L);
		return 1;
	}
	lua_pushstring(spell, text);
	if (const int error = QMudLuaSupport::callLuaWithTraceback(spell, 1, 1); error)
	{
		QMudLuaSupport::luaError(spell, "Run-time error", "spellcheck_string", "world.SpellCheckDlg");
		lua_settop(spell, 0);
		lua_pushnil(L);
		return 1;
	}

	if (lua_isstring(spell, -1))
	{
		const char *out = lua_tostring(spell, -1);
		lua_settop(spell, 0);
		lua_pushstring(L, out ? out : "");
		return 1;
	}
	lua_settop(spell, 0);
	lua_pushnil(L);
	return 1;
}

static int luaSetInputFont(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
		return 0;
	const QString fontName  = QString::fromUtf8(luaL_checkstring(L, 1));
	const int     pointSize = static_cast<int>(luaL_checkinteger(L, 2));
	const int     weight    = static_cast<int>(luaL_checkinteger(L, 3));
	const int     italic    = static_cast<int>(luaL_optinteger(L, 4, 0));
	runtime->setWorldAttribute(QStringLiteral("input_font_name"), fontName);
	runtime->setWorldAttribute(QStringLiteral("input_font_height"), QString::number(pointSize));
	runtime->setWorldAttribute(QStringLiteral("input_font_weight"), QString::number(weight));
	runtime->setWorldAttribute(QStringLiteral("input_font_italic"), QString::number(italic));
	if (WorldView *view = runtime->view())
		view->applyRuntimeSettings();
	return 0;
}

static int luaSetMainTitle(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
		return 0;
	const QString title = concatLuaArgs(L, 1);
	runtime->setMainTitleOverride(title);
	return 0;
}

static int luaSetNotes(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
		return 0;
	const QString notes = QString::fromUtf8(luaL_checkstring(L, 1));
	runtime->setWorldMultilineAttribute(QStringLiteral("notes"), notes);
	return 0;
}

static int luaSetOutputFont(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
		return 0;
	const QString fontName  = QString::fromUtf8(luaL_checkstring(L, 1));
	const int     pointSize = static_cast<int>(luaL_checkinteger(L, 2));
	runtime->setWorldAttribute(QStringLiteral("output_font_name"), fontName);
	runtime->setWorldAttribute(QStringLiteral("output_font_height"), QString::number(pointSize));
	if (WorldView *view = runtime->view())
		view->applyRuntimeSettings();
	return 0;
}

static int luaSetScroll(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	WorldView *view = runtime->view();
	if (!view)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	const int  position = static_cast<int>(luaL_checkinteger(L, 1));
	const bool visible  = optBool(L, 2, true);
	lua_pushnumber(L, view->setOutputScroll(position, visible));
	return 1;
}

static int luaSetSelection(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
		return 0;
	WorldView *view = runtime->view();
	if (!view)
		return 0;
	const int startLine   = static_cast<int>(luaL_checkinteger(L, 1));
	const int endLine     = static_cast<int>(luaL_checkinteger(L, 2));
	const int startColumn = static_cast<int>(luaL_checkinteger(L, 3));
	const int endColumn   = static_cast<int>(luaL_checkinteger(L, 4));
	view->setOutputSelection(startLine, endLine, startColumn, endColumn);
	return 0;
}

static int luaSetTitle(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
		return 0;
	const QString title = concatLuaArgs(L, 1);
	runtime->setWindowTitleOverride(title);
	return 0;
}

static int luaSetCursor(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	WorldView *view = runtime->view();
	if (!view)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	const int cursor = static_cast<int>(luaL_checkinteger(L, 1));
	view->setWorldCursor(cursor);
	lua_pushnumber(L, eOK);
	return 1;
}

static int luaSetCustomColourName(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	const int     index = static_cast<int>(luaL_checkinteger(L, 1));
	const QString name  = QString::fromUtf8(luaL_checkstring(L, 2));
	lua_pushnumber(L, runtime->setCustomColourName(index, name));
	return 1;
}

static int luaBase64Decode(lua_State *L)
{
	const auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr; !runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const QString input      = QString::fromUtf8(luaL_checkstring(L, 1));
	QByteArray    normalized = input.toUtf8();
	normalized.replace('\r', "");
	normalized.replace('\n', "");
	const auto [decoded, decodingStatus] =
	    QByteArray::fromBase64Encoding(normalized, QByteArray::AbortOnBase64DecodingErrors);
	if (decodingStatus != QByteArray::Base64DecodingStatus::Ok)
	{
		lua_pushnil(L);
		return 1;
	}
	lua_pushstring(L, QString::fromUtf8(decoded).toUtf8().constData());
	return 1;
}

static int luaBase64Encode(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushstring(L, "");
		return 1;
	}
	const QString input     = QString::fromUtf8(luaL_checkstring(L, 1));
	const bool    multiLine = optBool(L, 2, false);
	const QString encoded   = WorldRuntime::base64Encode(input, multiLine);
	lua_pushstring(L, encoded.toUtf8().constData());
	return 1;
}

static int luaAppendToNotepad(lua_State *L)
{
	const char   *title    = luaL_checkstring(L, 1);
	const QString contents = concatLuaArgs(L, 2);
	auto         *engine   = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime  = engine ? engine->worldRuntime() : nullptr;
	MainWindow   *mw       = resolveMainWindow();
	bool          ok       = false;
	if (mw)
		ok = mw->appendToNotepad(QString::fromUtf8(title), contents, false, runtime);
	lua_pushboolean(L, ok);
	return 1;
}

static int luaReplaceNotepad(lua_State *L)
{
	const char   *title    = luaL_checkstring(L, 1);
	const QString contents = concatLuaArgs(L, 2);
	auto         *engine   = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime  = engine ? engine->worldRuntime() : nullptr;
	MainWindow   *mw       = resolveMainWindow();
	bool          ok       = false;
	if (mw)
		ok = mw->appendToNotepad(QString::fromUtf8(title), contents, true, runtime);
	lua_pushboolean(L, ok);
	return 0;
}

static int luaSendToNotepad(lua_State *L)
{
	const char   *title    = luaL_checkstring(L, 1);
	const QString contents = concatLuaArgs(L, 2);
	auto         *engine   = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime  = engine ? engine->worldRuntime() : nullptr;
	MainWindow   *mw       = resolveMainWindow();
	bool          ok       = false;
	if (mw)
		ok = mw->sendToNotepad(QString::fromUtf8(title), contents, runtime);
	lua_pushboolean(L, ok);
	return 1;
}

static int luaGetConnectDuration(lua_State *L)
{
	const auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime || !runtime->isConnected() || !runtime->connectTime().isValid())
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	const qint64 seconds = runtime->connectTime().secsTo(QDateTime::currentDateTime());
	lua_pushnumber(L, static_cast<lua_Number>(seconds));
	return 1;
}

static int luaGetConnectTime(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime || !runtime->connectTime().isValid())
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	lua_pushnumber(L, toOleDate(runtime->connectTime()));
	return 1;
}

static int luaGetStatusTime(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime || !runtime->statusTime().isValid())
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	lua_pushnumber(L, toOleDate(runtime->statusTime()));
	return 1;
}

static int luaGetInfo(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnil(L);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	MainWindowHost               *host  = resolveMainWindowHostForRuntime(runtime);
	auto                         *frame = dynamic_cast<MainWindow *>(host);

	const int                     infoType = static_cast<int>(luaL_checkinteger(L, 1));
	const QMap<QString, QString> &attrs    = runtime->worldAttributes();
	const QMap<QString, QString> &multi    = runtime->worldMultilineAttributes();
	auto                          getAttr  = [&](const QString &key) -> QString { return attrs.value(key); };
	auto                          getMulti = [&](const QString &key) -> QString
	{
		const QString multiValue = multi.value(key);
		return multiValue.isEmpty() ? attrs.value(key) : multiValue;
	};

	QDateTime value;
	switch (infoType)
	{
	// strings
	case 1:
		lua_pushstring(L, getAttr(QStringLiteral("site")).toLocal8Bit().constData());
		return 1;
	case 2:
		lua_pushstring(L, getAttr(QStringLiteral("name")).toLocal8Bit().constData());
		return 1;
	case 3:
		lua_pushstring(L, getAttr(QStringLiteral("player")).toLocal8Bit().constData());
		return 1;
	case 4:
		lua_pushstring(L, getMulti(QStringLiteral("send_to_world_file_preamble")).toLocal8Bit().constData());
		return 1;
	case 5:
		lua_pushstring(L, getMulti(QStringLiteral("send_to_world_file_postamble")).toLocal8Bit().constData());
		return 1;
	case 6:
		lua_pushstring(L, getAttr(QStringLiteral("send_to_world_line_preamble")).toLocal8Bit().constData());
		return 1;
	case 7:
		lua_pushstring(L, getAttr(QStringLiteral("send_to_world_line_postamble")).toLocal8Bit().constData());
		return 1;
	case 8:
		lua_pushstring(L, getAttr(QStringLiteral("notes")).toLocal8Bit().constData());
		return 1;
	case 9:
		lua_pushstring(L, getAttr(QStringLiteral("new_activity_sound")).toLocal8Bit().constData());
		return 1;
	case 10:
		lua_pushstring(L, getAttr(QStringLiteral("script_editor")).toLocal8Bit().constData());
		return 1;
	case 11:
		lua_pushstring(L, getMulti(QStringLiteral("log_file_preamble")).toLocal8Bit().constData());
		return 1;
	case 12:
		lua_pushstring(L, getMulti(QStringLiteral("log_file_postamble")).toLocal8Bit().constData());
		return 1;
	case 13:
		lua_pushstring(L, getAttr(QStringLiteral("log_line_preamble_input")).toLocal8Bit().constData());
		return 1;
	case 14:
		lua_pushstring(L, getAttr(QStringLiteral("log_line_preamble_notes")).toLocal8Bit().constData());
		return 1;
	case 15:
		lua_pushstring(L, getAttr(QStringLiteral("log_line_preamble_output")).toLocal8Bit().constData());
		return 1;
	case 16:
		lua_pushstring(L, getAttr(QStringLiteral("log_line_postamble_input")).toLocal8Bit().constData());
		return 1;
	case 17:
		lua_pushstring(L, getAttr(QStringLiteral("log_line_postamble_notes")).toLocal8Bit().constData());
		return 1;
	case 18:
		lua_pushstring(L, getAttr(QStringLiteral("log_line_postamble_output")).toLocal8Bit().constData());
		return 1;
	case 19:
		lua_pushstring(L, getAttr(QStringLiteral("speed_walk_filler")).toLocal8Bit().constData());
		return 1;
	case 20:
		lua_pushstring(L, getAttr(QStringLiteral("output_font_name")).toLocal8Bit().constData());
		return 1;
	case 21:
		lua_pushstring(L, getAttr(QStringLiteral("speed_walk_prefix")).toLocal8Bit().constData());
		return 1;
	case 22:
		lua_pushstring(L, getMulti(QStringLiteral("connect_text")).toLocal8Bit().constData());
		return 1;
	case 23:
		lua_pushstring(L, getAttr(QStringLiteral("input_font_name")).toLocal8Bit().constData());
		return 1;
	case 24:
		lua_pushstring(L, getMulti(QStringLiteral("paste_postamble")).toLocal8Bit().constData());
		return 1;
	case 25:
		lua_pushstring(L, getMulti(QStringLiteral("paste_preamble")).toLocal8Bit().constData());
		return 1;
	case 26:
		lua_pushstring(L, getAttr(QStringLiteral("paste_line_postamble")).toLocal8Bit().constData());
		return 1;
	case 27:
		lua_pushstring(L, getAttr(QStringLiteral("paste_line_preamble")).toLocal8Bit().constData());
		return 1;
	case 28:
		lua_pushstring(L, getAttr(QStringLiteral("script_language")).toLocal8Bit().constData());
		return 1;
	case 29:
		lua_pushstring(L, getAttr(QStringLiteral("on_world_open")).toLocal8Bit().constData());
		return 1;
	case 30:
		lua_pushstring(L, getAttr(QStringLiteral("on_world_close")).toLocal8Bit().constData());
		return 1;
	case 31:
		lua_pushstring(L, getAttr(QStringLiteral("on_world_connect")).toLocal8Bit().constData());
		return 1;
	case 32:
		lua_pushstring(L, getAttr(QStringLiteral("on_world_disconnect")).toLocal8Bit().constData());
		return 1;
	case 33:
		lua_pushstring(L, getAttr(QStringLiteral("on_world_get_focus")).toLocal8Bit().constData());
		return 1;
	case 34:
		lua_pushstring(L, getAttr(QStringLiteral("on_world_lose_focus")).toLocal8Bit().constData());
		return 1;
	case 35:
		lua_pushstring(L, getAttr(QStringLiteral("script_filename")).toLocal8Bit().constData());
		return 1;
	case 36:
		lua_pushstring(L, getAttr(QStringLiteral("script_prefix")).toLocal8Bit().constData());
		return 1;
	case 37:
		lua_pushstring(L, getAttr(QStringLiteral("auto_say_string")).toLocal8Bit().constData());
		return 1;
	case 38:
		lua_pushstring(L, getAttr(QStringLiteral("auto_say_override")).toLocal8Bit().constData());
		return 1;
	case 39:
		lua_pushstring(L, getAttr(QStringLiteral("tab_completion_defaults")).toLocal8Bit().constData());
		return 1;
	case 40:
		lua_pushstring(L, getAttr(QStringLiteral("auto_log_file_name")).toLocal8Bit().constData());
		return 1;
	case 41:
		lua_pushstring(L, getAttr(QStringLiteral("recall_line_preamble")).toLocal8Bit().constData());
		return 1;
	case 42:
		lua_pushstring(L, getAttr(QStringLiteral("terminal_identification")).toLocal8Bit().constData());
		return 1;
	case 43:
		lua_pushstring(L, getAttr(QStringLiteral("mapping_failure")).toLocal8Bit().constData());
		return 1;
	case 44:
		lua_pushstring(L, getAttr(QStringLiteral("on_mxp_start")).toLocal8Bit().constData());
		return 1;
	case 45:
		lua_pushstring(L, getAttr(QStringLiteral("on_mxp_stop")).toLocal8Bit().constData());
		return 1;
	case 46:
		lua_pushstring(L, getAttr(QStringLiteral("on_mxp_error")).toLocal8Bit().constData());
		return 1;
	case 47:
		lua_pushstring(L, getAttr(QStringLiteral("on_mxp_open_tag")).toLocal8Bit().constData());
		return 1;
	case 48:
		lua_pushstring(L, getAttr(QStringLiteral("on_mxp_close_tag")).toLocal8Bit().constData());
		return 1;
	case 49:
		lua_pushstring(L, getAttr(QStringLiteral("on_mxp_set_variable")).toLocal8Bit().constData());
		return 1;
	case 50:
		lua_pushstring(L, getAttr(QStringLiteral("beep_sound")).toLocal8Bit().constData());
		return 1;
	case 51:
		lua_pushstring(L, runtime->logFileName().toLocal8Bit().constData());
		return 1;
	case 52:
		lua_pushstring(L, runtime->lastImmediateExpression().toLocal8Bit().constData());
		return 1;
	case 53:
		lua_pushstring(L, runtime->statusMessage().toLocal8Bit().constData());
		return 1;
	case 54:
		lua_pushstring(L, runtime->worldFilePath().toLocal8Bit().constData());
		return 1;
	case 55:
	{
		QString title = runtime->windowTitleOverride();
		if (title.isEmpty())
			title = getAttr(QStringLiteral("name"));
		lua_pushstring(L, title.toLocal8Bit().constData());
		return 1;
	}
	case 56:
		lua_pushstring(L, QCoreApplication::applicationFilePath().toLocal8Bit().constData());
		return 1;
	case 57:
		lua_pushstring(L, runtime->defaultWorldDirectory().toLocal8Bit().constData());
		return 1;
	case 58:
		lua_pushstring(L, runtime->defaultLogDirectory().toLocal8Bit().constData());
		return 1;
	case 59:
		lua_pushstring(L, QCoreApplication::applicationDirPath().toLocal8Bit().constData());
		return 1;
	case 60:
		lua_pushstring(L, runtime->pluginsDirectory().toLocal8Bit().constData());
		return 1;
	case 61:
		lua_pushstring(L, runtime->peerAddressString().toLocal8Bit().constData());
		return 1;
	case 62:
	{
		lua_pushstring(L, runtime->proxyAddressString().toLocal8Bit().constData());
		return 1;
	}
	case 63:
		lua_pushstring(L, QHostInfo::localHostName().toLocal8Bit().constData());
		return 1;
	case 64:
	{
		QString cwd = QDir::currentPath();
		if (const QChar sep = QDir::separator(); !cwd.endsWith(sep))
			cwd += sep;
		lua_pushstring(L, cwd.toLocal8Bit().constData());
		return 1;
	}
	case 65:
		lua_pushstring(L, getAttr(QStringLiteral("on_world_save")).toLocal8Bit().constData());
		return 1;
	case 66:
	{
		// Legacy behavior for plugins: this must point to the writable runtime root
		// (portable/startup dir), not necessarily the executable directory.
		QString base = runtime->startupDirectory();
		if (base.isEmpty())
			base = QCoreApplication::applicationDirPath();
		if (!base.endsWith(QDir::separator()))
			base.append(QDir::separator());
		lua_pushstring(L, base.toLocal8Bit().constData());
		return 1;
	}
	case 67:
	{
		if (runtime->worldFilePath().isEmpty())
		{
			lua_pushstring(L, "");
			return 1;
		}
		QFileInfo info(runtime->worldFilePath());
		QString   worldDir = info.absolutePath();
		if (const QChar sep = QDir::separator(); !worldDir.isEmpty() && !worldDir.endsWith(sep))
			worldDir += sep;
		lua_pushstring(L, worldDir.toLocal8Bit().constData());
		return 1;
	}
	case 68:
		lua_pushstring(L, runtime->startupDirectory().toLocal8Bit().constData());
		return 1;
	case 69:
		lua_pushstring(L, runtime->translatorFile().toLocal8Bit().constData());
		return 1;
	case 70:
		lua_pushstring(L, runtime->locale().toLocal8Bit().constData());
		return 1;
	case 71:
		lua_pushstring(L, runtime->fixedPitchFont().toLocal8Bit().constData());
		return 1;
	case 72:
		lua_pushstring(L, kVersionString);
		return 1;
	case 73:
		lua_pushstring(L, QStringLiteral(__DATE__ " " __TIME__).toLocal8Bit().constData());
		return 1;
	case 74:
	{
		QString root = runtime->startupDirectory();
		if (root.isEmpty())
			root = QCoreApplication::applicationDirPath();
		QString sounds = QDir(root).filePath(QStringLiteral("sounds"));
		if (const QChar sep = QDir::separator(); !sounds.endsWith(sep))
			sounds += sep;
		lua_pushstring(L, sounds.toLocal8Bit().constData());
		return 1;
	}
	case 75:
		lua_pushstring(L, runtime->lastTelnetSubnegotiation().toLocal8Bit().constData());
		return 1;
	case 76:
		lua_pushstring(L, runtime->firstSpecialFontPath().toLocal8Bit().constData());
		return 1;
	case 77:
		lua_pushstring(L, "");
		return 1;
	case 78:
		lua_pushstring(L, getAttr(QStringLiteral("foreground_image")).toLocal8Bit().constData());
		return 1;
	case 79:
		lua_pushstring(L, getAttr(QStringLiteral("background_image")).toLocal8Bit().constData());
		return 1;
	case 81:
	case 80:
		lua_pushstring(L, qmudPngVersionString().toLocal8Bit().constData());
		return 1;
	case 82:
		lua_pushstring(L, runtime->preferencesDatabaseName().toLocal8Bit().constData());
		return 1;
	case 83:
		lua_pushstring(L, sqliteVersionString().toLocal8Bit().constData());
		return 1;
	case 84:
		lua_pushstring(L, runtime->fileBrowsingDirectory().toLocal8Bit().constData());
		return 1;
	case 85:
		lua_pushstring(L, runtime->stateFilesDirectory().toLocal8Bit().constData());
		return 1;
	case 86:
	{
		QString word = runtime->wordUnderMenu();
		if (word.isEmpty())
		{
			if (frame)
			{
				if (WorldChildWindow *world = frame->activeWorldChildWindow())
					if (WorldView *view = world->view())
						word = view->wordUnderCursor();
			}
		}
		lua_pushstring(L, word.toLocal8Bit().constData());
		return 1;
	}
	case 87:
		lua_pushstring(L, runtime->lastCommandSent().toLocal8Bit().constData());
		return 1;
	case 88:
		lua_pushstring(L, runtime->windowTitleOverride().toLocal8Bit().constData());
		return 1;
	case 89:
		lua_pushstring(L, runtime->mainTitleOverride().toLocal8Bit().constData());
		return 1;
	case 272:
		lua_pushnumber(L, runtime->textRectangle().left);
		return 1;
	case 273:
		lua_pushnumber(L, runtime->textRectangle().top);
		return 1;
	case 274:
		lua_pushnumber(L, runtime->textRectangle().right);
		return 1;
	case 275:
		lua_pushnumber(L, runtime->textRectangle().bottom);
		return 1;
	case 276:
		lua_pushnumber(L, runtime->textRectangle().borderOffset);
		return 1;
	case 277:
		lua_pushnumber(L, runtime->textRectangle().borderWidth);
		return 1;
	case 278:
		lua_pushnumber(L, runtime->textRectangle().outsideFillColour);
		return 1;
	case 279:
		lua_pushnumber(L, runtime->textRectangle().outsideFillStyle);
		return 1;
	case 280:
	{
		if (WorldView *view = runtime->view())
			lua_pushnumber(L, view->outputClientHeight());
		else
			lua_pushnumber(L, 0);
		return 1;
	}
	case 281:
	{
		if (WorldView *view = runtime->view())
			lua_pushnumber(L, view->outputClientWidth());
		else
			lua_pushnumber(L, 0);
		return 1;
	}
	case 263:
	{
		if (WorldView *view = runtime->view())
			lua_pushnumber(L, view->height());
		else
			lua_pushnumber(L, 0);
		return 1;
	}
	case 264:
	{
		if (WorldView *view = runtime->view())
			lua_pushnumber(L, view->width());
		else
			lua_pushnumber(L, 0);
		return 1;
	}
	case 282:
		lua_pushnumber(L, runtime->textRectangle().borderColour);
		return 1;
	case 283:
	{
		if (WorldView *view = runtime->view(); view && view->hasLastMousePosition())
			lua_pushnumber(L, view->lastMousePosition().x());
		else
			lua_pushnumber(L, -1);
		return 1;
	}
	case 284:
	{
		if (WorldView *view = runtime->view(); view && view->hasLastMousePosition())
			lua_pushnumber(L, view->lastMousePosition().y());
		else
			lua_pushnumber(L, -1);
		return 1;
	}
	case 285:
		lua_pushboolean(L, runtime->view() != nullptr);
		return 1;
	case 286:
		lua_pushnumber(L, runtime->triggersMatchedThisSession());
		return 1;
	case 287:
		lua_pushnumber(L, runtime->aliasesMatchedThisSession());
		return 1;
	case 288:
		lua_pushnumber(L, runtime->timersFiredThisSession());
		return 1;
	case 290:
	{
		WorldView *view = runtime->view();
		lua_pushnumber(L, view ? view->outputTextRectangle().left() : 0);
		return 1;
	}
	case 291:
	{
		WorldView *view = runtime->view();
		lua_pushnumber(L, view ? view->outputTextRectangle().top() : 0);
		return 1;
	}
	case 292:
	{
		WorldView *view = runtime->view();
		lua_pushnumber(L, view ? view->outputTextRectangle().right() : 0);
		return 1;
	}
	case 293:
	{
		WorldView *view = runtime->view();
		lua_pushnumber(L, view ? view->outputTextRectangle().bottom() : 0);
		return 1;
	}

	// booleans
	case 101:
		lua_pushboolean(L, runtime->noCommandEcho());
		return 1;
	case 102:
		lua_pushboolean(L, runtime->debugIncomingPackets());
		return 1;
	case 103:
		lua_pushboolean(L, runtime->isCompressing());
		return 1;
	case 104:
		lua_pushboolean(L, runtime->isMxpActive());
		return 1;
	case 105:
		lua_pushboolean(L, runtime->isPuebloActive());
		return 1;
	case 106:
		lua_pushboolean(L, runtime->connectPhase() != WorldRuntime::eConnectConnectedToMud);
		return 1;
	case 107:
	{
		const int phase = runtime->connectPhase();
		lua_pushboolean(L, phase != WorldRuntime::eConnectNotConnected &&
		                       phase != WorldRuntime::eConnectConnectedToMud);
		return 1;
	}
	case 108:
		lua_pushboolean(L, runtime->disconnectOk());
		return 1;
	case 109:
		lua_pushboolean(L, runtime->traceEnabled());
		return 1;
	case 110:
		lua_pushboolean(L, runtime->scriptFileChanged());
		return 1;
	case 111:
		lua_pushboolean(L, runtime->worldFileModified());
		return 1;
	case 112:
		lua_pushboolean(L, runtime->isMapping());
		return 1;
	case 113:
		lua_pushboolean(L, runtime->isActive());
		return 1;
	case 114:
		lua_pushboolean(L, runtime->outputFrozen());
		return 1;
	case 115:
	{
		AppController *app = AppController::instance();
		lua_pushboolean(L, app && app->translatorLuaState() != nullptr);
		return 1;
	}
	case 118:
		lua_pushboolean(L, runtime->variablesChanged());
		return 1;
	case 119:
	{
		lua_pushboolean(L, runtime->luaCallbacks() != nullptr);
		return 1;
	}
	case 120:
	{
		WorldView *view = runtime->view();
		lua_pushboolean(L, view ? view->outputScrollBarWanted() : true);
		return 1;
	}
	case 121:
		lua_pushboolean(L, QElapsedTimer::isMonotonic() ? 1 : 0);
		return 1;
	case 122:
		lua_pushboolean(L, sqliteThreadsafe() ? 1 : 0);
		return 1;
	case 123:
		lua_pushboolean(L, runtime->doingSimulate());
		return 1;
	case 124:
		lua_pushboolean(L, runtime->lineOmittedFromOutput());
		return 1;
	case 125:
	{
		bool fullScreen = false;
		if (frame)
			fullScreen = frame->isFullScreenMode();
		lua_pushboolean(L, fullScreen);
		return 1;
	}

	// numbers
	case 201:
		lua_pushnumber(L, runtime->totalLinesReceived());
		return 1;
	case 202:
		lua_pushnumber(L, runtime->newLines());
		return 1;
	case 203:
		lua_pushnumber(L, runtime->totalLinesSent());
		return 1;
	case 204:
		lua_pushnumber(L, runtime->inputPacketCount());
		return 1;
	case 205:
		lua_pushnumber(L, runtime->outputPacketCount());
		return 1;
	case 206:
		lua_pushnumber(L, static_cast<lua_Number>(runtime->totalUncompressedBytes()));
		return 1;
	case 207:
		lua_pushnumber(L, static_cast<lua_Number>(runtime->totalCompressedBytes()));
		return 1;
	case 208:
		lua_pushnumber(L, runtime->mccpType());
		return 1;
	case 209:
		lua_pushnumber(L, runtime->mxpErrorCount());
		return 1;
	case 210:
		lua_pushnumber(L, static_cast<lua_Number>(runtime->mxpTagCount()));
		return 1;
	case 211:
		lua_pushnumber(L, static_cast<lua_Number>(runtime->mxpEntityCount()));
		return 1;
	case 212:
		lua_pushnumber(L, runtime->outputFontHeight());
		return 1;
	case 213:
		lua_pushnumber(L, runtime->outputFontWidth());
		return 1;
	case 214:
		lua_pushnumber(L, runtime->inputFontHeight());
		return 1;
	case 215:
		lua_pushnumber(L, runtime->inputFontWidth());
		return 1;
	case 216:
		lua_pushnumber(L, static_cast<lua_Number>(runtime->bytesIn()));
		return 1;
	case 217:
		lua_pushnumber(L, static_cast<lua_Number>(runtime->bytesOut()));
		return 1;
	case 218:
		lua_pushnumber(L, runtime->variableCount());
		return 1;
	case 219:
		lua_pushnumber(L, runtime->triggerCount());
		return 1;
	case 220:
		lua_pushnumber(L, runtime->timerCount());
		return 1;
	case 221:
		lua_pushnumber(L, runtime->aliasCount());
		return 1;
	case 222:
		lua_pushnumber(L, runtime->queuedCommandCount());
		return 1;
	case 223:
		lua_pushnumber(L, runtime->mappingCount());
		return 1;
	case 224:
		lua_pushnumber(L, static_cast<lua_Number>(runtime->lines().size()));
		return 1;
	case 225:
		lua_pushnumber(L, runtime->customElementCount());
		return 1;
	case 226:
		lua_pushnumber(L, runtime->customEntityCount());
		return 1;
	case 227:
		lua_pushnumber(L, runtime->connectPhase());
		return 1;
	case 228:
		lua_pushnumber(L, runtime->peerAddressV4());
		return 1;
	case 229:
	{
		lua_pushnumber(L, runtime->proxyAddressV4());
		return 1;
	}
	case 230:
		lua_pushnumber(L, engine->scriptExecutionDepth());
		return 1;
	case 231:
	{
		lua_pushnumber(L, static_cast<lua_Number>(runtime->logFilePosition()));
		return 1;
	}
	case 232:
	{
		double seconds = 0.0;
		if (QElapsedTimer::isMonotonic())
		{
			static QElapsedTimer monotonicClock;
			static double        monotonicBaseSeconds = 0.0;
			if (!monotonicClock.isValid())
			{
				monotonicClock.start();
				monotonicBaseSeconds = static_cast<double>(monotonicClock.msecsSinceReference()) / 1000.0;
			}
			seconds =
			    monotonicBaseSeconds + static_cast<double>(monotonicClock.nsecsElapsed()) / 1000000000.0;
		}
		else
		{
			seconds = static_cast<double>(QDateTime::currentDateTime().toMSecsSinceEpoch()) / 1000.0;
		}
		lua_pushnumber(L, seconds);
		return 1;
	}
	case 233:
		lua_pushnumber(L, runtime->triggerTimeSeconds());
		return 1;
	case 234:
		lua_pushnumber(L, runtime->aliasTimeSeconds());
		return 1;
	case 240:
		lua_pushnumber(L, runtime->outputFontWidth());
		return 1;
	case 241:
		lua_pushnumber(L, runtime->outputFontHeight());
		return 1;
	case 294:
	{
		long                        result = 0;
		const Qt::KeyboardModifiers mods   = QGuiApplication::keyboardModifiers();
		if (mods & Qt::ShiftModifier)
			result |= 0x01;
		if (mods & Qt::ControlModifier)
			result |= 0x02;
		if (mods & Qt::AltModifier)
			result |= 0x04;
		const Qt::MouseButtons buttons = QGuiApplication::mouseButtons();
		if (buttons & Qt::LeftButton)
		{
			result |= 0x10000;
		}
		if (buttons & Qt::RightButton)
		{
			result |= 0x20000;
		}
		if (buttons & Qt::MiddleButton)
		{
			result |= 0x40000;
		}
		lua_pushnumber(L, static_cast<lua_Number>(result));
		return 1;
	}
	case 295:
		lua_pushnumber(L, runtime->outputWindowRedrawCount());
		return 1;
	case 296:
	{
		int pos = 0;
		if (WorldView *view = runtime->view())
			pos = view->outputScrollPosition();
		lua_pushnumber(L, pos);
		return 1;
	}
	case 297:
	{
		if (!QElapsedTimer::isMonotonic())
		{
			lua_pushnumber(L, 0.0);
			return 1;
		}
		constexpr double frequency = 1000000000.0;
		lua_pushnumber(L, frequency);
		return 1;
	}
	case 298:
		lua_pushnumber(L, sqliteVersionNumber());
		return 1;
#ifdef Q_OS_WIN
	case 299:
		lua_pushnumber(L, static_cast<lua_Number>(GetACP()));
		return 1;
	case 300:
		lua_pushnumber(L, static_cast<lua_Number>(GetOEMCP()));
#else
	case 299:
	case 300:
		lua_pushnumber(L, 65001);
#endif
		return 1;
	case 310:
		lua_pushnumber(L, static_cast<lua_Number>(runtime->newlinesReceived()));
		return 1;
	case 235:
	{
		int count = 0;
		if (frame)
			count = frame->worldWindowCount();
		lua_pushnumber(L, count);
		return 1;
	}
	case 236:
	{
		int startCol = 0;
		if (WorldView *view = runtime->view())
			startCol = view->inputSelectionStartColumn();
		lua_pushnumber(L, startCol);
		return 1;
	}
	case 237:
	{
		int endCol = 0;
		if (WorldView *view = runtime->view())
			endCol = view->inputSelectionEndColumn();
		lua_pushnumber(L, endCol);
		return 1;
	}
	case 238:
	{
		int showCmd = kWindowShowNormal;
		if (frame)
		{
			if (WorldChildWindow *world = frame->findWorldChildWindow(runtime))
			{
				if (!world->isVisible())
					showCmd = kWindowShowHide;
				else if (world->isMinimized())
					showCmd = kWindowShowMinimized;
				else if (world->isMaximized())
					showCmd = kWindowShowMaximized;
			}
		}
		lua_pushnumber(L, showCmd);
		return 1;
	}
	case 239:
		lua_pushnumber(L, runtime->currentActionSource());
		return 1;
	case 242:
		lua_pushnumber(L, runtime->utf8ErrorCount());
		return 1;
	case 243:
	{
		AppController *app  = AppController::instance();
		const int      size = app ? app->getGlobalOption(QStringLiteral("FixedPitchFontSize")).toInt() : 0;
		lua_pushnumber(L, size);
		return 1;
	}
	case 244:
		lua_pushnumber(L, runtime->triggersEvaluatedCount());
		return 1;
	case 245:
		lua_pushnumber(L, runtime->triggersMatchedThisSession());
		return 1;
	case 246:
		lua_pushnumber(L, runtime->aliasesEvaluatedCount());
		return 1;
	case 247:
		lua_pushnumber(L, runtime->aliasesMatchedThisSession());
		return 1;
	case 248:
		lua_pushnumber(L, runtime->timersFiredThisSession());
		return 1;
	case 249:
	{
		const QWidget *client = frame ? frame->centralWidget() : nullptr;
		lua_pushnumber(L, client ? client->height() : 0);
		return 1;
	}
	case 250:
	{
		const QWidget *client = frame ? frame->centralWidget() : nullptr;
		lua_pushnumber(L, client ? client->width() : 0);
		return 1;
	}
	case 251:
	{
		const QToolBar *toolBar = frame ? frame->mainToolbar() : nullptr;
		lua_pushnumber(L, toolBar ? toolBar->height() : 0);
		return 1;
	}
	case 252:
	{
		const QToolBar *toolBar = frame ? frame->mainToolbar() : nullptr;
		lua_pushnumber(L, toolBar ? toolBar->width() : 0);
		return 1;
	}
	case 253:
	{
		const QToolBar *toolBar = frame ? frame->worldToolbar() : nullptr;
		lua_pushnumber(L, toolBar ? toolBar->height() : 0);
		return 1;
	}
	case 254:
	{
		const QToolBar *toolBar = frame ? frame->worldToolbar() : nullptr;
		lua_pushnumber(L, toolBar ? toolBar->width() : 0);
		return 1;
	}
	case 255:
	{
		const QToolBar *toolBar = frame ? frame->activityToolbar() : nullptr;
		lua_pushnumber(L, toolBar ? toolBar->height() : 0);
		return 1;
	}
	case 256:
	{
		const QToolBar *toolBar = frame ? frame->activityToolbar() : nullptr;
		lua_pushnumber(L, toolBar ? toolBar->width() : 0);
		return 1;
	}
	case 257:
	{
		const QWidget *infoBar = frame ? frame->infoBarWidget() : nullptr;
		lua_pushnumber(L, infoBar ? infoBar->height() : 0);
		return 1;
	}
	case 258:
	{
		const QWidget *infoBar = frame ? frame->infoBarWidget() : nullptr;
		lua_pushnumber(L, infoBar ? infoBar->width() : 0);
		return 1;
	}
	case 259:
	{
		const QStatusBar *statusBar = frame ? frame->frameStatusBar() : nullptr;
		lua_pushnumber(L, statusBar ? statusBar->height() : 0);
		return 1;
	}
	case 260:
	{
		const QStatusBar *statusBar = frame ? frame->frameStatusBar() : nullptr;
		lua_pushnumber(L, statusBar ? statusBar->width() : 0);
		return 1;
	}
	case 261:
	{
		int heightValue = 0;
		if (frame)
			if (WorldChildWindow *childWindow = frame->findWorldChildWindow(runtime))
				heightValue = childWindow->height();
		lua_pushnumber(L, heightValue);
		return 1;
	}
	case 262:
	{
		int widthValue = 0;
		if (frame)
			if (WorldChildWindow *childWindow = frame->findWorldChildWindow(runtime))
				widthValue = childWindow->width();
		lua_pushnumber(L, widthValue);
		return 1;
	}
	case 265:
		lua_pushnumber(L, QOperatingSystemVersion::current().majorVersion());
		return 1;
	case 266:
		lua_pushnumber(L, QOperatingSystemVersion::current().minorVersion());
		return 1;
	case 267:
		lua_pushnumber(L, QOperatingSystemVersion::current().microVersion());
		return 1;
	case 268:
#ifdef Q_OS_WIN
		lua_pushnumber(L, VER_PLATFORM_WIN32_NT);
#else
		lua_pushnumber(L, 0);
#endif
		return 1;
	case 269:
		lua_pushnumber(L, getAttr(QStringLiteral("foreground_image_mode")).toInt());
		return 1;
	case 270:
		lua_pushnumber(L, getAttr(QStringLiteral("background_image_mode")).toInt());
		return 1;
	case 271:
		lua_pushnumber(L, static_cast<lua_Number>(colorValue(
		                      WorldView::parseColor(getAttr(QStringLiteral("output_background_colour"))))));
		return 1;
	case 289:
		lua_pushnumber(L, runtime->lastLineWithIacGa());
		return 1;

	// dates
	case 301:
		value = runtime->connectTime();
		break;
	case 302:
		value = runtime->lastFlushTime();
		break;
	case 303:
		value = runtime->scriptFileModTime();
		break;
	case 304:
		value = QDateTime::currentDateTime();
		break;
	case 305:
		value = runtime->clientStartTime();
		break;
	case 306:
		value = runtime->worldStartTime();
		break;
	default:
		lua_pushnil(L);
		return 1;
	}

	lua_pushnumber(L, toOleDate(value));
	return 1;
}

static QString concatLuaArgs(lua_State *L, const int startIndex, const QString &delimiter)
{
	const int top = lua_gettop(L);
	if (startIndex > top)
		return {};

	lua_getglobal(L, "tostring");
	QStringList parts;
	for (int i = startIndex; i <= top; ++i)
	{
		lua_pushvalue(L, -1);
		lua_pushvalue(L, i);
		lua_call(L, 1, 1);
		size_t      len  = 0;
		const char *text = lua_tolstring(L, -1, &len);
		if (!text)
			luaL_error(L, "'tostring' must return a string to be concatenated");
		parts << QString::fromUtf8(text, static_cast<int>(len));
		lua_pop(L, 1);
	}
	lua_pop(L, 1);
	return parts.join(delimiter);
}

static void reportLuaError(const LuaCallbackEngine &engine, const QString &message)
{
	if (WorldRuntime *runtime = engine.worldRuntime())
	{
		const QString logFlag   = runtime->worldAttributes().value(QStringLiteral("log_script_errors"));
		bool          logErrors = isEnabledValue(logFlag);
		if (!logErrors)
		{
			if (bool parsedKeyword = false; parseBooleanKeywordValue(logFlag, parsedKeyword))
				logErrors = parsedKeyword;
		}
		if (logErrors)
		{
			QString       logDir     = runtime->defaultLogDirectory();
			const QString startupDir = runtime->startupDirectory();
			if (logDir.isEmpty())
				logDir = startupDir;
			else if (QDir::isRelativePath(logDir) && !startupDir.isEmpty())
				logDir = QDir(startupDir).filePath(logDir);

			QDir dir(logDir.isEmpty() ? QDir::currentPath() : logDir);
			if (!dir.exists())
				dir.mkpath(QStringLiteral("."));
			QFile file(dir.filePath(QStringLiteral("script_error_log.txt")));
			if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
			{
				QTextStream out(&file);
				out << "\n--- Scripting error on "
				    << QDateTime::currentDateTime().toString(QStringLiteral("dddd, MMMM d, yyyy, h:mm AP"))
				    << " ---\n";
				out << message << "\n";
			}
		}

		const QString flag =
		    runtime->worldAttributes().value(QStringLiteral("script_errors_to_output_window"));
		bool toOutput = isEnabledValue(flag);
		if (!toOutput)
		{
			if (bool parsedKeyword = false; parseBooleanKeywordValue(flag, parsedKeyword))
				toOutput = parsedKeyword;
		}
		if (toOutput && !runtime->suppressScriptErrorOutputToWorld())
		{
			runtime->outputText(message, true, true);
			return;
		}
	}
	qWarning() << message;
}

static bool optBool(lua_State *L, const int index, const bool defaultValue)
{
	if (lua_gettop(L) < index || lua_isnil(L, index))
		return defaultValue;
	if (lua_isboolean(L, index))
		return lua_toboolean(L, index) != 0;
	return luaL_checknumber(L, index) != 0;
}

static int luaToInt(lua_State *L, const int index)
{
	const double value = luaL_checknumber(L, index);
	if (!qIsFinite(value))
		luaL_error(L, "number is not finite");
	return static_cast<int>(value);
}

static int luaToIntOpt(lua_State *L, const int index)
{
	if (lua_isnoneornil(L, index))
		return 0;
	const double value = luaL_checknumber(L, index);
	if (!qIsFinite(value))
		luaL_error(L, "number is not finite");
	return static_cast<int>(value);
}

static long luaToLong(lua_State *L, const int index)
{
	const double value = luaL_checknumber(L, index);
	if (!qIsFinite(value))
		luaL_error(L, "number is not finite");
	return static_cast<long>(value);
}

static long luaToLongOpt(lua_State *L, const int index, const long defaultValue)
{
	if (lua_isnoneornil(L, index))
		return defaultValue;
	const double value = luaL_checknumber(L, index);
	if (!qIsFinite(value))
		luaL_error(L, "number is not finite");
	return static_cast<long>(value);
}

static WorldRuntime *runtimeFromLua(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	return engine ? engine->worldRuntime() : nullptr;
}

static QString pluginIdFromLua(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	return engine ? engine->pluginId() : QString{};
}

static QColor colourFromRef(const int value)
{
	const int r = value & 0xFF;
	const int g = value >> 8 & 0xFF;
	const int b = value >> 16 & 0xFF;
	return {r, g, b};
}

static int colourRefFromQColor(const QColor &color)
{
	return color.blue() << 16 | color.green() << 8 | color.red();
}

static int luaUtilsTimer(lua_State *L)
{
	static QElapsedTimer timer;
	if (!timer.isValid())
		timer.start();
	const double seconds = static_cast<double>(timer.nsecsElapsed()) / 1000000000.0;
	lua_pushnumber(L, seconds);
	return 1;
}

static int luaUtilsFontPicker(lua_State *L)
{
	const char *fontname   = luaL_optstring(L, 1, "");
	const int   fontsize   = static_cast<int>(luaL_optnumber(L, 2, 10));
	const int   fontcolour = static_cast<int>(luaL_optnumber(L, 3, 0));

	QFont       font(QString::fromUtf8(fontname), fontsize);
	bool        ok     = false;
	const QFont chosen = QFontDialog::getFont(&ok, font, resolveMainWindow(), QStringLiteral("Select Font"));
	if (!ok)
	{
		lua_pushnil(L);
		return 1;
	}

	QColor       colour = colourFromRef(fontcolour);
	const QColor picked =
	    QColorDialog::getColor(colour, resolveMainWindow(), QStringLiteral("Select Colour"));
	if (picked.isValid())
		colour = picked;

	lua_newtable(L);

	const QString family    = chosen.family();
	QByteArray    nameBytes = family.toUtf8();
	lua_pushlstring(L, nameBytes.constData(), nameBytes.size());
	lua_setfield(L, -2, "name");

	const QString styleName  = QFontDatabase::styleString(chosen);
	QByteArray    styleBytes = styleName.toUtf8();
	lua_pushlstring(L, styleBytes.constData(), styleBytes.size());
	lua_setfield(L, -2, "style");

	lua_pushnumber(L, chosen.pointSize());
	lua_setfield(L, -2, "size");

	lua_pushnumber(L, colourRefFromQColor(colour));
	lua_setfield(L, -2, "colour");

	lua_pushboolean(L, chosen.bold());
	lua_setfield(L, -2, "bold");

	lua_pushboolean(L, chosen.italic());
	lua_setfield(L, -2, "italic");

	lua_pushboolean(L, chosen.underline());
	lua_setfield(L, -2, "underline");

	lua_pushboolean(L, chosen.strikeOut());
	lua_setfield(L, -2, "strikeout");

	lua_pushnumber(L, 0);
	lua_setfield(L, -2, "charset");

	return 1;
}

static int luaUtilsMsgBoxInternal(lua_State *L)
{
	size_t      boxmsgLen   = 0;
	size_t      boxtitleLen = 0;
	const char *boxmsg      = luaL_checklstring(L, 1, &boxmsgLen);
	const char *boxtitle    = luaL_optlstring(L, 2, "QMud", &boxtitleLen);
	const char *boxtype     = luaL_optstring(L, 3, "ok");
	const char *boxicon     = luaL_optstring(L, 4, "!");
	const int   boxdefault  = static_cast<int>(luaL_optnumber(L, 5, 1));

	if (boxmsgLen > 1000)
		luaL_error(L, "msgbox message too long (max 1000 characters)");
	if (boxtitleLen > 100)
		luaL_error(L, "msgbox title too long (max 100 characters)");

	const QString                type          = QString::fromUtf8(boxtype).toLower();
	QMessageBox::StandardButtons buttons       = QMessageBox::NoButton;
	QMessageBox::StandardButton  defaultButton = QMessageBox::NoButton;

	if (type == QStringLiteral("ok"))
	{
		buttons       = QMessageBox::Ok;
		defaultButton = QMessageBox::Ok;
	}
	else if (type == QStringLiteral("abortretryignore"))
	{
		buttons = QMessageBox::Abort | QMessageBox::Retry | QMessageBox::Ignore;
		if (boxdefault == 1)
			defaultButton = QMessageBox::Abort;
		else if (boxdefault == 2)
			defaultButton = QMessageBox::Retry;
		else if (boxdefault == 3)
			defaultButton = QMessageBox::Ignore;
		else
			luaL_error(L, "msgbox default button must be 1, 2 or 3");
	}
	else if (type == QStringLiteral("okcancel"))
	{
		buttons = QMessageBox::Ok | QMessageBox::Cancel;
		if (boxdefault == 1)
			defaultButton = QMessageBox::Ok;
		else if (boxdefault == 2)
			defaultButton = QMessageBox::Cancel;
		else
			luaL_error(L, "msgbox default button must be 1 or 2");
	}
	else if (type == QStringLiteral("retrycancel"))
	{
		buttons = QMessageBox::Retry | QMessageBox::Cancel;
		if (boxdefault == 1)
			defaultButton = QMessageBox::Retry;
		else if (boxdefault == 2)
			defaultButton = QMessageBox::Cancel;
		else
			luaL_error(L, "msgbox default button must be 1 or 2");
	}
	else if (type == QStringLiteral("yesno"))
	{
		buttons = QMessageBox::Yes | QMessageBox::No;
		if (boxdefault == 1)
			defaultButton = QMessageBox::Yes;
		else if (boxdefault == 2)
			defaultButton = QMessageBox::No;
		else
			luaL_error(L, "msgbox default button must be 1 or 2");
	}
	else if (type == QStringLiteral("yesnocancel"))
	{
		buttons = QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel;
		if (boxdefault == 1)
			defaultButton = QMessageBox::Yes;
		else if (boxdefault == 2)
			defaultButton = QMessageBox::No;
		else if (boxdefault == 3)
			defaultButton = QMessageBox::Cancel;
		else
			luaL_error(L, "msgbox default button must be 1, 2 or 3");
	}
	else
	{
		luaL_error(L, "msgbox type unknown");
	}

	const char        iconChar = asciiToLower(boxicon[0]);
	QMessageBox::Icon icon     = QMessageBox::Warning;
	switch (iconChar)
	{
	case '?':
		icon = QMessageBox::Question;
		break;
	case '!':
		icon = QMessageBox::Warning;
		break;
	case 'i':
		icon = QMessageBox::Information;
		break;
	case '.':
		icon = QMessageBox::Critical;
		break;
	default:
		luaL_error(L, "msgbox icon unknown");
	}

	QMessageBox box(icon, QString::fromUtf8(boxtitle), QString::fromUtf8(boxmsg), buttons,
	                resolveMainWindow());
	if (defaultButton != QMessageBox::NoButton)
		box.setDefaultButton(defaultButton);

	const auto  result = static_cast<QMessageBox::StandardButton>(box.exec());
	const char *out    = nullptr;
	switch (result)
	{
	case QMessageBox::Yes:
		out = "yes";
		break;
	case QMessageBox::No:
		out = "no";
		break;
	case QMessageBox::Ok:
		out = "ok";
		break;
	case QMessageBox::Retry:
		out = "retry";
		break;
	case QMessageBox::Ignore:
		out = "ignore";
		break;
	case QMessageBox::Cancel:
		out = "cancel";
		break;
	case QMessageBox::Abort:
		out = "abort";
		break;
	default:
		out = "other";
		break;
	}

	lua_pushstring(L, out);
	return 1;
}

static int luaUtilsMsgBox(lua_State *L)
{
	return luaUtilsMsgBoxInternal(L);
}

static int luaUtilsUMsgBox(lua_State *L)
{
	return luaUtilsMsgBoxInternal(L);
}

static int luaUtilsMd5(lua_State *L)
{
	size_t           len  = 0;
	const char      *data = luaL_checklstring(L, 1, &len);
	const QByteArray bytes(data, static_cast<int>(len));
	const QByteArray digest = QCryptographicHash::hash(bytes, QCryptographicHash::Md5);
	lua_pushlstring(L, digest.constData(), digest.size());
	return 1;
}

static int luaUtilsSha256(lua_State *L)
{
	size_t           len  = 0;
	const char      *data = luaL_checklstring(L, 1, &len);
	const QByteArray bytes(data, static_cast<int>(len));
	const QByteArray digest = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
	lua_pushlstring(L, digest.constData(), digest.size());
	return 1;
}

static int luaUtilsFromHex(lua_State *L)
{
	size_t      len  = 0;
	const char *data = luaL_checklstring(L, 1, &len);
	QByteArray  out;
	out.reserve(static_cast<int>(len / 2) + 1);

	int           nybbles = 0;
	unsigned char value   = 0;
	for (size_t i = 0; i < len; ++i)
	{
		const auto c = static_cast<unsigned char>(data[i]);
		if (isAsciiSpace(c))
			continue;

		const int nibbleValue = asciiHexValue(c);
		if (nibbleValue < 0)
		{
			luaL_error(L, "Not a hex digit ('%c') at position %d", c, static_cast<int>(i + 1));
		}

		const auto nibble = static_cast<unsigned char>(nibbleValue);

		value = static_cast<unsigned char>(value << 4 | nibble);
		++nybbles;
		if (nybbles == 2)
		{
			out.append(static_cast<char>(value));
			nybbles = 0;
			value   = 0;
		}
	}

	if (nybbles == 1)
		out.append(static_cast<char>(value));

	lua_pushlstring(L, out.constData(), out.size());
	return 1;
}

static int luaUtilsFunctionList(lua_State *L)
{
	lua_newtable(L);
	for (int i = 0; kInternalFunctionMetadataTable[i].functionName[0]; ++i)
	{
		lua_pushstring(L, kInternalFunctionMetadataTable[i].functionName);
		lua_rawseti(L, -2, i + 1);
	}
	return 1;
}

static int luaUtilsFunctionArgs(lua_State *L)
{
	lua_newtable(L);
	for (int i = 0; kInternalFunctionMetadataTable[i].functionName[0]; ++i)
	{
		lua_pushstring(L, kInternalFunctionMetadataTable[i].functionName);
		lua_pushstring(L, kInternalFunctionMetadataTable[i].argumentsSignature);
		lua_rawset(L, -3);
	}
	return 1;
}

static int luaUtilsGetFontFamilies(lua_State *L)
{
	const QStringList families = QFontDatabase::families();
	lua_newtable(L);
	for (const QString &family : families)
	{
		const QByteArray bytes = family.toUtf8();
		lua_pushlstring(L, bytes.constData(), bytes.size());
		lua_pushboolean(L, 1);
		lua_rawset(L, -3);
	}
	return 1;
}

static int luaUtilsMenuFontSize(lua_State *L)
{
	const QFont oldFont = QApplication::font("QMenu");
	const qreal oldSize = oldFont.pointSizeF();
	if (const qreal points = luaL_optnumber(L, 1, 0); points >= 5.0 && points <= 30.0)
	{
		QFont menuFont = oldFont;
		menuFont.setPointSizeF(points);
		QApplication::setFont(menuFont, "QMenu");
	}
	lua_pushnumber(L, oldSize > 0.0 ? oldSize : 0.0);
	return 1;
}

static int luaUtilsMetaphone(lua_State *L)
{
	const QString input             = QString::fromUtf8(luaL_checkstring(L, 1));
	const int     length            = static_cast<int>(luaL_optnumber(L, 2, 4));
	const auto [primary, secondary] = qmudDoubleMetaphone(input, length);
	lua_pushstring(L, primary.toUtf8().constData());
	if (secondary.isEmpty())
		lua_pushnil(L);
	else
		lua_pushstring(L, secondary.toUtf8().constData());
	return 2;
}

static int luaUtilsGlyphAvailable(lua_State *L)
{
	const QString  family    = QString::fromUtf8(luaL_checkstring(L, 1));
	const auto     codepoint = static_cast<char32_t>(luaL_checkinteger(L, 2));

	const QFont    font(family, 5);
	const QRawFont raw = QRawFont::fromFont(font);
	if (!raw.isValid())
	{
		lua_pushnil(L);
		return 1;
	}

	const QList<quint32> glyphs = raw.glyphIndexesForString(QString::fromUcs4(&codepoint, 1));
	const int            glyph  = glyphs.isEmpty() ? 0 : static_cast<int>(glyphs.first());
	lua_pushinteger(L, glyph > 0 ? glyph : 0);
	return 1;
}

static int luaUtilsHash(lua_State *L)
{
	size_t           len  = 0;
	const char      *data = luaL_checklstring(L, 1, &len);
	const QByteArray bytes(data, static_cast<int>(len));
	const QByteArray digest = QCryptographicHash::hash(bytes, QCryptographicHash::Sha1);
	lua_pushstring(L, digest.toHex().constData());
	return 1;
}

static int luaUtilsReadDir(lua_State *L)
{
	const QString   pattern = QString::fromUtf8(luaL_checkstring(L, 1));
	const QFileInfo patternInfo(pattern);
	QDir            dir = patternInfo.dir();
	if (!dir.exists())
	{
		lua_newtable(L);
		return 1;
	}

	const QString mask = patternInfo.fileName().isEmpty() ? QStringLiteral("*") : patternInfo.fileName();
	const QFileInfoList entries = dir.entryInfoList(
	    QStringList(mask), QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System | QDir::Hidden,
	    QDir::Name | QDir::IgnoreCase);

	lua_newtable(L);
	for (const QFileInfo &fi : entries)
	{
		const QByteArray name = fi.fileName().toUtf8();
		lua_pushlstring(L, name.constData(), name.size());
		lua_newtable(L);

		lua_pushstring(L, "size");
		lua_pushnumber(L, static_cast<lua_Number>(fi.size()));
		lua_rawset(L, -3);

		if (const qint64 createSecs = fi.birthTime().isValid() ? fi.birthTime().toSecsSinceEpoch() : -1;
		    createSecs >= 0)
		{
			lua_pushstring(L, "create_time");
			lua_pushnumber(L, static_cast<lua_Number>(createSecs));
			lua_rawset(L, -3);
		}

		if (const qint64 accessSecs = fi.lastRead().isValid() ? fi.lastRead().toSecsSinceEpoch() : -1;
		    accessSecs >= 0)
		{
			lua_pushstring(L, "access_time");
			lua_pushnumber(L, static_cast<lua_Number>(accessSecs));
			lua_rawset(L, -3);
		}

		lua_pushstring(L, "write_time");
		lua_pushnumber(L, static_cast<lua_Number>(fi.lastModified().toSecsSinceEpoch()));
		lua_rawset(L, -3);

		const QFile::Permissions perms    = fi.permissions();
		const bool               readonly = !(perms & QFileDevice::WriteOwner);

		lua_pushstring(L, "archive");
		lua_pushboolean(L, fi.isFile());
		lua_rawset(L, -3);
		lua_pushstring(L, "hidden");
		lua_pushboolean(L, fi.isHidden());
		lua_rawset(L, -3);
		lua_pushstring(L, "normal");
		lua_pushboolean(L, fi.isFile() && !fi.isHidden() && !fi.isSymLink());
		lua_rawset(L, -3);
		lua_pushstring(L, "readonly");
		lua_pushboolean(L, readonly);
		lua_rawset(L, -3);
		lua_pushstring(L, "directory");
		lua_pushboolean(L, fi.isDir());
		lua_rawset(L, -3);
		lua_pushstring(L, "system");
		lua_pushboolean(L, fi.isSymLink());
		lua_rawset(L, -3);

		lua_rawset(L, -3);
	}

	return 1;
}

static QString slashPathWithTrailingSlash(const QString &input)
{
	QString path = QDir::fromNativeSeparators(input.trimmed());
	if (path.isEmpty())
		return path;
	if (!path.endsWith(QLatin1Char('/')))
		path += QLatin1Char('/');
	return path;
}

static int luaUtilsInfo(lua_State *L)
{
	lua_newtable(L);

	AppController *app = AppController::instance();
	if (const QString currentDir = slashPathWithTrailingSlash(QDir::currentPath()); !currentDir.isEmpty())
	{
		lua_pushstring(L, "current_directory");
		lua_pushstring(L, currentDir.toUtf8().constData());
		lua_rawset(L, -3);
	}

	const QString appDir = slashPathWithTrailingSlash(QCoreApplication::applicationDirPath());
	lua_pushstring(L, "app_directory");
	lua_pushstring(L, appDir.toUtf8().constData());
	lua_rawset(L, -3);

	if (app)
	{
		auto setString = [L](const char *key, const QString &value)
		{
			lua_pushstring(L, key);
			lua_pushstring(L, value.toUtf8().constData());
			lua_rawset(L, -3);
		};

		auto globalString = [app](const char *key) -> QString
		{ return app->getGlobalOption(QString::fromLatin1(key)).toString(); };

		setString("world_files_directory", slashPathWithTrailingSlash(app->makeAbsolutePath(
		                                       globalString("DefaultWorldFileDirectory"))));
		setString("state_files_directory",
		          slashPathWithTrailingSlash(app->makeAbsolutePath(globalString("StateFilesDirectory"))));
		setString("log_files_directory",
		          slashPathWithTrailingSlash(app->makeAbsolutePath(globalString("DefaultLogFileDirectory"))));
		setString("plugins_directory",
		          slashPathWithTrailingSlash(app->makeAbsolutePath(globalString("PluginsDirectory"))));
		setString("startup_directory",
		          slashPathWithTrailingSlash(QFileInfo(app->iniFilePath()).absolutePath()));
		setString("locale", globalString("Locale"));
		setString("fixed_pitch_font", globalString("FixedPitchFont"));
		lua_pushstring(L, "fixed_pitch_font_size");
		lua_pushinteger(L, app->getGlobalOption(QStringLiteral("FixedPitchFontSize")).toInt());
		lua_rawset(L, -3);

		QString translatorFile;
		if (WorldRuntime *runtime = runtimeFromLua(L))
			translatorFile = runtime->translatorFile();
		setString("translator_file", QDir::fromNativeSeparators(translatorFile));
	}

	return 1;
}

static int luaUtilsReloadGlobalPrefs(lua_State *L)
{
	const int stackTop = lua_gettop(L);
	Q_UNUSED(stackTop);
	if (AppController *app = AppController::instance())
		app->reloadGlobalPreferencesForLua();
	return 0;
}

static int luaUtilsInfoTypes(lua_State *L)
{
	lua_newtable(L);
	for (int i = 0; kInfoTypeMappingTable[i].infoType; ++i)
	{
		lua_pushstring(L, kInfoTypeMappingTable[i].description);
		lua_rawseti(L, -2, kInfoTypeMappingTable[i].infoType);
	}
	return 1;
}

static int luaUtilsSendToFront(lua_State *L)
{
	const QString titlePrefix = QString::fromUtf8(luaL_checkstring(L, 1));
	bool          found       = false;
	for (QWidget *widget : QApplication::topLevelWidgets())
	{
		if (!widget)
			continue;
		if (const QString title = widget->windowTitle(); !title.startsWith(titlePrefix))
			continue;
		widget->raise();
		widget->activateWindow();
		found = true;
		break;
	}
	lua_pushboolean(L, found ? 1 : 0);
	return 1;
}

static int luaUtilsSetBackgroundColour(lua_State *L)
{
	const long colour = static_cast<long>(luaL_optnumber(L, 1, 0));
	if (MainWindow *frame = resolveMainWindow())
		frame->setFrameBackgroundColour(colour);
	return 0;
}

static int luaUtilsToHex(lua_State *L)
{
	size_t           len  = 0;
	const char      *data = luaL_checklstring(L, 1, &len);
	const QByteArray bytes(data, static_cast<int>(len));
	QByteArray       hex = bytes.toHex();
	hex                  = hex.toUpper();
	lua_pushlstring(L, hex.constData(), hex.size());
	return 1;
}

static int utf8AdditionalBytes(const unsigned char c)
{
	if ((c & 0xE0) == 0xC0)
		return 1;
	if ((c & 0xF0) == 0xE0)
		return 2;
	if ((c & 0xF8) == 0xF0)
		return 3;
	if ((c & 0xFC) == 0xF8)
		return 4;
	if ((c & 0xFE) == 0xFC)
		return 5;
	return -1;
}

static int utf8EncodeCodepoint(const quint32 code, unsigned char *out)
{
	if (code <= 0x7F)
	{
		out[0] = static_cast<unsigned char>(code);
		return 1;
	}
	if (code <= 0x7FF)
	{
		const quint32 top5 = code >> 6 & 0x1F;
		const quint32 low6 = code & 0x3F;
		out[0]             = static_cast<unsigned char>(0xC0 | top5);
		out[1]             = static_cast<unsigned char>(0x80 | low6);
		return 2;
	}
	if (code <= 0xFFFF)
	{
		const quint32 top4 = code >> 12 & 0x0F;
		const quint32 mid6 = code >> 6 & 0x3F;
		const quint32 low6 = code & 0x3F;
		out[0]             = static_cast<unsigned char>(0xE0 | top4);
		out[1]             = static_cast<unsigned char>(0x80 | mid6);
		out[2]             = static_cast<unsigned char>(0x80 | low6);
		return 3;
	}
	if (code <= 0x1FFFFF)
	{
		const quint32 top3 = code >> 18 & 0x07;
		const quint32 b1   = code >> 12 & 0x3F;
		const quint32 b2   = code >> 6 & 0x3F;
		const quint32 b3   = code & 0x3F;
		out[0]             = static_cast<unsigned char>(0xF0 | top3);
		out[1]             = static_cast<unsigned char>(0x80 | b1);
		out[2]             = static_cast<unsigned char>(0x80 | b2);
		out[3]             = static_cast<unsigned char>(0x80 | b3);
		return 4;
	}
	if (code <= 0x3FFFFFF)
	{
		const quint32 top2 = code >> 24 & 0x03;
		const quint32 b1   = code >> 18 & 0x3F;
		const quint32 b2   = code >> 12 & 0x3F;
		const quint32 b3   = code >> 6 & 0x3F;
		const quint32 b4   = code & 0x3F;
		out[0]             = static_cast<unsigned char>(0xF8 | top2);
		out[1]             = static_cast<unsigned char>(0x80 | b1);
		out[2]             = static_cast<unsigned char>(0x80 | b2);
		out[3]             = static_cast<unsigned char>(0x80 | b3);
		out[4]             = static_cast<unsigned char>(0x80 | b4);
		return 5;
	}
	const quint32 top1 = code >> 30 & 0x01;
	const quint32 b1   = code >> 24 & 0x3F;
	const quint32 b2   = code >> 18 & 0x3F;
	const quint32 b3   = code >> 12 & 0x3F;
	const quint32 b4   = code >> 6 & 0x3F;
	const quint32 b5   = code & 0x3F;
	out[0]             = static_cast<unsigned char>(0xFC | top1);
	out[1]             = static_cast<unsigned char>(0x80 | b1);
	out[2]             = static_cast<unsigned char>(0x80 | b2);
	out[3]             = static_cast<unsigned char>(0x80 | b3);
	out[4]             = static_cast<unsigned char>(0x80 | b4);
	out[5]             = static_cast<unsigned char>(0x80 | b5);
	return 6;
}

static int luaUtilsSplit(lua_State *L)
{
	size_t           inputLen  = 0;
	const char      *inputData = luaL_checklstring(L, 1, &inputLen);
	size_t           sepLen    = 0;
	const char      *sepData   = luaL_checklstring(L, 2, &sepLen);
	const QByteArray input(inputData, static_cast<int>(inputLen));
	const QByteArray sep(sepData, static_cast<int>(sepLen));
	const int        count = static_cast<int>(luaL_optnumber(L, 3, 0));

	if (sep.isEmpty())
		luaL_error(L, "Separator must not be empty");
	if (count < 0)
		luaL_error(L, "Count must be positive or zero");

	lua_newtable(L);
	int       i     = 1;
	qsizetype start = 0;
	while (start <= input.size())
	{
		const qsizetype pos = input.indexOf(sep, start);
		if (pos < 0 || (count != 0 && i > count))
		{
			const QByteArray part = input.mid(start);
			lua_pushlstring(L, part.constData(), part.size());
			lua_rawseti(L, -2, i);
			break;
		}
		const QByteArray part = input.mid(start, pos - start);
		lua_pushlstring(L, part.constData(), part.size());
		lua_rawseti(L, -2, i++);
		start = pos + sep.size();
	}
	return 1;
}

static int luaUtilsShellExecute(lua_State *L)
{
	const QString filename  = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString params    = QString::fromUtf8(luaL_optstring(L, 2, ""));
	const QString defdir    = QString::fromUtf8(luaL_optstring(L, 3, ""));
	const QString operation = QString::fromUtf8(luaL_optstring(L, 4, "open")).toLower();
	Q_UNUSED(luaL_optnumber(L, 5, 1));

	bool    ok = false;
	QString error;

	if (!params.isEmpty() && !QUrl::fromUserInput(filename).isValid())
	{
		ok = QProcess::startDetached(filename, QProcess::splitCommand(params),
		                             defdir.isEmpty() ? QString() : defdir);
		if (!ok)
			error = QStringLiteral("Could not start process.");
	}
	else
	{
		QUrl url = QUrl::fromUserInput(filename, defdir);
		if (!url.isValid())
		{
			lua_pushnil(L);
			lua_pushstring(L, "Invalid URL or path.");
			return 2;
		}
		if (operation == QStringLiteral("explore") && url.isLocalFile())
		{
			if (const QFileInfo fi(url.toLocalFile()); fi.exists() && !fi.isDir())
				url = QUrl::fromLocalFile(fi.absolutePath());
		}
		ok = QDesktopServices::openUrl(url);
		if (!ok)
			error = QStringLiteral("Failed to open target.");
	}

	if (!ok)
	{
		lua_pushnil(L);
		lua_pushstring(L, error.toUtf8().constData());
		return 2;
	}
	lua_pushboolean(L, 1);
	return 1;
}

static int luaUtilsShowDebugStatus(lua_State *L)
{
	const bool enabled = optBool(L, 1, true);
	if (MainWindow *frame = resolveMainWindow())
		frame->setShowDebugStatus(enabled);
	return 0;
}

static int luaUtilsSpellCheckDialog(lua_State *L)
{
	const QString misspelt = QString::fromUtf8(luaL_checkstring(L, 1));
	QStringList   suggestions;
	if (lua_gettop(L) >= 2 && !lua_isnil(L, 2))
	{
		if (!lua_istable(L, 2))
			luaL_error(L, "argument 2 must be a table of suggestions, or nil");
		for (int i = 1;; ++i)
		{
			lua_rawgeti(L, 2, i);
			if (lua_isnil(L, -1))
			{
				lua_pop(L, 1);
				break;
			}
			suggestions.push_back(QString::fromUtf8(luaL_checkstring(L, -1)));
			lua_pop(L, 1);
		}
	}

	SpellCheckDialog dlg(misspelt, suggestions, resolveMainWindow());
	if (dlg.exec() != QDialog::Accepted)
	{
		lua_pushnil(L);
		return 1;
	}

	const QByteArray action      = dlg.action().toUtf8();
	const QByteArray replacement = dlg.replacement().toUtf8();
	lua_pushlstring(L, action.constData(), action.size());
	lua_pushlstring(L, replacement.constData(), replacement.size());
	return 2;
}

static int luaUtilsUtf8Convert(lua_State *L)
{
	size_t           len  = 0;
	const char      *data = luaL_checklstring(L, 1, &len);
	const QByteArray input(data, static_cast<int>(len));
	QByteArray       out;
	out.reserve(input.size() * 2);
	unsigned char utf8[6];
	for (unsigned char c : input)
	{
		const int n = utf8EncodeCodepoint(c, utf8);
		out.append(reinterpret_cast<const char *>(utf8), n);
	}
	lua_pushlstring(L, out.constData(), out.size());
	return 1;
}

static int luaUtilsUtf8Encode(lua_State *L)
{
	const int     numArgs = lua_gettop(L);
	QByteArray    out;
	unsigned char utf8[6];
	for (int i = 1; i <= numArgs; ++i)
	{
		if (lua_istable(L, i))
		{
			for (int j = 1;; ++j)
			{
				lua_rawgeti(L, i, j);
				if (lua_isnil(L, -1))
				{
					lua_pop(L, 1);
					break;
				}
				const double f = luaL_checknumber(L, -1);
				if (f < 0 || f > 0x7FFFFFFF || qFloor(f) != f)
					luaL_error(L, "Unicode code (%f) at index [%d] of table at argument #%d is invalid", f, j,
					           i);
				const int n = utf8EncodeCodepoint(static_cast<quint32>(f), utf8);
				out.append(reinterpret_cast<const char *>(utf8), n);
				lua_pop(L, 1);
			}
		}
		else
		{
			const double f = luaL_checknumber(L, i);
			if (f < 0 || f > 0x7FFFFFFF || qFloor(f) != f)
				luaL_error(L, "Unicode code (%f) at argument #%d is invalid", f, i);
			const int n = utf8EncodeCodepoint(static_cast<quint32>(f), utf8);
			out.append(reinterpret_cast<const char *>(utf8), n);
		}
	}
	lua_pushlstring(L, out.constData(), out.size());
	return 1;
}

static int luaUtilsUtf8Decode(lua_State *L)
{
	size_t      length = 0;
	const auto *data   = reinterpret_cast<const unsigned char *>(luaL_checklstring(L, 1, &length));
	lua_newtable(L);

	int    item = 1;
	size_t pos  = 0;
	while (pos < length)
	{
		const unsigned char c     = data[pos++];
		quint32             value = 0;
		if (c < 0x80)
			value = c;
		else
		{
			const int ab = utf8AdditionalBytes(c);
			if (ab < 0 || pos + static_cast<size_t>(ab) > length)
				luaL_error(L, "Bad UTF-8 string. Error at column %d", static_cast<int>(pos));

			switch (ab)
			{
			case 1:
				if ((c & 0x3E) == 0)
					luaL_error(L, "Bad UTF-8 string. Error at column %d", static_cast<int>(pos));
				value = c & 0x1F;
				break;
			case 2:
				if (c == 0xE0 && (data[pos] & 0x20) == 0)
					luaL_error(L, "Bad UTF-8 string. Error at column %d", static_cast<int>(pos));
				value = c & 0x0F;
				break;
			case 3:
				if (c == 0xF0 && (data[pos] & 0x30) == 0)
					luaL_error(L, "Bad UTF-8 string. Error at column %d", static_cast<int>(pos));
				value = c & 0x07;
				break;
			case 4:
				if (c == 0xF8 && (data[pos] & 0x38) == 0)
					luaL_error(L, "Bad UTF-8 string. Error at column %d", static_cast<int>(pos));
				value = c & 0x03;
				break;
			case 5:
				if (c == 0xFE || c == 0xFF || (c == 0xFC && (data[pos] & 0x3C) == 0))
					luaL_error(L, "Bad UTF-8 string. Error at column %d", static_cast<int>(pos));
				value = c & 0x01;
				break;
			default:
				luaL_error(L, "Bad UTF-8 string. Error at column %d", static_cast<int>(pos));
			}

			for (int j = 0; j < ab; ++j)
			{
				const unsigned char cc = data[pos++];
				if ((cc & 0xC0) != 0x80)
					luaL_error(L, "Bad UTF-8 string. Error at column %d", static_cast<int>(pos));
				const auto low6 = cc & 0x3F;
				value           = value << 6 | low6;
			}
		}

		lua_pushnumber(L, value);
		lua_rawseti(L, -2, item++);
	}

	return 1;
}

static int utf8ValidateAndMap(const unsigned char *data, const size_t length, int &errorColumn,
                              QVector<int> *starts)
{
	errorColumn = 0;
	if (starts)
		starts->clear();

	int    count = 0;
	size_t pos   = 0;
	while (pos < length)
	{
		if (starts)
			starts->push_back(static_cast<int>(pos));
		const size_t        start = pos;
		const unsigned char c     = data[pos++];
		if (c < 0x80)
		{
			++count;
			continue;
		}

		const int ab = utf8AdditionalBytes(c);
		if (ab < 0 || pos + static_cast<size_t>(ab) > length)
		{
			errorColumn = static_cast<int>(start) + 1;
			return -1;
		}

		if ((ab == 1 && (c & 0x3E) == 0) || (ab == 2 && c == 0xE0 && (data[pos] & 0x20) == 0) ||
		    (ab == 3 && c == 0xF0 && (data[pos] & 0x30) == 0) ||
		    (ab == 4 && c == 0xF8 && (data[pos] & 0x38) == 0) ||
		    (ab == 5 && (c == 0xFE || c == 0xFF || (c == 0xFC && (data[pos] & 0x3C) == 0))))
		{
			errorColumn = static_cast<int>(start) + 1;
			return -1;
		}

		for (int i = 0; i < ab; ++i)
		{
			if (const unsigned char cc = data[pos++]; (cc & 0xC0) != 0x80)
			{
				errorColumn = static_cast<int>(pos);
				return -1;
			}
		}

		++count;
	}

	return count;
}

static int luaUtilsUtf8Valid(lua_State *L)
{
	size_t      length      = 0;
	const auto *data        = reinterpret_cast<const unsigned char *>(luaL_checklstring(L, 1, &length));
	int         errorColumn = 0;
	const int   count       = utf8ValidateAndMap(data, length, errorColumn, nullptr);
	if (count < 0)
	{
		lua_pushnil(L);
		lua_pushnumber(L, errorColumn);
		return 2;
	}
	lua_pushnumber(L, count);
	return 1;
}

static int luaUtilsUtf8Sub(lua_State *L)
{
	size_t       length = 0;
	const char  *s      = luaL_checklstring(L, 1, &length);
	ptrdiff_t    start  = luaL_checkinteger(L, 2);
	ptrdiff_t    end    = luaL_optinteger(L, 3, -1);

	QVector<int> starts;
	int          errorColumn = 0;
	const int    utf8Length =
	    utf8ValidateAndMap(reinterpret_cast<const unsigned char *>(s), length, errorColumn, &starts);
	if (utf8Length < 0)
	{
		lua_pushnil(L);
		lua_pushnumber(L, errorColumn);
		return 2;
	}

	if (start < 0)
		start = utf8Length + start + 1;
	if (start < 1)
		start = 1;
	if (end < 0)
		end = utf8Length + end + 1;
	if (end > utf8Length)
		end = utf8Length;

	if (start > end)
	{
		lua_pushliteral(L, "");
		return 1;
	}

	const int startIndex = starts.at(static_cast<int>(start - 1));
	const int endIndexExclusive =
	    end >= utf8Length ? static_cast<int>(length) : starts.at(static_cast<int>(end));
	lua_pushlstring(L, s + startIndex, static_cast<size_t>(endIndexExclusive - startIndex));
	return 1;
}

static int luaUtilsXmlRead(lua_State *L)
{
	size_t           xmlLength = 0;
	const char      *xmlData   = luaL_checklstring(L, 1, &xmlLength);
	QXmlStreamReader xml(QString::fromUtf8(xmlData, static_cast<int>(xmlLength)));

	struct XmlNodeFrame
	{
			QString                        name;
			QList<QPair<QString, QString>> attrs;
			QString                        text;
			int                            childrenRef{LUA_REFNIL};
			int                            childIndex{1};
	};
	QVector<XmlNodeFrame> stack;

	QString               rootName;
	int                   rootRef = LUA_REFNIL;

	while (!xml.atEnd())
	{
		xml.readNext();
		if (xml.isStartElement())
		{
			XmlNodeFrame frame;
			frame.name = xml.name().toString();
			for (const auto attributes = xml.attributes(); const auto &attr : attributes)
				frame.attrs.push_back({attr.name().toString(), attr.value().toString()});
			stack.push_back(frame);
			if (rootName.isEmpty())
				rootName = frame.name;
		}
		else if (xml.isCharacters() && !xml.isWhitespace())
		{
			if (!stack.isEmpty())
				stack.back().text += xml.text().toString();
		}
		else if (xml.isEndElement())
		{
			if (stack.isEmpty())
				continue;
			XmlNodeFrame frame = stack.takeLast();
			lua_newtable(L);
			lua_pushstring(L, "name");
			lua_pushstring(L, frame.name.toUtf8().constData());
			lua_rawset(L, -3);
			if (!frame.text.isEmpty())
			{
				lua_pushstring(L, "value");
				lua_pushstring(L, frame.text.toUtf8().constData());
				lua_rawset(L, -3);
			}
			if (!frame.attrs.isEmpty())
			{
				lua_pushstring(L, "attributes");
				lua_newtable(L);
				for (const auto &[k, v] : frame.attrs)
				{
					lua_pushstring(L, k.toUtf8().constData());
					lua_pushstring(L, v.toUtf8().constData());
					lua_rawset(L, -3);
				}
				lua_rawset(L, -3);
			}
			if (frame.childrenRef != LUA_REFNIL)
			{
				lua_pushstring(L, "nodes");
				lua_rawgeti(L, LUA_REGISTRYINDEX, frame.childrenRef);
				lua_rawset(L, -3);
				luaL_unref(L, LUA_REGISTRYINDEX, frame.childrenRef);
			}

			const int nodeRef = luaL_ref(L, LUA_REGISTRYINDEX);
			if (stack.isEmpty())
				rootRef = nodeRef;
			else
			{
				XmlNodeFrame &parent = stack.back();
				if (parent.childrenRef == LUA_REFNIL)
				{
					lua_newtable(L);
					parent.childrenRef = luaL_ref(L, LUA_REGISTRYINDEX);
				}
				lua_rawgeti(L, LUA_REGISTRYINDEX, parent.childrenRef);
				lua_rawgeti(L, LUA_REGISTRYINDEX, nodeRef);
				lua_rawseti(L, -2, parent.childIndex++);
				lua_pop(L, 1);
				luaL_unref(L, LUA_REGISTRYINDEX, nodeRef);
			}
		}
	}

	if (xml.hasError())
	{
		lua_pushnil(L);
		lua_pushstring(L, xml.errorString().toUtf8().constData());
		lua_pushnumber(L, static_cast<lua_Number>(xml.lineNumber()));
		return 3;
	}

	if (rootRef == LUA_REFNIL)
	{
		lua_pushnil(L);
		lua_pushstring(L, "No XML root element.");
		lua_pushnumber(L, 0);
		return 3;
	}

	lua_rawgeti(L, LUA_REGISTRYINDEX, rootRef);
	luaL_unref(L, LUA_REGISTRYINDEX, rootRef);
	lua_pushstring(L, rootName.toUtf8().constData());
	return 2;
}

static QString luaUtilsFilterString(lua_State *L)
{
	constexpr int kFiltersIndex = 4;
	if (!lua_istable(L, kFiltersIndex))
		return QStringLiteral("All files (*.*)");

	QStringList filters;
	for (lua_pushnil(L); lua_next(L, kFiltersIndex) != 0; lua_pop(L, 1))
	{
		if (!lua_isstring(L, -2) || !lua_isstring(L, -1))
			luaL_error(L, "table of filters must be suffix/description pair");

		const QString suffix      = QString::fromUtf8(lua_tostring(L, -2));
		const QString description = QString::fromUtf8(lua_tostring(L, -1));
		if (suffix.isEmpty())
			continue;
		filters << QStringLiteral("%1 (*.%2)").arg(description, suffix);
	}

	if (filters.isEmpty())
		return QStringLiteral("All files (*.*)");
	return filters.join(QStringLiteral(";;"));
}

static int luaUtilsFilePicker(lua_State *L)
{
	const char *title            = luaL_optstring(L, 1, "");
	const char *defaultName      = luaL_optstring(L, 2, "");
	const char *defaultExtension = luaL_optstring(L, 3, "");
	const bool  saveDialog       = optBool(L, 5, false);

	QString     initialDir = QDir::currentPath();
	if (WorldRuntime *runtime = runtimeFromLua(L))
	{
		if (const QString runtimeDir = runtime->fileBrowsingDirectory(); !runtimeDir.isEmpty())
			initialDir = runtimeDir;
	}

	QFileDialog dialog(resolveMainWindow(), QString::fromUtf8(title), initialDir, luaUtilsFilterString(L));
	dialog.setAcceptMode(saveDialog ? QFileDialog::AcceptSave : QFileDialog::AcceptOpen);
	dialog.setFileMode(saveDialog ? QFileDialog::AnyFile : QFileDialog::ExistingFile);
	if (defaultName[0] != '\0')
		dialog.selectFile(QString::fromUtf8(defaultName));
	if (defaultExtension[0] != '\0')
		dialog.setDefaultSuffix(QString::fromUtf8(defaultExtension));

	if (dialog.exec() != QDialog::Accepted)
	{
		lua_pushnil(L);
		return 1;
	}

	const QStringList files = dialog.selectedFiles();
	if (files.isEmpty())
	{
		lua_pushnil(L);
		return 1;
	}

	const QString &path = files.first();
	if (WorldRuntime *runtime = runtimeFromLua(L))
		runtime->setFileBrowsingDirectory(QFileInfo(path).absolutePath());
	const QByteArray bytes = QDir::toNativeSeparators(path).toUtf8();
	lua_pushlstring(L, bytes.constData(), bytes.size());
	return 1;
}

static int luaUtilsTextBox(lua_State *L, const bool multiline)
{
	size_t      inputmsgLen   = 0;
	size_t      inputtitleLen = 0;
	const char *inputmsg      = luaL_checklstring(L, 1, &inputmsgLen);
	const char *inputtitle    = luaL_optlstring(L, 2, "QMud", &inputtitleLen);
	const char *inputdefault  = luaL_optstring(L, 3, "");
	const char *inputfont     = luaL_optstring(L, 4, "");
	const int   inputsize     = static_cast<int>(luaL_optnumber(L, 5, 9));

	if (inputmsgLen > 1000)
		luaL_error(L, "inputbox message too long (max 1000 characters)");
	if (inputtitleLen > 100)
		luaL_error(L, "inputbox title too long (max 100 characters)");

	int     maxLength = 0;
	bool    readOnly  = false;
	QString okLabel;
	QString cancelLabel;
	int     boxWidth  = 0;
	int     boxHeight = 0;

	if (constexpr int extraIndex = 6; lua_istable(L, extraIndex))
	{
		auto getInt = [&](const char *key) -> int
		{
			lua_getfield(L, extraIndex, key);
			int value = 0;
			if (!lua_isnil(L, -1))
				value = static_cast<int>(luaL_checknumber(L, -1));
			lua_pop(L, 1);
			return value;
		};
		auto getBool = [&](const char *key) -> bool
		{
			lua_getfield(L, extraIndex, key);
			const bool value = lua_toboolean(L, -1) != 0;
			lua_pop(L, 1);
			return value;
		};
		auto getString = [&](const char *key) -> QString
		{
			lua_getfield(L, extraIndex, key);
			QString value;
			if (lua_isstring(L, -1))
				value = QString::fromUtf8(lua_tostring(L, -1));
			lua_pop(L, 1);
			return value;
		};

		maxLength   = getInt("max_length");
		readOnly    = getBool("read_only");
		okLabel     = getString("ok_button");
		cancelLabel = getString("cancel_button");
		boxWidth    = getInt("box_width");
		boxHeight   = getInt("box_height");
	}

	QDialog dialog(resolveMainWindow());
	dialog.setWindowTitle(QString::fromUtf8(inputtitle));
	auto                              layout = std::make_unique<QVBoxLayout>(&dialog);
	std::unique_ptr<QLabel>           messageLabel;
	std::unique_ptr<QPlainTextEdit>   multilineEdit;
	std::unique_ptr<QLineEdit>        singleLineEdit;
	std::unique_ptr<QDialogButtonBox> buttons;

	if (inputmsg[0] != '\0')
	{
		messageLabel = std::make_unique<QLabel>(QString::fromUtf8(inputmsg), &dialog);
		messageLabel->setWordWrap(true);
		layout->addWidget(messageLabel.get());
	}

	QFont font;
	if (inputfont[0] != '\0')
		font = qmudPreferredMonospaceFont(QString::fromUtf8(inputfont), inputsize);
	else if (inputsize > 0)
		font = qmudPreferredMonospaceFont(QString(), inputsize);

	QString result;
	if (multiline)
	{
		multilineEdit = std::make_unique<QPlainTextEdit>(&dialog);
		multilineEdit->setPlainText(QString::fromUtf8(inputdefault));
		multilineEdit->setReadOnly(readOnly);
		if (font.pointSize() > 0 || !font.family().isEmpty())
			multilineEdit->setFont(font);
		layout->addWidget(multilineEdit.get());
		result = multilineEdit->toPlainText();
		QObject::connect(&dialog, &QDialog::accepted,
		                 [edit = multilineEdit.get(), &result] { result = edit->toPlainText(); });
	}
	else
	{
		singleLineEdit = std::make_unique<QLineEdit>(&dialog);
		singleLineEdit->setText(QString::fromUtf8(inputdefault));
		singleLineEdit->setReadOnly(readOnly);
		if (maxLength > 0)
			singleLineEdit->setMaxLength(maxLength);
		if (font.pointSize() > 0 || !font.family().isEmpty())
			singleLineEdit->setFont(font);
		layout->addWidget(singleLineEdit.get());
		result = singleLineEdit->text();
		QObject::connect(&dialog, &QDialog::accepted,
		                 [edit = singleLineEdit.get(), &result] { result = edit->text(); });
	}

	buttons = std::make_unique<QDialogButtonBox>(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
	if (!okLabel.isEmpty())
		buttons->button(QDialogButtonBox::Ok)->setText(okLabel);
	if (!cancelLabel.isEmpty())
		buttons->button(QDialogButtonBox::Cancel)->setText(cancelLabel);
	QObject::connect(buttons.get(), &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	QObject::connect(buttons.get(), &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	layout->addWidget(buttons.get());

	if (boxWidth > 0 || boxHeight > 0)
		dialog.resize(qMax(200, boxWidth), qMax(120, boxHeight));

	if (dialog.exec() != QDialog::Accepted)
	{
		lua_pushnil(L);
		return 1;
	}

	const QByteArray outBytes = result.toUtf8();
	lua_pushlstring(L, outBytes.constData(), outBytes.size());
	return 1;
}

static int luaUtilsInputBox(lua_State *L)
{
	return luaUtilsTextBox(L, false);
}

static int luaUtilsEditBox(lua_State *L)
{
	return luaUtilsTextBox(L, true);
}

static int luaUtilsListBox(lua_State *L)
{
	size_t        choosemsgLen   = 0;
	size_t        choosetitleLen = 0;
	const char   *choosemsg      = luaL_checklstring(L, 1, &choosemsgLen);
	const char   *choosetitle    = luaL_optlstring(L, 2, "QMud", &choosetitleLen);
	constexpr int tableIndex     = 3;

	if (choosemsgLen > 1000)
		luaL_error(L, "message too long (max 1000 characters)");
	if (choosetitleLen > 100)
		luaL_error(L, "title too long (max 100 characters)");
	if (!lua_istable(L, tableIndex))
		luaL_error(L, "must have table of choices as 3rd argument");

	const bool haveDefault     = lua_gettop(L) >= 4 && !lua_isnil(L, 4);
	const bool defaultIsNumber = haveDefault && lua_type(L, 4) == LUA_TNUMBER;
	QString    defaultString;
	lua_Number defaultNumber = 0;
	if (haveDefault)
	{
		if (!lua_isstring(L, 4))
			luaL_error(L, "default key must be string or number");
		if (defaultIsNumber)
			defaultNumber = lua_tonumber(L, 4);
		else
			defaultString = QString::fromUtf8(lua_tostring(L, 4));
	}

	struct Choice
	{
			bool       isNumber{false};
			lua_Number numberKey{0};
			QString    stringKey;
			QString    value;
	};
	QVector<Choice> choices;
	int             defaultIndex = -1;

	for (lua_pushnil(L); lua_next(L, tableIndex) != 0; lua_pop(L, 1))
	{
		if (!lua_isstring(L, -1))
			luaL_error(L, "table must have string or number values");

		Choice choice;
		choice.value = QString::fromUtf8(lua_tostring(L, -1));

		if (lua_type(L, -2) == LUA_TSTRING)
		{
			choice.stringKey = QString::fromUtf8(lua_tostring(L, -2));
			if (haveDefault && !defaultIsNumber && choice.stringKey == defaultString)
				defaultIndex = static_cast<int>(choices.size());
		}
		else
		{
			choice.isNumber  = true;
			choice.numberKey = lua_tonumber(L, -2);
			if (haveDefault && defaultIsNumber && choice.numberKey == defaultNumber)
				defaultIndex = static_cast<int>(choices.size());
		}

		choices.push_back(choice);
	}

	QDialog dialog(resolveMainWindow());
	dialog.setWindowTitle(QString::fromUtf8(choosetitle));
	auto layout = std::make_unique<QVBoxLayout>(&dialog);
	auto label  = std::make_unique<QLabel>(QString::fromUtf8(choosemsg), &dialog);
	auto list   = std::make_unique<QListWidget>(&dialog);
	auto buttons =
	    std::make_unique<QDialogButtonBox>(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
	label->setWordWrap(true);
	layout->addWidget(label.get());

	for (const Choice &choice : choices)
		list->addItem(choice.value);
	if (defaultIndex >= 0)
		list->setCurrentRow(defaultIndex);
	layout->addWidget(list.get());
	QObject::connect(buttons.get(), &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	QObject::connect(buttons.get(), &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	QObject::connect(list.get(), &QListWidget::itemDoubleClicked, &dialog, &QDialog::accept);
	layout->addWidget(buttons.get());

	if (dialog.exec() != QDialog::Accepted)
	{
		lua_pushnil(L);
		return 1;
	}

	const int row = list->currentRow();
	if (row < 0 || row >= choices.size())
	{
		lua_pushnil(L);
		return 1;
	}

	if (const Choice &choice = choices.at(row); choice.isNumber)
		lua_pushnumber(L, choice.numberKey);
	else
	{
		const QByteArray bytes = choice.stringKey.toUtf8();
		lua_pushlstring(L, bytes.constData(), bytes.size());
	}
	return 1;
}

static int luaUtilsMultiListBox(lua_State *L)
{
	const char   *title      = luaL_optstring(L, 1, "");
	constexpr int tableIndex = 2;
	if (!lua_istable(L, tableIndex))
		luaL_error(L, "must have table of choices as second argument");

	QSet<QString> defaultKeys;
	if (lua_istable(L, 3))
	{
		for (lua_pushnil(L); lua_next(L, 3) != 0; lua_pop(L, 1))
		{
			if (!lua_isstring(L, -2))
				luaL_error(L, "defaults table must have string keys");
			const QString key = QString::fromUtf8(lua_tostring(L, -2));
			if (!(lua_isnil(L, -1) || (lua_isboolean(L, -1) && !lua_toboolean(L, -1))))
				defaultKeys.insert(key);
		}
	}

	const bool noSort = optBool(L, 4, false);

	struct Entry
	{
			bool       isNumber{false};
			lua_Number numberKey{0};
			QString    stringKey;
			QString    value;
	};
	QVector<Entry> entries;

	for (lua_pushnil(L); lua_next(L, tableIndex) != 0; lua_pop(L, 1))
	{
		if (!(lua_isstring(L, -2) || lua_isnumber(L, -2)))
			luaL_error(L, "table must have string or number keys");
		if (!(lua_isstring(L, -1) || lua_isnumber(L, -1)))
			luaL_error(L, "table must have string or number values");

		Entry e;
		e.value = QString::fromUtf8(lua_tostring(L, -1));
		if (lua_type(L, -2) == LUA_TSTRING)
			e.stringKey = QString::fromUtf8(lua_tostring(L, -2));
		else
		{
			e.isNumber  = true;
			e.numberKey = lua_tonumber(L, -2);
		}
		entries.push_back(e);
	}

	if (!noSort)
	{
		std::ranges::sort(entries, [](const Entry &a, const Entry &b)
		                  { return a.value.compare(b.value, Qt::CaseInsensitive) < 0; });
	}

	QDialog dialog(resolveMainWindow());
	dialog.setWindowTitle(QString::fromUtf8(title));
	auto layout = std::make_unique<QVBoxLayout>(&dialog);
	auto list   = std::make_unique<QListWidget>(&dialog);
	auto buttons =
	    std::make_unique<QDialogButtonBox>(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
	list->setSelectionMode(QAbstractItemView::MultiSelection);
	for (int i = 0; i < entries.size(); ++i)
	{
		const Entry &e = entries.at(i);
		list->addItem(e.value);
		auto *item = list->item(list->count() - 1);
		item->setData(Qt::UserRole, i);
		if (!e.isNumber && defaultKeys.contains(e.stringKey))
			item->setSelected(true);
	}
	layout->addWidget(list.get());
	QObject::connect(buttons.get(), &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	QObject::connect(buttons.get(), &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	layout->addWidget(buttons.get());

	if (dialog.exec() != QDialog::Accepted)
	{
		lua_pushnil(L);
		return 1;
	}

	lua_newtable(L);
	for (QListWidgetItem *item : list->selectedItems())
	{
		const int index = item->data(Qt::UserRole).toInt();
		if (index < 0 || index >= entries.size())
			continue;
		if (const Entry &e = entries.at(index); e.isNumber)
			lua_pushnumber(L, e.numberKey);
		else
			lua_pushstring(L, e.stringKey.toUtf8().constData());
		lua_pushboolean(L, 1);
		lua_rawset(L, -3);
	}
	return 1;
}

static int luaUtilsActivateNotepad(lua_State *L)
{
	const char *title = luaL_checkstring(L, 1);
	MainWindow *mw    = resolveMainWindow();
	bool        ok    = false;
	if (mw)
		ok = mw->activateNotepad(QString::fromUtf8(title));
	lua_pushboolean(L, ok);
	return 1;
}

static int luaUtilsAppendToNotepad(lua_State *L)
{
	const char   *title    = luaL_checkstring(L, 1);
	const char   *contents = luaL_checkstring(L, 2);
	const bool    replace  = optBool(L, 3, false);
	auto         *engine   = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime  = engine ? engine->worldRuntime() : nullptr;
	MainWindow   *mw       = resolveMainWindow();
	bool          ok       = false;
	if (mw)
		ok = mw->appendToNotepad(QString::fromUtf8(title), QString::fromUtf8(contents), replace, runtime);
	lua_pushboolean(L, ok);
	return 1;
}

static QByteArray foldBase64WithCrlf(const QByteArray &encoded, const bool multiline)
{
	if (!multiline || encoded.isEmpty())
		return encoded;

	constexpr int kWrapColumn = 76;
	const int     encodedSize = sizeToInt(encoded.size());
	QByteArray    out;
	out.reserve(encodedSize + (encodedSize / kWrapColumn + 1) * 2);
	for (int i = 0; i < encodedSize; i += kWrapColumn)
	{
		const int remaining = encodedSize - i;
		const int take      = remaining < kWrapColumn ? remaining : kWrapColumn;
		out.append(encoded.constData() + i, take);
		if (i + take < encodedSize)
			out.append("\r\n", 2);
	}
	return out;
}

static int luaUtilsBase64Encode(lua_State *L)
{
	size_t           len       = 0;
	const char      *data      = luaL_checklstring(L, 1, &len);
	const bool       multiline = optBool(L, 2, false);
	const QByteArray input(data, static_cast<int>(len));
	const QByteArray encoded = foldBase64WithCrlf(input.toBase64(), multiline);
	lua_pushlstring(L, encoded.constData(), encoded.size());
	return 1;
}

static int luaUtilsBase64Decode(lua_State *L)
{
	size_t           len  = 0;
	const char      *data = luaL_checklstring(L, 1, &len);
	const QByteArray input(data, static_cast<int>(len));
	const QByteArray decoded = QByteArray::fromBase64(input, QByteArray::AbortOnBase64DecodingErrors);
	if (decoded.isNull())
	{
		lua_pushnil(L);
		return 1;
	}
	lua_pushlstring(L, decoded.constData(), decoded.size());
	return 1;
}

static int luaUtilsCallbacksList(lua_State *L)
{
	static const char *kCallbacks[] = {"OnPluginBroadcast",
	                                   "OnPluginChatAccept",
	                                   "OnPluginChatDisplay",
	                                   "OnPluginChatMessage",
	                                   "OnPluginChatMessageOut",
	                                   "OnPluginChatNewUser",
	                                   "OnPluginChatUserDisconnect",
	                                   "OnPluginClose",
	                                   "OnPluginCommand",
	                                   "OnPluginCommandChanged",
	                                   "OnPluginCommandEntered",
	                                   "OnPluginConnect",
	                                   "OnPluginDisable",
	                                   "OnPluginDisconnect",
	                                   "OnPluginDrawOutputWindow",
	                                   "OnPluginEnable",
	                                   "OnPluginGetFocus",
	                                   "OnPluginIacGa",
	                                   "OnPluginInstall",
	                                   "OnPluginLineReceived",
	                                   "OnPluginListChanged",
	                                   "OnPluginLoseFocus",
	                                   "OnPluginMouseMoved",
	                                   "OnPluginMXPcloseTag",
	                                   "OnPluginMXPerror",
	                                   "OnPluginMXPopenTag",
	                                   "OnPluginMXPsetEntity",
	                                   "OnPluginMXPsetVariable",
	                                   "OnPluginMXPstart",
	                                   "OnPluginMXPstop",
	                                   "OnPluginPacketReceived",
	                                   "OnPluginPartialLine",
	                                   "OnPluginPlaySound",
	                                   "OnPluginSaveState",
	                                   "OnPluginScreendraw",
	                                   "OnPluginSelectionChanged",
	                                   "OnPluginSend",
	                                   "OnPluginSent",
	                                   "OnPluginTabComplete",
	                                   "OnPluginTelnetOption",
	                                   "OnPluginTelnetRequest",
	                                   "OnPluginTelnetSubnegotiation",
	                                   "OnPluginTick",
	                                   "OnPluginTrace",
	                                   "OnPluginPacketDebug",
	                                   "OnPluginWorldOutputResized",
	                                   "OnPluginWorldSave",
	                                   nullptr};

	lua_newtable(L);
	int index = 1;
	for (int i = 0; kCallbacks[i] != nullptr; ++i)
	{
		lua_pushstring(L, kCallbacks[i]);
		lua_rawseti(L, -2, index++);
	}
	return 1;
}

static int luaUtilsChoose(lua_State *L)
{
	return luaUtilsListBox(L);
}

static int luaUtilsDirectoryPicker(lua_State *L)
{
	const QString title      = QString::fromUtf8(luaL_optstring(L, 1, ""));
	const QString defaultDir = QString::fromUtf8(luaL_optstring(L, 2, ""));
	const QString initial    = defaultDir.isEmpty() ? QDir::currentPath() : defaultDir;
	const QString chosen     = QFileDialog::getExistingDirectory(resolveMainWindow(), title, initial);
	if (chosen.isEmpty())
	{
		lua_pushnil(L);
		return 1;
	}
	const QByteArray bytes = chosen.toUtf8();
	lua_pushlstring(L, bytes.constData(), bytes.size());
	return 1;
}

static int luaUtilsEditDistance(lua_State *L)
{
	const QString source   = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString target   = QString::fromUtf8(luaL_checkstring(L, 2));
	const int     distance = qmudEditDistance(source, target);
	lua_pushinteger(L, distance);
	return 1;
}

static int luaUtilsFilterPicker(lua_State *L)
{
	if (!lua_istable(L, 1))
		luaL_error(L, "must have table of choices as first argument");

	const QString title         = QString::fromUtf8(luaL_optstring(L, 2, "Filter"));
	const QString initialFilter = QString::fromUtf8(luaL_optstring(L, 3, ""));
	const bool    noSort        = optBool(L, 4, false);

	struct Choice
	{
			bool       isNumber{false};
			lua_Number numberKey{0};
			QString    stringKey;
			QString    value;
	};
	QVector<Choice> choices;
	for (lua_pushnil(L); lua_next(L, 1) != 0; lua_pop(L, 1))
	{
		if (!lua_isstring(L, -1))
			luaL_error(L, "table must have string or number values");
		Choice choice;
		choice.value = QString::fromUtf8(lua_tostring(L, -1));
		if (lua_type(L, -2) == LUA_TSTRING)
			choice.stringKey = QString::fromUtf8(lua_tostring(L, -2));
		else
		{
			choice.isNumber  = true;
			choice.numberKey = lua_tonumber(L, -2);
		}
		choices.push_back(choice);
	}

	if (!noSort)
	{
		std::ranges::sort(choices, [](const Choice &a, const Choice &b)
		                  { return a.value.localeAwareCompare(b.value) < 0; });
	}

	QDialog dialog(resolveMainWindow());
	dialog.setWindowTitle(title);
	auto layout = std::make_unique<QVBoxLayout>(&dialog);
	auto filter = std::make_unique<QLineEdit>(&dialog);
	auto list   = std::make_unique<QListWidget>(&dialog);
	auto buttons =
	    std::make_unique<QDialogButtonBox>(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
	filter->setPlaceholderText(QStringLiteral("Filter"));
	filter->setText(initialFilter);
	layout->addWidget(filter.get());
	layout->addWidget(list.get());
	auto refill = [listWidget = list.get(), filterWidget = filter.get(), &choices]
	{
		const QString needle = filterWidget->text().trimmed();
		listWidget->clear();
		for (int i = 0; i < choices.size(); ++i)
		{
			if (!needle.isEmpty() && !choices[i].value.contains(needle, Qt::CaseInsensitive))
				continue;
			listWidget->addItem(choices[i].value);
			QListWidgetItem *item = listWidget->item(listWidget->count() - 1);
			item->setData(Qt::UserRole, i);
		}
	};
	QObject::connect(filter.get(), &QLineEdit::textChanged, &dialog, refill);
	refill();
	QObject::connect(buttons.get(), &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	QObject::connect(buttons.get(), &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	QObject::connect(list.get(), &QListWidget::itemDoubleClicked, &dialog, &QDialog::accept);
	layout->addWidget(buttons.get());

	if (dialog.exec() != QDialog::Accepted || !list->currentItem())
	{
		lua_pushnil(L);
		return 1;
	}
	const int idx = list->currentItem()->data(Qt::UserRole).toInt();
	if (idx < 0 || idx >= choices.size())
	{
		lua_pushnil(L);
		return 1;
	}
	if (const Choice &choice = choices.at(idx); choice.isNumber)
		lua_pushnumber(L, choice.numberKey);
	else
	{
		const QByteArray bytes = choice.stringKey.toUtf8();
		lua_pushlstring(L, bytes.constData(), bytes.size());
	}
	return 1;
}

static int luaUtilsColourCube(lua_State *L)
{
	const int which = static_cast<int>(luaL_checkinteger(L, 1));
	if (which != 1 && which != 2)
	{
		luaL_error(L, "Unknown option");
		return 0;
	}
	AppController *app = AppController::instance();
	if (!app)
	{
		luaL_error(L, "App controller is not available");
		return 0;
	}
	app->setXtermColourCube(which);
	return 0;
}

static int luaUtilsCompress(lua_State *L)
{
	size_t           len    = 0;
	const char      *data   = luaL_checklstring(L, 1, &len);
	const int        method = static_cast<int>(luaL_optinteger(L, 2, -1));
	const QByteArray input(data, static_cast<int>(len));
	const QByteArray compressed = qCompress(input, method);
	lua_pushlstring(L, compressed.constData(), compressed.size());
	return 1;
}

static int luaUtilsDecompress(lua_State *L)
{
	size_t           len  = 0;
	const char      *data = luaL_checklstring(L, 1, &len);
	const QByteArray input(data, static_cast<int>(len));
	const QByteArray out = qUncompress(input);
	if (out.isNull())
	{
		lua_pushnil(L);
		return 1;
	}
	lua_pushlstring(L, out.constData(), out.size());
	return 1;
}

static void registerUtilsBindings(lua_State *L, LuaCallbackEngine *engine)
{
	lua_getglobal(L, "utils");
	if (!lua_istable(L, -1))
	{
		lua_pop(L, 1);
		lua_newtable(L);
	}

	auto setFn = [L, engine](const char *name, const lua_CFunction fn)
	{
		lua_pushlightuserdata(L, engine);
		lua_pushcclosure(L, fn, 1);
		lua_setfield(L, -2, name);
	};

	static const LuaBindingEntry kUtilsBindings[] = {
	    {"activatenotepad",     luaUtilsActivateNotepad    },
	    {"appendtonotepad",     luaUtilsAppendToNotepad    },
	    {"base64decode",        luaUtilsBase64Decode       },
	    {"base64encode",        luaUtilsBase64Encode       },
	    {"callbackslist",       luaUtilsCallbacksList      },
	    {"choose",              luaUtilsChoose             },
	    {"colourcube",          luaUtilsColourCube         },
	    {"compress",            luaUtilsCompress           },
	    {"decompress",          luaUtilsDecompress         },
	    {"directorypicker",     luaUtilsDirectoryPicker    },
	    {"edit_distance",       luaUtilsEditDistance       },
	    {"editbox",             luaUtilsEditBox            },
	    {"filepicker",          luaUtilsFilePicker         },
	    {"filterpicker",        luaUtilsFilterPicker       },
	    {"fontpicker",          luaUtilsFontPicker         },
	    {"fromhex",             luaUtilsFromHex            },
	    {"functionargs",        luaUtilsFunctionArgs       },
	    {"functionlist",        luaUtilsFunctionList       },
	    {"getfontfamilies",     luaUtilsGetFontFamilies    },
	    {"glyph_available",     luaUtilsGlyphAvailable     },
	    {"hash",                luaUtilsHash               },
	    {"info",                luaUtilsInfo               },
	    {"infotypes",           luaUtilsInfoTypes          },
	    {"inputbox",            luaUtilsInputBox           },
	    {"listbox",             luaUtilsListBox            },
	    {"md5",	             luaUtilsMd5                },
	    {"menufontsize",        luaUtilsMenuFontSize       },
	    {"metaphone",           luaUtilsMetaphone          },
	    {"msgbox",              luaUtilsMsgBox             },
	    {"multilistbox",        luaUtilsMultiListBox       },
	    {"readdir",             luaUtilsReadDir            },
	    {"reload_global_prefs", luaUtilsReloadGlobalPrefs  },
	    {"sendtofront",         luaUtilsSendToFront        },
	    {"setbackgroundcolour", luaUtilsSetBackgroundColour},
	    {"sha256",              luaUtilsSha256             },
	    {"shellexecute",        luaUtilsShellExecute       },
	    {"showdebugstatus",     luaUtilsShowDebugStatus    },
	    {"spellcheckdialog",    luaUtilsSpellCheckDialog   },
	    {"split",               luaUtilsSplit              },
	    {"timer",               luaUtilsTimer              },
	    {"tohex",               luaUtilsToHex              },
	    {"umsgbox",             luaUtilsUMsgBox            },
	    {"utf8convert",         luaUtilsUtf8Convert        },
	    {"utf8decode",          luaUtilsUtf8Decode         },
	    {"utf8encode",          luaUtilsUtf8Encode         },
	    {"utf8sub",             luaUtilsUtf8Sub            },
	    {"utf8valid",           luaUtilsUtf8Valid          },
	    {"xmlread",             luaUtilsXmlRead            },
	};
	for (const auto &[name, function] : kUtilsBindings)
		setFn(name, function);

	lua_setglobal(L, "utils");
}

namespace
{
	struct RexPattern
	{
			QRegularExpression regex;
			int                compileFlags{0};
	};

	constexpr auto kRexMetaName          = "rex_pcremeta";
	constexpr int  kPCRE_CASELESS        = 0x00000001;
	constexpr int  kPCRE_MULTILINE       = 0x00000002;
	constexpr int  kPCRE_DOTALL          = 0x00000004;
	constexpr int  kPCRE_EXTENDED        = 0x00000008;
	constexpr int  kPCRE_ANCHORED        = 0x00000010;
	constexpr int  kPCRE_DOLLAR_ENDONLY  = 0x00000020;
	constexpr int  kPCRE_EXTRA           = 0x00000040;
	constexpr int  kPCRE_NOTBOL          = 0x00000080;
	constexpr int  kPCRE_NOTEOL          = 0x00000100;
	constexpr int  kPCRE_UNGREEDY        = 0x00000200;
	constexpr int  kPCRE_NOTEMPTY        = 0x00000400;
	constexpr int  kPCRE_UTF8            = 0x00000800;
	constexpr int  kPCRE_AUTO_CALLOUT    = 0x00004000;
	constexpr int  kPCRE_PARTIAL         = 0x00008000;
	constexpr int  kPCRE_NO_AUTO_CAPTURE = 0x00001000;
	constexpr int  kPCRE_FIRSTLINE       = 0x00040000;
	constexpr int  kPCRE_DUPNAMES        = 0x00080000;
	constexpr int  kPCRE_NEWLINE_CR      = 0x00100000;
	constexpr int  kPCRE_NEWLINE_LF      = 0x00200000;
	constexpr int  kPCRE_NEWLINE_CRLF    = 0x00300000;
	constexpr int  kPCRE_NEWLINE_ANY     = 0x00400000;
	constexpr int  kPCRE_NEWLINE_ANYCRLF = 0x00500000;

	int            rexStartOffset(lua_State *L, const int len)
	{
		constexpr int kStartOffsetArg = 3;
		int           startoffset     = static_cast<int>(luaL_optinteger(L, kStartOffsetArg, 1));
		if (startoffset > 0)
			startoffset--;
		else if (startoffset < 0)
		{
			startoffset += len;
			if (startoffset < 0)
				startoffset = 0;
		}
		return startoffset;
	}

	RexPattern *rexCheck(lua_State *L)
	{
		auto *ud = static_cast<RexPattern **>(luaL_checkudata(L, 1, kRexMetaName));
		if (!ud || !*ud)
		{
			luaL_argerror(L, 1, "compiled regexp expected");
			return nullptr;
		}
		return *ud;
	}

	void rexPushSubstrings(lua_State *L, const QRegularExpressionMatch &m)
	{
		lua_newtable(L);
		const int last = m.lastCapturedIndex();
		for (int i = 1; i <= last; ++i)
		{
			const QString captured = m.captured(i);
			if (m.capturedStart(i) >= 0)
				lua_pushstring(L, captured.toUtf8().constData());
			else
				lua_pushboolean(L, 0);
			lua_rawseti(L, -2, i);
		}

		const QStringList names = m.regularExpression().namedCaptureGroups();
		for (int i = 1; i < names.size(); ++i)
		{
			const QString &name = names.at(i);
			if (name.isEmpty())
				continue;
			lua_pushstring(L, name.toUtf8().constData());
			if (m.capturedStart(i) >= 0)
				lua_pushstring(L, m.captured(i).toUtf8().constData());
			else
				lua_pushboolean(L, 0);
			lua_rawset(L, -3);
		}
	}

	void rexPushOffsets(lua_State *L, const QRegularExpressionMatch &m)
	{
		lua_newtable(L);
		int       j    = 1;
		const int last = m.lastCapturedIndex();
		for (int i = 1; i <= last; ++i)
		{
			const auto s = m.capturedStart(i);
			const auto e = m.capturedEnd(i);
			if (s >= 0)
			{
				lua_pushinteger(L, s + 1);
				lua_rawseti(L, -2, j++);
				lua_pushinteger(L, e);
				lua_rawseti(L, -2, j++);
			}
			else
			{
				lua_pushboolean(L, 0);
				lua_rawseti(L, -2, j++);
				lua_pushboolean(L, 0);
				lua_rawseti(L, -2, j++);
			}
		}
	}
} // namespace

static int luaRexVersion(lua_State *L)
{
	Q_UNUSED(L);
	lua_pushstring(L, "QRegularExpression (PCRE2)");
	return 1;
}

static int luaRexFlags(lua_State *L)
{
	struct RexFlagPair
	{
			const char *key;
			int         val;
	};
	static const RexFlagPair kPcreFlags[] = {
	    {"CASELESS",        kPCRE_CASELESS       },
	    {"MULTILINE",       kPCRE_MULTILINE      },
	    {"DOTALL",          kPCRE_DOTALL         },
	    {"EXTENDED",        kPCRE_EXTENDED       },
	    {"ANCHORED",        kPCRE_ANCHORED       },
	    {"DOLLAR_ENDONLY",  kPCRE_DOLLAR_ENDONLY },
	    {"EXTRA",           kPCRE_EXTRA          },
	    {"NOTBOL",          kPCRE_NOTBOL         },
	    {"NOTEOL",          kPCRE_NOTEOL         },
	    {"UNGREEDY",        kPCRE_UNGREEDY       },
	    {"NOTEMPTY",        kPCRE_NOTEMPTY       },
	    {"UTF8",            kPCRE_UTF8           },
	    {"AUTO_CALLOUT",    kPCRE_AUTO_CALLOUT   },
	    {"NO_AUTO_CAPTURE", kPCRE_NO_AUTO_CAPTURE},
	    {"PARTIAL",         kPCRE_PARTIAL        },
	    {"FIRSTLINE",       kPCRE_FIRSTLINE      },
	    {"DUPNAMES",        kPCRE_DUPNAMES       },
	    {"NEWLINE_CR",      kPCRE_NEWLINE_CR     },
	    {"NEWLINE_LF",      kPCRE_NEWLINE_LF     },
	    {"NEWLINE_CRLF",    kPCRE_NEWLINE_CRLF   },
	    {"NEWLINE_ANY",     kPCRE_NEWLINE_ANY    },
	    {"NEWLINE_ANYCRLF", kPCRE_NEWLINE_ANYCRLF},
	    {nullptr,           0	                }
    };
	lua_newtable(L);
	for (const RexFlagPair *p = kPcreFlags; p->key != nullptr; ++p)
	{
		lua_pushstring(L, p->key);
		lua_pushinteger(L, p->val);
		lua_rawset(L, -3);
	}
	return 1;
}

static int luaRexFlagsPosix(lua_State *L)
{
	lua_newtable(L);
	lua_pushstring(L, "EXTENDED");
	lua_pushinteger(L, kRegexExtended);
	lua_rawset(L, -3);
	lua_pushstring(L, "ICASE");
	lua_pushinteger(L, kRegexIgnoreCase);
	lua_rawset(L, -3);
	lua_pushstring(L, "NOSUB");
	lua_pushinteger(L, kRegexNoSub);
	lua_rawset(L, -3);
	lua_pushstring(L, "NEWLINE");
	lua_pushinteger(L, kRegexNewline);
	lua_rawset(L, -3);
	return 1;
}

static int luaRexNewCommon(lua_State *L)
{
	size_t      plen    = 0;
	const char *pattern = luaL_checklstring(L, 1, &plen);
	Q_UNUSED(plen);
	const int                          cflags = static_cast<int>(luaL_optinteger(L, 2, 0));

	QRegularExpression::PatternOptions options = QRegularExpression::NoPatternOption;
	if (cflags & kPCRE_CASELESS)
		options |= QRegularExpression::CaseInsensitiveOption;
	if (cflags & kPCRE_MULTILINE)
		options |= QRegularExpression::MultilineOption;
	if (cflags & kPCRE_DOTALL)
		options |= QRegularExpression::DotMatchesEverythingOption;
	if (cflags & kPCRE_EXTENDED)
		options |= QRegularExpression::ExtendedPatternSyntaxOption;
	if (cflags & kPCRE_UNGREEDY)
		options |= QRegularExpression::InvertedGreedinessOption;
	if (cflags & kPCRE_NO_AUTO_CAPTURE)
		options |= QRegularExpression::DontCaptureOption;

	QString patternText = QString::fromUtf8(pattern);
	QString prefix;
	if (cflags & kPCRE_FIRSTLINE)
		prefix += QStringLiteral("(*FIRSTLINE)");
	if (cflags & kPCRE_DUPNAMES)
		prefix += QStringLiteral("(?J)");
	const int newlineFlags = cflags & (kPCRE_NEWLINE_CR | kPCRE_NEWLINE_LF | kPCRE_NEWLINE_CRLF |
	                                   kPCRE_NEWLINE_ANY | kPCRE_NEWLINE_ANYCRLF);
	switch (newlineFlags)
	{
	case kPCRE_NEWLINE_CR:
		prefix += QStringLiteral("(*CR)");
		break;
	case kPCRE_NEWLINE_LF:
		prefix += QStringLiteral("(*LF)");
		break;
	case kPCRE_NEWLINE_CRLF:
		prefix += QStringLiteral("(*CRLF)");
		break;
	case kPCRE_NEWLINE_ANY:
		prefix += QStringLiteral("(*ANY)");
		break;
	case kPCRE_NEWLINE_ANYCRLF:
		prefix += QStringLiteral("(*ANYCRLF)");
		break;
	default:
		break;
	}
	if (!prefix.isEmpty())
		patternText.prepend(prefix);

	auto *ud            = static_cast<RexPattern **>(lua_newuserdata(L, sizeof(RexPattern *)));
	*ud                 = new RexPattern();
	(*ud)->compileFlags = cflags;
	(*ud)->regex        = QRegularExpression(patternText, options);
	luaL_getmetatable(L, kRexMetaName);
	lua_setmetatable(L, -2);

	if (!(*ud)->regex.isValid())
	{
		const QString err = (*ud)->regex.errorString();
		const auto    off = (*ud)->regex.patternErrorOffset();
		delete *ud;
		*ud = nullptr;
		luaL_error(L, "%s (pattern offset: %lld)", err.toUtf8().constData(), static_cast<long long>(off + 1));
	}

	return 1;
}

static int luaRexNew(lua_State *L)
{
	return luaRexNewCommon(L);
}

static int luaRexNewPosix(lua_State *L)
{
	return luaRexNewCommon(L);
}

static int luaRexToString(lua_State *L)
{
	RexPattern      *pat   = rexCheck(L);
	const QString    label = QStringLiteral("pcre_regex (0x%1)").arg(reinterpret_cast<quintptr>(pat), 0, 16);
	const QByteArray bytes = label.toUtf8();
	lua_pushlstring(L, bytes.constData(), bytes.size());
	return 1;
}

static int luaRexGc(lua_State *L)
{
	if (auto *ud = static_cast<RexPattern **>(luaL_checkudata(L, 1, kRexMetaName)); ud && *ud)
	{
		delete *ud;
		*ud = nullptr;
	}
	return 0;
}

static int luaRexMatchGeneric(lua_State *L, const bool offsetMode)
{
	RexPattern                      *pat         = rexCheck(L);
	size_t                           len         = 0;
	const char                      *text        = luaL_checklstring(L, 2, &len);
	const int                        startoffset = rexStartOffset(L, static_cast<int>(len));
	const int                        eflags      = static_cast<int>(luaL_optinteger(L, 4, 0));

	const QString                    subject      = QString::fromUtf8(text, static_cast<int>(len));
	QRegularExpression::MatchOptions matchOptions = QRegularExpression::NoMatchOption;
	if (eflags & kPCRE_ANCHORED || pat->compileFlags & kPCRE_ANCHORED)
		matchOptions |= QRegularExpression::AnchorAtOffsetMatchOption;

	const QRegularExpressionMatch m =
	    pat->regex.match(subject, startoffset, QRegularExpression::NormalMatch, matchOptions);
	if (!m.hasMatch())
		return 0;
	if (eflags & kPCRE_NOTEMPTY && m.capturedLength(0) == 0)
		return 0;

	lua_pushinteger(L, m.capturedStart(0) + 1);
	lua_pushinteger(L, m.capturedEnd(0));
	if (offsetMode)
		rexPushOffsets(L, m);
	else
		rexPushSubstrings(L, m);
	return 3;
}

static int luaRexExec(lua_State *L)
{
	return luaRexMatchGeneric(L, true);
}

static int luaRexMatch(lua_State *L)
{
	return luaRexMatchGeneric(L, false);
}

static int luaRexGmatch(lua_State *L)
{
	RexPattern *pat  = rexCheck(L);
	size_t      len  = 0;
	const char *text = luaL_checklstring(L, 2, &len);
	luaL_checktype(L, 3, LUA_TFUNCTION);
	const int     maxmatch = static_cast<int>(luaL_optinteger(L, 4, 0));
	const int     eflags   = static_cast<int>(luaL_optinteger(L, 5, 0));
	const bool    anchored = (eflags & kPCRE_ANCHORED) != 0 || (pat->compileFlags & kPCRE_ANCHORED) != 0;
	const bool    notEmpty = (eflags & kPCRE_NOTEMPTY) != 0;

	const QString subject       = QString::fromUtf8(text, static_cast<int>(len));
	auto          advanceOffset = [](const QString &s, const int pos) -> int
	{
		if (pos < 0)
			return 0;
		if (pos >= s.size())
			return sizeToInt(s.size());
		if (s.at(pos).isHighSurrogate() && pos + 1 < s.size() && s.at(pos + 1).isLowSurrogate())
			return pos + 2;
		return pos + 1;
	};

	int nmatch      = 0;
	int startoffset = 0;
	while (startoffset <= subject.size())
	{
		if (maxmatch > 0 && nmatch >= maxmatch)
			break;

		QRegularExpression::MatchOptions matchOptions = QRegularExpression::NoMatchOption;
		if (anchored)
			matchOptions |= QRegularExpression::AnchorAtOffsetMatchOption;
		const QRegularExpressionMatch m =
		    pat->regex.match(subject, startoffset, QRegularExpression::NormalMatch, matchOptions);
		if (!m.hasMatch())
			break;

		if (notEmpty && m.capturedLength(0) == 0)
		{
			startoffset = advanceOffset(subject, startoffset);
			continue;
		}

		++nmatch;

		lua_pushvalue(L, 3); // callback
		lua_pushstring(L, m.captured(0).toUtf8().constData());
		rexPushSubstrings(L, m);
		lua_call(L, 2, 1);
		const bool stop = lua_toboolean(L, -1) != 0;
		lua_pop(L, 1);
		if (stop)
			break;

		int nextOffset = sizeToInt(m.capturedEnd(0));
		if (nextOffset <= startoffset)
			nextOffset = advanceOffset(subject, startoffset);
		startoffset = nextOffset;
	}

	lua_pushinteger(L, nmatch);
	return 1;
}

static void registerRexLibrary(lua_State *L)
{
	luaL_newmetatable(L, kRexMetaName);
	lua_pushvalue(L, -1);
	lua_setfield(L, -2, "__index");
	lua_pushcfunction(L, luaRexExec);
	lua_setfield(L, -2, "exec");
	lua_pushcfunction(L, luaRexMatch);
	lua_setfield(L, -2, "match");
	lua_pushcfunction(L, luaRexGmatch);
	lua_setfield(L, -2, "gmatch");
	lua_pushcfunction(L, luaRexGc);
	lua_setfield(L, -2, "__gc");
	lua_pushcfunction(L, luaRexToString);
	lua_setfield(L, -2, "__tostring");
	lua_pop(L, 1);

	lua_newtable(L);
	lua_pushcfunction(L, luaRexFlags);
	lua_setfield(L, -2, "flags");
	lua_pushcfunction(L, luaRexFlagsPosix);
	lua_setfield(L, -2, "flagsPOSIX");
	lua_pushcfunction(L, luaRexNew);
	lua_setfield(L, -2, "new");
	lua_pushcfunction(L, luaRexNewPosix);
	lua_setfield(L, -2, "newPOSIX");
	lua_pushcfunction(L, luaRexVersion);
	lua_setfield(L, -2, "version");
	lua_setglobal(L, "rex");
}

static qint64 luaBitCheckInteger(lua_State *L, const int index)
{
	return static_cast<qint64>(luaL_checknumber(L, index));
}

static quint64 luaBitCheckUInteger(lua_State *L, const int index)
{
	return static_cast<quint64>(static_cast<qint64>(luaL_checknumber(L, index)));
}

static int luaBitBand(lua_State *L)
{
	const int n = lua_gettop(L);
	qint64    w = luaBitCheckInteger(L, 1);
	for (int i = 2; i <= n; ++i)
		w &= luaBitCheckInteger(L, i);
	lua_pushnumber(L, static_cast<lua_Number>(w));
	return 1;
}

static int luaBitBor(lua_State *L)
{
	const int n = lua_gettop(L);
	qint64    w = luaBitCheckInteger(L, 1);
	for (int i = 2; i <= n; ++i)
		w |= luaBitCheckInteger(L, i);
	lua_pushnumber(L, static_cast<lua_Number>(w));
	return 1;
}

static int luaBitBxor(lua_State *L)
{
	const int n = lua_gettop(L);
	qint64    w = luaBitCheckInteger(L, 1);
	for (int i = 2; i <= n; ++i)
		w ^= luaBitCheckInteger(L, i);
	lua_pushnumber(L, static_cast<lua_Number>(w));
	return 1;
}

static int luaBitBnot(lua_State *L)
{
	const qint64 w = luaBitCheckInteger(L, 1);
	lua_pushnumber(L, static_cast<lua_Number>(~w));
	return 1;
}

static int luaBitLshift(lua_State *L)
{
	const qint64  w     = luaBitCheckInteger(L, 1);
	const quint64 shift = luaBitCheckUInteger(L, 2);
	lua_pushnumber(L, static_cast<lua_Number>(w << shift));
	return 1;
}

static int luaBitRshift(lua_State *L)
{
	const quint64 w     = luaBitCheckUInteger(L, 1);
	const quint64 shift = luaBitCheckUInteger(L, 2);
	lua_pushnumber(L, static_cast<lua_Number>(w >> shift));
	return 1;
}

static int luaBitArshift(lua_State *L)
{
	const qint64  w     = luaBitCheckInteger(L, 1);
	const quint64 shift = luaBitCheckUInteger(L, 2);
	lua_pushnumber(L, static_cast<lua_Number>(w >> shift));
	return 1;
}

static int luaBitMod(lua_State *L)
{
	const qint64 a = luaBitCheckInteger(L, 1);
	const qint64 b = luaBitCheckInteger(L, 2);
	if (b == 0)
		luaL_error(L, "divide by zero");
	lua_pushnumber(L, static_cast<lua_Number>(a % b));
	return 1;
}

static int luaBitToNumber(lua_State *L)
{
	const char *text = luaL_checkstring(L, 1);
	const int   base = static_cast<int>(luaL_optinteger(L, 2, 10));
	if (base != 10)
		luaL_argcheck(L, 2 <= base && base <= 36, 2, "base out of range");
	QString      input = QString::fromUtf8(text).trimmed();
	bool         ok    = false;
	const qint64 value = input.toLongLong(&ok, base);
	if (!ok)
		luaL_error(L, "Bad digit");
	lua_pushnumber(L, static_cast<lua_Number>(value));
	return 1;
}

static int luaBitToString(lua_State *L)
{
	const qint64 n    = luaBitCheckInteger(L, 1);
	const int    base = static_cast<int>(luaL_optinteger(L, 2, 10));
	if (base != 10)
		luaL_argcheck(L, 2 <= base && base <= 36, 2, "base out of range");
	quint64 un = n < 0 ? static_cast<quint64>(-n) : static_cast<quint64>(n);
	if (un >= 4503599627370496ULL)
		luaL_error(L, "Number too big");
	QString out = QString::number(un, base).toUpper();
	if (n < 0)
		out.prepend(QLatin1Char('-'));
	const QByteArray bytes = out.toUtf8();
	lua_pushlstring(L, bytes.constData(), bytes.size());
	return 1;
}

static int luaBitTest(lua_State *L)
{
	const int n    = lua_gettop(L);
	qint64    w    = luaBitCheckInteger(L, 1);
	qint64    mask = 0;
	for (int i = 2; i <= n; ++i)
		mask |= luaBitCheckInteger(L, i);
	lua_pushboolean(L, (w & mask) == mask);
	return 1;
}

static int luaBitClear(lua_State *L)
{
	const int n = lua_gettop(L);
	qint64    w = luaBitCheckInteger(L, 1);
	for (int i = 2; i <= n; ++i)
		w &= ~luaBitCheckInteger(L, i);
	lua_pushnumber(L, static_cast<lua_Number>(w));
	return 1;
}

static void registerBitLibrary(lua_State *L)
{
	lua_newtable(L);
	lua_pushcfunction(L, luaBitArshift);
	lua_setfield(L, -2, "ashr");
	lua_pushcfunction(L, luaBitBand);
	lua_setfield(L, -2, "band");
	lua_pushcfunction(L, luaBitBor);
	lua_setfield(L, -2, "bor");
	lua_pushcfunction(L, luaBitMod);
	lua_setfield(L, -2, "mod");
	lua_pushcfunction(L, luaBitBnot);
	lua_setfield(L, -2, "neg");
	lua_pushcfunction(L, luaBitLshift);
	lua_setfield(L, -2, "shl");
	lua_pushcfunction(L, luaBitRshift);
	lua_setfield(L, -2, "shr");
	lua_pushcfunction(L, luaBitToNumber);
	lua_setfield(L, -2, "tonumber");
	lua_pushcfunction(L, luaBitToString);
	lua_setfield(L, -2, "tostring");
	lua_pushcfunction(L, luaBitTest);
	lua_setfield(L, -2, "test");
	lua_pushcfunction(L, luaBitClear);
	lua_setfield(L, -2, "clear");
	lua_pushcfunction(L, luaBitBxor);
	lua_setfield(L, -2, "xor");
	lua_setglobal(L, "bit");
}

static void registerFlagTables(lua_State *L)
{
	static const LuaNamedInt kTriggerFlags[] = {
	    {"Enabled",           eEnabled                 },
	    {"OmitFromLog",       eOmitFromLog             },
	    {"OmitFromOutput",    eOmitFromOutput          },
	    {"KeepEvaluating",    eKeepEvaluating          },
	    {"IgnoreCase",        eIgnoreCase              },
	    {"RegularExpression", eTriggerRegularExpression},
	    {"ExpandVariables",   eExpandVariables         },
	    {"Replace",           eReplace                 },
	    {"LowercaseWildcard", eLowercaseWildcard       },
	    {"Temporary",         eTemporary               },
	    {"OneShot",           eTriggerOneShot          },
	    {nullptr,             0	                    }
    };

	static const LuaNamedInt kAliasFlags[] = {
	    {"Enabled",           eEnabled               },
	    {"KeepEvaluating",    eKeepEvaluating        },
	    {"IgnoreAliasCase",   eIgnoreAliasCase       },
	    {"OmitFromLogFile",   eOmitFromLogFile       },
	    {"RegularExpression", eAliasRegularExpression},
	    {"ExpandVariables",   eExpandVariables       },
	    {"Replace",           eReplace               },
	    {"AliasSpeedWalk",    eAliasSpeedWalk        },
	    {"AliasQueue",        eAliasQueue            },
	    {"AliasMenu",         eAliasMenu             },
	    {"Temporary",         eTemporary             },
	    {"OneShot",           eAliasOneShot          },
	    {nullptr,             0	                  }
    };

	static const LuaNamedInt kTimerFlags[] = {
	    {"Enabled",          eEnabled         },
	    {"AtTime",           eAtTime          },
	    {"OneShot",          eOneShot         },
	    {"TimerSpeedWalk",   eTimerSpeedWalk  },
	    {"TimerNote",        eTimerNote       },
	    {"ActiveWhenClosed", eActiveWhenClosed},
	    {"Replace",          eReplace         },
	    {"Temporary",        eTemporary       },
	    {nullptr,            0                }
    };

	static const LuaNamedInt kCustomColours[] = {
	    {"NoChange",    -1},
        {"Custom1",     0 },
        {"Custom2",     1 },
        {"Custom3",     2 },
        {"Custom4",     3 },
	    {"Custom5",     4 },
        {"Custom6",     5 },
        {"Custom7",     6 },
        {"Custom8",     7 },
        {"Custom9",     8 },
	    {"Custom10",    9 },
        {"Custom11",    10},
        {"Custom12",    11},
        {"Custom13",    12},
        {"Custom14",    13},
	    {"Custom15",    14},
        {"Custom16",    15},
        {"CustomOther", 16},
        {nullptr,       0 }
    };

	registerNamedIntTable(L, "trigger_flag", kTriggerFlags);
	registerNamedIntTable(L, "alias_flag", kAliasFlags);
	registerNamedIntTable(L, "timer_flag", kTimerFlags);
	registerNamedIntTable(L, "custom_colour", kCustomColours);
}

static void registerSendToTable(lua_State *L)
{
	static const LuaNamedInt kSendTo[] = {
	    {"world",           eSendToWorld          },
	    {"command",         eSendToCommand        },
	    {"output",          eSendToOutput         },
	    {"status",          eSendToStatus         },
	    {"notepad",         eSendToNotepad        },
	    {"notepadappend",   eAppendToNotepad      },
	    {"logfile",         eSendToLogFile        },
	    {"notepadreplace",  eReplaceNotepad       },
	    {"commandqueue",    eSendToCommandQueue   },
	    {"variable",        eSendToVariable       },
	    {"execute",         eSendToExecute        },
	    {"speedwalk",       eSendToSpeedwalk      },
	    {"script",          eSendToScript         },
	    {"immediate",       eSendImmediate        },
	    {"scriptafteromit", eSendToScriptAfterOmit},
	    {nullptr,           0	                 }
    };
	registerNamedIntTable(L, "sendto", kSendTo);
}

static void registerMiniwinConstants(lua_State *L)
{
	static const LuaNamedInt kMiniwin[] = {
	    {"pos_stretch_to_view",                            0              },
	    {"pos_stretch_to_view_with_aspect",                1              },
	    {"pos_stretch_to_owner",                           2              },
	    {"pos_stretch_to_owner_with_aspect",               3              },
	    {"pos_top_left",	                               4              },
	    {"pos_top_center",	                             5              },
	    {"pos_top_right",	                              6              },
	    {"pos_center_right",	                           7              },
	    {"pos_bottom_right",	                           8              },
	    {"pos_bottom_center",	                          9              },
	    {"pos_bottom_left",	                            10             },
	    {"pos_center_left",	                            11             },
	    {"pos_center_all",	                             12             },
	    {"pos_tile",	                                   13             },

	    {"create_underneath",	                          0x01           },
	    {"create_absolute_location",                       0x02           },
	    {"create_transparent",                             0x04           },
	    {"create_ignore_mouse",                            0x08           },
	    {"create_keep_hotspots",                           0x10           },

	    {"pen_solid",	                                  0              },
	    {"pen_dash",	                                   1              },
	    {"pen_dot",	                                    2              },
	    {"pen_dash_dot",	                               3              },
	    {"pen_dash_dot_dot",	                           4              },
	    {"pen_null",	                                   5              },
	    {"pen_inside_frame",	                           6              },
	    {"pen_endcap_round",	                           0x00000000     },
	    {"pen_endcap_square",	                          0x00000100     },
	    {"pen_endcap_flat",	                            0x00000200     },
	    {"pen_join_round",	                             0x00000000     },
	    {"pen_join_bevel",	                             0x00001000     },
	    {"pen_join_miter",	                             0x00002000     },

	    {"brush_solid",	                                0              },
	    {"brush_null",	                                 1              },
	    {"brush_hatch_horizontal",                         2              },
	    {"brush_hatch_vertical",                           3              },
	    {"brush_hatch_forwards_diagonal",                  4              },
	    {"brush_hatch_backwards_diagonal",                 5              },
	    {"brush_hatch_cross",	                          6              },
	    {"brush_hatch_cross_diagonal",                     7              },
	    {"brush_fine_pattern",                             8              },
	    {"brush_medium_pattern",                           9              },
	    {"brush_coarse_pattern",                           10             },
	    {"brush_waves_horizontal",                         11             },
	    {"brush_waves_vertical",                           12             },

	    {"rect_frame",	                                 1              },
	    {"rect_fill",	                                  2              },
	    {"rect_invert",	                                3              },
	    {"rect_3d_rect",	                               4              },
	    {"rect_draw_edge",	                             5              },
	    {"rect_flood_fill_border",                         6              },
	    {"rect_flood_fill_surface",                        7              },

	    {"rect_edge_raised",	                           5              },
	    {"rect_edge_etched",	                           6              },
	    {"rect_edge_bump",	                             9              },
	    {"rect_edge_sunken",	                           10             },

	    {"rect_edge_at_top_left",                          0x0003         },
	    {"rect_edge_at_top_right",                         0x0006         },
	    {"rect_edge_at_bottom_left",                       0x0009         },
	    {"rect_edge_at_bottom_right",                      0x000C         },
	    {"rect_edge_at_all",	                           0x000F         },

	    {"rect_diagonal_end_top_left",                     0x0013         },
	    {"rect_diagonal_end_top_right",                    0x0016         },
	    {"rect_diagonal_end_bottom_left",                  0x0019         },
	    {"rect_diagonal_end_bottom_right",                 0x001C         },

	    {"rect_option_fill_middle",                        0x0800         },
	    {"rect_option_softer_buttons",                     0x1000         },
	    {"rect_option_flat_borders",                       0x4000         },
	    {"rect_option_monochrom_borders",                  0x8000         },

	    {"circle_ellipse",	                             1              },
	    {"circle_rectangle",	                           2              },
	    {"circle_round_rectangle",                         3              },
	    {"circle_chord",	                               4              },
	    {"circle_pie",	                                 5              },

	    {"gradient_horizontal",                            1              },
	    {"gradient_vertical",	                          2              },
	    {"gradient_texture",	                           3              },

	    {"font_charset_ansi",	                          0              },
	    {"font_charset_default",                           DEFAULT_CHARSET},
	    {"font_charset_symbol",                            2              },

	    {"font_family_any",	                            0x00           },
	    {"font_family_roman",	                          0x10           },
	    {"font_family_swiss",	                          0x20           },
	    {"font_family_modern",                             0x30           },
	    {"font_family_script",                             0x40           },
	    {"font_family_decorative",                         0x50           },

	    {"font_pitch_default",                             0              },
	    {"font_pitch_fixed",	                           1              },
	    {"font_pitch_variable",                            2              },
	    {"font_pitch_monospaced",                          10             },
	    {"font_truetype",	                              0x04           },

	    {"image_copy",	                                 1              },
	    {"image_stretch",	                              2              },
	    {"image_transparent_copy",                         3              },

	    {"image_fill_ellipse",                             1              },
	    {"image_fill_rectangle",                           2              },
	    {"image_fill_round_fill_rectangle",                3              },

	    {"filter_noise",	                               1              },
	    {"filter_monochrome_noise",                        2              },
	    {"filter_blur",	                                3              },
	    {"filter_sharpen",	                             4              },
	    {"filter_find_edges",	                          5              },
	    {"filter_emboss",	                              6              },
	    {"filter_brightness",	                          7              },
	    {"filter_contrast",	                            8              },
	    {"filter_gamma",	                               9              },
	    {"filter_red_brightness",                          10             },
	    {"filter_red_contrast",                            11             },
	    {"filter_red_gamma",	                           12             },
	    {"filter_green_brightness",                        13             },
	    {"filter_green_contrast",                          14             },
	    {"filter_green_gamma",                             15             },
	    {"filter_blue_brightness",                         16             },
	    {"filter_blue_contrast",                           17             },
	    {"filter_blue_gamma",	                          18             },
	    {"filter_grayscale",	                           19             },
	    {"filter_normal_grayscale",                        20             },
	    {"filter_brightness_multiply",                     21             },
	    {"filter_red_brightness_multiply",                 22             },
	    {"filter_green_brightness_multiply",               23             },
	    {"filter_blue_brightness_multiply",                24             },
	    {"filter_lesser_blur",                             25             },
	    {"filter_minor_blur",	                          26             },
	    {"filter_average",	                             27             },

	    {"blend_normal",	                               1              },
	    {"blend_average",	                              2              },
	    {"blend_interpolate",	                          3              },
	    {"blend_dissolve",	                             4              },
	    {"blend_darken",	                               5              },
	    {"blend_multiply",	                             6              },
	    {"blend_colour_burn",	                          7              },
	    {"blend_linear_burn",	                          8              },
	    {"blend_inverse_colour_burn",                      9              },
	    {"blend_subtract",	                             10             },
	    {"blend_lighten",	                              11             },
	    {"blend_screen",	                               12             },
	    {"blend_colour_dodge",                             13             },
	    {"blend_linear_dodge",                             14             },
	    {"blend_inverse_colour_dodge",                     15             },
	    {"blend_add",	                                  16             },
	    {"blend_overlay",	                              17             },
	    {"blend_soft_light",	                           18             },
	    {"blend_hard_light",	                           19             },
	    {"blend_vivid_light",	                          20             },
	    {"blend_linear_light",                             21             },
	    {"blend_pin_light",	                            22             },
	    {"blend_hard_mix",	                             23             },
	    {"blend_difference",	                           24             },
	    {"blend_exclusion",	                            25             },
	    {"blend_reflect",	                              26             },
	    {"blend_glow",	                                 27             },
	    {"blend_freeze",	                               28             },
	    {"blend_heat",	                                 29             },
	    {"blend_negation",	                             30             },
	    {"blend_phoenix",	                              31             },
	    {"blend_stamp",	                                32             },
	    {"blend_xor",	                                  33             },
	    {"blend_and",	                                  34             },
	    {"blend_or",	                                   35             },
	    {"blend_red",	                                  36             },
	    {"blend_green",	                                37             },
	    {"blend_blue",	                                 38             },
	    {"blend_yellow",	                               39             },
	    {"blend_cyan",	                                 40             },
	    {"blend_magenta",	                              41             },
	    {"blend_green_limited_by_red",                     42             },
	    {"blend_green_limited_by_blue",                    43             },
	    {"blend_green_limited_by_average_of_red_and_blue", 44             },
	    {"blend_blue_limited_by_red",                      45             },
	    {"blend_blue_limited_by_green",                    46             },
	    {"blend_blue_limited_by_average_of_red_and_green", 47             },
	    {"blend_red_limited_by_green",                     48             },
	    {"blend_red_limited_by_blue",                      49             },
	    {"blend_red_limited_by_average_of_green_and_blue", 50             },
	    {"blend_red_only",	                             51             },
	    {"blend_green_only",	                           52             },
	    {"blend_blue_only",	                            53             },
	    {"blend_discard_red",	                          54             },
	    {"blend_discard_green",                            55             },
	    {"blend_discard_blue",                             56             },
	    {"blend_all_red",	                              57             },
	    {"blend_all_green",	                            58             },
	    {"blend_all_blue",	                             59             },
	    {"blend_hue_mode",	                             60             },
	    {"blend_saturation_mode",                          61             },
	    {"blend_colour_mode",	                          62             },
	    {"blend_luminance_mode",                           63             },
	    {"blend_hsl",	                                  64             },

	    {"cursor_none",	                                -1             },
	    {"cursor_arrow",	                               0              },
	    {"cursor_hand",	                                1              },
	    {"cursor_ibeam",	                               2              },
	    {"cursor_plus",	                                3              },
	    {"cursor_wait",	                                4              },
	    {"cursor_up",	                                  5              },
	    {"cursor_nw_se_arrow",                             6              },
	    {"cursor_ne_sw_arrow",                             7              },
	    {"cursor_ew_arrow",	                            8              },
	    {"cursor_ns_arrow",	                            9              },
	    {"cursor_both_arrow",	                          10             },
	    {"cursor_x",	                                   11             },
	    {"cursor_help",	                                12             },

	    {"hotspot_report_all_mouseovers",                  0x01           },

	    {"hotspot_got_shift",	                          0x01           },
	    {"hotspot_got_control",                            0x02           },
	    {"hotspot_got_alt",	                            0x04           },
	    {"hotspot_got_lh_mouse",                           0x10           },
	    {"hotspot_got_rh_mouse",                           0x20           },
	    {"hotspot_got_dbl_click",                          0x40           },
	    {"hotspot_got_not_first",                          0x80           },
	    {"hotspot_got_middle_mouse",                       0x200          },

	    {"merge_straight",	                             0              },
	    {"merge_transparent",	                          1              },

	    {"drag_got_shift",	                             0x01           },
	    {"drag_got_control",	                           0x02           },
	    {"drag_got_alt",	                               0x04           },

	    {"wheel_got_shift",	                            0x01           },
	    {"wheel_got_control",	                          0x02           },
	    {"wheel_got_alt",	                              0x04           },
	    {"wheel_scroll_back",	                          0x100          },

	    {nullptr,	                                      0              }
    };

	registerNamedIntTable(L, "miniwin", kMiniwin);
}

static void registerLuaPreloads(lua_State *L)
{
	lua_getglobal(L, "package");
	if (!lua_istable(L, -1))
	{
		lua_pop(L, 1);
		return;
	}
	lua_getfield(L, -1, "preload");
	if (lua_istable(L, -1))
	{
		lua_pushcfunction(L, luaopen_lsqlite3);
		lua_setfield(L, -2, "sqlite3");
		lua_pushcfunction(L, luaopen_lsqlite3);
		lua_setfield(L, -2, "lsqlite3");
#ifdef QMUD_BUNDLED_BC
		lua_pushcfunction(L, luaopen_bc);
		lua_setfield(L, -2, "bc");
#endif
	}
	lua_pop(L, 2);
}

static void registerSqliteLibrary(lua_State *L)
{
	// Prefer standard Lua loader path first.
	lua_getglobal(L, "require");
	if (lua_isfunction(L, -1))
	{
		lua_pushstring(L, "sqlite3");
		if (lua_pcall(L, 1, 1, 0) == 0)
		{
			lua_pushvalue(L, -1);
			lua_setglobal(L, "sqlite3");
			lua_pushvalue(L, -1);
			lua_setglobal(L, "lsqlite3");
			lua_pop(L, 1);
			return;
		}
		lua_pop(L, 1); // require error
	}
	else
	{
		lua_pop(L, 1);
	}

	// Fallback to bundled sqlite binding; luaopen_lsqlite3 installs sqlite3/lsqlite3 globals.
	lua_pushcfunction(L, luaopen_lsqlite3);
	(void)lua_pcall(L, 0, 1, 0);
	lua_pop(L, 1);
}

static void registerBcLibrary(lua_State *L)
{
	// System-first: load distro-provided Lua bc module through Lua's loader path.
	lua_getglobal(L, "require");
	if (lua_isfunction(L, -1))
	{
		lua_pushstring(L, "bc");
		if (lua_pcall(L, 1, 1, 0) == 0)
		{
			lua_setglobal(L, "bc");
			return;
		}
		lua_pop(L, 1); // require error
	}
	else
	{
		lua_pop(L, 1);
	}

#ifdef QMUD_BUNDLED_BC
	lua_pushcfunction(L, luaopen_bc);
	if (lua_pcall(L, 0, 1, 0) == 0)
		lua_setglobal(L, "bc");
	else
		lua_pop(L, 1);
#endif
}

static void registerLpegLibrary(lua_State *L)
{
	// System-first: load distro-provided lua-lpeg module through Lua's loader path.
	lua_getglobal(L, "require");
	if (lua_isfunction(L, -1))
	{
		lua_pushstring(L, "lpeg");
		if (lua_pcall(L, 1, 1, 0) == 0)
		{
			if (lua_istable(L, -1))
			{
				const int lpegIndex = lua_gettop(L);

				lua_getfield(L, lpegIndex, "ptree");
				if (lua_isfunction(L, -1))
					lua_setfield(L, lpegIndex, "print");
				else
					lua_pop(L, 1);

				luaL_getmetatable(L, "lpeg-pattern");
				if (lua_istable(L, -1))
				{
					constexpr const char *kMetaFns[] = {"__add", "__div", "__len", "__mul",
					                                    "__pow", "__sub", "__unm", nullptr};
					for (const char *const *fn = kMetaFns; *fn; ++fn)
					{
						lua_getfield(L, -1, *fn);
						if (lua_isfunction(L, -1))
							lua_setfield(L, lpegIndex, *fn);
						else
							lua_pop(L, 1);
					}
				}
				lua_pop(L, 1);
			}
			lua_setglobal(L, "lpeg");
			return;
		}
		lua_pop(L, 1); // require error
	}
	else
	{
		lua_pop(L, 1);
	}
}

namespace
{
	struct LuaProgressDialog
	{
			QProgressDialog *dialog{nullptr};
			int              step{1};
	};

	constexpr auto     kProgressMetaName = "mushclient.progress_dialog_handle";

	LuaProgressDialog *progressCheck(lua_State *L)
	{
		auto *ud = static_cast<LuaProgressDialog **>(luaL_checkudata(L, 1, kProgressMetaName));
		if (!ud || !*ud)
		{
			luaL_argerror(L, 1, "progress dialog userdata expected");
			return nullptr;
		}
		return *ud;
	}
} // namespace

static int luaProgressGc(lua_State *L)
{
	if (auto *ud = static_cast<LuaProgressDialog **>(luaL_checkudata(L, 1, kProgressMetaName)); ud && *ud)
	{
		if ((*ud)->dialog)
		{
			(*ud)->dialog->close();
			delete (*ud)->dialog;
			(*ud)->dialog = nullptr;
		}
		delete *ud;
		*ud = nullptr;
	}
	return 0;
}

static int luaProgressToString(lua_State *L)
{
	Q_UNUSED(L);
	lua_pushstring(L, "progress_dialog");
	return 1;
}

static int luaProgressNew(lua_State *L)
{
	const char *title = luaL_optstring(L, 1, "Progress ...");
	auto       *ud    = static_cast<LuaProgressDialog **>(lua_newuserdata(L, sizeof(LuaProgressDialog *)));
	*ud               = new LuaProgressDialog();

	auto *dlg = new QProgressDialog(resolveMainWindow());
	dlg->setWindowTitle(QString::fromUtf8(title));
	dlg->setLabelText(QString());
	dlg->setCancelButtonText(QStringLiteral("Cancel"));
	dlg->setRange(0, 100);
	dlg->setValue(0);
	dlg->setAutoClose(false);
	dlg->setAutoReset(false);
	dlg->show();

	(*ud)->dialog = dlg;
	luaL_getmetatable(L, kProgressMetaName);
	lua_setmetatable(L, -2);
	return 1;
}

static int luaProgressSetStatus(lua_State *L)
{
	LuaProgressDialog *p      = progressCheck(L);
	const char        *status = luaL_checkstring(L, 2);
	if (p->dialog)
		p->dialog->setLabelText(QString::fromUtf8(status));
	return 0;
}

static int luaProgressSetRange(lua_State *L)
{
	LuaProgressDialog *p     = progressCheck(L);
	const int          start = static_cast<int>(luaL_checkinteger(L, 2));
	const int          end   = static_cast<int>(luaL_checkinteger(L, 3));
	if (p->dialog)
		p->dialog->setRange(start, end);
	return 0;
}

static int luaProgressSetPosition(lua_State *L)
{
	LuaProgressDialog *p   = progressCheck(L);
	const int          pos = static_cast<int>(luaL_checkinteger(L, 2));
	if (p->dialog)
		p->dialog->setValue(pos);
	return 0;
}

static int luaProgressSetStep(lua_State *L)
{
	LuaProgressDialog *p = progressCheck(L);
	p->step              = static_cast<int>(luaL_checkinteger(L, 2));
	if (p->step <= 0)
		p->step = 1;
	return 0;
}

static int luaProgressStep(lua_State *L)
{
	if (LuaProgressDialog *p = progressCheck(L); p->dialog)
		p->dialog->setValue(p->dialog->value() + p->step);
	return 0;
}

static int luaProgressCheckCancel(lua_State *L)
{
	LuaProgressDialog *p = progressCheck(L);
	lua_pushboolean(L, p->dialog ? p->dialog->wasCanceled() : 0);
	return 1;
}

static void registerProgressLibrary(lua_State *L)
{
	luaL_newmetatable(L, kProgressMetaName);
	lua_pushvalue(L, -1);
	lua_setfield(L, -2, "__index");
	lua_pushcfunction(L, luaProgressGc);
	lua_setfield(L, -2, "__gc");
	lua_pushcfunction(L, luaProgressToString);
	lua_setfield(L, -2, "__tostring");
	lua_pushcfunction(L, luaProgressCheckCancel);
	lua_setfield(L, -2, "checkcancel");
	lua_pushcfunction(L, luaProgressGc);
	lua_setfield(L, -2, "close");
	lua_pushcfunction(L, luaProgressSetPosition);
	lua_setfield(L, -2, "position");
	lua_pushcfunction(L, luaProgressSetRange);
	lua_setfield(L, -2, "range");
	lua_pushcfunction(L, luaProgressSetStep);
	lua_setfield(L, -2, "setstep");
	lua_pushcfunction(L, luaProgressSetStatus);
	lua_setfield(L, -2, "status");
	lua_pushcfunction(L, luaProgressStep);
	lua_setfield(L, -2, "step");
	lua_pop(L, 1);

	lua_newtable(L);
	lua_pushcfunction(L, luaProgressNew);
	lua_setfield(L, -2, "new");
	lua_setglobal(L, "progress");
}

static void registerCheckFunction(lua_State *L)
{
	const auto code = "function check (result) "
	                  "if result ~= error_code.eOK then "
	                  "error (error_desc [result] or "
	                  "string.format (\"Unknown error code: %i\", result), 2) "
	                  "end end";
	if (luaL_dostring(L, code) != 0)
	{
		const char *err = lua_tostring(L, -1);
		qWarning() << "Failed to register check function:" << (err ? err : "unknown");
		lua_pop(L, 1);
	}
}

static void pushVariant(lua_State *L, const QVariant &value)
{
	if (!value.isValid() || value.isNull())
	{
		lua_pushnil(L);
		return;
	}

	switch (value.typeId())
	{
	case QMetaType::Bool:
		lua_pushboolean(L, value.toBool());
		return;
	case QMetaType::Float:
	case QMetaType::Int:
	case QMetaType::LongLong:
	case QMetaType::UInt:
	case QMetaType::ULongLong:
	case QMetaType::Double:
		lua_pushnumber(L, value.toDouble());
		return;
	case QMetaType::QDateTime:
		lua_pushnumber(L, static_cast<lua_Number>(value.toDateTime().toLocalTime().toSecsSinceEpoch()));
		return;
	case QMetaType::QDate:
		lua_pushnumber(
		    L, static_cast<lua_Number>(QDateTime(value.toDate(), QTime(0, 0), QTimeZone::systemTimeZone())
		                                   .toLocalTime()
		                                   .toSecsSinceEpoch()));
		return;
	case QMetaType::QTime:
	{
		const QDateTime dt(QDate::currentDate(), value.toTime(), QTimeZone::systemTimeZone());
		lua_pushnumber(L, static_cast<lua_Number>(dt.toLocalTime().toSecsSinceEpoch()));
		return;
	}
	default:
		break;
	}

	const QByteArray text = value.toString().toLocal8Bit();
	lua_pushlstring(L, text.constData(), text.size());
}

static void pushStringList(lua_State *L, const QStringList &list)
{
	lua_newtable(L);
	int index = 1;
	for (const QString &entry : list)
	{
		const QByteArray bytes = entry.toLocal8Bit();
		lua_pushlstring(L, bytes.constData(), bytes.size());
		lua_rawseti(L, -2, index++);
	}
}

static int pushOptionalStringList(lua_State *L, const QStringList &list)
{
	if (list.isEmpty())
	{
		lua_pushnil(L);
		return 1;
	}
	pushStringList(L, list);
	return 1;
}

static bool isEnabledValue(const QString &value)
{
	return value == QStringLiteral("1") || value.compare(QStringLiteral("y"), Qt::CaseInsensitive) == 0 ||
	       value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
}

static bool parseBooleanKeywordValue(const QString &text, bool &out)
{
	if (text.compare(QStringLiteral("y"), Qt::CaseInsensitive) == 0 ||
	    text.compare(QStringLiteral("yes"), Qt::CaseInsensitive) == 0 ||
	    text.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0)
	{
		out = true;
		return true;
	}
	if (text.compare(QStringLiteral("n"), Qt::CaseInsensitive) == 0 ||
	    text.compare(QStringLiteral("no"), Qt::CaseInsensitive) == 0 ||
	    text.compare(QStringLiteral("false"), Qt::CaseInsensitive) == 0)
	{
		out = false;
		return true;
	}
	return false;
}

static QString attrFlag(const bool value)
{
	return value ? QStringLiteral("1") : QStringLiteral("0");
}

static int buildTriggerMatchFlags(const WorldRuntime::Trigger &trigger)
{
	auto attrBool = [&](const QString &key) { return isEnabledValue(trigger.attributes.value(key)); };
	auto attrInt  = [&](const QString &key) { return trigger.attributes.value(key).toInt(); };

	int  match = 0;
	match |= (attrInt(QStringLiteral("text_colour")) & 0x0F) << 4;
	match |= (attrInt(QStringLiteral("back_colour")) & 0x0F) << 8;
	if (attrBool(QStringLiteral("bold")))
		match |= kStyleHilite;
	if (attrBool(QStringLiteral("italic")))
		match |= kStyleBlink;
	if (attrBool(QStringLiteral("underline")))
		match |= kStyleUnderline;
	if (attrBool(QStringLiteral("inverse")))
		match |= kStyleInverse;
	if (attrBool(QStringLiteral("match_text_colour")))
		match |= kTriggerMatchText;
	if (attrBool(QStringLiteral("match_back_colour")))
		match |= kTriggerMatchBack;
	if (attrBool(QStringLiteral("match_bold")))
		match |= kTriggerMatchHilite;
	if (attrBool(QStringLiteral("match_italic")))
		match |= kTriggerMatchBlink;
	if (attrBool(QStringLiteral("match_inverse")))
		match |= kTriggerMatchInverse;
	if (attrBool(QStringLiteral("match_underline")))
		match |= kTriggerMatchUnderline;
	return match;
}

static int buildTriggerStyleFlags(const WorldRuntime::Trigger &trigger)
{
	auto attrBool = [&](const QString &key) { return isEnabledValue(trigger.attributes.value(key)); };
	int  style    = 0;
	if (attrBool(QStringLiteral("make_bold")))
		style |= kStyleHilite;
	if (attrBool(QStringLiteral("make_italic")))
		style |= kStyleBlink;
	if (attrBool(QStringLiteral("make_underline")))
		style |= kStyleUnderline;
	return style;
}

static QString normalizeObjectName(const QString &name)
{
	return name.trimmed().toLower();
}

static bool isValidLabel(const QString &label)
{
	const QString trimmed = label.trimmed();
	if (trimmed.isEmpty())
		return false;
	for (int i = 0; i < trimmed.size(); ++i)
	{
		const QChar ch = trimmed.at(i);
		if (ch == QLatin1Char('_'))
			continue;
		if (!ch.isLetterOrNumber())
			return false;
	}
	return true;
}

static QString makeAutoName(const QString &prefix)
{
	return prefix + QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-');
}

static bool parseBoolArg(lua_State *L, bool &out)
{
	constexpr int kValueIndex = 3;
	if (lua_isboolean(L, kValueIndex))
	{
		out = lua_toboolean(L, kValueIndex);
		return true;
	}
	if (lua_isnumber(L, kValueIndex))
	{
		out = lua_tonumber(L, kValueIndex) != 0.0;
		return true;
	}
	const QString text = QString::fromUtf8(luaL_checkstring(L, kValueIndex));
	if (parseBooleanKeywordValue(text, out))
		return true;
	bool         ok     = false;
	const double number = text.toDouble(&ok);
	if (!ok)
		return false;
	out = number != 0.0;
	return true;
}

static bool parseNumberArg(lua_State *L, double &out)
{
	constexpr int kValueIndex = 3;
	if (lua_isnumber(L, kValueIndex))
	{
		out = lua_tonumber(L, kValueIndex);
		return true;
	}
	const QString text = QString::fromUtf8(luaL_checkstring(L, kValueIndex));
	bool          ok   = false;
	out                = text.toDouble(&ok);
	return ok;
}

static int triggerColourFromCustom(const int customColour)
{
	if (customColour <= 0)
		return -1;
	if (customColour == OTHER_CUSTOM + 1)
		return OTHER_CUSTOM;
	if (customColour > OTHER_CUSTOM + 1)
		return -1;
	return customColour - 1;
}

static int customFromTriggerColour(const int colour)
{
	if (colour < 0)
		return 0;
	if (colour == OTHER_CUSTOM)
		return OTHER_CUSTOM + 1;
	if (colour >= MAX_CUSTOM)
		return 0;
	return colour + 1;
}

static void setDefaultAttr(QMap<QString, QString> &attrs, const QString &key, const QString &value)
{
	if (!attrs.contains(key))
		attrs.insert(key, value);
}

static void applyTriggerDefaults(WorldRuntime::Trigger &trigger)
{
	QMap<QString, QString> &a = trigger.attributes;
	setDefaultAttr(a, QStringLiteral("enabled"), QStringLiteral("1"));
	setDefaultAttr(a, QStringLiteral("ignore_case"), QStringLiteral("0"));
	setDefaultAttr(a, QStringLiteral("omit_from_log"), QStringLiteral("0"));
	setDefaultAttr(a, QStringLiteral("omit_from_output"), QStringLiteral("0"));
	setDefaultAttr(a, QStringLiteral("keep_evaluating"), QStringLiteral("0"));
	setDefaultAttr(a, QStringLiteral("expand_variables"), QStringLiteral("0"));
	setDefaultAttr(a, QStringLiteral("send_to"), QStringLiteral("0"));
	setDefaultAttr(a, QStringLiteral("regexp"), QStringLiteral("0"));
	setDefaultAttr(a, QStringLiteral("repeat"), QStringLiteral("0"));
	setDefaultAttr(a, QStringLiteral("sequence"), QStringLiteral("100"));
	setDefaultAttr(a, QStringLiteral("sound_if_inactive"), QStringLiteral("0"));
	setDefaultAttr(a, QStringLiteral("lowercase_wildcard"), QStringLiteral("0"));
	setDefaultAttr(a, QStringLiteral("temporary"), QStringLiteral("0"));
	setDefaultAttr(a, QStringLiteral("user"), QStringLiteral("0"));
	setDefaultAttr(a, QStringLiteral("one_shot"), QStringLiteral("0"));
	setDefaultAttr(a, QStringLiteral("lines_to_match"), QStringLiteral("0"));
	setDefaultAttr(a, QStringLiteral("colour_change_type"), QStringLiteral("0"));
	setDefaultAttr(a, QStringLiteral("match_back_colour"), QStringLiteral("0"));
	setDefaultAttr(a, QStringLiteral("match_text_colour"), QStringLiteral("0"));
	setDefaultAttr(a, QStringLiteral("match_bold"), QStringLiteral("0"));
	setDefaultAttr(a, QStringLiteral("match_italic"), QStringLiteral("0"));
	setDefaultAttr(a, QStringLiteral("match_underline"), QStringLiteral("0"));
	setDefaultAttr(a, QStringLiteral("match_inverse"), QStringLiteral("0"));
	setDefaultAttr(a, QStringLiteral("bold"), QStringLiteral("0"));
	setDefaultAttr(a, QStringLiteral("italic"), QStringLiteral("0"));
	setDefaultAttr(a, QStringLiteral("underline"), QStringLiteral("0"));
	setDefaultAttr(a, QStringLiteral("inverse"), QStringLiteral("0"));
	setDefaultAttr(a, QStringLiteral("make_bold"), QStringLiteral("0"));
	setDefaultAttr(a, QStringLiteral("make_italic"), QStringLiteral("0"));
	setDefaultAttr(a, QStringLiteral("make_underline"), QStringLiteral("0"));
	setDefaultAttr(a, QStringLiteral("custom_colour"), QStringLiteral("0"));
}

static void applyAliasDefaults(WorldRuntime::Alias &alias)
{
	QMap<QString, QString> &a = alias.attributes;
	setDefaultAttr(a, QStringLiteral("enabled"), QStringLiteral("1"));
	setDefaultAttr(a, QStringLiteral("ignore_case"), QStringLiteral("0"));
	setDefaultAttr(a, QStringLiteral("expand_variables"), QStringLiteral("0"));
	setDefaultAttr(a, QStringLiteral("omit_from_log"), QStringLiteral("0"));
	setDefaultAttr(a, QStringLiteral("regexp"), QStringLiteral("0"));
	setDefaultAttr(a, QStringLiteral("omit_from_output"), QStringLiteral("0"));
	setDefaultAttr(a, QStringLiteral("sequence"), QStringLiteral("100"));
	setDefaultAttr(a, QStringLiteral("keep_evaluating"), QStringLiteral("0"));
	setDefaultAttr(a, QStringLiteral("menu"), QStringLiteral("0"));
	setDefaultAttr(a, QStringLiteral("send_to"), QStringLiteral("0"));
	setDefaultAttr(a, QStringLiteral("echo_alias"), QStringLiteral("0"));
	setDefaultAttr(a, QStringLiteral("omit_from_command_history"), QStringLiteral("0"));
	setDefaultAttr(a, QStringLiteral("user"), QStringLiteral("0"));
	setDefaultAttr(a, QStringLiteral("one_shot"), QStringLiteral("0"));
	setDefaultAttr(a, QStringLiteral("temporary"), QStringLiteral("0"));
}

static void applyTimerDefaults(WorldRuntime::Timer &timer)
{
	QMap<QString, QString> &a = timer.attributes;
	setDefaultAttr(a, QStringLiteral("enabled"), QStringLiteral("1"));
	setDefaultAttr(a, QStringLiteral("one_shot"), QStringLiteral("0"));
	setDefaultAttr(a, QStringLiteral("active_closed"), QStringLiteral("0"));
	setDefaultAttr(a, QStringLiteral("send_to"), QStringLiteral("0"));
	setDefaultAttr(a, QStringLiteral("omit_from_output"), QStringLiteral("0"));
	setDefaultAttr(a, QStringLiteral("omit_from_log"), QStringLiteral("0"));
	setDefaultAttr(a, QStringLiteral("at_time"), QStringLiteral("0"));
	setDefaultAttr(a, QStringLiteral("hour"), QStringLiteral("0"));
	setDefaultAttr(a, QStringLiteral("minute"), QStringLiteral("0"));
	setDefaultAttr(a, QStringLiteral("second"), QStringLiteral("0"));
	setDefaultAttr(a, QStringLiteral("offset_hour"), QStringLiteral("0"));
	setDefaultAttr(a, QStringLiteral("offset_minute"), QStringLiteral("0"));
	setDefaultAttr(a, QStringLiteral("offset_second"), QStringLiteral("0"));
	setDefaultAttr(a, QStringLiteral("user"), QStringLiteral("0"));
	setDefaultAttr(a, QStringLiteral("temporary"), QStringLiteral("0"));
}

static QTime timeFromParts(const int hour, const int minute, const double second)
{
	if (hour < 0 || minute < 0 || second < 0.0)
		return {};
	const int secInt = qFloor(second);
	int       msec   = qRound((second - secInt) * 1000.0);
	int       adjSec = secInt;
	if (msec >= 1000)
	{
		msec -= 1000;
		adjSec += 1;
	}
	return {hour, minute, adjSec, msec};
}

static void resetTimerFields(WorldRuntime::Timer &timer)
{
	if (!isEnabledValue(timer.attributes.value(QStringLiteral("enabled"))))
		return;

	const bool      atTime       = isEnabledValue(timer.attributes.value(QStringLiteral("at_time")));
	const int       hour         = timer.attributes.value(QStringLiteral("hour")).toInt();
	const int       minute       = timer.attributes.value(QStringLiteral("minute")).toInt();
	const double    second       = timer.attributes.value(QStringLiteral("second")).toDouble();
	const int       offsetHour   = timer.attributes.value(QStringLiteral("offset_hour")).toInt();
	const int       offsetMinute = timer.attributes.value(QStringLiteral("offset_minute")).toInt();
	const double    offsetSecond = timer.attributes.value(QStringLiteral("offset_second")).toDouble();

	const QDateTime now = QDateTime::currentDateTime();
	timer.lastFired     = now;

	if (atTime)
	{
		QTime at = timeFromParts(hour, minute, second);
		if (!at.isValid())
			return;
		QDateTime fire(QDate::currentDate(), at);
		if (fire < now)
			fire = fire.addDays(1);
		timer.nextFireTime = fire;
		return;
	}

	const auto intervalMs = static_cast<qint64>((hour * 3600 + minute * 60 + qFloor(second)) * 1000.0 +
	                                            (second - qFloor(second)) * 1000.0);
	const auto offsetMs =
	    static_cast<qint64>((offsetHour * 3600 + offsetMinute * 60 + qFloor(offsetSecond)) * 1000.0 +
	                        (offsetSecond - qFloor(offsetSecond)) * 1000.0);
	timer.nextFireTime = now.addMSecs(intervalMs - offsetMs);
}

static int luaNote(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
		return 0;
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
		return 0;
	const QString text = concatLuaArgs(L, 1);
	runtime->outputText(text, true, true);
	return 0;
}

static int luaPrint(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
		return 0;
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
		return 0;
	const QString text = concatLuaArgs(L, 1, QStringLiteral(" "));
	runtime->outputText(text, true, true);
	return 0;
}

static int luaTell(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
		return 0;
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
		return 0;
	const QString text = concatLuaArgs(L, 1);
	runtime->outputText(text, true, false);
	return 0;
}

static int luaSend(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}
	const QString text = concatLuaArgs(L, 1);
	const bool    echo = isEnabledValue(runtime->worldAttributes().value(QStringLiteral("display_my_input")));
	const int     result = runtime->sendCommand(text, echo, false, false, false, false);
	lua_pushnumber(L, result);
	return 1;
}

static int luaSendNoEcho(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}
	const QString text   = concatLuaArgs(L, 1);
	const int     result = runtime->sendCommand(text, false, false, false, false, false);
	lua_pushnumber(L, result);
	return 1;
}

static int luaSendImmediate(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}
	const QString                 text     = concatLuaArgs(L, 1);
	const QMap<QString, QString> &attrs    = runtime->worldAttributes();
	const bool                    echo     = isEnabledValue(attrs.value(QStringLiteral("display_my_input")));
	const bool                    logInput = isEnabledValue(attrs.value(QStringLiteral("log_input")));
	const int                     result   = runtime->sendCommand(text, echo, false, logInput, false, true);
	lua_pushnumber(L, result);
	return 1;
}

static int luaSendPush(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}
	const QString text = concatLuaArgs(L, 1);
	const bool    echo = isEnabledValue(runtime->worldAttributes().value(QStringLiteral("display_my_input")));
	const int     result = runtime->sendCommand(text, echo, false, false, true, false);
	lua_pushnumber(L, result);
	return 1;
}

static int luaSendSpecial(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}
	const char *message = luaL_checkstring(L, 1);
	const bool  echo    = optBool(L, 2, false);
	const bool  queue   = optBool(L, 3, false);
	const bool  log     = optBool(L, 4, false);
	const bool  history = optBool(L, 5, false);
	const int   result  = runtime->sendCommand(QString::fromUtf8(message), echo, queue, log, history, false);
	lua_pushnumber(L, result);
	return 1;
}

static int luaConnect(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}
	if (runtime->connectPhase() != WorldRuntime::eConnectNotConnected)
	{
		lua_pushnumber(L, eWorldOpen);
		return 1;
	}
	const QMap<QString, QString> &attrs = runtime->worldAttributes();
	const QString                 host  = attrs.value(QStringLiteral("site"));
	const auto                    port  = static_cast<quint16>(attrs.value(QStringLiteral("port")).toUInt());
	runtime->connectToWorld(host, port);
	lua_pushnumber(L, eOK);
	return 1;
}

static int luaDisconnect(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}
	if (const int phase = runtime->connectPhase();
	    phase == WorldRuntime::eConnectNotConnected || phase == WorldRuntime::eConnectDisconnecting)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}
	runtime->disconnectFromWorld();
	lua_pushnumber(L, eOK);
	return 1;
}

static int luaIsConnected(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushboolean(L, 0);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	lua_pushboolean(L, runtime && runtime->isConnected());
	return 1;
}

static int luaOpenLog(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eLogFileBadWrite);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eLogFileBadWrite);
		return 1;
	}
	const QString fileName = QString::fromUtf8(luaL_checkstring(L, 1));
	const bool    append   = optBool(L, 2, false);
	const int     result   = runtime->openLog(fileName, append);
	lua_pushnumber(L, result);
	return 1;
}

static int luaCloseLog(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eLogFileNotOpen);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eLogFileNotOpen);
		return 1;
	}
	const int result = runtime->closeLog();
	lua_pushnumber(L, result);
	return 1;
}

static int luaWriteLog(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eLogFileNotOpen);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eLogFileNotOpen);
		return 1;
	}
	const QString text   = concatLuaArgs(L, 1);
	const int     result = runtime->writeLog(text);
	lua_pushnumber(L, result);
	return 1;
}

static int luaIsLogOpen(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushboolean(L, 0);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	lua_pushboolean(L, runtime && runtime->isLogOpen());
	return 1;
}

static int luaGetQueue(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnil(L);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const QStringList commands = runtime->queuedCommands();
	if (commands.isEmpty())
	{
		lua_pushnil(L);
		return 1;
	}
	lua_newtable(L);
	for (int i = 0; i < commands.size(); ++i)
	{
		QString entry = commands.at(i);
		if (entry.size() > 1)
			entry = entry.mid(1);
		else if (!entry.isEmpty())
			entry.clear();
		const QByteArray bytes = entry.toUtf8();
		lua_pushlstring(L, bytes.constData(), bytes.size());
		lua_rawseti(L, -2, i + 1);
	}
	return 1;
}

static QString acceleratorSendTag(const int sendTo)
{
	if (sendTo == eSendToExecute)
		return {};
	return QStringLiteral("\t[%1]").arg(sendTo);
}

static int luaAcceleratorTo(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}

	const char *keyText  = luaL_checkstring(L, 1);
	const char *sendText = luaL_checkstring(L, 2);
	const int   sendTo   = static_cast<int>(luaL_checkinteger(L, 3));
	if (sendTo < 0 || sendTo >= eSendToLast)
	{
		lua_pushnumber(L, eOptionOutOfRange);
		return 1;
	}

	quint32 virt = 0;
	quint16 key  = 0;
	if (!AcceleratorUtils::stringToAccelerator(QString::fromUtf8(keyText), virt, key))
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}

	const qint64 mapKey = static_cast<qint64>(virt) << 16 | key;

	if (sendText[0] == '\0')
	{
		runtime->removeAccelerator(mapKey);
		lua_pushnumber(L, eOK);
		return 1;
	}

	int commandId = runtime->acceleratorCommandForKey(mapKey);
	if (commandId < 0)
	{
		commandId = runtime->allocateAcceleratorCommand();
		if (commandId < 0)
		{
			lua_pushnumber(L, eBadParameter);
			return 1;
		}
	}

	WorldRuntime::AcceleratorEntry entry;
	entry.text     = QString::fromUtf8(sendText);
	entry.sendTo   = sendTo;
	entry.pluginId = engine->pluginId();
	runtime->registerAccelerator(mapKey, commandId, entry);

	lua_pushnumber(L, eOK);
	return 1;
}

static int luaAccelerator(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}

	const char *keyText  = luaL_checkstring(L, 1);
	const char *sendText = luaL_checkstring(L, 2);

	quint32     virt = 0;
	quint16     key  = 0;
	if (!AcceleratorUtils::stringToAccelerator(QString::fromUtf8(keyText), virt, key))
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}

	const qint64 mapKey = static_cast<qint64>(virt) << 16 | key;
	if (sendText[0] == '\0')
	{
		runtime->removeAccelerator(mapKey);
		lua_pushnumber(L, eOK);
		return 1;
	}

	int commandId = runtime->acceleratorCommandForKey(mapKey);
	if (commandId < 0)
	{
		commandId = runtime->allocateAcceleratorCommand();
		if (commandId < 0)
		{
			lua_pushnumber(L, eBadParameter);
			return 1;
		}
	}

	WorldRuntime::AcceleratorEntry entry;
	entry.text     = QString::fromUtf8(sendText);
	entry.sendTo   = eSendToExecute;
	entry.pluginId = engine->pluginId();
	runtime->registerAccelerator(mapKey, commandId, entry);

	lua_pushnumber(L, eOK);
	return 1;
}

static int luaAcceleratorList(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	lua_newtable(L);
	if (!engine)
		return 1;
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
		return 1;

	const QVector<qint64> keys  = runtime->acceleratorKeys();
	int                   index = 1;
	for (qint64 mapKey : keys)
	{
		int commandId = runtime->acceleratorCommandForKey(mapKey);
		if (commandId < 0)
			continue;
		const WorldRuntime::AcceleratorEntry *entry = runtime->acceleratorEntryForCommand(commandId);
		if (!entry)
			continue;
		const auto    keyCode   = static_cast<quint16>(mapKey & 0xFFFF);
		const auto    virt      = static_cast<quint32>(mapKey >> 16 & 0xFFFFFFFF);
		const QString keyString = AcceleratorUtils::acceleratorToString(virt, keyCode);
		const QString line      = QStringLiteral("%1 = %2%3")
		                         .arg(keyString.isEmpty() ? QStringLiteral("<unknown>") : keyString,
		                              entry->text, acceleratorSendTag(entry->sendTo));
		const QByteArray bytes = line.toUtf8();
		lua_pushlstring(L, bytes.constData(), bytes.size());
		lua_rawseti(L, -2, index++);
	}

	return 1;
}

static int luaGetReceivedBytes(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	lua_pushnumber(L, runtime ? static_cast<lua_Number>(runtime->bytesIn()) : 0.0);
	return 1;
}

static int luaGetSentBytes(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	lua_pushnumber(L, runtime ? static_cast<lua_Number>(runtime->bytesOut()) : 0.0);
	return 1;
}

static int luaGetLineCount(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	lua_pushnumber(L, runtime ? runtime->totalLinesReceived() : 0);
	return 1;
}

static int luaGetLinesInBufferCount(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	lua_pushnumber(L, runtime ? runtime->luaContextLinesInBufferCount() : 0);
	return 1;
}

static int luaGetRecentLines(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushstring(L, "");
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushstring(L, "");
		return 1;
	}
	const int count = static_cast<int>(luaL_checkinteger(L, 1));
	if (count <= 0)
	{
		lua_pushstring(L, "");
		return 1;
	}
	const QVector<WorldRuntime::LineEntry> &lines     = runtime->lines();
	const int                               lineCount = sizeToInt(lines.size());
	const int                               start     = qMax(0, lineCount - count);
	QStringList                             out;
	for (int i = start; i < lineCount; ++i)
		out.append(lines.at(i).text);
	// Legacy behavior: GetRecentLines joins with '\n'.
	const QString joined = out.join(QStringLiteral("\n"));
	lua_pushstring(L, joined.toLocal8Bit().constData());
	return 1;
}

template <typename Option> static int pushOptionNameList(lua_State *L, const Option *options)
{
	lua_newtable(L);
	int index = 1;
	for (int i = 0; options[i].name; ++i)
	{
		lua_pushstring(L, options[i].name);
		lua_rawseti(L, -2, index++);
	}
	return 1;
}

static int luaGetOptionList(lua_State *L)
{
	return pushOptionNameList(L, worldNumericOptions());
}

static int luaGetAlphaOptionList(lua_State *L)
{
	return pushOptionNameList(L, worldAlphaOptions());
}

static int luaSetStatus(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
		return 0;
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
		return 0;
	const QString text = concatLuaArgs(L, 1);
	runtime->setStatusMessage(text);
	return 0;
}

static int luaGetEchoInput(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushboolean(L, 0);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushboolean(L, 0);
		return 1;
	}
	const bool enabled = isEnabledValue(runtime->worldAttributes().value(QStringLiteral("display_my_input")));
	lua_pushboolean(L, enabled);
	return 1;
}

static int luaSetEchoInput(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
		return 0;
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
		return 0;
	const bool enabled = optBool(L, 1, true);
	runtime->setWorldAttribute(QStringLiteral("display_my_input"),
	                           enabled ? QStringLiteral("1") : QStringLiteral("0"));
	return 0;
}

static int luaGetLogInput(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushboolean(L, 0);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushboolean(L, 0);
		return 1;
	}
	const bool enabled = isEnabledValue(runtime->worldAttributes().value(QStringLiteral("log_input")));
	lua_pushboolean(L, enabled);
	return 1;
}

static int luaSetLogInput(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
		return 0;
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
		return 0;
	const bool enabled = optBool(L, 1, true);
	runtime->setWorldAttribute(QStringLiteral("log_input"),
	                           enabled ? QStringLiteral("1") : QStringLiteral("0"));
	return 0;
}

static int luaGetLogNotes(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushboolean(L, 0);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushboolean(L, 0);
		return 1;
	}
	const bool enabled = isEnabledValue(runtime->worldAttributes().value(QStringLiteral("log_notes")));
	lua_pushboolean(L, enabled);
	return 1;
}

static int luaSetLogNotes(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
		return 0;
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
		return 0;
	const bool enabled = optBool(L, 1, true);
	runtime->setWorldAttribute(QStringLiteral("log_notes"),
	                           enabled ? QStringLiteral("1") : QStringLiteral("0"));
	return 0;
}

static int luaGetLogOutput(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushboolean(L, 0);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushboolean(L, 0);
		return 1;
	}
	const bool enabled = isEnabledValue(runtime->worldAttributes().value(QStringLiteral("log_output")));
	lua_pushboolean(L, enabled);
	return 1;
}

static int luaSetLogOutput(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
		return 0;
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
		return 0;
	const bool enabled = optBool(L, 1, true);
	runtime->setWorldAttribute(QStringLiteral("log_output"),
	                           enabled ? QStringLiteral("1") : QStringLiteral("0"));
	return 0;
}

static int luaGetTrace(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushboolean(L, 0);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushboolean(L, 0);
		return 1;
	}
	lua_pushboolean(L, runtime->traceEnabled());
	return 1;
}

static int luaSetTrace(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
		return 0;
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
		return 0;
	const bool enabled = optBool(L, 1, true);
	runtime->setTraceEnabled(enabled);
	return 0;
}

static int luaTraceOut(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
		return 0;
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime || !runtime->traceEnabled())
		return 0;
	const QString message         = concatLuaArgs(L, 1);
	const bool    traceWasEnabled = runtime->traceEnabled();
	runtime->setTraceEnabled(false);
	const bool handledByPlugin = runtime->firePluginTrace(message);
	runtime->setTraceEnabled(traceWasEnabled);
	if (handledByPlugin)
		return 0;
	runtime->outputText(QStringLiteral("TRACE: %1").arg(message), true, true);
	return 0;
}

static int luaGetWorldID(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushstring(L, "");
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushstring(L, "");
		return 1;
	}
	const QString value = runtime->worldAttributes().value(QStringLiteral("id"));
	lua_pushstring(L, value.toLocal8Bit().constData());
	return 1;
}

static int luaWorldName(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushstring(L, "");
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushstring(L, "");
		return 1;
	}
	const QString value = runtime->worldAttributes().value(QStringLiteral("name"));
	lua_pushstring(L, value.toLocal8Bit().constData());
	return 1;
}

static int luaWorldAddress(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushstring(L, "");
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushstring(L, "");
		return 1;
	}
	const QString value = runtime->worldAttributes().value(QStringLiteral("site"));
	lua_pushstring(L, value.toLocal8Bit().constData());
	return 1;
}

static int luaWorldPort(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	bool      ok   = false;
	const int port = runtime->worldAttributes().value(QStringLiteral("port")).toInt(&ok);
	lua_pushinteger(L, ok ? port : 0);
	return 1;
}

static int luaCreateGUID(lua_State *L)
{
	const QString guid = QUuid::createUuid().toString(QUuid::WithoutBraces).toUpper();
	lua_pushstring(L, guid.toLocal8Bit().constData());
	return 1;
}

static int luaGetUniqueID(lua_State *L)
{
	const QString    guid   = QUuid::createUuid().toString(QUuid::WithoutBraces).toUpper().remove('-');
	const QByteArray digest = QCryptographicHash::hash(guid.toUtf8(), QCryptographicHash::Sha1);
	const QByteArray hex    = digest.toHex().left(24);
	lua_pushstring(L, hex.constData());
	return 1;
}

static int luaGetMappingCount(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	lua_pushnumber(L, runtime ? runtime->mappingCount() : 0);
	return 1;
}

static int luaVersion(lua_State *L)
{
	const QString version = WorldRuntime::clientVersionString();
	lua_pushstring(L, version.toUtf8().constData());
	return 1;
}

static WorldRuntime *findWorldRuntimeByAttribute(const QString &attributeName, const QString &value)
{
	if (attributeName.isEmpty() || value.isEmpty())
		return nullptr;
	return findWorldRuntimeWhere(
	    [&attributeName, &value](const WorldRuntime *runtime)
	    {
		    const QMap<QString, QString> &attributes = runtime->worldAttributes();
		    const auto                    attrIt     = attributes.constFind(attributeName);
		    return attrIt != attributes.cend() && attrIt.value().compare(value, Qt::CaseInsensitive) == 0;
	    });
}

static WorldRuntime *findWorldRuntimeById(const QString &id)
{
	return findWorldRuntimeByAttribute(QStringLiteral("id"), id);
}

static WorldRuntime *findWorldRuntimeByName(const QString &name)
{
	return findWorldRuntimeByAttribute(QStringLiteral("name"), name);
}

static WorldRuntime *findWorldRuntimeByPointer(const WorldRuntime *runtimePtr)
{
	if (!runtimePtr)
		return nullptr;
	return findWorldRuntimeWhere([runtimePtr](const WorldRuntime *runtime) { return runtime == runtimePtr; });
}

static WorldRuntime *resolveWorldRuntimeFromProxy(const WorldRuntime *worldPtr, const char *worldId)
{
	WorldRuntime *target = findWorldRuntimeByPointer(worldPtr);
	if (!target && worldId)
		target = findWorldRuntimeById(QString::fromUtf8(worldId));
	return target;
}

static int luaWorldProxyCall(lua_State *L)
{
	auto       *engine   = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	auto       *worldPtr = static_cast<WorldRuntime *>(lua_touserdata(L, lua_upvalueindex(2)));
	const char *worldId  = lua_tostring(L, lua_upvalueindex(3));
	const char *name     = lua_tostring(L, lua_upvalueindex(4));
	if (!engine || !name)
		return 0;

	WorldRuntime *target = resolveWorldRuntimeFromProxy(worldPtr, worldId);
	if (!target)
		return 0;

	if (lua_gettop(L) >= 1)
		lua_remove(L, 1); // drop proxy table for ":" calls

	const int argCount = lua_gettop(L);
	lua_getglobal(L, "world");
	lua_getfield(L, -1, name);
	if (!lua_isfunction(L, -1))
	{
		lua_pop(L, 2);
		return 0;
	}
	lua_remove(L, -2);
	lua_insert(L, 1);

	WorldRuntime *previous = engine->swapWorldRuntime(target);
	QElapsedTimer timer;
	timer.start();
	ScriptExecutionDepthGuard depthGuard(engine);
	const int                 status = lua_pcall(L, argCount, LUA_MULTRET, 0);
	addScriptTimeForRuntime(target, timer.nsecsElapsed());
	engine->swapWorldRuntime(previous);

	if (status != 0)
	{
		return lua_error(L);
	}
	return lua_gettop(L);
}

static int luaWorldProxyIndex(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnil(L);
		return 1;
	}

	const char *key = lua_tostring(L, 2);
	if (!key)
	{
		lua_pushnil(L);
		return 1;
	}

	lua_getfield(L, 1, "__worldId");
	const char *worldId = lua_tostring(L, -1);
	lua_pop(L, 1);
	if (!worldId)
	{
		lua_pushnil(L);
		return 1;
	}

	lua_getfield(L, 1, "__worldPtr");
	auto *worldPtr = static_cast<WorldRuntime *>(lua_touserdata(L, -1));
	lua_pop(L, 1);
	WorldRuntime *target = resolveWorldRuntimeFromProxy(worldPtr, worldId);
	if (!target)
	{
		lua_pushnil(L);
		return 1;
	}

	lua_getglobal(L, "world");
	lua_getfield(L, -1, key);
	const bool isFunction = lua_isfunction(L, -1);
	lua_pop(L, 2);
	if (!isFunction)
	{
		lua_pushnil(L);
		return 1;
	}

	lua_pushlightuserdata(L, engine);
	lua_pushlightuserdata(L, target);
	lua_pushstring(L, worldId);
	lua_pushstring(L, key);
	lua_pushcclosure(L, luaWorldProxyCall, 4);
	return 1;
}

static int luaWorldProxyToString(lua_State *L)
{
	Q_UNUSED(L);
	lua_pushliteral(L, "world");
	return 1;
}

static void pushWorldProxy(lua_State *L, LuaCallbackEngine *engine, WorldRuntime *runtime,
                           const QString &worldId)
{
	static constexpr char kWorldProxyMeta[] = "QMud.WorldProxy";
	lua_newtable(L);
	lua_pushstring(L, worldId.toUtf8().constData());
	lua_setfield(L, -2, "__worldId");
	lua_pushlightuserdata(L, runtime);
	lua_setfield(L, -2, "__worldPtr");
	if (luaL_newmetatable(L, kWorldProxyMeta))
	{
		lua_pushlightuserdata(L, engine);
		lua_pushcclosure(L, luaWorldProxyIndex, 1);
		lua_setfield(L, -2, "__index");
		lua_pushcfunction(L, luaWorldProxyToString);
		lua_setfield(L, -2, "__tostring");
	}
	lua_setmetatable(L, -2);
}

static const char *kWorldLibNames[] = {"Accelerator",
                                       "AcceleratorTo",
                                       "AcceleratorList",
                                       "Activate",
                                       "ActivateClient",
                                       "ActivateNotepad",
                                       "AddAlias",
                                       "AddFont",
                                       "AddMapperComment",
                                       "AddSpellCheckWord",
                                       "AddTimer",
                                       "AddToMapper",
                                       "AddTrigger",
                                       "AddTriggerEx",
                                       "AdjustColour",
                                       "ANSI",
                                       "AnsiNote",
                                       "AppendToNotepad",
                                       "ArrayClear",
                                       "ArrayCount",
                                       "ArrayCreate",
                                       "ArrayDelete",
                                       "ArrayDeleteKey",
                                       "ArrayExists",
                                       "ArrayExport",
                                       "ArrayExportKeys",
                                       "ArrayGet",
                                       "ArrayGetFirstKey",
                                       "ArrayGetLastKey",
                                       "ArrayImport",
                                       "ArrayKeyExists",
                                       "ArrayList",
                                       "ArrayListAll",
                                       "ArrayListKeys",
                                       "ArrayListValues",
                                       "ArraySet",
                                       "ArraySize",
                                       "Base64Decode",
                                       "Base64Encode",
                                       "BlendPixel",
                                       "Bookmark",
                                       "GetBoldColour",
                                       "SetBoldColour",
                                       "BroadcastPlugin",
                                       "CallPlugin",
                                       "ChangeDir",
                                       "ChatAcceptCalls",
                                       "ChatCall",
                                       "ChatCallzChat",
                                       "ChatDisconnect",
                                       "ChatDisconnectAll",
                                       "ChatEverybody",
                                       "ChatGetID",
                                       "ChatGroup",
                                       "ChatID",
                                       "ChatMessage",
                                       "ChatNameChange",
                                       "ChatNote",
                                       "ChatPasteEverybody",
                                       "ChatPasteText",
                                       "ChatPeekConnections",
                                       "ChatPersonal",
                                       "ChatPing",
                                       "ChatRequestConnections",
                                       "ChatSendFile",
                                       "ChatStopAcceptingCalls",
                                       "ChatStopFileTransfer",
                                       "CloseLog",
                                       "CloseNotepad",
                                       "ColourNameToRGB",
                                       "ColourNote",
                                       "ColourTell",
                                       "Connect",
                                       "CreateGUID",
                                       "SetCustomColourBackground",
                                       "GetCustomColourBackground",
                                       "SetCustomColourText",
                                       "GetCustomColourText",
                                       "DatabaseOpen",
                                       "DatabaseClose",
                                       "DatabasePrepare",
                                       "DatabaseFinalize",
                                       "DatabaseColumns",
                                       "DatabaseStep",
                                       "DatabaseError",
                                       "DatabaseColumnName",
                                       "DatabaseColumnText",
                                       "DatabaseColumnValue",
                                       "DatabaseColumnType",
                                       "DatabaseTotalChanges",
                                       "DatabaseChanges",
                                       "DatabaseLastInsertRowid",
                                       "DatabaseList",
                                       "DatabaseInfo",
                                       "DatabaseExec",
                                       "DatabaseColumnNames",
                                       "DatabaseColumnValues",
                                       "DatabaseGetField",
                                       "DatabaseReset",
                                       "Debug",
                                       "DeleteAlias",
                                       "DeleteAliasGroup",
                                       "DeleteAllMapItems",
                                       "DeleteCommandHistory",
                                       "DeleteGroup",
                                       "DeleteLastMapItem",
                                       "DeleteLines",
                                       "DeleteOutput",
                                       "DeleteTemporaryAliases",
                                       "DeleteTemporaryTimers",
                                       "DeleteTemporaryTriggers",
                                       "DeleteTimer",
                                       "DeleteTimerGroup",
                                       "DeleteTrigger",
                                       "DeleteTriggerGroup",
                                       "DeleteVariable",
                                       "DiscardQueue",
                                       "Disconnect",
                                       "DoAfter",
                                       "DoAfterNote",
                                       "DoAfterSpecial",
                                       "DoAfterSpeedWalk",
                                       "DoCommand",
                                       "GetEchoInput",
                                       "SetEchoInput",
                                       "EditDistance",
                                       "EnableAlias",
                                       "EnableAliasGroup",
                                       "EnableGroup",
                                       "EnableMapping",
                                       "EnablePlugin",
                                       "EnableTimer",
                                       "EnableTimerGroup",
                                       "EnableTrigger",
                                       "EnableTriggerGroup",
                                       "ErrorDesc",
                                       "EvaluateSpeedwalk",
                                       "Execute",
                                       "ExportXML",
                                       "FilterPixel",
                                       "FixupEscapeSequences",
                                       "FixupHTML",
                                       "FlashIcon",
                                       "FlushLog",
                                       "GenerateName",
                                       "GetAlias",
                                       "GetAliasInfo",
                                       "GetAliasList",
                                       "GetAliasOption",
                                       "GetAliasWildcard",
                                       "GetAlphaOption",
                                       "GetAlphaOptionList",
                                       "GetChatInfo",
                                       "GetChatList",
                                       "GetChatOption",
                                       "GetClipboard",
                                       "GetCommand",
                                       "GetCommandList",
                                       "GetConnectDuration",
                                       "GetCurrentValue",
                                       "GetCustomColourName",
                                       "GetDefaultValue",
                                       "GetDeviceCaps",
                                       "GetEntity",
                                       "GetXMLEntity",
                                       "GetFrame",
                                       "GetGlobalOption",
                                       "GetGlobalOptionList",
                                       "GetHostAddress",
                                       "GetHostName",
                                       "GetInfo",
                                       "GetInternalCommandsList",
                                       "GetLineCount",
                                       "GetLineInfo",
                                       "GetLinesInBufferCount",
                                       "GetLoadedValue",
                                       "GetMainWindowPosition",
                                       "GetWorldWindowPosition",
                                       "GetNotepadWindowPosition",
                                       "GetMapColour",
                                       "GetMappingCount",
                                       "GetMappingItem",
                                       "GetMappingString",
                                       "GetNotepadLength",
                                       "GetNotepadList",
                                       "GetNotepadText",
                                       "GetNotes",
                                       "GetNoteStyle",
                                       "GetOption",
                                       "GetOptionList",
                                       "GetPluginAliasInfo",
                                       "GetPluginAliasList",
                                       "GetPluginAliasOption",
                                       "GetPluginID",
                                       "GetPluginInfo",
                                       "GetPluginList",
                                       "GetPluginName",
                                       "GetPluginTimerInfo",
                                       "GetPluginTimerList",
                                       "GetPluginTimerOption",
                                       "GetPluginTriggerInfo",
                                       "GetPluginTriggerList",
                                       "GetPluginTriggerOption",
                                       "GetPluginVariable",
                                       "GetPluginVariableList",
                                       "GetQueue",
                                       "GetReceivedBytes",
                                       "GetRecentLines",
                                       "GetScriptTime",
                                       "GetSelectionEndColumn",
                                       "GetSelectionEndLine",
                                       "GetSelectionStartColumn",
                                       "GetSelectionStartLine",
                                       "GetSentBytes",
                                       "GetSoundStatus",
                                       "GetStyleInfo",
                                       "GetSysColor",
                                       "GetSystemMetrics",
                                       "GetTimer",
                                       "GetTimerInfo",
                                       "GetTimerList",
                                       "GetTimerOption",
                                       "GetTrigger",
                                       "GetTriggerInfo",
                                       "GetTriggerList",
                                       "GetTriggerOption",
                                       "GetTriggerWildcard",
                                       "GetUdpPort",
                                       "GetUniqueID",
                                       "GetUniqueNumber",
                                       "GetVariable",
                                       "GetVariableList",
                                       "GetWorld",
                                       "GetWorldById",
                                       "GetWorldID",
                                       "GetWorldIdList",
                                       "GetWorldList",
                                       "Hash",
                                       "Help",
                                       "Hyperlink",
                                       "ImportXML",
                                       "Info",
                                       "InfoBackground",
                                       "InfoClear",
                                       "InfoColour",
                                       "InfoFont",
                                       "IsAlias",
                                       "IsConnected",
                                       "IsLogOpen",
                                       "IsPluginInstalled",
                                       "IsTimer",
                                       "IsTrigger",
                                       "LoadPlugin",
                                       "GetLogInput",
                                       "SetLogInput",
                                       "GetLogNotes",
                                       "SetLogNotes",
                                       "GetLogOutput",
                                       "SetLogOutput",
                                       "LogSend",
                                       "MakeRegularExpression",
                                       "MapColour",
                                       "MapColourList",
                                       "Menu",
                                       "Metaphone",
                                       "GetMapping",
                                       "SetMapping",
                                       "GetNormalColour",
                                       "SetNormalColour",
                                       "MoveMainWindow",
                                       "MoveWorldWindow",
                                       "MoveNotepadWindow",
                                       "MtSrand",
                                       "MtRand",
                                       "Note",
                                       "NoteHr",
                                       "GetNoteColour",
                                       "SetNoteColour",
                                       "GetNoteColourBack",
                                       "SetNoteColourBack",
                                       "GetNoteColourFore",
                                       "SetNoteColourFore",
                                       "NoteColourName",
                                       "NoteColourRGB",
                                       "NotepadColour",
                                       "NotepadFont",
                                       "NotepadReadOnly",
                                       "NotepadSaveMethod",
                                       "NoteStyle",
                                       "Open",
                                       "OpenBrowser",
                                       "OpenLog",
                                       "PasteCommand",
                                       "Pause",
                                       "PickColour",
                                       "PlaySound",
                                       "PlaySoundMemory",
                                       "PluginSupports",
                                       "PushCommand",
                                       "Queue",
                                       "ReadNamesFile",
                                       "Redraw",
                                       "ReloadPlugin",
                                       "RemoveBacktracks",
                                       "GetRemoveMapReverses",
                                       "SetRemoveMapReverses",
                                       "Repaint",
                                       "Replace",
                                       "ReplaceNotepad",
                                       "Reset",
                                       "ResetIP",
                                       "ResetStatusTime",
                                       "ResetTimer",
                                       "ResetTimers",
                                       "ReverseSpeedwalk",
                                       "RGBColourToName",
                                       "Save",
                                       "SaveNotepad",
                                       "SaveState",
                                       "SelectCommand",
                                       "Send",
                                       "SendImmediate",
                                       "SendNoEcho",
                                       "SendPkt",
                                       "SendPush",
                                       "SendSpecial",
                                       "SendToNotepad",
                                       "SetAliasOption",
                                       "SetAlphaOption",
                                       "SetBackgroundColour",
                                       "SetBackgroundImage",
                                       "SetChatOption",
                                       "SetChanged",
                                       "SetClipboard",
                                       "SetCommand",
                                       "SetCommandSelection",
                                       "SetCommandWindowHeight",
                                       "SetCursor",
                                       "SetCustomColourName",
                                       "SetEntity",
                                       "SetForegroundImage",
                                       "SetFrameBackgroundColour",
                                       "SetInputFont",
                                       "SetSelection",
                                       "SetMainTitle",
                                       "SetNotes",
                                       "SetOption",
                                       "SetOutputFont",
                                       "SetScroll",
                                       "SetStatus",
                                       "SetTimerOption",
                                       "SetTitle",
                                       "SetToolBarPosition",
                                       "SetTriggerOption",
                                       "SetUnseenLines",
                                       "SetVariable",
                                       "SetWorldWindowStatus",
                                       "ShiftTabCompleteItem",
                                       "ShowInfoBar",
                                       "Simulate",
                                       "Sound",
                                       "GetSpeedWalkDelay",
                                       "SetSpeedWalkDelay",
                                       "SpellCheck",
                                       "SpellCheckCommand",
                                       "SpellCheckDlg",
                                       "StripANSI",
                                       "StopSound",
                                       "StopEvaluatingTriggers",
                                       "Tell",
                                       "TextRectangle",
                                       "GetTrace",
                                       "SetTrace",
                                       "TraceOut",
                                       "TranslateDebug",
                                       "TranslateGerman",
                                       "Transparency",
                                       "Trim",
                                       "UdpListen",
                                       "UdpPortList",
                                       "UdpSend",
                                       "UnloadPlugin",
                                       "Version",
                                       "WindowAddHotspot",
                                       "WindowArc",
                                       "WindowBezier",
                                       "WindowBlendImage",
                                       "WindowCircleOp",
                                       "WindowCreate",
                                       "WindowCreateImage",
                                       "WindowDeleteAllHotspots",
                                       "WindowDelete",
                                       "WindowDeleteHotspot",
                                       "WindowDragHandler",
                                       "WindowDrawImage",
                                       "WindowDrawImageAlpha",
                                       "WindowFilter",
                                       "WindowFont",
                                       "WindowFontInfo",
                                       "WindowFontList",
                                       "WindowGetImageAlpha",
                                       "WindowGetPixel",
                                       "WindowGradient",
                                       "WindowHotspotInfo",
                                       "WindowHotspotList",
                                       "WindowHotspotTooltip",
                                       "WindowImageFromWindow",
                                       "WindowImageInfo",
                                       "WindowImageList",
                                       "WindowImageOp",
                                       "WindowInfo",
                                       "WindowLine",
                                       "WindowList",
                                       "WindowLoadImage",
                                       "WindowLoadImageMemory",
                                       "WindowMenu",
                                       "WindowMergeImageAlpha",
                                       "WindowMoveHotspot",
                                       "WindowPolygon",
                                       "WindowPosition",
                                       "WindowRectOp",
                                       "WindowResize",
                                       "WindowScrollwheelHandler",
                                       "WindowSetZOrder",
                                       "WindowSetPixel",
                                       "WindowShow",
                                       "WindowText",
                                       "WindowTextWidth",
                                       "WindowTransformImage",
                                       "WindowWrite",
                                       "WorldAddress",
                                       "WorldName",
                                       "WorldPort",
                                       "WriteLog",
                                       nullptr};

static int         findTriggerIndex(const QList<WorldRuntime::Trigger> &triggers, const QString &name)
{
	const QString normalized = normalizeObjectName(name);
	for (int i = 0; i < triggers.size(); ++i)
	{
		if (const QString label = triggers.at(i).attributes.value(QStringLiteral("name"));
		    normalizeObjectName(label) == normalized)
			return i;
	}
	return -1;
}

static int findAliasIndex(const QList<WorldRuntime::Alias> &aliases, const QString &name)
{
	const QString normalized = normalizeObjectName(name);
	for (int i = 0; i < aliases.size(); ++i)
	{
		if (const QString label = aliases.at(i).attributes.value(QStringLiteral("name"));
		    normalizeObjectName(label) == normalized)
			return i;
	}
	return -1;
}

static int findTimerIndex(const QList<WorldRuntime::Timer> &timers, const QString &name)
{
	const QString normalized = normalizeObjectName(name);
	for (int i = 0; i < timers.size(); ++i)
	{
		if (const QString label = timers.at(i).attributes.value(QStringLiteral("name"));
		    normalizeObjectName(label) == normalized)
			return i;
	}
	return -1;
}

static QList<WorldRuntime::Trigger> &mutableTriggerList(WorldRuntime *runtime, WorldRuntime::Plugin *plugin)
{
	return plugin ? plugin->triggers : runtime->triggersMutable();
}

static QList<WorldRuntime::Alias> &mutableAliasList(WorldRuntime *runtime, WorldRuntime::Plugin *plugin)
{
	return plugin ? plugin->aliases : runtime->aliasesMutable();
}

static QList<WorldRuntime::Timer> &mutableTimerList(WorldRuntime *runtime, WorldRuntime::Plugin *plugin)
{
	return plugin ? plugin->timers : runtime->timersMutable();
}

static void commitTriggerListMutation(WorldRuntime *runtime, const WorldRuntime::Plugin *plugin)
{
	if (!plugin && runtime)
		runtime->markTriggersChanged();
}

static void commitAliasListMutation(WorldRuntime *runtime, const WorldRuntime::Plugin *plugin)
{
	if (!plugin && runtime)
		runtime->markAliasesChanged();
}

static void commitTimerListMutation(WorldRuntime *runtime, const WorldRuntime::Plugin *plugin,
                                    const bool structureChanged)
{
	if (!runtime)
		return;
	if (!plugin)
		runtime->markTimersChanged();
	if (structureChanged)
		runtime->noteTimerStructureMutation();
}

static bool resolvePluginContext(const LuaCallbackEngine *engine, WorldRuntime *runtime,
                                 WorldRuntime::Plugin *&plugin, int &errorCode)
{
	plugin = nullptr;
	if (!engine || !runtime)
	{
		errorCode = eWorldClosed;
		return false;
	}
	const QString pluginId = engine->pluginId();
	if (pluginId.isEmpty())
		return true;
	plugin = runtime->pluginForId(pluginId);
	if (!plugin)
	{
		errorCode = eNoSuchPlugin;
		return false;
	}
	return true;
}

static int addTriggerInternal(const LuaCallbackEngine *engine, const QString &rawName,
                              const QString &matchText, const QString &responseText, const int flags,
                              const int colour, const int wildcard, const QString &soundFileName,
                              const QString &scriptName, const int sendTo, const int sequence)
{
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
		return eWorldClosed;

	if (matchText.isEmpty())
		return eTriggerCannotBeEmpty;

	QString name = rawName.trimmed();
	if (!name.isEmpty() && !isValidLabel(name))
		return eInvalidObjectLabel;

	if (sequence < 0 || sequence > 10000)
		return eTriggerSequenceOutOfRange;
	if (sendTo < 0 || sendTo >= eSendToLast)
		return eTriggerSendToInvalid;
	if (sendTo == eSendToVariable && (name.isEmpty() || !isValidLabel(name)))
		return eTriggerLabelNotSpecified;

	if (name.isEmpty())
		name = makeAutoName(QStringLiteral("*trigger"));

	WorldRuntime::Plugin *plugin = nullptr;
	if (int errorCode = eOK; !resolvePluginContext(engine, runtime, plugin, errorCode))
		return errorCode;

	QList<WorldRuntime::Trigger> &triggers      = mutableTriggerList(runtime, plugin);
	const int                     existingIndex = findTriggerIndex(triggers, name);
	if (existingIndex >= 0 && !(flags & eReplace))
		return eTriggerAlreadyExists;

	int insertIndex = existingIndex;
	if (existingIndex >= 0)
		triggers.removeAt(existingIndex);
	else
		insertIndex = static_cast<int>(triggers.size());

	WorldRuntime::Trigger trigger;
	trigger.attributes.insert(QStringLiteral("name"), name);
	trigger.attributes.insert(QStringLiteral("match"), matchText);
	trigger.children.insert(QStringLiteral("send"), responseText);
	if (!soundFileName.isEmpty())
		trigger.attributes.insert(QStringLiteral("sound"), soundFileName);
	if (!scriptName.isEmpty())
		trigger.attributes.insert(QStringLiteral("script"), scriptName);
	trigger.attributes.insert(QStringLiteral("send_to"), QString::number(sendTo));
	trigger.attributes.insert(QStringLiteral("sequence"), QString::number(sequence));
	trigger.attributes.insert(QStringLiteral("enabled"), attrFlag(flags & eEnabled));
	trigger.attributes.insert(QStringLiteral("ignore_case"), attrFlag(flags & eIgnoreCase));
	trigger.attributes.insert(QStringLiteral("omit_from_log"), attrFlag(flags & eOmitFromLog));
	trigger.attributes.insert(QStringLiteral("omit_from_output"), attrFlag(flags & eOmitFromOutput));
	trigger.attributes.insert(QStringLiteral("keep_evaluating"), attrFlag(flags & eKeepEvaluating));
	trigger.attributes.insert(QStringLiteral("regexp"), attrFlag(flags & eTriggerRegularExpression));
	trigger.attributes.insert(QStringLiteral("expand_variables"), attrFlag(flags & eExpandVariables));
	trigger.attributes.insert(QStringLiteral("temporary"), attrFlag(flags & eTemporary));
	trigger.attributes.insert(QStringLiteral("lowercase_wildcard"), attrFlag(flags & eLowercaseWildcard));
	trigger.attributes.insert(QStringLiteral("one_shot"), attrFlag(flags & eTriggerOneShot));
	trigger.attributes.insert(QStringLiteral("clipboard_arg"),
	                          QString::number(wildcard >= 0 && wildcard <= 10 ? wildcard : 0));
	trigger.attributes.insert(QStringLiteral("custom_colour"),
	                          QString::number(customFromTriggerColour(colour)));
	trigger.attributes.insert(QStringLiteral("variable"), name);
	applyTriggerDefaults(trigger);

	triggers.insert(insertIndex, trigger);
	commitTriggerListMutation(runtime, plugin);
	return eOK;
}

static int addAliasInternal(const LuaCallbackEngine *engine, const QString &rawName, const QString &matchText,
                            const QString &responseText, const int flags, const QString &scriptName)
{
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
		return eWorldClosed;

	if (matchText.isEmpty())
		return eAliasCannotBeEmpty;

	QString name = rawName.trimmed();
	if (!name.isEmpty() && !isValidLabel(name))
		return eInvalidObjectLabel;

	if (name.isEmpty())
		name = makeAutoName(QStringLiteral("*alias"));

	WorldRuntime::Plugin *plugin = nullptr;
	if (int errorCode = eOK; !resolvePluginContext(engine, runtime, plugin, errorCode))
		return errorCode;

	QList<WorldRuntime::Alias> &aliases       = mutableAliasList(runtime, plugin);
	const int                   existingIndex = findAliasIndex(aliases, name);
	if (existingIndex >= 0 && !(flags & eReplace))
		return eAliasAlreadyExists;

	int insertIndex = existingIndex;
	if (existingIndex >= 0)
		aliases.removeAt(existingIndex);
	else
		insertIndex = static_cast<int>(aliases.size());

	int sendTo = eSendToWorld;
	if (flags & eAliasSpeedWalk)
		sendTo = eSendToSpeedwalk;
	else if (flags & eAliasQueue)
		sendTo = eSendToCommandQueue;

	WorldRuntime::Alias alias;
	alias.attributes.insert(QStringLiteral("name"), name);
	alias.attributes.insert(QStringLiteral("match"), matchText);
	alias.children.insert(QStringLiteral("send"), responseText);
	if (!scriptName.isEmpty())
		alias.attributes.insert(QStringLiteral("script"), scriptName);
	alias.attributes.insert(QStringLiteral("send_to"), QString::number(sendTo));
	alias.attributes.insert(QStringLiteral("sequence"), QString::number(DEFAULT_ALIAS_SEQUENCE));
	alias.attributes.insert(QStringLiteral("enabled"), attrFlag(flags & eEnabled));
	alias.attributes.insert(QStringLiteral("ignore_case"), attrFlag(flags & eIgnoreAliasCase));
	alias.attributes.insert(QStringLiteral("omit_from_log"), attrFlag(flags & eOmitFromLogFile));
	alias.attributes.insert(QStringLiteral("regexp"), attrFlag(flags & eAliasRegularExpression));
	alias.attributes.insert(QStringLiteral("omit_from_output"), attrFlag(flags & eAliasOmitFromOutput));
	alias.attributes.insert(QStringLiteral("expand_variables"), attrFlag(flags & eExpandVariables));
	alias.attributes.insert(QStringLiteral("menu"), attrFlag(flags & eAliasMenu));
	alias.attributes.insert(QStringLiteral("temporary"), attrFlag(flags & eTemporary));
	alias.attributes.insert(QStringLiteral("one_shot"), attrFlag(flags & eAliasOneShot));
	alias.attributes.insert(QStringLiteral("keep_evaluating"), attrFlag(flags & eKeepEvaluating));
	applyAliasDefaults(alias);

	aliases.insert(insertIndex, alias);
	commitAliasListMutation(runtime, plugin);
	return eOK;
}

static int addTimerInternal(const LuaCallbackEngine *engine, const QString &rawName, const int hour,
                            const int minute, const double second, const QString &responseText,
                            const int flags, const QString &scriptName)
{
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
		return eWorldClosed;

	QString name = rawName.trimmed();
	if (!name.isEmpty() && !isValidLabel(name))
		return eInvalidObjectLabel;
	if (name.isEmpty())
		name = makeAutoName(QStringLiteral("*timer"));

	if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0.0 || second > 59.9999)
		return eTimeInvalid;
	if (!(flags & eAtTime) && hour == 0 && minute == 0 && second == 0.0)
		return eTimeInvalid;

	WorldRuntime::Plugin *plugin = nullptr;
	if (int errorCode = eOK; !resolvePluginContext(engine, runtime, plugin, errorCode))
		return errorCode;

	QList<WorldRuntime::Timer> &timers        = mutableTimerList(runtime, plugin);
	const int                   existingIndex = findTimerIndex(timers, name);
	if (existingIndex >= 0 && !(flags & eReplace))
		return eTimerAlreadyExists;

	int insertIndex = existingIndex;
	if (existingIndex >= 0)
		timers.removeAt(existingIndex);
	else
		insertIndex = static_cast<int>(timers.size());

	int sendTo = eSendToWorld;
	if (flags & eTimerSpeedWalk)
		sendTo = eSendToSpeedwalk;
	else if (flags & eTimerNote)
		sendTo = eSendToOutput;

	WorldRuntime::Timer timer;
	timer.attributes.insert(QStringLiteral("name"), name);
	timer.children.insert(QStringLiteral("send"), responseText);
	timer.attributes.insert(QStringLiteral("at_time"), attrFlag(flags & eAtTime));
	timer.attributes.insert(QStringLiteral("hour"), QString::number(hour));
	timer.attributes.insert(QStringLiteral("minute"), QString::number(minute));
	timer.attributes.insert(QStringLiteral("second"), QString::number(second, 'f', 4));
	timer.attributes.insert(QStringLiteral("offset_hour"), QStringLiteral("0"));
	timer.attributes.insert(QStringLiteral("offset_minute"), QStringLiteral("0"));
	timer.attributes.insert(QStringLiteral("offset_second"), QStringLiteral("0"));
	timer.attributes.insert(QStringLiteral("send_to"), QString::number(sendTo));
	timer.attributes.insert(QStringLiteral("enabled"), attrFlag(flags & eEnabled));
	timer.attributes.insert(QStringLiteral("one_shot"), attrFlag(flags & eOneShot));
	timer.attributes.insert(QStringLiteral("temporary"), attrFlag(flags & eTemporary));
	timer.attributes.insert(QStringLiteral("active_closed"), attrFlag(flags & eActiveWhenClosed));
	if (!scriptName.isEmpty())
		timer.attributes.insert(QStringLiteral("script"), scriptName);
	applyTimerDefaults(timer);
	resetTimerFields(timer);

	timers.insert(insertIndex, timer);
	commitTimerListMutation(runtime, plugin, true);
	return eOK;
}

static int luaAddTrigger(lua_State *L)
{
	auto         *engine       = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	const QString name         = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString matchText    = QString::fromUtf8(luaL_checkstring(L, 2));
	const QString responseText = QString::fromUtf8(luaL_checkstring(L, 3));
	const auto    flags        = static_cast<int>(luaL_checkinteger(L, 4));
	const int     colour       = static_cast<int>(luaL_checkinteger(L, 5));
	const int     wildcard     = static_cast<int>(luaL_checkinteger(L, 6));
	const QString soundFile    = QString::fromUtf8(luaL_optstring(L, 7, ""));
	const QString scriptName   = QString::fromUtf8(luaL_optstring(L, 8, ""));
	const int     result = addTriggerInternal(engine, name, matchText, responseText, flags, colour, wildcard,
	                                          soundFile, scriptName, eSendToWorld, DEFAULT_TRIGGER_SEQUENCE);
	lua_pushnumber(L, result);
	return 1;
}

static int luaAddTriggerEx(lua_State *L)
{
	auto         *engine       = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	const QString name         = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString matchText    = QString::fromUtf8(luaL_checkstring(L, 2));
	const QString responseText = QString::fromUtf8(luaL_checkstring(L, 3));
	const auto    flags        = static_cast<int>(luaL_checkinteger(L, 4));
	const int     colour       = static_cast<int>(luaL_checkinteger(L, 5));
	const int     wildcard     = static_cast<int>(luaL_checkinteger(L, 6));
	const QString soundFile    = QString::fromUtf8(luaL_optstring(L, 7, ""));
	const QString scriptName   = QString::fromUtf8(luaL_optstring(L, 8, ""));
	const int     sendTo       = static_cast<int>(luaL_checkinteger(L, 9));
	const int     sequence     = static_cast<int>(luaL_checkinteger(L, 10));
	const int     result = addTriggerInternal(engine, name, matchText, responseText, flags, colour, wildcard,
	                                          soundFile, scriptName, sendTo, sequence);
	lua_pushnumber(L, result);
	return 1;
}

static int luaAddAlias(lua_State *L)
{
	auto         *engine       = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	const QString name         = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString matchText    = QString::fromUtf8(luaL_checkstring(L, 2));
	const QString responseText = QString::fromUtf8(luaL_checkstring(L, 3));
	const auto    flags        = static_cast<int>(luaL_checkinteger(L, 4));
	const QString scriptName   = QString::fromUtf8(luaL_optstring(L, 5, ""));
	const int     result       = addAliasInternal(engine, name, matchText, responseText, flags, scriptName);
	lua_pushnumber(L, result);
	return 1;
}

static int luaAddTimer(lua_State *L)
{
	auto         *engine       = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	const QString name         = QString::fromUtf8(luaL_checkstring(L, 1));
	const auto    hour         = static_cast<int>(luaL_checkinteger(L, 2));
	const int     minute       = static_cast<int>(luaL_checkinteger(L, 3));
	const auto    second       = luaL_checknumber(L, 4);
	const QString responseText = QString::fromUtf8(luaL_checkstring(L, 5));
	const int     flags        = static_cast<int>(luaL_checkinteger(L, 6));
	const QString scriptName   = QString::fromUtf8(luaL_optstring(L, 7, ""));
	const int result = addTimerInternal(engine, name, hour, minute, second, responseText, flags, scriptName);
	lua_pushnumber(L, result);
	return 1;
}

static int luaDeleteTrigger(lua_State *L)
{
	const auto   *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}
	QString               name   = QString::fromUtf8(luaL_checkstring(L, 1)).trimmed();
	WorldRuntime::Plugin *plugin = nullptr;
	if (int errorCode = eOK; !resolvePluginContext(engine, runtime, plugin, errorCode))
	{
		lua_pushnumber(L, errorCode);
		return 1;
	}
	QList<WorldRuntime::Trigger> &triggers = mutableTriggerList(runtime, plugin);
	const int                     index    = findTriggerIndex(triggers, name);
	if (index < 0)
	{
		lua_pushnumber(L, eTriggerNotFound);
		return 1;
	}
	triggers.removeAt(index);
	commitTriggerListMutation(runtime, plugin);
	lua_pushnumber(L, eOK);
	return 1;
}

static int luaDeleteAlias(lua_State *L)
{
	const auto   *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}
	QString               name   = QString::fromUtf8(luaL_checkstring(L, 1)).trimmed();
	WorldRuntime::Plugin *plugin = nullptr;
	if (int errorCode = eOK; !resolvePluginContext(engine, runtime, plugin, errorCode))
	{
		lua_pushnumber(L, errorCode);
		return 1;
	}
	QList<WorldRuntime::Alias> &aliases = mutableAliasList(runtime, plugin);
	const int                   index   = findAliasIndex(aliases, name);
	if (index < 0)
	{
		lua_pushnumber(L, eAliasNotFound);
		return 1;
	}
	aliases.removeAt(index);
	commitAliasListMutation(runtime, plugin);
	lua_pushnumber(L, eOK);
	return 1;
}

static int luaDeleteTimer(lua_State *L)
{
	const auto   *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}
	QString               name   = QString::fromUtf8(luaL_checkstring(L, 1)).trimmed();
	WorldRuntime::Plugin *plugin = nullptr;
	if (int errorCode = eOK; !resolvePluginContext(engine, runtime, plugin, errorCode))
	{
		lua_pushnumber(L, errorCode);
		return 1;
	}
	QList<WorldRuntime::Timer> &timers = mutableTimerList(runtime, plugin);
	const int                   index  = findTimerIndex(timers, name);
	if (index < 0)
	{
		lua_pushnumber(L, eTimerNotFound);
		return 1;
	}
	timers.removeAt(index);
	commitTimerListMutation(runtime, plugin, true);
	lua_pushnumber(L, eOK);
	return 1;
}

static int luaDeleteTemporaryTriggers(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	WorldRuntime::Plugin *plugin = nullptr;
	if (int errorCode = eOK; !resolvePluginContext(engine, runtime, plugin, errorCode))
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	QList<WorldRuntime::Trigger> &triggers = mutableTriggerList(runtime, plugin);
	int                           removed  = 0;
	for (int i = sizeToInt(triggers.size()) - 1; i >= 0; --i)
	{
		if (isEnabledValue(triggers.at(i).attributes.value(QStringLiteral("temporary"))))
		{
			triggers.removeAt(i);
			removed++;
		}
	}
	commitTriggerListMutation(runtime, plugin);
	lua_pushnumber(L, removed);
	return 1;
}

static int luaDeleteTemporaryAliases(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	WorldRuntime::Plugin *plugin = nullptr;
	if (int errorCode = eOK; !resolvePluginContext(engine, runtime, plugin, errorCode))
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	QList<WorldRuntime::Alias> &aliases = mutableAliasList(runtime, plugin);
	int                         removed = 0;
	for (int i = sizeToInt(aliases.size()) - 1; i >= 0; --i)
	{
		if (isEnabledValue(aliases.at(i).attributes.value(QStringLiteral("temporary"))))
		{
			aliases.removeAt(i);
			removed++;
		}
	}
	commitAliasListMutation(runtime, plugin);
	lua_pushnumber(L, removed);
	return 1;
}

static int luaDeleteTemporaryTimers(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	WorldRuntime::Plugin *plugin = nullptr;
	if (int errorCode = eOK; !resolvePluginContext(engine, runtime, plugin, errorCode))
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	QList<WorldRuntime::Timer> &timers  = mutableTimerList(runtime, plugin);
	int                         removed = 0;
	for (int i = sizeToInt(timers.size()) - 1; i >= 0; --i)
	{
		if (isEnabledValue(timers.at(i).attributes.value(QStringLiteral("temporary"))))
		{
			timers.removeAt(i);
			removed++;
		}
	}
	commitTimerListMutation(runtime, plugin, removed > 0);
	lua_pushnumber(L, removed);
	return 1;
}

static int luaDeleteTriggerGroup(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	const QString groupName = QString::fromUtf8(luaL_checkstring(L, 1));
	if (groupName.trimmed().isEmpty())
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	WorldRuntime::Plugin *plugin = nullptr;
	if (int errorCode = eOK; !resolvePluginContext(engine, runtime, plugin, errorCode))
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	QList<WorldRuntime::Trigger> &triggers = mutableTriggerList(runtime, plugin);
	int                           removed  = 0;
	for (int i = sizeToInt(triggers.size()) - 1; i >= 0; --i)
	{
		if (groupMatches(triggers.at(i).attributes.value(QStringLiteral("group")), groupName))
		{
			triggers.removeAt(i);
			removed++;
		}
	}
	commitTriggerListMutation(runtime, plugin);
	lua_pushnumber(L, removed);
	return 1;
}

static int luaDeleteAliasGroup(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	const QString groupName = QString::fromUtf8(luaL_checkstring(L, 1));
	if (groupName.trimmed().isEmpty())
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	WorldRuntime::Plugin *plugin = nullptr;
	if (int errorCode = eOK; !resolvePluginContext(engine, runtime, plugin, errorCode))
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	QList<WorldRuntime::Alias> &aliases = mutableAliasList(runtime, plugin);
	int                         removed = 0;
	for (int i = sizeToInt(aliases.size()) - 1; i >= 0; --i)
	{
		if (groupMatches(aliases.at(i).attributes.value(QStringLiteral("group")), groupName))
		{
			aliases.removeAt(i);
			removed++;
		}
	}
	commitAliasListMutation(runtime, plugin);
	lua_pushnumber(L, removed);
	return 1;
}

static int luaDeleteTimerGroup(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	const QString groupName = QString::fromUtf8(luaL_checkstring(L, 1));
	if (groupName.trimmed().isEmpty())
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	WorldRuntime::Plugin *plugin = nullptr;
	if (int errorCode = eOK; !resolvePluginContext(engine, runtime, plugin, errorCode))
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	QList<WorldRuntime::Timer> &timers  = mutableTimerList(runtime, plugin);
	int                         removed = 0;
	for (int i = sizeToInt(timers.size()) - 1; i >= 0; --i)
	{
		if (groupMatches(timers.at(i).attributes.value(QStringLiteral("group")), groupName))
		{
			timers.removeAt(i);
			removed++;
		}
	}
	commitTimerListMutation(runtime, plugin, removed > 0);
	lua_pushnumber(L, removed);
	return 1;
}

static int luaDeleteGroup(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	const QString groupName = QString::fromUtf8(luaL_checkstring(L, 1));
	if (groupName.trimmed().isEmpty())
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	WorldRuntime::Plugin *plugin = nullptr;
	if (int errorCode = eOK; !resolvePluginContext(engine, runtime, plugin, errorCode))
	{
		lua_pushnumber(L, 0);
		return 1;
	}

	auto removeGroup = [&](auto &list)
	{
		int removed = 0;
		for (int i = list.size() - 1; i >= 0; --i)
		{
			if (groupMatches(list.at(i).attributes.value(QStringLiteral("group")), groupName))
			{
				list.removeAt(i);
				removed++;
			}
		}
		return removed;
	};

	QList<WorldRuntime::Trigger> &triggers = mutableTriggerList(runtime, plugin);
	QList<WorldRuntime::Alias>   &aliases  = mutableAliasList(runtime, plugin);
	QList<WorldRuntime::Timer>   &timers   = mutableTimerList(runtime, plugin);

	const int                     triggerRemoved = removeGroup(triggers);
	const int                     aliasRemoved   = removeGroup(aliases);
	const int                     timerRemoved   = removeGroup(timers);
	const int                     removed        = triggerRemoved + aliasRemoved + timerRemoved;

	commitTriggerListMutation(runtime, plugin);
	commitAliasListMutation(runtime, plugin);
	commitTimerListMutation(runtime, plugin, timerRemoved > 0);

	lua_pushnumber(L, removed);
	return 1;
}

static int luaEnableTrigger(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}
	const QString         name    = QString::fromUtf8(luaL_checkstring(L, 1));
	const bool            enabled = optBool(L, 2, true);
	WorldRuntime::Plugin *plugin  = nullptr;
	if (int errorCode = eOK; !resolvePluginContext(engine, runtime, plugin, errorCode))
	{
		lua_pushnumber(L, errorCode);
		return 1;
	}
	QList<WorldRuntime::Trigger> &triggers = mutableTriggerList(runtime, plugin);
	const int                     index    = findTriggerIndex(triggers, name);
	if (index < 0)
	{
		lua_pushnumber(L, eTriggerNotFound);
		return 1;
	}
	triggers[index].attributes.insert(QStringLiteral("enabled"), attrFlag(enabled));
	commitTriggerListMutation(runtime, plugin);
	lua_pushnumber(L, eOK);
	return 1;
}

static int luaEnableAlias(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}
	const QString         name    = QString::fromUtf8(luaL_checkstring(L, 1));
	const bool            enabled = optBool(L, 2, true);
	WorldRuntime::Plugin *plugin  = nullptr;
	if (int errorCode = eOK; !resolvePluginContext(engine, runtime, plugin, errorCode))
	{
		lua_pushnumber(L, errorCode);
		return 1;
	}
	QList<WorldRuntime::Alias> &aliases = mutableAliasList(runtime, plugin);
	const int                   index   = findAliasIndex(aliases, name);
	if (index < 0)
	{
		lua_pushnumber(L, eAliasNotFound);
		return 1;
	}
	aliases[index].attributes.insert(QStringLiteral("enabled"), attrFlag(enabled));
	commitAliasListMutation(runtime, plugin);
	lua_pushnumber(L, eOK);
	return 1;
}

static int luaEnableTimer(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}
	const QString         name    = QString::fromUtf8(luaL_checkstring(L, 1));
	const bool            enabled = optBool(L, 2, true);
	WorldRuntime::Plugin *plugin  = nullptr;
	if (int errorCode = eOK; !resolvePluginContext(engine, runtime, plugin, errorCode))
	{
		lua_pushnumber(L, errorCode);
		return 1;
	}
	QList<WorldRuntime::Timer> &timers = mutableTimerList(runtime, plugin);
	const int                   index  = findTimerIndex(timers, name);
	if (index < 0)
	{
		lua_pushnumber(L, eTimerNotFound);
		return 1;
	}
	timers[index].attributes.insert(QStringLiteral("enabled"), attrFlag(enabled));
	commitTimerListMutation(runtime, plugin);
	lua_pushnumber(L, eOK);
	return 1;
}

static int luaEnableTriggerGroup(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	const QString groupName = QString::fromUtf8(luaL_checkstring(L, 1));
	const bool    enabled   = optBool(L, 2, true);
	if (groupName.trimmed().isEmpty())
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	WorldRuntime::Plugin *plugin = nullptr;
	if (int errorCode = eOK; !resolvePluginContext(engine, runtime, plugin, errorCode))
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	QList<WorldRuntime::Trigger> &triggers = mutableTriggerList(runtime, plugin);
	int                           count    = 0;
	for (auto &trigger : triggers)
	{
		if (groupMatches(trigger.attributes.value(QStringLiteral("group")), groupName))
		{
			trigger.attributes.insert(QStringLiteral("enabled"), attrFlag(enabled));
			count++;
		}
	}
	commitTriggerListMutation(runtime, plugin);
	lua_pushnumber(L, count);
	return 1;
}

static int luaEnableAliasGroup(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	const QString groupName = QString::fromUtf8(luaL_checkstring(L, 1));
	const bool    enabled   = optBool(L, 2, true);
	if (groupName.trimmed().isEmpty())
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	WorldRuntime::Plugin *plugin = nullptr;
	if (int errorCode = eOK; !resolvePluginContext(engine, runtime, plugin, errorCode))
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	QList<WorldRuntime::Alias> &aliases = mutableAliasList(runtime, plugin);
	int                         count   = 0;
	for (auto &alias : aliases)
	{
		if (groupMatches(alias.attributes.value(QStringLiteral("group")), groupName))
		{
			alias.attributes.insert(QStringLiteral("enabled"), attrFlag(enabled));
			count++;
		}
	}
	commitAliasListMutation(runtime, plugin);
	lua_pushnumber(L, count);
	return 1;
}

static int luaEnableTimerGroup(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	const QString groupName = QString::fromUtf8(luaL_checkstring(L, 1));
	const bool    enabled   = optBool(L, 2, true);
	if (groupName.trimmed().isEmpty())
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	WorldRuntime::Plugin *plugin = nullptr;
	if (int errorCode = eOK; !resolvePluginContext(engine, runtime, plugin, errorCode))
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	QList<WorldRuntime::Timer> &timers = mutableTimerList(runtime, plugin);
	int                         count  = 0;
	for (auto &timer : timers)
	{
		if (groupMatches(timer.attributes.value(QStringLiteral("group")), groupName))
		{
			timer.attributes.insert(QStringLiteral("enabled"), attrFlag(enabled));
			count++;
		}
	}
	commitTimerListMutation(runtime, plugin);
	lua_pushnumber(L, count);
	return 1;
}

static int luaGetTrigger(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, eWorldClosed);
		for (int i = 0; i < 7; ++i)
			lua_pushnil(L);
		return 8;
	}
	const QString         name   = QString::fromUtf8(luaL_checkstring(L, 1));
	WorldRuntime::Plugin *plugin = nullptr;
	if (int errorCode = eOK; !resolvePluginContext(engine, runtime, plugin, errorCode))
	{
		lua_pushnumber(L, errorCode);
		for (int i = 0; i < 7; ++i)
			lua_pushnil(L);
		return 8;
	}
	const QList<WorldRuntime::Trigger> &triggers = plugin ? plugin->triggers : runtime->triggers();
	const int                           index    = findTriggerIndex(triggers, name);
	if (index < 0)
	{
		lua_pushnumber(L, eTriggerNotFound);
		for (int i = 0; i < 7; ++i)
			lua_pushnil(L);
		return 8;
	}
	const WorldRuntime::Trigger &trigger = triggers.at(index);
	lua_pushnumber(L, eOK);
	lua_pushstring(L, trigger.attributes.value(QStringLiteral("match")).toLocal8Bit().constData());
	lua_pushstring(L, trigger.children.value(QStringLiteral("send")).toLocal8Bit().constData());

	int flags = 0;
	if (isEnabledValue(trigger.attributes.value(QStringLiteral("ignore_case"))))
		flags |= eIgnoreCase;
	if (isEnabledValue(trigger.attributes.value(QStringLiteral("omit_from_output"))))
		flags |= eOmitFromOutput;
	if (isEnabledValue(trigger.attributes.value(QStringLiteral("keep_evaluating"))))
		flags |= eKeepEvaluating;
	if (isEnabledValue(trigger.attributes.value(QStringLiteral("omit_from_log"))))
		flags |= eOmitFromLog;
	if (isEnabledValue(trigger.attributes.value(QStringLiteral("enabled"))))
		flags |= eEnabled;
	if (isEnabledValue(trigger.attributes.value(QStringLiteral("regexp"))))
		flags |= eTriggerRegularExpression;
	if (isEnabledValue(trigger.attributes.value(QStringLiteral("lowercase_wildcard"))))
		flags |= eLowercaseWildcard;
	if (isEnabledValue(trigger.attributes.value(QStringLiteral("one_shot"))))
		flags |= eTriggerOneShot;

	lua_pushnumber(L, flags);
	lua_pushnumber(
	    L, triggerColourFromCustom(trigger.attributes.value(QStringLiteral("custom_colour")).toInt()));
	lua_pushnumber(L, trigger.attributes.value(QStringLiteral("clipboard_arg")).toInt());
	lua_pushstring(L, trigger.attributes.value(QStringLiteral("sound")).toLocal8Bit().constData());
	lua_pushstring(L, trigger.attributes.value(QStringLiteral("script")).toLocal8Bit().constData());
	return 8;
}

static int luaGetAlias(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, eWorldClosed);
		for (int i = 0; i < 4; ++i)
			lua_pushnil(L);
		return 5;
	}
	const QString         name   = QString::fromUtf8(luaL_checkstring(L, 1));
	WorldRuntime::Plugin *plugin = nullptr;
	if (int errorCode = eOK; !resolvePluginContext(engine, runtime, plugin, errorCode))
	{
		lua_pushnumber(L, errorCode);
		for (int i = 0; i < 4; ++i)
			lua_pushnil(L);
		return 5;
	}
	const QList<WorldRuntime::Alias> &aliases = plugin ? plugin->aliases : runtime->aliases();
	const int                         index   = findAliasIndex(aliases, name);
	if (index < 0)
	{
		lua_pushnumber(L, eAliasNotFound);
		for (int i = 0; i < 4; ++i)
			lua_pushnil(L);
		return 5;
	}
	const WorldRuntime::Alias &alias = aliases.at(index);
	lua_pushnumber(L, eOK);
	lua_pushstring(L, alias.attributes.value(QStringLiteral("match")).toLocal8Bit().constData());
	lua_pushstring(L, alias.children.value(QStringLiteral("send")).toLocal8Bit().constData());

	int flags = 0;
	if (isEnabledValue(alias.attributes.value(QStringLiteral("enabled"))))
		flags |= eEnabled;
	if (isEnabledValue(alias.attributes.value(QStringLiteral("ignore_case"))))
		flags |= eIgnoreAliasCase;
	if (isEnabledValue(alias.attributes.value(QStringLiteral("omit_from_log"))))
		flags |= eOmitFromLogFile;
	if (isEnabledValue(alias.attributes.value(QStringLiteral("regexp"))))
		flags |= eAliasRegularExpression;
	if (isEnabledValue(alias.attributes.value(QStringLiteral("omit_from_output"))))
		flags |= eAliasOmitFromOutput;
	if (isEnabledValue(alias.attributes.value(QStringLiteral("expand_variables"))))
		flags |= eExpandVariables;
	const int sendTo = alias.attributes.value(QStringLiteral("send_to")).toInt();
	if (sendTo == eSendToSpeedwalk)
		flags |= eAliasSpeedWalk;
	if (sendTo == eSendToCommandQueue)
		flags |= eAliasQueue;
	if (isEnabledValue(alias.attributes.value(QStringLiteral("menu"))))
		flags |= eAliasMenu;
	if (isEnabledValue(alias.attributes.value(QStringLiteral("temporary"))))
		flags |= eTemporary;
	if (isEnabledValue(alias.attributes.value(QStringLiteral("one_shot"))))
		flags |= eAliasOneShot;

	lua_pushnumber(L, flags);
	lua_pushstring(L, alias.attributes.value(QStringLiteral("script")).toLocal8Bit().constData());
	return 5;
}

static int luaGetTimer(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, eWorldClosed);
		for (int i = 0; i < 6; ++i)
			lua_pushnil(L);
		return 7;
	}
	const QString         name   = QString::fromUtf8(luaL_checkstring(L, 1));
	WorldRuntime::Plugin *plugin = nullptr;
	if (int errorCode = eOK; !resolvePluginContext(engine, runtime, plugin, errorCode))
	{
		lua_pushnumber(L, errorCode);
		for (int i = 0; i < 6; ++i)
			lua_pushnil(L);
		return 7;
	}
	const QList<WorldRuntime::Timer> &timers = plugin ? plugin->timers : runtime->timers();
	const int                         index  = findTimerIndex(timers, name);
	if (index < 0)
	{
		lua_pushnumber(L, eTimerNotFound);
		for (int i = 0; i < 6; ++i)
			lua_pushnil(L);
		return 7;
	}
	const WorldRuntime::Timer &timer = timers.at(index);
	lua_pushnumber(L, eOK);
	lua_pushnumber(L, timer.attributes.value(QStringLiteral("hour")).toInt());
	lua_pushnumber(L, timer.attributes.value(QStringLiteral("minute")).toInt());
	lua_pushnumber(L, timer.attributes.value(QStringLiteral("second")).toDouble());
	lua_pushstring(L, timer.children.value(QStringLiteral("send")).toLocal8Bit().constData());

	int flags = 0;
	if (isEnabledValue(timer.attributes.value(QStringLiteral("enabled"))))
		flags |= eEnabled;
	if (isEnabledValue(timer.attributes.value(QStringLiteral("one_shot"))))
		flags |= eOneShot;
	const int sendTo = timer.attributes.value(QStringLiteral("send_to")).toInt();
	if (sendTo == eSendToSpeedwalk)
		flags |= eTimerSpeedWalk;
	if (sendTo == eSendToOutput)
		flags |= eTimerNote;
	if (isEnabledValue(timer.attributes.value(QStringLiteral("active_closed"))))
		flags |= eActiveWhenClosed;
	lua_pushnumber(L, flags);
	lua_pushstring(L, timer.attributes.value(QStringLiteral("script")).toLocal8Bit().constData());
	return 7;
}

template <typename Item> static int pushNamedAttributeList(lua_State *L, const QList<Item> &items)
{
	if (items.isEmpty())
	{
		lua_pushnil(L);
		return 1;
	}
	lua_newtable(L);
	static const auto kNameAttribute = QStringLiteral("name");
	int               index          = 1;
	for (const auto &item : items)
	{
		const auto    attrIt = item.attributes.constFind(kNameAttribute);
		const QString name   = attrIt != item.attributes.cend() ? attrIt.value() : QString{};
		lua_pushstring(L, name.toLocal8Bit().constData());
		lua_rawseti(L, -2, index++);
	}
	return 1;
}

static int luaGetTriggerList(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	WorldRuntime::Plugin *plugin = nullptr;
	if (int errorCode = eOK; !resolvePluginContext(engine, runtime, plugin, errorCode))
	{
		lua_pushnil(L);
		return 1;
	}
	const QList<WorldRuntime::Trigger> &triggers = plugin ? plugin->triggers : runtime->triggers();
	return pushNamedAttributeList(L, triggers);
}

static int luaGetAliasList(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	WorldRuntime::Plugin *plugin = nullptr;
	if (int errorCode = eOK; !resolvePluginContext(engine, runtime, plugin, errorCode))
	{
		lua_pushnil(L);
		return 1;
	}
	const QList<WorldRuntime::Alias> &aliases = plugin ? plugin->aliases : runtime->aliases();
	return pushNamedAttributeList(L, aliases);
}

static int luaGetTimerList(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	WorldRuntime::Plugin *plugin = nullptr;
	if (int errorCode = eOK; !resolvePluginContext(engine, runtime, plugin, errorCode))
	{
		lua_pushnil(L);
		return 1;
	}
	const QList<WorldRuntime::Timer> &timers = plugin ? plugin->timers : runtime->timers();
	return pushNamedAttributeList(L, timers);
}

static int luaIsTrigger(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}
	const QString         name   = QString::fromUtf8(luaL_checkstring(L, 1));
	WorldRuntime::Plugin *plugin = nullptr;
	if (int errorCode = eOK; !resolvePluginContext(engine, runtime, plugin, errorCode))
	{
		lua_pushnumber(L, errorCode);
		return 1;
	}
	const QList<WorldRuntime::Trigger> &triggers = plugin ? plugin->triggers : runtime->triggers();
	lua_pushnumber(L, findTriggerIndex(triggers, name) >= 0 ? eOK : eTriggerNotFound);
	return 1;
}

static int luaIsAlias(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}
	const QString         name   = QString::fromUtf8(luaL_checkstring(L, 1));
	WorldRuntime::Plugin *plugin = nullptr;
	if (int errorCode = eOK; !resolvePluginContext(engine, runtime, plugin, errorCode))
	{
		lua_pushnumber(L, errorCode);
		return 1;
	}
	const QList<WorldRuntime::Alias> &aliases = plugin ? plugin->aliases : runtime->aliases();
	lua_pushnumber(L, findAliasIndex(aliases, name) >= 0 ? eOK : eAliasNotFound);
	return 1;
}

static int luaIsTimer(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}
	const QString         name   = QString::fromUtf8(luaL_checkstring(L, 1));
	WorldRuntime::Plugin *plugin = nullptr;
	if (int errorCode = eOK; !resolvePluginContext(engine, runtime, plugin, errorCode))
	{
		lua_pushnumber(L, errorCode);
		return 1;
	}
	const QList<WorldRuntime::Timer> &timers = plugin ? plugin->timers : runtime->timers();
	lua_pushnumber(L, findTimerIndex(timers, name) >= 0 ? eOK : eTimerNotFound);
	return 1;
}

static int luaGetTriggerInfo(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const QString         name     = QString::fromUtf8(luaL_checkstring(L, 1));
	const int             infoType = static_cast<int>(luaL_checkinteger(L, 2));
	WorldRuntime::Plugin *plugin   = nullptr;
	if (int errorCode = eOK; !resolvePluginContext(engine, runtime, plugin, errorCode))
	{
		lua_pushnil(L);
		return 1;
	}
	const QList<WorldRuntime::Trigger> &triggers = plugin ? plugin->triggers : runtime->triggers();
	const int                           index    = findTriggerIndex(triggers, name);
	if (index < 0)
	{
		lua_pushnil(L);
		return 1;
	}
	const WorldRuntime::Trigger &trigger = triggers.at(index);
	QVariant                     value;
	switch (infoType)
	{
	case 1:
		value = trigger.attributes.value(QStringLiteral("match"));
		break;
	case 2:
		value = trigger.children.value(QStringLiteral("send"));
		break;
	case 3:
		value = trigger.attributes.value(QStringLiteral("sound"));
		break;
	case 4:
		value = trigger.attributes.value(QStringLiteral("script"));
		break;
	case 5:
		value = isEnabledValue(trigger.attributes.value(QStringLiteral("omit_from_log")));
		break;
	case 6:
		value = isEnabledValue(trigger.attributes.value(QStringLiteral("omit_from_output")));
		break;
	case 7:
		value = isEnabledValue(trigger.attributes.value(QStringLiteral("keep_evaluating")));
		break;
	case 8:
		value = isEnabledValue(trigger.attributes.value(QStringLiteral("enabled")));
		break;
	case 9:
		value = isEnabledValue(trigger.attributes.value(QStringLiteral("regexp")));
		break;
	case 10:
		value = isEnabledValue(trigger.attributes.value(QStringLiteral("ignore_case")));
		break;
	case 11:
		value = isEnabledValue(trigger.attributes.value(QStringLiteral("repeat")));
		break;
	case 12:
		value = isEnabledValue(trigger.attributes.value(QStringLiteral("sound_if_inactive")));
		break;
	case 13:
		value = isEnabledValue(trigger.attributes.value(QStringLiteral("expand_variables")));
		break;
	case 14:
		value = trigger.attributes.value(QStringLiteral("clipboard_arg")).toInt();
		break;
	case 15:
		value = trigger.attributes.value(QStringLiteral("send_to")).toInt();
		break;
	case 16:
		value = trigger.attributes.value(QStringLiteral("sequence")).toInt();
		break;
	case 17:
		value = buildTriggerMatchFlags(trigger);
		break;
	case 18:
		value = buildTriggerStyleFlags(trigger);
		break;
	case 19:
		value = triggerColourFromCustom(trigger.attributes.value(QStringLiteral("custom_colour")).toInt());
		break;
	case 20:
		value = trigger.invocationCount;
		break;
	case 21:
		value = trigger.matched;
		break;
	case 22:
		if (trigger.lastMatched.isValid())
			value = trigger.lastMatched.toSecsSinceEpoch();
		break;
	case 23:
		value = isEnabledValue(trigger.attributes.value(QStringLiteral("temporary")));
		break;
	case 24:
		value = trigger.included;
		break;
	case 25:
		value = isEnabledValue(trigger.attributes.value(QStringLiteral("lowercase_wildcard")));
		break;
	case 26:
		value = trigger.attributes.value(QStringLiteral("group"));
		break;
	case 27:
		value = trigger.attributes.value(QStringLiteral("variable"));
		break;
	case 28:
		value = trigger.attributes.value(QStringLiteral("user")).toLongLong();
		break;
	case 29:
		value = trigger.attributes.value(QStringLiteral("other_text_colour")).toLongLong();
		break;
	case 30:
		value = trigger.attributes.value(QStringLiteral("other_back_colour")).toLongLong();
		break;
	case 31:
		value = trigger.matchAttempts;
		break;
	case 32:
		value = trigger.lastMatchTarget;
		break;
	case 33:
		value = trigger.executingScript;
		break;
	case 34:
		value = engine->hasFunction(trigger.attributes.value(QStringLiteral("script")));
		break;
	case 35:
		value = 0;
		break;
	case 36:
		value = isEnabledValue(trigger.attributes.value(QStringLiteral("one_shot")));
		break;
	default:
		break;
	}
	pushVariant(L, value);
	return 1;
}

static int luaGetAliasInfo(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const QString         name     = QString::fromUtf8(luaL_checkstring(L, 1));
	const int             infoType = static_cast<int>(luaL_checkinteger(L, 2));
	WorldRuntime::Plugin *plugin   = nullptr;
	if (int errorCode = eOK; !resolvePluginContext(engine, runtime, plugin, errorCode))
	{
		lua_pushnil(L);
		return 1;
	}
	const QList<WorldRuntime::Alias> &aliases = plugin ? plugin->aliases : runtime->aliases();
	const int                         index   = findAliasIndex(aliases, name);
	if (index < 0)
	{
		lua_pushnil(L);
		return 1;
	}
	const auto &[attributes, children, included, matched, invocationCount, matchAttempts, lastMatchTarget,
	             lastMatched, executingScript] = aliases.at(index);
	QVariant value;
	switch (infoType)
	{
	case 1:
		value = attributes.value(QStringLiteral("match"));
		break;
	case 2:
		value = children.value(QStringLiteral("send"));
		break;
	case 3:
		value = attributes.value(QStringLiteral("script"));
		break;
	case 4:
		value = isEnabledValue(attributes.value(QStringLiteral("omit_from_log")));
		break;
	case 5:
		value = isEnabledValue(attributes.value(QStringLiteral("omit_from_output")));
		break;
	case 6:
		value = isEnabledValue(attributes.value(QStringLiteral("enabled")));
		break;
	case 7:
		value = isEnabledValue(attributes.value(QStringLiteral("regexp")));
		break;
	case 8:
		value = isEnabledValue(attributes.value(QStringLiteral("ignore_case")));
		break;
	case 9:
		value = isEnabledValue(attributes.value(QStringLiteral("expand_variables")));
		break;
	case 10:
		value = invocationCount;
		break;
	case 11:
		value = matched;
		break;
	case 12:
		value = isEnabledValue(attributes.value(QStringLiteral("menu")));
		break;
	case 13:
		if (lastMatched.isValid())
			value = lastMatched.toSecsSinceEpoch();
		break;
	case 14:
		value = isEnabledValue(attributes.value(QStringLiteral("temporary")));
		break;
	case 15:
		value = included;
		break;
	case 16:
		value = attributes.value(QStringLiteral("group"));
		break;
	case 17:
		value = attributes.value(QStringLiteral("variable"));
		break;
	case 18:
		value = attributes.value(QStringLiteral("send_to")).toInt();
		break;
	case 19:
		value = isEnabledValue(attributes.value(QStringLiteral("keep_evaluating")));
		break;
	case 20:
		value = attributes.value(QStringLiteral("sequence")).toInt();
		break;
	case 21:
		value = isEnabledValue(attributes.value(QStringLiteral("echo_alias")));
		break;
	case 22:
		value = isEnabledValue(attributes.value(QStringLiteral("omit_from_command_history")));
		break;
	case 23:
		value = attributes.value(QStringLiteral("user")).toLongLong();
		break;
	case 24:
		value = matchAttempts;
		break;
	case 25:
		value = lastMatchTarget;
		break;
	case 26:
		value = executingScript;
		break;
	case 27:
		value = engine->hasFunction(attributes.value(QStringLiteral("script")));
		break;
	case 28:
		value = 0;
		break;
	case 29:
		value = isEnabledValue(attributes.value(QStringLiteral("one_shot")));
		break;
	default:
		break;
	}
	pushVariant(L, value);
	return 1;
}

static int luaGetTimerInfo(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const QString         name     = QString::fromUtf8(luaL_checkstring(L, 1));
	const int             infoType = static_cast<int>(luaL_checkinteger(L, 2));
	WorldRuntime::Plugin *plugin   = nullptr;
	if (int errorCode = eOK; !resolvePluginContext(engine, runtime, plugin, errorCode))
	{
		lua_pushnil(L);
		return 1;
	}
	const QList<WorldRuntime::Timer> &timers = plugin ? plugin->timers : runtime->timers();
	const int                         index  = findTimerIndex(timers, name);
	if (index < 0)
	{
		lua_pushnil(L);
		return 1;
	}
	const auto &[attributes, children, included, lastFired, nextFireTime, firedCount, invocationCount,
	             executingScript] = timers.at(index);
	QVariant   value;
	const bool atTime = isEnabledValue(attributes.value(QStringLiteral("at_time")));
	switch (infoType)
	{
	case 1:
		value = attributes.value(QStringLiteral("hour")).toInt();
		break;
	case 2:
		value = attributes.value(QStringLiteral("minute")).toInt();
		break;
	case 3:
		value = attributes.value(QStringLiteral("second")).toDouble();
		break;
	case 4:
		value = children.value(QStringLiteral("send"));
		break;
	case 5:
		value = attributes.value(QStringLiteral("script"));
		break;
	case 6:
		value = isEnabledValue(attributes.value(QStringLiteral("enabled")));
		break;
	case 7:
		value = isEnabledValue(attributes.value(QStringLiteral("one_shot")));
		break;
	case 8:
		value = atTime;
		break;
	case 9:
		value = invocationCount;
		break;
	case 10:
		value = firedCount;
		break;
	case 11:
		if (lastFired.isValid())
			value = lastFired.toSecsSinceEpoch();
		break;
	case 12:
		if (nextFireTime.isValid())
			value = nextFireTime.toSecsSinceEpoch();
		break;
	case 13:
		if (nextFireTime.isValid())
		{
			const qint64 secs = QDateTime::currentDateTime().secsTo(nextFireTime);
			value             = static_cast<double>(secs < 0 ? 0 : secs);
		}
		break;
	case 14:
		value = isEnabledValue(attributes.value(QStringLiteral("temporary")));
		break;
	case 15:
		value = attributes.value(QStringLiteral("send_to")).toInt() == eSendToSpeedwalk;
		break;
	case 16:
		value = attributes.value(QStringLiteral("send_to")).toInt() == eSendToOutput;
		break;
	case 17:
		value = isEnabledValue(attributes.value(QStringLiteral("active_closed")));
		break;
	case 18:
		value = included;
		break;
	case 19:
		value = attributes.value(QStringLiteral("group"));
		break;
	case 20:
		value = attributes.value(QStringLiteral("send_to")).toInt();
		break;
	case 21:
		value = attributes.value(QStringLiteral("user")).toLongLong();
		break;
	case 22:
		value = attributes.value(QStringLiteral("name"));
		break;
	case 23:
		value = isEnabledValue(attributes.value(QStringLiteral("omit_from_output")));
		break;
	case 24:
		value = isEnabledValue(attributes.value(QStringLiteral("omit_from_log")));
		break;
	case 25:
		value = executingScript;
		break;
	case 26:
		value = engine->hasFunction(attributes.value(QStringLiteral("script")));
		break;
	default:
		break;
	}
	pushVariant(L, value);
	return 1;
}

static int luaGetTriggerOption(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const QString         name    = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString         optName = QString::fromUtf8(luaL_checkstring(L, 2)).trimmed().toLower();
	WorldRuntime::Plugin *plugin  = nullptr;
	if (int errorCode = eOK; !resolvePluginContext(engine, runtime, plugin, errorCode))
	{
		lua_pushnil(L);
		return 1;
	}
	const QList<WorldRuntime::Trigger> &triggers = plugin ? plugin->triggers : runtime->triggers();
	const int                           index    = findTriggerIndex(triggers, name);
	if (index < 0)
	{
		lua_pushnil(L);
		return 1;
	}
	const WorldRuntime::Trigger &trigger = triggers.at(index);
	if (optName == QStringLiteral("group") || optName == QStringLiteral("match") ||
	    optName == QStringLiteral("script") || optName == QStringLiteral("sound") ||
	    optName == QStringLiteral("variable"))
	{
		lua_pushstring(L, trigger.attributes.value(optName).toLocal8Bit().constData());
		return 1;
	}
	if (optName == QStringLiteral("send"))
	{
		lua_pushstring(L, trigger.children.value(QStringLiteral("send")).toLocal8Bit().constData());
		return 1;
	}
	if (optName == QStringLiteral("match_style"))
	{
		lua_pushnumber(L, buildTriggerMatchFlags(trigger));
		return 1;
	}
	if (optName == QStringLiteral("new_style"))
	{
		lua_pushnumber(L, buildTriggerStyleFlags(trigger));
		return 1;
	}

	auto pushBool = [&](const QString &key)
	{ lua_pushnumber(L, isEnabledValue(trigger.attributes.value(key)) ? 1 : 0); };

	if (optName == QStringLiteral("enabled") || optName == QStringLiteral("expand_variables") ||
	    optName == QStringLiteral("ignore_case") || optName == QStringLiteral("keep_evaluating") ||
	    optName == QStringLiteral("multi_line") || optName == QStringLiteral("omit_from_log") ||
	    optName == QStringLiteral("omit_from_output") || optName == QStringLiteral("regexp") ||
	    optName == QStringLiteral("repeat") || optName == QStringLiteral("sound_if_inactive") ||
	    optName == QStringLiteral("lowercase_wildcard") || optName == QStringLiteral("temporary") ||
	    optName == QStringLiteral("one_shot"))
	{
		pushBool(optName);
		return 1;
	}

	if (optName == QStringLiteral("clipboard_arg") || optName == QStringLiteral("colour_change_type") ||
	    optName == QStringLiteral("custom_colour") || optName == QStringLiteral("lines_to_match") ||
	    optName == QStringLiteral("other_text_colour") || optName == QStringLiteral("other_back_colour") ||
	    optName == QStringLiteral("send_to") || optName == QStringLiteral("sequence") ||
	    optName == QStringLiteral("user"))
	{
		lua_pushnumber(L, trigger.attributes.value(optName).toDouble());
		return 1;
	}

	lua_pushnil(L);
	return 1;
}

static int luaSetTriggerOption(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}
	const QString         name    = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString         optName = QString::fromUtf8(luaL_checkstring(L, 2)).trimmed().toLower();
	WorldRuntime::Plugin *plugin  = nullptr;
	if (int errorCode = eOK; !resolvePluginContext(engine, runtime, plugin, errorCode))
	{
		lua_pushnumber(L, errorCode);
		return 1;
	}
	QList<WorldRuntime::Trigger> &triggers = mutableTriggerList(runtime, plugin);
	const int                     index    = findTriggerIndex(triggers, name);
	if (index < 0)
	{
		lua_pushnumber(L, eTriggerNotFound);
		return 1;
	}
	WorldRuntime::Trigger &trigger             = triggers[index];
	const auto             commitTriggerChange = [&]
	{
		applyTriggerDefaults(trigger);
		commitTriggerListMutation(runtime, plugin);
	};

	if (optName == QStringLiteral("ignore_case") || optName == QStringLiteral("regexp"))
	{
		lua_pushnumber(L, ePluginCannotSetOption);
		return 1;
	}

	if (optName == QStringLiteral("group") || optName == QStringLiteral("match") ||
	    optName == QStringLiteral("script") || optName == QStringLiteral("sound") ||
	    optName == QStringLiteral("variable"))
	{
		trigger.attributes.insert(optName, QString::fromUtf8(luaL_checkstring(L, 3)));
		commitTriggerChange();
		lua_pushnumber(L, eOK);
		return 1;
	}

	auto setMatchStyle = [&](const int match)
	{
		trigger.attributes.insert(QStringLiteral("text_colour"), QString::number(match >> 4 & 0x0F));
		trigger.attributes.insert(QStringLiteral("back_colour"), QString::number(match >> 8 & 0x0F));
		trigger.attributes.insert(QStringLiteral("bold"), attrFlag(match & kStyleHilite));
		trigger.attributes.insert(QStringLiteral("italic"), attrFlag(match & kStyleBlink));
		trigger.attributes.insert(QStringLiteral("underline"), attrFlag(match & kStyleUnderline));
		trigger.attributes.insert(QStringLiteral("inverse"), attrFlag(match & kStyleInverse));
		trigger.attributes.insert(QStringLiteral("match_text_colour"), attrFlag(match & kTriggerMatchText));
		trigger.attributes.insert(QStringLiteral("match_back_colour"), attrFlag(match & kTriggerMatchBack));
		trigger.attributes.insert(QStringLiteral("match_bold"), attrFlag(match & kTriggerMatchHilite));
		trigger.attributes.insert(QStringLiteral("match_italic"), attrFlag(match & kTriggerMatchBlink));
		trigger.attributes.insert(QStringLiteral("match_inverse"), attrFlag(match & kTriggerMatchInverse));
		trigger.attributes.insert(QStringLiteral("match_underline"),
		                          attrFlag(match & kTriggerMatchUnderline));
	};

	auto setNewStyle = [&](const int style)
	{
		trigger.attributes.insert(QStringLiteral("make_bold"), attrFlag(style & kStyleHilite));
		trigger.attributes.insert(QStringLiteral("make_italic"), attrFlag(style & kStyleBlink));
		trigger.attributes.insert(QStringLiteral("make_underline"), attrFlag(style & kStyleUnderline));
	};

	if (optName == QStringLiteral("match_style") || optName == QStringLiteral("new_style"))
	{
		double number = 0.0;
		if (!parseNumberArg(L, number))
		{
			lua_pushnumber(L, eOptionOutOfRange);
			return 1;
		}
		if (optName == QStringLiteral("match_style"))
		{
			if (number < 0 || number > 0xFFFF)
			{
				lua_pushnumber(L, eOptionOutOfRange);
				return 1;
			}
			setMatchStyle(static_cast<int>(number));
		}
		else
		{
			if (number < 0 || number > 7)
			{
				lua_pushnumber(L, eOptionOutOfRange);
				return 1;
			}
			setNewStyle(static_cast<int>(number));
		}

		commitTriggerChange();
		lua_pushnumber(L, eOK);
		return 1;
	}
	if (optName == QStringLiteral("send"))
	{
		trigger.children.insert(QStringLiteral("send"), QString::fromUtf8(luaL_checkstring(L, 3)));
		commitTriggerChange();
		lua_pushnumber(L, eOK);
		return 1;
	}

	auto setBool = [&](const QString &key) -> bool
	{
		bool value = false;
		if (!parseBoolArg(L, value))
			return false;
		trigger.attributes.insert(key, attrFlag(value));
		return true;
	};

	if (optName == QStringLiteral("enabled") || optName == QStringLiteral("expand_variables") ||
	    optName == QStringLiteral("ignore_case") || optName == QStringLiteral("keep_evaluating") ||
	    optName == QStringLiteral("multi_line") || optName == QStringLiteral("omit_from_log") ||
	    optName == QStringLiteral("omit_from_output") || optName == QStringLiteral("regexp") ||
	    optName == QStringLiteral("repeat") || optName == QStringLiteral("sound_if_inactive") ||
	    optName == QStringLiteral("lowercase_wildcard") || optName == QStringLiteral("temporary") ||
	    optName == QStringLiteral("one_shot"))
	{
		if (!setBool(optName))
		{
			lua_pushnumber(L, eOptionOutOfRange);
			return 1;
		}
		commitTriggerChange();
		lua_pushnumber(L, eOK);
		return 1;
	}

	double number = 0.0;
	if (!parseNumberArg(L, number))
	{
		lua_pushnumber(L, eOptionOutOfRange);
		return 1;
	}

	if (optName == QStringLiteral("clipboard_arg"))
	{
		if (number < 0 || number > 10)
		{
			lua_pushnumber(L, eOptionOutOfRange);
			return 1;
		}
		trigger.attributes.insert(optName, QString::number(static_cast<int>(number)));
	}
	else if (optName == QStringLiteral("colour_change_type"))
	{
		if (number < TRIGGER_COLOUR_CHANGE_BOTH || number > TRIGGER_COLOUR_CHANGE_BACKGROUND)
		{
			lua_pushnumber(L, eOptionOutOfRange);
			return 1;
		}
		trigger.attributes.insert(optName, QString::number(static_cast<int>(number)));
	}
	else if (optName == QStringLiteral("custom_colour"))
	{
		if (number < 0 || number > MAX_CUSTOM + 1)
		{
			lua_pushnumber(L, eOptionOutOfRange);
			return 1;
		}
		trigger.attributes.insert(optName, QString::number(static_cast<int>(number)));
	}
	else if (optName == QStringLiteral("lines_to_match") || optName == QStringLiteral("sequence"))
	{
		if (number < 0 || number > 10000)
		{
			lua_pushnumber(L, eOptionOutOfRange);
			return 1;
		}
		trigger.attributes.insert(optName, QString::number(static_cast<int>(number)));
	}
	else if (optName == QStringLiteral("other_text_colour") || optName == QStringLiteral("other_back_colour"))
	{
		if (number < 0 || number > 0xFFFFFF)
		{
			lua_pushnumber(L, eOptionOutOfRange);
			return 1;
		}
		trigger.attributes.insert(optName, QString::number(static_cast<long long>(number)));
	}
	else if (optName == QStringLiteral("send_to"))
	{
		const int sendTo = static_cast<int>(number);
		if (sendTo < 0 || sendTo >= eSendToLast)
		{
			lua_pushnumber(L, eOptionOutOfRange);
			return 1;
		}
		trigger.attributes.insert(optName, QString::number(sendTo));
	}
	else if (optName == QStringLiteral("user"))
	{
		trigger.attributes.insert(optName, QString::number(static_cast<long long>(number)));
	}
	else
	{
		lua_pushnumber(L, eUnknownOption);
		return 1;
	}

	commitTriggerChange();
	lua_pushnumber(L, eOK);
	return 1;
}

static int luaGetAliasOption(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const QString         name    = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString         optName = QString::fromUtf8(luaL_checkstring(L, 2)).trimmed().toLower();
	WorldRuntime::Plugin *plugin  = nullptr;
	if (int errorCode = eOK; !resolvePluginContext(engine, runtime, plugin, errorCode))
	{
		lua_pushnil(L);
		return 1;
	}
	const QList<WorldRuntime::Alias> &aliases = plugin ? plugin->aliases : runtime->aliases();
	const int                         index   = findAliasIndex(aliases, name);
	if (index < 0)
	{
		lua_pushnil(L);
		return 1;
	}
	const WorldRuntime::Alias &alias = aliases.at(index);
	if (optName == QStringLiteral("group") || optName == QStringLiteral("match") ||
	    optName == QStringLiteral("script") || optName == QStringLiteral("variable"))
	{
		lua_pushstring(L, alias.attributes.value(optName).toLocal8Bit().constData());
		return 1;
	}
	if (optName == QStringLiteral("send"))
	{
		lua_pushstring(L, alias.children.value(QStringLiteral("send")).toLocal8Bit().constData());
		return 1;
	}

	auto pushBool = [&](const QString &key)
	{ lua_pushnumber(L, isEnabledValue(alias.attributes.value(key)) ? 1 : 0); };

	if (optName == QStringLiteral("enabled") || optName == QStringLiteral("expand_variables") ||
	    optName == QStringLiteral("ignore_case") || optName == QStringLiteral("omit_from_log") ||
	    optName == QStringLiteral("omit_from_command_history") ||
	    optName == QStringLiteral("omit_from_output") || optName == QStringLiteral("regexp") ||
	    optName == QStringLiteral("menu") || optName == QStringLiteral("keep_evaluating") ||
	    optName == QStringLiteral("echo_alias") || optName == QStringLiteral("temporary") ||
	    optName == QStringLiteral("one_shot"))
	{
		pushBool(optName);
		return 1;
	}

	if (optName == QStringLiteral("send_to") || optName == QStringLiteral("sequence") ||
	    optName == QStringLiteral("user"))
	{
		lua_pushnumber(L, alias.attributes.value(optName).toDouble());
		return 1;
	}

	lua_pushnil(L);
	return 1;
}

static int luaSetAliasOption(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}
	const QString         name    = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString         optName = QString::fromUtf8(luaL_checkstring(L, 2)).trimmed().toLower();
	WorldRuntime::Plugin *plugin  = nullptr;
	if (int errorCode = eOK; !resolvePluginContext(engine, runtime, plugin, errorCode))
	{
		lua_pushnumber(L, errorCode);
		return 1;
	}
	QList<WorldRuntime::Alias> &aliases = mutableAliasList(runtime, plugin);
	const int                   index   = findAliasIndex(aliases, name);
	if (index < 0)
	{
		lua_pushnumber(L, eAliasNotFound);
		return 1;
	}
	WorldRuntime::Alias &alias             = aliases[index];
	const auto           commitAliasChange = [&]
	{
		applyAliasDefaults(alias);
		commitAliasListMutation(runtime, plugin);
	};

	if (optName == QStringLiteral("ignore_case") || optName == QStringLiteral("regexp"))
	{
		lua_pushnumber(L, ePluginCannotSetOption);
		return 1;
	}

	if (optName == QStringLiteral("group") || optName == QStringLiteral("match") ||
	    optName == QStringLiteral("script") || optName == QStringLiteral("variable"))
	{
		alias.attributes.insert(optName, QString::fromUtf8(luaL_checkstring(L, 3)));
		commitAliasChange();
		lua_pushnumber(L, eOK);
		return 1;
	}
	if (optName == QStringLiteral("send"))
	{
		alias.children.insert(QStringLiteral("send"), QString::fromUtf8(luaL_checkstring(L, 3)));
		commitAliasChange();
		lua_pushnumber(L, eOK);
		return 1;
	}

	auto setBool = [&](const QString &key) -> bool
	{
		bool value = false;
		if (!parseBoolArg(L, value))
			return false;
		alias.attributes.insert(key, attrFlag(value));
		return true;
	};

	if (optName == QStringLiteral("enabled") || optName == QStringLiteral("expand_variables") ||
	    optName == QStringLiteral("ignore_case") || optName == QStringLiteral("omit_from_log") ||
	    optName == QStringLiteral("omit_from_command_history") ||
	    optName == QStringLiteral("omit_from_output") || optName == QStringLiteral("regexp") ||
	    optName == QStringLiteral("menu") || optName == QStringLiteral("keep_evaluating") ||
	    optName == QStringLiteral("echo_alias") || optName == QStringLiteral("temporary") ||
	    optName == QStringLiteral("one_shot"))
	{
		if (!setBool(optName))
		{
			lua_pushnumber(L, eOptionOutOfRange);
			return 1;
		}
		commitAliasChange();
		lua_pushnumber(L, eOK);
		return 1;
	}

	double number = 0.0;
	if (!parseNumberArg(L, number))
	{
		lua_pushnumber(L, eOptionOutOfRange);
		return 1;
	}

	if (optName == QStringLiteral("send_to"))
	{
		const int sendTo = static_cast<int>(number);
		if (sendTo < 0 || sendTo >= eSendToLast)
		{
			lua_pushnumber(L, eOptionOutOfRange);
			return 1;
		}
		alias.attributes.insert(optName, QString::number(sendTo));
	}
	else if (optName == QStringLiteral("sequence"))
	{
		if (number < 0 || number > 10000)
		{
			lua_pushnumber(L, eOptionOutOfRange);
			return 1;
		}
		alias.attributes.insert(optName, QString::number(static_cast<int>(number)));
	}
	else if (optName == QStringLiteral("user"))
	{
		alias.attributes.insert(optName, QString::number(static_cast<long long>(number)));
	}
	else
	{
		lua_pushnumber(L, eUnknownOption);
		return 1;
	}

	commitAliasChange();
	lua_pushnumber(L, eOK);
	return 1;
}

static int luaGetTimerOption(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const QString         name    = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString         optName = QString::fromUtf8(luaL_checkstring(L, 2)).trimmed().toLower();
	WorldRuntime::Plugin *plugin  = nullptr;
	if (int errorCode = eOK; !resolvePluginContext(engine, runtime, plugin, errorCode))
	{
		lua_pushnil(L);
		return 1;
	}
	const QList<WorldRuntime::Timer> &timers = plugin ? plugin->timers : runtime->timers();
	const int                         index  = findTimerIndex(timers, name);
	if (index < 0)
	{
		lua_pushnil(L);
		return 1;
	}
	const WorldRuntime::Timer &timer = timers.at(index);
	if (optName == QStringLiteral("group") || optName == QStringLiteral("script") ||
	    optName == QStringLiteral("variable"))
	{
		lua_pushstring(L, timer.attributes.value(optName).toLocal8Bit().constData());
		return 1;
	}
	if (optName == QStringLiteral("send"))
	{
		lua_pushstring(L, timer.children.value(QStringLiteral("send")).toLocal8Bit().constData());
		return 1;
	}

	auto pushBool = [&](const QString &key)
	{ lua_pushnumber(L, isEnabledValue(timer.attributes.value(key)) ? 1 : 0); };

	if (optName == QStringLiteral("enabled") || optName == QStringLiteral("at_time") ||
	    optName == QStringLiteral("one_shot") || optName == QStringLiteral("omit_from_output") ||
	    optName == QStringLiteral("omit_from_log") || optName == QStringLiteral("active_closed") ||
	    optName == QStringLiteral("temporary"))
	{
		pushBool(optName);
		return 1;
	}

	if (optName == QStringLiteral("hour") || optName == QStringLiteral("minute") ||
	    optName == QStringLiteral("second") || optName == QStringLiteral("offset_hour") ||
	    optName == QStringLiteral("offset_minute") || optName == QStringLiteral("offset_second") ||
	    optName == QStringLiteral("send_to") || optName == QStringLiteral("user"))
	{
		lua_pushnumber(L, timer.attributes.value(optName).toDouble());
		return 1;
	}

	lua_pushnil(L);
	return 1;
}

static int luaSetTimerOption(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}
	const QString         name    = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString         optName = QString::fromUtf8(luaL_checkstring(L, 2)).trimmed().toLower();
	WorldRuntime::Plugin *plugin  = nullptr;
	if (int errorCode = eOK; !resolvePluginContext(engine, runtime, plugin, errorCode))
	{
		lua_pushnumber(L, errorCode);
		return 1;
	}
	QList<WorldRuntime::Timer> &timers = mutableTimerList(runtime, plugin);
	const int                   index  = findTimerIndex(timers, name);
	if (index < 0)
	{
		lua_pushnumber(L, eTimerNotFound);
		return 1;
	}
	WorldRuntime::Timer &timer             = timers[index];
	const auto           commitTimerChange = [&]
	{
		applyTimerDefaults(timer);
		commitTimerListMutation(runtime, plugin);
	};

	if (optName == QStringLiteral("group") || optName == QStringLiteral("script") ||
	    optName == QStringLiteral("variable"))
	{
		timer.attributes.insert(optName, QString::fromUtf8(luaL_checkstring(L, 3)));
		commitTimerChange();
		lua_pushnumber(L, eOK);
		return 1;
	}
	if (optName == QStringLiteral("send"))
	{
		timer.children.insert(QStringLiteral("send"), QString::fromUtf8(luaL_checkstring(L, 3)));
		commitTimerChange();
		lua_pushnumber(L, eOK);
		return 1;
	}

	auto setBool = [&](const QString &key) -> bool
	{
		bool value = false;
		if (!parseBoolArg(L, value))
			return false;
		timer.attributes.insert(key, attrFlag(value));
		return true;
	};

	if (optName == QStringLiteral("enabled") || optName == QStringLiteral("at_time") ||
	    optName == QStringLiteral("one_shot") || optName == QStringLiteral("omit_from_output") ||
	    optName == QStringLiteral("omit_from_log") || optName == QStringLiteral("active_closed") ||
	    optName == QStringLiteral("temporary"))
	{
		if (!setBool(optName))
		{
			lua_pushnumber(L, eOptionOutOfRange);
			return 1;
		}
		if (optName == QStringLiteral("at_time"))
			resetTimerFields(timer);
		commitTimerChange();
		lua_pushnumber(L, eOK);
		return 1;
	}

	double number = 0.0;
	if (!parseNumberArg(L, number))
	{
		lua_pushnumber(L, eOptionOutOfRange);
		return 1;
	}

	bool resetSchedule = false;
	if (optName == QStringLiteral("hour") || optName == QStringLiteral("offset_hour"))
	{
		if (number < 0 || number > 23)
		{
			lua_pushnumber(L, eOptionOutOfRange);
			return 1;
		}
		timer.attributes.insert(optName, QString::number(static_cast<int>(number)));
		resetSchedule = true;
	}
	else if (optName == QStringLiteral("minute") || optName == QStringLiteral("offset_minute"))
	{
		if (number < 0 || number > 59)
		{
			lua_pushnumber(L, eOptionOutOfRange);
			return 1;
		}
		timer.attributes.insert(optName, QString::number(static_cast<int>(number)));
		resetSchedule = true;
	}
	else if (optName == QStringLiteral("second") || optName == QStringLiteral("offset_second"))
	{
		if (number < 0 || number > 59.9999)
		{
			lua_pushnumber(L, eOptionOutOfRange);
			return 1;
		}
		timer.attributes.insert(optName, QString::number(number, 'f', 4));
		resetSchedule = true;
	}
	else if (optName == QStringLiteral("send_to"))
	{
		const int sendTo = static_cast<int>(number);
		if (sendTo < 0 || sendTo >= eSendToLast)
		{
			lua_pushnumber(L, eOptionOutOfRange);
			return 1;
		}
		timer.attributes.insert(optName, QString::number(sendTo));
	}
	else if (optName == QStringLiteral("user"))
	{
		timer.attributes.insert(optName, QString::number(static_cast<long long>(number)));
	}
	else
	{
		lua_pushnumber(L, eUnknownOption);
		return 1;
	}

	if (resetSchedule)
		resetTimerFields(timer);

	commitTimerChange();
	lua_pushnumber(L, eOK);
	return 1;
}

static int luaGetTriggerWildcard(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const QString name         = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString wildcardName = QString::fromUtf8(luaL_checkstring(L, 2));
	QString       value;
	bool          ok = false;
	if (!engine->pluginId().isEmpty())
		ok = runtime->pluginTriggerWildcard(engine->pluginId(), name, wildcardName, value);
	else
		ok = runtime->triggerWildcard(name, wildcardName, value);
	if (!ok)
	{
		// MUSHclient parity: missing wildcard lookup returns empty string for Lua callers.
		lua_pushstring(L, "");
		return 1;
	}
	lua_pushstring(L, value.toLocal8Bit().constData());
	return 1;
}

static int luaGetAliasWildcard(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const QString name         = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString wildcardName = QString::fromUtf8(luaL_checkstring(L, 2));
	QString       value;
	bool          ok = false;
	if (!engine->pluginId().isEmpty())
		ok = runtime->pluginAliasWildcard(engine->pluginId(), name, wildcardName, value);
	else
		ok = runtime->aliasWildcard(name, wildcardName, value);
	if (!ok)
	{
		// MUSHclient parity: missing wildcard lookup returns empty string for Lua callers.
		lua_pushstring(L, "");
		return 1;
	}
	lua_pushstring(L, value.toLocal8Bit().constData());
	return 1;
}

static int luaResetTimer(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnumber(L, eWorldClosed);
		return 1;
	}
	const QString         name   = QString::fromUtf8(luaL_checkstring(L, 1));
	WorldRuntime::Plugin *plugin = nullptr;
	if (int errorCode = eOK; !resolvePluginContext(engine, runtime, plugin, errorCode))
	{
		lua_pushnumber(L, errorCode);
		return 1;
	}
	QList<WorldRuntime::Timer> &timers = mutableTimerList(runtime, plugin);
	const int                   index  = findTimerIndex(timers, name);
	if (index < 0)
	{
		lua_pushnumber(L, eTimerNotFound);
		return 1;
	}
	applyTimerDefaults(timers[index]);
	resetTimerFields(timers[index]);
	commitTimerListMutation(runtime, plugin);
	lua_pushnumber(L, eOK);
	return 1;
}

static int luaResetTimers(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
		return 0;
	WorldRuntime::Plugin *plugin = nullptr;
	if (int errorCode = eOK; !resolvePluginContext(engine, runtime, plugin, errorCode))
		return 0;
	for (QList<WorldRuntime::Timer> &timers = mutableTimerList(runtime, plugin); auto &timer : timers)
	{
		applyTimerDefaults(timer);
		resetTimerFields(timer);
	}
	commitTimerListMutation(runtime, plugin);
	return 0;
}

static int luaStopEvaluatingTriggers(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
		return 0;
	const bool allPlugins = optBool(L, 1, false);
	runtime->setStopTriggerEvaluation(allPlugins ? WorldRuntime::StopAllSequences
	                                             : WorldRuntime::StopCurrentSequence);
	return 0;
}

static int luaGetPluginTriggerInfo(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const QString         pluginId = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString         name     = QString::fromUtf8(luaL_checkstring(L, 2));
	const int             infoType = static_cast<int>(luaL_checkinteger(L, 3));
	WorldRuntime::Plugin *plugin   = runtime->pluginForId(pluginId);
	if (!plugin)
	{
		lua_pushnil(L);
		return 1;
	}
	const int index = findTriggerIndex(plugin->triggers, name);
	if (index < 0)
	{
		lua_pushnil(L);
		return 1;
	}
	const WorldRuntime::Trigger &trigger = plugin->triggers.at(index);
	QVariant                     value;
	switch (infoType)
	{
	case 1:
		value = trigger.attributes.value(QStringLiteral("match"));
		break;
	case 2:
		value = trigger.children.value(QStringLiteral("send"));
		break;
	case 3:
		value = trigger.attributes.value(QStringLiteral("sound"));
		break;
	case 4:
		value = trigger.attributes.value(QStringLiteral("script"));
		break;
	case 5:
		value = isEnabledValue(trigger.attributes.value(QStringLiteral("omit_from_log")));
		break;
	case 6:
		value = isEnabledValue(trigger.attributes.value(QStringLiteral("omit_from_output")));
		break;
	case 7:
		value = isEnabledValue(trigger.attributes.value(QStringLiteral("keep_evaluating")));
		break;
	case 8:
		value = isEnabledValue(trigger.attributes.value(QStringLiteral("enabled")));
		break;
	case 9:
		value = isEnabledValue(trigger.attributes.value(QStringLiteral("regexp")));
		break;
	case 10:
		value = isEnabledValue(trigger.attributes.value(QStringLiteral("ignore_case")));
		break;
	case 11:
		value = isEnabledValue(trigger.attributes.value(QStringLiteral("repeat")));
		break;
	case 12:
		value = isEnabledValue(trigger.attributes.value(QStringLiteral("sound_if_inactive")));
		break;
	case 13:
		value = isEnabledValue(trigger.attributes.value(QStringLiteral("expand_variables")));
		break;
	case 14:
		value = trigger.attributes.value(QStringLiteral("clipboard_arg")).toInt();
		break;
	case 15:
		value = trigger.attributes.value(QStringLiteral("send_to")).toInt();
		break;
	case 16:
		value = trigger.attributes.value(QStringLiteral("sequence")).toInt();
		break;
	case 17:
		value = buildTriggerMatchFlags(trigger);
		break;
	case 18:
		value = buildTriggerStyleFlags(trigger);
		break;
	case 19:
		value = triggerColourFromCustom(trigger.attributes.value(QStringLiteral("custom_colour")).toInt());
		break;
	case 20:
		value = trigger.invocationCount;
		break;
	case 21:
		value = trigger.matched;
		break;
	case 22:
		if (trigger.lastMatched.isValid())
			value = trigger.lastMatched.toSecsSinceEpoch();
		break;
	case 23:
		value = isEnabledValue(trigger.attributes.value(QStringLiteral("temporary")));
		break;
	case 24:
		value = trigger.included;
		break;
	case 25:
		value = isEnabledValue(trigger.attributes.value(QStringLiteral("lowercase_wildcard")));
		break;
	case 26:
		value = trigger.attributes.value(QStringLiteral("group"));
		break;
	case 27:
		value = trigger.attributes.value(QStringLiteral("variable"));
		break;
	case 28:
		value = trigger.attributes.value(QStringLiteral("user")).toLongLong();
		break;
	case 31:
		value = trigger.matchAttempts;
		break;
	case 32:
		value = trigger.lastMatchTarget;
		break;
	case 33:
		value = trigger.executingScript;
		break;
	case 34:
		value = plugin->lua ? plugin->lua->hasFunction(trigger.attributes.value(QStringLiteral("script")))
		                    : false;
		break;
	case 35:
		value = 0;
		break;
	case 36:
		value = isEnabledValue(trigger.attributes.value(QStringLiteral("one_shot")));
		break;
	default:
		break;
	}
	pushVariant(L, value);
	return 1;
}

static int luaGetPluginAliasInfo(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const QString         pluginId = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString         name     = QString::fromUtf8(luaL_checkstring(L, 2));
	const int             infoType = static_cast<int>(luaL_checkinteger(L, 3));
	WorldRuntime::Plugin *plugin   = runtime->pluginForId(pluginId);
	if (!plugin)
	{
		lua_pushnil(L);
		return 1;
	}
	const int index = findAliasIndex(plugin->aliases, name);
	if (index < 0)
	{
		lua_pushnil(L);
		return 1;
	}
	const auto &[attributes, children, included, matched, invocationCount, matchAttempts, lastMatchTarget,
	             lastMatched, executingScript] = plugin->aliases.at(index);
	QVariant value;
	switch (infoType)
	{
	case 1:
		value = attributes.value(QStringLiteral("match"));
		break;
	case 2:
		value = children.value(QStringLiteral("send"));
		break;
	case 3:
		value = attributes.value(QStringLiteral("script"));
		break;
	case 4:
		value = isEnabledValue(attributes.value(QStringLiteral("omit_from_log")));
		break;
	case 5:
		value = isEnabledValue(attributes.value(QStringLiteral("omit_from_output")));
		break;
	case 6:
		value = isEnabledValue(attributes.value(QStringLiteral("enabled")));
		break;
	case 7:
		value = isEnabledValue(attributes.value(QStringLiteral("regexp")));
		break;
	case 8:
		value = isEnabledValue(attributes.value(QStringLiteral("ignore_case")));
		break;
	case 9:
		value = isEnabledValue(attributes.value(QStringLiteral("expand_variables")));
		break;
	case 10:
		value = invocationCount;
		break;
	case 11:
		value = matched;
		break;
	case 12:
		value = isEnabledValue(attributes.value(QStringLiteral("menu")));
		break;
	case 13:
		if (lastMatched.isValid())
			value = lastMatched.toSecsSinceEpoch();
		break;
	case 14:
		value = isEnabledValue(attributes.value(QStringLiteral("temporary")));
		break;
	case 15:
		value = included;
		break;
	case 16:
		value = attributes.value(QStringLiteral("group"));
		break;
	case 17:
		value = attributes.value(QStringLiteral("variable"));
		break;
	case 18:
		value = attributes.value(QStringLiteral("send_to")).toInt();
		break;
	case 19:
		value = isEnabledValue(attributes.value(QStringLiteral("keep_evaluating")));
		break;
	case 20:
		value = attributes.value(QStringLiteral("sequence")).toInt();
		break;
	case 21:
		value = isEnabledValue(attributes.value(QStringLiteral("echo_alias")));
		break;
	case 22:
		value = isEnabledValue(attributes.value(QStringLiteral("omit_from_command_history")));
		break;
	case 23:
		value = attributes.value(QStringLiteral("user")).toLongLong();
		break;
	case 24:
		value = matchAttempts;
		break;
	case 25:
		value = lastMatchTarget;
		break;
	case 26:
		value = executingScript;
		break;
	case 27:
		value = plugin->lua ? plugin->lua->hasFunction(attributes.value(QStringLiteral("script"))) : false;
		break;
	case 28:
		value = 0;
		break;
	case 29:
		value = isEnabledValue(attributes.value(QStringLiteral("one_shot")));
		break;
	default:
		break;
	}
	pushVariant(L, value);
	return 1;
}

static int luaGetPluginTimerInfo(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const QString         pluginId = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString         name     = QString::fromUtf8(luaL_checkstring(L, 2));
	const int             infoType = static_cast<int>(luaL_checkinteger(L, 3));
	WorldRuntime::Plugin *plugin   = runtime->pluginForId(pluginId);
	if (!plugin)
	{
		lua_pushnil(L);
		return 1;
	}
	const int index = findTimerIndex(plugin->timers, name);
	if (index < 0)
	{
		lua_pushnil(L);
		return 1;
	}
	const auto &[attributes, children, included, lastFired, nextFireTime, firedCount, invocationCount,
	             executingScript] = plugin->timers.at(index);
	QVariant   value;
	const bool atTime = isEnabledValue(attributes.value(QStringLiteral("at_time")));
	switch (infoType)
	{
	case 1:
		value = attributes.value(QStringLiteral("hour")).toInt();
		break;
	case 2:
		value = attributes.value(QStringLiteral("minute")).toInt();
		break;
	case 3:
		value = attributes.value(QStringLiteral("second")).toDouble();
		break;
	case 4:
		value = children.value(QStringLiteral("send"));
		break;
	case 5:
		value = attributes.value(QStringLiteral("script"));
		break;
	case 6:
		value = isEnabledValue(attributes.value(QStringLiteral("enabled")));
		break;
	case 7:
		value = isEnabledValue(attributes.value(QStringLiteral("one_shot")));
		break;
	case 8:
		value = atTime;
		break;
	case 9:
		value = invocationCount;
		break;
	case 10:
		value = firedCount;
		break;
	case 11:
		if (lastFired.isValid())
			value = lastFired.toSecsSinceEpoch();
		break;
	case 12:
		if (nextFireTime.isValid())
			value = nextFireTime.toSecsSinceEpoch();
		break;
	case 13:
		if (nextFireTime.isValid())
		{
			const qint64 secs = QDateTime::currentDateTime().secsTo(nextFireTime);
			value             = static_cast<double>(secs < 0 ? 0 : secs);
		}
		break;
	case 14:
		value = isEnabledValue(attributes.value(QStringLiteral("temporary")));
		break;
	case 15:
		value = attributes.value(QStringLiteral("send_to")).toInt() == eSendToSpeedwalk;
		break;
	case 16:
		value = attributes.value(QStringLiteral("send_to")).toInt() == eSendToOutput;
		break;
	case 17:
		value = isEnabledValue(attributes.value(QStringLiteral("active_closed")));
		break;
	case 18:
		value = included;
		break;
	case 19:
		value = attributes.value(QStringLiteral("group"));
		break;
	case 20:
		value = attributes.value(QStringLiteral("send_to")).toInt();
		break;
	case 21:
		value = attributes.value(QStringLiteral("user")).toLongLong();
		break;
	case 22:
		value = attributes.value(QStringLiteral("name"));
		break;
	case 23:
		value = isEnabledValue(attributes.value(QStringLiteral("omit_from_output")));
		break;
	case 24:
		value = isEnabledValue(attributes.value(QStringLiteral("omit_from_log")));
		break;
	case 25:
		value = executingScript;
		break;
	case 26:
		value = plugin->lua ? plugin->lua->hasFunction(attributes.value(QStringLiteral("script"))) : false;
		break;
	default:
		break;
	}
	pushVariant(L, value);
	return 1;
}

static int luaGetPluginTriggerOption(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const QString         pluginId = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString         name     = QString::fromUtf8(luaL_checkstring(L, 2));
	const QString         optName  = QString::fromUtf8(luaL_checkstring(L, 3)).trimmed().toLower();
	WorldRuntime::Plugin *plugin   = runtime->pluginForId(pluginId);
	if (!plugin)
	{
		lua_pushnil(L);
		return 1;
	}
	const int index = findTriggerIndex(plugin->triggers, name);
	if (index < 0)
	{
		lua_pushnil(L);
		return 1;
	}
	const WorldRuntime::Trigger &trigger = plugin->triggers.at(index);
	if (optName == QStringLiteral("group") || optName == QStringLiteral("match") ||
	    optName == QStringLiteral("script") || optName == QStringLiteral("sound") ||
	    optName == QStringLiteral("variable"))
	{
		lua_pushstring(L, trigger.attributes.value(optName).toLocal8Bit().constData());
		return 1;
	}
	if (optName == QStringLiteral("send"))
	{
		lua_pushstring(L, trigger.children.value(QStringLiteral("send")).toLocal8Bit().constData());
		return 1;
	}
	if (optName == QStringLiteral("match_style"))
	{
		lua_pushnumber(L, buildTriggerMatchFlags(trigger));
		return 1;
	}
	if (optName == QStringLiteral("new_style"))
	{
		lua_pushnumber(L, buildTriggerStyleFlags(trigger));
		return 1;
	}
	if (optName == QStringLiteral("enabled") || optName == QStringLiteral("expand_variables") ||
	    optName == QStringLiteral("ignore_case") || optName == QStringLiteral("keep_evaluating") ||
	    optName == QStringLiteral("multi_line") || optName == QStringLiteral("omit_from_log") ||
	    optName == QStringLiteral("omit_from_output") || optName == QStringLiteral("regexp") ||
	    optName == QStringLiteral("repeat") || optName == QStringLiteral("sound_if_inactive") ||
	    optName == QStringLiteral("lowercase_wildcard") || optName == QStringLiteral("temporary") ||
	    optName == QStringLiteral("one_shot"))
	{
		lua_pushnumber(L, isEnabledValue(trigger.attributes.value(optName)) ? 1 : 0);
		return 1;
	}
	if (optName == QStringLiteral("clipboard_arg") || optName == QStringLiteral("colour_change_type") ||
	    optName == QStringLiteral("custom_colour") || optName == QStringLiteral("lines_to_match") ||
	    optName == QStringLiteral("other_text_colour") || optName == QStringLiteral("other_back_colour") ||
	    optName == QStringLiteral("send_to") || optName == QStringLiteral("sequence") ||
	    optName == QStringLiteral("user"))
	{
		lua_pushnumber(L, trigger.attributes.value(optName).toDouble());
		return 1;
	}
	lua_pushnil(L);
	return 1;
}

static int luaGetPluginAliasOption(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const QString         pluginId = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString         name     = QString::fromUtf8(luaL_checkstring(L, 2));
	const QString         optName  = QString::fromUtf8(luaL_checkstring(L, 3)).trimmed().toLower();
	WorldRuntime::Plugin *plugin   = runtime->pluginForId(pluginId);
	if (!plugin)
	{
		lua_pushnil(L);
		return 1;
	}
	const int index = findAliasIndex(plugin->aliases, name);
	if (index < 0)
	{
		lua_pushnil(L);
		return 1;
	}
	const WorldRuntime::Alias &alias = plugin->aliases.at(index);
	if (optName == QStringLiteral("group") || optName == QStringLiteral("match") ||
	    optName == QStringLiteral("script") || optName == QStringLiteral("variable"))
	{
		lua_pushstring(L, alias.attributes.value(optName).toLocal8Bit().constData());
		return 1;
	}
	if (optName == QStringLiteral("send"))
	{
		lua_pushstring(L, alias.children.value(QStringLiteral("send")).toLocal8Bit().constData());
		return 1;
	}
	if (optName == QStringLiteral("enabled") || optName == QStringLiteral("expand_variables") ||
	    optName == QStringLiteral("ignore_case") || optName == QStringLiteral("omit_from_log") ||
	    optName == QStringLiteral("omit_from_command_history") ||
	    optName == QStringLiteral("omit_from_output") || optName == QStringLiteral("regexp") ||
	    optName == QStringLiteral("menu") || optName == QStringLiteral("keep_evaluating") ||
	    optName == QStringLiteral("echo_alias") || optName == QStringLiteral("temporary") ||
	    optName == QStringLiteral("one_shot"))
	{
		lua_pushnumber(L, isEnabledValue(alias.attributes.value(optName)) ? 1 : 0);
		return 1;
	}
	if (optName == QStringLiteral("send_to") || optName == QStringLiteral("sequence") ||
	    optName == QStringLiteral("user"))
	{
		lua_pushnumber(L, alias.attributes.value(optName).toDouble());
		return 1;
	}
	lua_pushnil(L);
	return 1;
}

static int luaGetPluginTimerOption(lua_State *L)
{
	auto         *engine  = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	WorldRuntime *runtime = engine ? engine->worldRuntime() : nullptr;
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const QString         pluginId = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString         name     = QString::fromUtf8(luaL_checkstring(L, 2));
	const QString         optName  = QString::fromUtf8(luaL_checkstring(L, 3)).trimmed().toLower();
	WorldRuntime::Plugin *plugin   = runtime->pluginForId(pluginId);
	if (!plugin)
	{
		lua_pushnil(L);
		return 1;
	}
	const int index = findTimerIndex(plugin->timers, name);
	if (index < 0)
	{
		lua_pushnil(L);
		return 1;
	}
	const WorldRuntime::Timer &timer = plugin->timers.at(index);
	if (optName == QStringLiteral("group") || optName == QStringLiteral("script") ||
	    optName == QStringLiteral("variable"))
	{
		lua_pushstring(L, timer.attributes.value(optName).toLocal8Bit().constData());
		return 1;
	}
	if (optName == QStringLiteral("send"))
	{
		lua_pushstring(L, timer.children.value(QStringLiteral("send")).toLocal8Bit().constData());
		return 1;
	}
	if (optName == QStringLiteral("enabled") || optName == QStringLiteral("at_time") ||
	    optName == QStringLiteral("one_shot") || optName == QStringLiteral("omit_from_output") ||
	    optName == QStringLiteral("omit_from_log") || optName == QStringLiteral("active_closed") ||
	    optName == QStringLiteral("temporary"))
	{
		lua_pushnumber(L, isEnabledValue(timer.attributes.value(optName)) ? 1 : 0);
		return 1;
	}
	if (optName == QStringLiteral("hour") || optName == QStringLiteral("minute") ||
	    optName == QStringLiteral("second") || optName == QStringLiteral("offset_hour") ||
	    optName == QStringLiteral("offset_minute") || optName == QStringLiteral("offset_second") ||
	    optName == QStringLiteral("send_to") || optName == QStringLiteral("user"))
	{
		lua_pushnumber(L, timer.attributes.value(optName).toDouble());
		return 1;
	}
	lua_pushnil(L);
	return 1;
}

static int luaGetVariable(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnil(L);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const QString name = QString::fromUtf8(luaL_checkstring(L, 1));
	if (name.trimmed().isEmpty())
	{
		lua_pushnil(L);
		return 1;
	}
	if (!engine->pluginId().isEmpty())
	{
		QString value;
		if (!runtime->findPluginVariable(engine->pluginId(), name, value))
		{
			lua_pushnil(L);
			return 1;
		}
		lua_pushstring(L, value.toLocal8Bit().constData());
		return 1;
	}

	QString value;
	if (!runtime->findVariable(name, value))
	{
		lua_pushnil(L);
		return 1;
	}
	lua_pushstring(L, value.toLocal8Bit().constData());
	return 1;
}

static int luaSetVariable(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	const QString name = QString::fromUtf8(luaL_checkstring(L, 1));
	if (name.trimmed().isEmpty())
	{
		lua_pushnumber(L, eInvalidObjectLabel);
		return 1;
	}
	const QString value = QString::fromUtf8(luaL_checkstring(L, 2));
	if (!engine->pluginId().isEmpty())
		runtime->setPluginVariableValue(engine->pluginId(), name, value);
	else
		runtime->setVariable(name, value);
	lua_pushnumber(L, eOK);
	return 1;
}

static int luaGetVariableList(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnil(L);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}

	lua_newtable(L);
	if (!engine->pluginId().isEmpty())
	{
		const QString pluginId = engine->pluginId();
		for (const QStringList names = runtime->pluginVariableList(pluginId); const QString &name : names)
		{
			QString value;
			if (!runtime->findPluginVariable(pluginId, name, value))
				continue;
			lua_pushstring(L, name.toLocal8Bit().constData());
			lua_pushstring(L, value.toLocal8Bit().constData());
			lua_rawset(L, -3);
		}
		return 1;
	}
	for (const auto &[attributes, content] : runtime->variables())
	{
		if (const QString name = attributes.value(QStringLiteral("name")); !name.isEmpty())
		{
			lua_pushstring(L, name.toLocal8Bit().constData());
			lua_pushstring(L, content.toLocal8Bit().constData());
			lua_rawset(L, -3);
		}
	}
	return 1;
}

static int luaArrayCreate(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	const QString name = QString::fromUtf8(luaL_checkstring(L, 1));
	lua_pushnumber(L, runtime->arrayCreate(name));
	return 1;
}

static int luaArrayDelete(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	const QString name = QString::fromUtf8(luaL_checkstring(L, 1));
	lua_pushnumber(L, runtime->arrayDelete(name));
	return 1;
}

static int luaArrayClear(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	const QString name = QString::fromUtf8(luaL_checkstring(L, 1));
	lua_pushnumber(L, runtime->arrayClear(name));
	return 1;
}

static int luaArrayExists(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushboolean(L, 0);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushboolean(L, 0);
		return 1;
	}
	const QString name = QString::fromUtf8(luaL_checkstring(L, 1));
	lua_pushboolean(L, runtime->arrayExists(name));
	return 1;
}

static int luaArrayCount(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	lua_pushnumber(L, runtime ? runtime->arrayCount() : 0);
	return 1;
}

static int luaArraySize(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, 0);
		return 1;
	}
	const QString name = QString::fromUtf8(luaL_checkstring(L, 1));
	lua_pushnumber(L, runtime->arraySize(name));
	return 1;
}

static int luaArrayKeyExists(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushboolean(L, 0);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushboolean(L, 0);
		return 1;
	}
	const QString name = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString key  = QString::fromUtf8(luaL_checkstring(L, 2));
	lua_pushboolean(L, runtime->arrayKeyExists(name, key));
	return 1;
}

static int luaArrayGet(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnil(L);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const QString name = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString key  = QString::fromUtf8(luaL_checkstring(L, 2));
	QString       value;
	if (!runtime->arrayGet(name, key, value))
	{
		lua_pushnil(L);
		return 1;
	}
	lua_pushstring(L, value.toLocal8Bit().constData());
	return 1;
}

static int luaArraySet(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	const QString name  = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString key   = QString::fromUtf8(luaL_checkstring(L, 2));
	const QString value = QString::fromUtf8(luaL_checkstring(L, 3));
	lua_pushnumber(L, runtime->arraySet(name, key, value));
	return 1;
}

static int luaArrayDeleteKey(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	const QString name = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString key  = QString::fromUtf8(luaL_checkstring(L, 2));
	lua_pushnumber(L, runtime->arrayDeleteKey(name, key));
	return 1;
}

static int luaArrayGetFirstKey(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnil(L);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const QString name = QString::fromUtf8(luaL_checkstring(L, 1));
	QString       key;
	if (!runtime->arrayFirstKey(name, key))
	{
		lua_pushnil(L);
		return 1;
	}
	lua_pushstring(L, key.toLocal8Bit().constData());
	return 1;
}

static int luaArrayGetLastKey(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnil(L);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const QString name = QString::fromUtf8(luaL_checkstring(L, 1));
	QString       key;
	if (!runtime->arrayLastKey(name, key))
	{
		lua_pushnil(L);
		return 1;
	}
	lua_pushstring(L, key.toLocal8Bit().constData());
	return 1;
}

static bool arrayDelimiterFromLua(lua_State *L, const int index, QChar &delimiter)
{
	auto delim = QStringLiteral(",");
	if (lua_gettop(L) >= index && !lua_isnil(L, index))
		delim = QString::fromUtf8(luaL_checkstring(L, index));
	if (delim.size() != 1 || delim == QStringLiteral("\\"))
		return false;
	delimiter = delim.at(0);
	return true;
}

static int luaArrayExport(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	const QString name = QString::fromUtf8(luaL_checkstring(L, 1));
	if (!runtime->arrayExists(name))
	{
		lua_pushnumber(L, eArrayDoesNotExist);
		return 1;
	}
	QChar delimiter;
	if (!arrayDelimiterFromLua(L, 2, delimiter))
	{
		lua_pushnumber(L, eBadDelimiter);
		return 1;
	}
	if (const int count = runtime->arraySize(name); count == 0)
	{
		lua_pushstring(L, "");
		return 1;
	}

	const QStringList keys = runtime->arrayListKeys(name);
	QStringList       parts;
	parts.reserve(keys.size() * 2);
	const QString delimStr(delimiter);
	for (const QString &key : keys)
	{
		QString value;
		runtime->arrayGet(name, key, value);
		QString safeKey = key;
		safeKey.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
		safeKey.replace(delimStr, QStringLiteral("\\") + delimStr);
		QString safeValue = value;
		safeValue.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
		safeValue.replace(delimStr, QStringLiteral("\\") + delimStr);
		parts.append(safeKey);
		parts.append(safeValue);
	}
	lua_pushstring(L, parts.join(delimStr).toLocal8Bit().constData());
	return 1;
}

static int luaArrayExportKeys(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	const QString name = QString::fromUtf8(luaL_checkstring(L, 1));
	if (!runtime->arrayExists(name))
	{
		lua_pushnumber(L, eArrayDoesNotExist);
		return 1;
	}
	QChar delimiter;
	if (!arrayDelimiterFromLua(L, 2, delimiter))
	{
		lua_pushnumber(L, eBadDelimiter);
		return 1;
	}
	const QStringList keys = runtime->arrayListKeys(name);
	if (keys.isEmpty())
	{
		lua_pushstring(L, "");
		return 1;
	}
	const QString delimStr(delimiter);
	QStringList   safeKeys;
	safeKeys.reserve(keys.size());
	for (const QString &key : keys)
	{
		QString safeKey = key;
		safeKey.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
		safeKey.replace(delimStr, QStringLiteral("\\") + delimStr);
		safeKeys.append(safeKey);
	}
	lua_pushstring(L, safeKeys.join(delimStr).toLocal8Bit().constData());
	return 1;
}

static int luaArrayImport(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	const QString name = QString::fromUtf8(luaL_checkstring(L, 1));

	if (lua_istable(L, 2))
	{
		if (!runtime->arrayExists(name))
		{
			lua_pushnumber(L, eArrayDoesNotExist);
			return 1;
		}
		int duplicates = 0;
		for (lua_pushnil(L); lua_next(L, 2) != 0; lua_pop(L, 1))
		{
			if (!lua_isstring(L, -2))
				luaL_error(L, "table must have string keys");
			const char   *key    = lua_tostring(L, -2);
			const char   *value  = lua_tostring(L, -1);
			const QString keyStr = QString::fromUtf8(key ? key : "");
			const QString valStr = QString::fromUtf8(value ? value : "");
			if (const int rc = runtime->arraySet(name, keyStr, valStr); rc == eSetReplacingExistingValue)
				duplicates++;
		}
		lua_pushnumber(L, duplicates ? eImportedWithDuplicates : eOK);
		return 1;
	}

	const QString values = QString::fromUtf8(luaL_checkstring(L, 2));
	QChar         delimiter;
	if (!arrayDelimiterFromLua(L, 3, delimiter))
	{
		lua_pushnumber(L, eBadDelimiter);
		return 1;
	}
	if (!runtime->arrayExists(name))
	{
		lua_pushnumber(L, eArrayDoesNotExist);
		return 1;
	}

	QString       valueString = values;
	const QString delimStr(delimiter);
	QString       tempMarker;
	if (const QString escapedDelimiter = QStringLiteral("\\") + delimStr;
	    valueString.contains(escapedDelimiter))
	{
		bool found = false;
		for (int code = 1; code <= 255; ++code)
		{
			if (const QChar candidate(code); !valueString.contains(candidate))
			{
				tempMarker = QString(candidate);
				found      = true;
				break;
			}
		}
		if (!found)
		{
			lua_pushnumber(L, eCannotImport);
			return 1;
		}
		valueString.replace(escapedDelimiter, tempMarker);
	}

	const QStringList parts = valueString.split(delimStr, Qt::KeepEmptyParts);
	if (parts.size() % 2 != 0)
	{
		lua_pushnumber(L, eArrayNotEvenNumberOfValues);
		return 1;
	}

	int duplicates = 0;
	for (int i = 0; i < parts.size(); i += 2)
	{
		QString key = parts.at(i);
		QString val = parts.at(i + 1);
		key.replace(QStringLiteral("\\\\"), QStringLiteral("\\"));
		if (!tempMarker.isEmpty())
			key.replace(tempMarker, delimStr);
		val.replace(QStringLiteral("\\\\"), QStringLiteral("\\"));
		if (!tempMarker.isEmpty())
			val.replace(tempMarker, delimStr);
		if (const int rc = runtime->arraySet(name, key, val); rc == eSetReplacingExistingValue)
			duplicates++;
	}

	lua_pushnumber(L, duplicates ? eImportedWithDuplicates : eOK);
	return 1;
}

static int luaArrayListAll(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnil(L);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const QStringList names = runtime->arrayListAll();
	return pushOptionalStringList(L, names);
}

static int luaArrayListKeys(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnil(L);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const QString     name = QString::fromUtf8(luaL_checkstring(L, 1));
	const QStringList keys = runtime->arrayListKeys(name);
	return pushOptionalStringList(L, keys);
}

static int luaArrayListValues(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnil(L);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const QString     name   = QString::fromUtf8(luaL_checkstring(L, 1));
	const QStringList values = runtime->arrayListValues(name);
	return pushOptionalStringList(L, values);
}

static int luaArrayList(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
		return 0;
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
		return 0;
	const QString     name = QString::fromUtf8(luaL_checkstring(L, 1));
	const QStringList keys = runtime->arrayListKeys(name);
	if (keys.isEmpty())
		return 0;
	lua_newtable(L);
	for (const QString &key : keys)
	{
		QString value;
		if (!runtime->arrayGet(name, key, value))
			continue;
		lua_pushstring(L, key.toLocal8Bit().constData());
		lua_pushstring(L, value.toLocal8Bit().constData());
		lua_rawset(L, -3);
	}
	return 1;
}

static int luaDatabaseOpen(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	const QString name     = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString filename = QString::fromUtf8(luaL_checkstring(L, 2));
	const int     flags = static_cast<int>(luaL_optnumber(L, 3, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE));
	lua_pushnumber(L, runtime->databaseOpen(name, filename, flags));
	return 1;
}

static int luaDatabaseClose(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	const QString name = QString::fromUtf8(luaL_checkstring(L, 1));
	lua_pushnumber(L, runtime->databaseClose(name));
	return 1;
}

static int luaDatabasePrepare(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	const QString name = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString sql  = QString::fromUtf8(luaL_checkstring(L, 2));
	lua_pushnumber(L, runtime->databasePrepare(name, sql));
	return 1;
}

static int luaDatabaseFinalize(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	const QString name = QString::fromUtf8(luaL_checkstring(L, 1));
	lua_pushnumber(L, runtime->databaseFinalize(name));
	return 1;
}

static int luaDatabaseReset(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	const QString name = QString::fromUtf8(luaL_checkstring(L, 1));
	lua_pushnumber(L, runtime->databaseReset(name));
	return 1;
}

static int luaDatabaseColumns(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	const QString name = QString::fromUtf8(luaL_checkstring(L, 1));
	lua_pushnumber(L, runtime->databaseColumns(name));
	return 1;
}

static int luaDatabaseStep(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	const QString name = QString::fromUtf8(luaL_checkstring(L, 1));
	lua_pushnumber(L, runtime->databaseStep(name));
	return 1;
}

static int luaDatabaseError(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushstring(L, "database id not found");
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushstring(L, "database id not found");
		return 1;
	}
	const QString name = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString text = runtime->databaseError(name);
	lua_pushstring(L, text.toLocal8Bit().constData());
	return 1;
}

static int luaDatabaseColumnName(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnil(L);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const QString name   = QString::fromUtf8(luaL_checkstring(L, 1));
	const int     column = static_cast<int>(luaL_checknumber(L, 2));
	if (const QString result = runtime->databaseColumnName(name, column); result.isEmpty())
		lua_pushnil(L);
	else
		lua_pushstring(L, result.toLocal8Bit().constData());
	return 1;
}

static int luaDatabaseColumnText(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnil(L);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const QString name   = QString::fromUtf8(luaL_checkstring(L, 1));
	const int     column = static_cast<int>(luaL_checknumber(L, 2));
	bool          ok     = false;
	const QString value  = runtime->databaseColumnText(name, column, &ok);
	if (!ok)
		lua_pushnil(L);
	else
		lua_pushstring(L, value.toLocal8Bit().constData());
	return 1;
}

static void pushDatabaseColumnValue(lua_State *L, const QVariant &value)
{
	if (!value.isValid() || value.isNull())
	{
		lua_pushnil(L);
		return;
	}
	switch (value.typeId())
	{
	case QMetaType::Int:
	case QMetaType::LongLong:
		lua_pushinteger(L, value.toLongLong());
		break;
	case QMetaType::Double:
		lua_pushnumber(L, value.toDouble());
		break;
	default:
		lua_pushstring(L, value.toString().toLocal8Bit().constData());
		break;
	}
}

static int luaDatabaseColumnValue(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnil(L);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const QString name   = QString::fromUtf8(luaL_checkstring(L, 1));
	const int     column = static_cast<int>(luaL_checknumber(L, 2));
	if (QVariant value; runtime->databaseColumnValue(name, column, value))
		pushDatabaseColumnValue(L, value);
	else
		lua_pushnil(L);
	return 1;
}

static int luaDatabaseColumnType(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	const QString name   = QString::fromUtf8(luaL_checkstring(L, 1));
	const int     column = static_cast<int>(luaL_checknumber(L, 2));
	lua_pushnumber(L, runtime->databaseColumnType(name, column));
	return 1;
}

static int luaDatabaseTotalChanges(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	const QString name = QString::fromUtf8(luaL_checkstring(L, 1));
	lua_pushnumber(L, runtime->databaseTotalChanges(name));
	return 1;
}

static int luaDatabaseChanges(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	const QString name = QString::fromUtf8(luaL_checkstring(L, 1));
	lua_pushnumber(L, runtime->databaseChanges(name));
	return 1;
}

static int luaDatabaseLastInsertRowid(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushstring(L, "");
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushstring(L, "");
		return 1;
	}
	const QString name  = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString value = runtime->databaseLastInsertRowid(name);
	lua_pushstring(L, value.toLocal8Bit().constData());
	return 1;
}

static int luaDatabaseList(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnil(L);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const QStringList names = runtime->databaseList();
	return pushOptionalStringList(L, names);
}

static int luaDatabaseInfo(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnil(L);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const QString  name     = QString::fromUtf8(luaL_checkstring(L, 1));
	const int      infoType = static_cast<int>(luaL_checknumber(L, 2));
	const QVariant value    = runtime->databaseInfo(name, infoType);
	pushVariant(L, value);
	return 1;
}

static int luaDatabaseExec(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eBadParameter);
		return 1;
	}
	const QString name = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString sql  = QString::fromUtf8(luaL_checkstring(L, 2));
	lua_pushnumber(L, runtime->databaseExec(name, sql));
	return 1;
}

static int luaDatabaseColumnNames(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnil(L);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const QString     name  = QString::fromUtf8(luaL_checkstring(L, 1));
	const QStringList names = runtime->databaseColumnNames(name);
	return pushOptionalStringList(L, names);
}

static int luaDatabaseColumnValues(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnil(L);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const QString     name = QString::fromUtf8(luaL_checkstring(L, 1));
	QVector<QVariant> values;
	if (!runtime->databaseColumnValues(name, values))
	{
		lua_pushnil(L);
		return 1;
	}
	if (values.isEmpty())
	{
		lua_pushnil(L);
		return 1;
	}
	lua_newtable(L);
	int index = 1;
	for (const QVariant &value : values)
	{
		pushDatabaseColumnValue(L, value);
		lua_rawseti(L, -2, index++);
	}
	return 1;
}

static int luaDatabaseGetField(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnil(L);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const QString name = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString sql  = QString::fromUtf8(luaL_checkstring(L, 2));
	if (const int rc = runtime->databasePrepare(name, sql); rc != SQLITE_OK)
	{
		lua_pushnil(L);
		return 1;
	}
	if (const int stepRc = runtime->databaseStep(name); stepRc == SQLITE_ROW)
	{
		if (QVariant value; runtime->databaseColumnValue(name, 1, value))
			pushDatabaseColumnValue(L, value);
		else
			lua_pushnil(L);
	}
	else
	{
		lua_pushnil(L);
	}
	runtime->databaseFinalize(name);
	return 1;
}

static int luaGetPluginVariable(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnil(L);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const QString pluginId = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString name     = QString::fromUtf8(luaL_checkstring(L, 2));
	if (!runtime->isPluginInstalled(pluginId))
	{
		lua_pushnil(L);
		return 1;
	}
	QString value;
	if (!runtime->findPluginVariable(pluginId, name, value))
	{
		lua_pushnil(L);
		return 1;
	}
	lua_pushstring(L, value.toLocal8Bit().constData());
	return 1;
}

static int luaGetPluginVariableList(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnil(L);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const QString pluginId = QString::fromUtf8(luaL_checkstring(L, 1));
	if (!pluginId.isEmpty() && !runtime->isPluginInstalled(pluginId))
	{
		return 0;
	}
	lua_newtable(L);
	if (pluginId.isEmpty())
	{
		for (const auto &[attributes, content] : runtime->variables())
		{
			if (const QString name = attributes.value(QStringLiteral("name")); !name.isEmpty())
			{
				lua_pushstring(L, name.toLocal8Bit().constData());
				lua_pushstring(L, content.toLocal8Bit().constData());
				lua_rawset(L, -3);
			}
		}
		return 1;
	}
	for (const QStringList names = runtime->pluginVariableList(pluginId); const QString &name : names)
	{
		QString value;
		if (!runtime->findPluginVariable(pluginId, name, value))
			continue;
		lua_pushstring(L, name.toLocal8Bit().constData());
		lua_pushstring(L, value.toLocal8Bit().constData());
		lua_rawset(L, -3);
	}
	return 1;
}

static int luaGetPluginTriggerList(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnil(L);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const QString pluginId = QString::fromUtf8(luaL_checkstring(L, 1));
	if (!runtime->isPluginInstalled(pluginId))
	{
		lua_pushnil(L);
		return 1;
	}
	const QStringList names = runtime->pluginTriggerList(pluginId);
	return pushOptionalStringList(L, names);
}

static int luaGetPluginAliasList(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnil(L);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const QString pluginId = QString::fromUtf8(luaL_checkstring(L, 1));
	if (!runtime->isPluginInstalled(pluginId))
	{
		lua_pushnil(L);
		return 1;
	}
	const QStringList names = runtime->pluginAliasList(pluginId);
	return pushOptionalStringList(L, names);
}

static int luaGetPluginTimerList(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnil(L);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const QString pluginId = QString::fromUtf8(luaL_checkstring(L, 1));
	if (!runtime->isPluginInstalled(pluginId))
	{
		lua_pushnil(L);
		return 1;
	}
	const QStringList names = runtime->pluginTimerList(pluginId);
	return pushOptionalStringList(L, names);
}

static int luaGetPluginID(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushstring(L, "");
		return 1;
	}
	const QByteArray id = engine->pluginId().toLocal8Bit();
	lua_pushlstring(L, id.constData(), id.size());
	return 1;
}

static int luaGetPluginName(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushstring(L, "");
		return 1;
	}
	const QByteArray name = engine->pluginName().toLocal8Bit();
	lua_pushlstring(L, name.constData(), name.size());
	return 1;
}

static int luaGetPluginList(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnil(L);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const QStringList ids = runtime->pluginIdList();
	return pushOptionalStringList(L, ids);
}

static int luaGetPluginInfo(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnil(L);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const QString  pluginId = QString::fromUtf8(luaL_checkstring(L, 1));
	const int      infoType = static_cast<int>(luaL_checkinteger(L, 2));
	const QVariant value    = runtime->pluginInfo(pluginId, infoType);
	pushVariant(L, value);
	return 1;
}

static int luaIsPluginInstalled(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushboolean(L, 0);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushboolean(L, 0);
		return 1;
	}
	const QString pluginId = QString::fromUtf8(luaL_checkstring(L, 1));
	lua_pushboolean(L, runtime->isPluginInstalled(pluginId));
	return 1;
}

static int luaEnablePlugin(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eNoSuchPlugin);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eNoSuchPlugin);
		return 1;
	}
	const QString pluginId = QString::fromUtf8(luaL_checkstring(L, 1));
	if (const bool enable = optBool(L, 2, true); !runtime->enablePlugin(pluginId, enable))
	{
		lua_pushnumber(L, eNoSuchPlugin);
		return 1;
	}
	lua_pushnumber(L, eOK);
	return 1;
}

static int luaLoadPlugin(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eProblemsLoadingPlugin);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eProblemsLoadingPlugin);
		return 1;
	}
	const QString fileName = QString::fromUtf8(luaL_checkstring(L, 1));
	QString       error;
	if (!runtime->loadPluginFile(fileName, &error, false))
	{
		if (error.contains(QStringLiteral("not found"), Qt::CaseInsensitive))
			lua_pushnumber(L, ePluginFileNotFound);
		else
			lua_pushnumber(L, eProblemsLoadingPlugin);
		return 1;
	}
	lua_pushnumber(L, eOK);
	return 1;
}

static int luaUnloadPlugin(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eNoSuchPlugin);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eNoSuchPlugin);
		return 1;
	}
	if (const QString pluginId = QString::fromUtf8(luaL_checkstring(L, 1)); !runtime->unloadPlugin(pluginId))
	{
		lua_pushnumber(L, eNoSuchPlugin);
		return 1;
	}
	lua_pushnumber(L, eOK);
	return 1;
}

static int luaReloadPlugin(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eNoSuchPlugin);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eNoSuchPlugin);
		return 1;
	}
	const QString pluginId = QString::fromUtf8(luaL_checkstring(L, 1));
	const int     result   = runtime->reloadPlugin(pluginId);
	lua_pushnumber(L, result);
	return 1;
}

static int luaPluginSupports(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eNoSuchPlugin);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eNoSuchPlugin);
		return 1;
	}
	const QString pluginId = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString routine  = QString::fromUtf8(luaL_checkstring(L, 2));
	lua_pushnumber(L, runtime->pluginSupports(pluginId, routine));
	return 1;
}

static int luaCallPlugin(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eNoSuchPlugin);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eNoSuchPlugin);
		return 1;
	}
	const QString pluginId = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString routine  = QString::fromUtf8(luaL_checkstring(L, 2));
	lua_remove(L, 1); // remove plugin ID
	lua_remove(L, 1); // remove routine name
	return runtime->callPluginLua(pluginId, routine, L, 1, engine->pluginId());
}

static int luaSaveState(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eNotAPlugin);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, ePluginCouldNotSaveState);
		return 1;
	}
	const QString pluginId = engine->pluginId();
	if (pluginId.isEmpty())
	{
		lua_pushnumber(L, eNotAPlugin);
		return 1;
	}
	const int result = runtime->savePluginState(pluginId, true);
	lua_pushnumber(L, result);
	return 1;
}

static int luaGetOption(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, -1);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, -1);
		return 1;
	}
	const QString             key    = QString::fromUtf8(luaL_checkstring(L, 1));
	const WorldNumericOption *option = QMudWorldOptions::findWorldNumericOption(key);
	if (!option)
	{
		lua_pushnumber(L, -1);
		return 1;
	}
	if (const QString pluginId = pluginIdFromLua(L);
	    !pluginId.isEmpty() && option->flags & OPT_PLUGIN_CANNOT_READ)
	{
		lua_pushnumber(L, -1);
		return 1;
	}
	const QString canonical       = QString::fromLatin1(option->name);
	const QString value           = runtime->worldAttributes().value(canonical);
	const QString trimmed         = value.trimmed();
	const bool    isBooleanOption = option->minValue == 0 && option->maxValue == 0;
	if (option->flags & OPT_RGB_COLOUR)
	{
		const long rgb = colourNameFallback(value);
		lua_pushnumber(L, static_cast<lua_Number>(rgb >= 0 ? rgb : 0));
		return 1;
	}
	if (bool boolValue = false; parseBooleanKeywordValue(trimmed, boolValue))
	{
		lua_pushnumber(L, boolValue ? 1 : 0);
		return 1;
	}
	bool         ok     = false;
	const double number = trimmed.toDouble(&ok);
	if (ok && !trimmed.isEmpty())
	{
		if (option->flags & OPT_CUSTOM_COLOUR)
		{
			long adjusted = static_cast<long>(number) + 1;
			if (adjusted == 65536)
				adjusted = 0;
			lua_pushnumber(L, static_cast<lua_Number>(adjusted));
			return 1;
		}
		if (isBooleanOption)
			lua_pushnumber(L, number != 0.0 ? 1 : 0);
		else
			lua_pushnumber(L, static_cast<lua_Number>(static_cast<long>(number)));
	}
	else
		lua_pushnumber(L, 0);
	return 1;
}

static int luaSetOption(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eUnknownOption);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eUnknownOption);
		return 1;
	}
	const QString             key    = QString::fromUtf8(luaL_checkstring(L, 1));
	const WorldNumericOption *option = QMudWorldOptions::findWorldNumericOption(key);
	if (!option)
	{
		lua_pushnumber(L, eUnknownOption);
		return 1;
	}
	if (const QString pluginId = pluginIdFromLua(L);
	    !pluginId.isEmpty() && option->flags & OPT_PLUGIN_CANNOT_WRITE)
	{
		lua_pushnumber(L, ePluginCannotSetOption);
		return 1;
	}
	double numeric = 0.0;
	if (lua_isboolean(L, 2))
		numeric = lua_toboolean(L, 2) ? 1.0 : 0.0;
	else if (lua_isnil(L, 2))
		numeric = 0.0;
	else
		numeric = luaL_checknumber(L, 2);
	const int minimum = static_cast<int>(option->minValue);
	int       maximum = static_cast<int>(option->maxValue);
	if (minimum == 0 && maximum == 0)
		maximum = 1;
	if (numeric < minimum || numeric > maximum)
	{
		lua_pushnumber(L, eOptionOutOfRange);
		return 1;
	}

	long storedValue = static_cast<long>(numeric);
	// Legacy behavior: custom-color options store value-1 internally.
	if (option->flags & OPT_CUSTOM_COLOUR)
		storedValue -= 1;

	const QString canonical = QString::fromLatin1(option->name);
	runtime->setWorldAttribute(canonical, QString::number(storedValue));
	lua_pushnumber(L, eOK);
	return 1;
}

static int luaGetAlphaOption(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnil(L);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const QString           key    = QString::fromUtf8(luaL_checkstring(L, 1));
	const WorldAlphaOption *option = QMudWorldOptionDefaults::findWorldAlphaOption(key);
	if (!option)
	{
		lua_pushnil(L);
		return 1;
	}
	if (const QString pluginId = pluginIdFromLua(L);
	    !pluginId.isEmpty() && option->flags & OPT_PLUGIN_CANNOT_READ)
	{
		lua_pushnil(L);
		return 1;
	}
	constexpr int kOptMultiline = 0x000001;
	const QString canonical     = QString::fromLatin1(option->name);
	const bool    isMultiline   = (option->flags & kOptMultiline) != 0;
	const QString value         = isMultiline ? runtime->worldMultilineAttributes().value(canonical)
	                                          : runtime->worldAttributes().value(canonical);
	lua_pushstring(L, value.toLocal8Bit().constData());
	return 1;
}

static int luaSetAlphaOption(lua_State *L)
{
	auto *engine = static_cast<LuaCallbackEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!engine)
	{
		lua_pushnumber(L, eUnknownOption);
		return 1;
	}
	WorldRuntime *runtime = engine->worldRuntime();
	if (!runtime)
	{
		lua_pushnumber(L, eUnknownOption);
		return 1;
	}
	const QString           key    = QString::fromUtf8(luaL_checkstring(L, 1));
	const WorldAlphaOption *option = QMudWorldOptionDefaults::findWorldAlphaOption(key);
	if (!option)
	{
		lua_pushnumber(L, eUnknownOption);
		return 1;
	}
	if (const QString pluginId = pluginIdFromLua(L);
	    !pluginId.isEmpty() && option->flags & OPT_PLUGIN_CANNOT_WRITE)
	{
		lua_pushnumber(L, ePluginCannotSetOption);
		return 1;
	}

	constexpr int kOptMultiline = 0x000001;

	QString       value       = QString::fromUtf8(luaL_checkstring(L, 2));
	const QString canonical   = QString::fromLatin1(option->name);
	const bool    isMultiline = (option->flags & kOptMultiline) != 0;

	// Legacy behavior: single-line alpha options strip CR/LF.
	if (!isMultiline)
	{
		value.remove(QLatin1Char('\n'));
		value.remove(QLatin1Char('\r'));
	}

	// Legacy behavior: command stack character must be one printable non-space char.
	if (constexpr int kOptCommandStack = 0x000008; option->flags & kOptCommandStack)
	{
		if (value.size() > 1)
		{
			lua_pushnumber(L, eOptionOutOfRange);
			return 1;
		}
		if (value.isEmpty())
		{
			runtime->setWorldAttribute(QStringLiteral("enable_command_stack"), QStringLiteral("0"));
			lua_pushnumber(L, eOptionOutOfRange);
			return 1;
		}
		const QChar ch = value.at(0);
		if (const ushort code = ch.unicode(); code > 0x7F || code < 0x20 || code == 0x7F || ch.isSpace())
		{
			runtime->setWorldAttribute(QStringLiteral("enable_command_stack"), QStringLiteral("0"));
			lua_pushnumber(L, eOptionOutOfRange);
			return 1;
		}
	}

	// Legacy behavior: world-id must be 24 hex chars when non-empty, and lowercase.
	if (constexpr int kOptWorldId = 0x000010; option->flags & kOptWorldId)
	{
		if (!value.isEmpty())
		{
			if (constexpr int kPluginUniqueIdLength = 24; value.size() != kPluginUniqueIdLength)
			{
				lua_pushnumber(L, eOptionOutOfRange);
				return 1;
			}
			for (const QChar ch : value)
			{
				const ushort u          = ch.unicode();
				const bool   isDigit    = u >= '0' && u <= '9';
				const bool   isLowerHex = u >= 'a' && u <= 'f';
				if (const bool isUpperHex = u >= 'A' && u <= 'F'; !isDigit && !isLowerHex && !isUpperHex)
				{
					lua_pushnumber(L, eOptionOutOfRange);
					return 1;
				}
			}
			value = value.toLower();
		}
	}

	if (isMultiline)
		runtime->setWorldMultilineAttribute(canonical, value);
	else
		runtime->setWorldAttribute(canonical, value);
	lua_pushnumber(L, eOK);
	return 1;
}

static int luaWindowCreate(lua_State *L)
{
	WorldRuntime *runtime = runtimeFromLua(L);
	if (!runtime)
	{
		lua_pushnumber(L, eNoSuchWindow);
		return 1;
	}
	const QString name            = QString::fromUtf8(luaL_checkstring(L, 1));
	const int     left            = luaToInt(L, 2);
	const int     top             = luaToInt(L, 3);
	const int     width           = luaToInt(L, 4);
	const int     height          = luaToInt(L, 5);
	const int     position        = luaToInt(L, 6);
	const int     flags           = luaToInt(L, 7);
	const long    backgroundValue = luaToLong(L, 8);
	const QColor  background      = colorFromValue(backgroundValue);
	const int     result = runtime->windowCreate(name, left, top, width, height, position, flags, background,
	                                             pluginIdFromLua(L));
	lua_pushnumber(L, result);
	return 1;
}

static int luaWindowCreateImage(lua_State *L)
{
	WorldRuntime *runtime = runtimeFromLua(L);
	if (!runtime)
	{
		lua_pushnumber(L, eNoSuchWindow);
		return 1;
	}
	const QString name    = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString imageId = QString::fromUtf8(luaL_checkstring(L, 2));
	const long    row1    = static_cast<long>(luaL_checknumber(L, 3));
	const long    row2    = static_cast<long>(luaL_checknumber(L, 4));
	const long    row3    = static_cast<long>(luaL_checknumber(L, 5));
	const long    row4    = static_cast<long>(luaL_checknumber(L, 6));
	const long    row5    = static_cast<long>(luaL_checknumber(L, 7));
	const long    row6    = static_cast<long>(luaL_checknumber(L, 8));
	const long    row7    = static_cast<long>(luaL_checknumber(L, 9));
	const long    row8    = static_cast<long>(luaL_checknumber(L, 10));
	lua_pushnumber(L,
	               runtime->windowCreateImage(name, imageId, row1, row2, row3, row4, row5, row6, row7, row8));
	return 1;
}

static int luaWindowDelete(lua_State *L)
{
	WorldRuntime *runtime = runtimeFromLua(L);
	if (!runtime)
	{
		lua_pushnumber(L, eNoSuchWindow);
		return 1;
	}
	const QString name = QString::fromUtf8(luaL_checkstring(L, 1));
	lua_pushnumber(L, runtime->windowDelete(name));
	return 1;
}

static int luaWindowShow(lua_State *L)
{
	WorldRuntime *runtime = runtimeFromLua(L);
	if (!runtime)
	{
		lua_pushnumber(L, eNoSuchWindow);
		return 1;
	}
	const QString name = QString::fromUtf8(luaL_checkstring(L, 1));
	const bool    show = optBool(L, 2, true);
	lua_pushnumber(L, runtime->windowShow(name, show));
	return 1;
}

static int luaWindowList(lua_State *L)
{
	WorldRuntime *runtime = runtimeFromLua(L);
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const QStringList names = runtime->windowList();
	return pushOptionalStringList(L, names);
}

static int luaWindowInfo(lua_State *L)
{
	WorldRuntime *runtime = runtimeFromLua(L);
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const QString name     = QString::fromUtf8(luaL_checkstring(L, 1));
	const int     infoType = luaToInt(L, 2);
	pushVariant(L, runtime->windowInfo(name, infoType));
	return 1;
}

static int luaWindowRectOp(lua_State *L)
{
	WorldRuntime *runtime = runtimeFromLua(L);
	if (!runtime)
	{
		lua_pushnumber(L, eNoSuchWindow);
		return 1;
	}
	const QString name    = QString::fromUtf8(luaL_checkstring(L, 1));
	const int     action  = luaToInt(L, 2);
	const int     left    = luaToInt(L, 3);
	const int     top     = luaToInt(L, 4);
	const int     right   = luaToInt(L, 5);
	const int     bottom  = luaToInt(L, 6);
	const long    colour1 = luaToLong(L, 7);
	const long    colour2 = luaToLongOpt(L, 8, 0);
	lua_pushnumber(L, runtime->windowRectOp(name, action, left, top, right, bottom, colour1, colour2));
	return 1;
}

static int luaWindowCircleOp(lua_State *L)
{
	WorldRuntime *runtime = runtimeFromLua(L);
	if (!runtime)
	{
		lua_pushnumber(L, eNoSuchWindow);
		return 1;
	}
	const QString name        = QString::fromUtf8(luaL_checkstring(L, 1));
	const int     action      = luaToInt(L, 2);
	const int     left        = luaToInt(L, 3);
	const int     top         = luaToInt(L, 4);
	const int     right       = luaToInt(L, 5);
	const int     bottom      = luaToInt(L, 6);
	const long    penColour   = luaToLong(L, 7);
	const long    penStyle    = luaToLong(L, 8);
	const int     penWidth    = luaToInt(L, 9);
	const long    brushColour = luaToLong(L, 10);
	const long    brushStyle  = luaToLongOpt(L, 11, 0);
	const int     extra1      = luaToIntOpt(L, 12);
	const int     extra2      = luaToIntOpt(L, 13);
	const int     extra3      = luaToIntOpt(L, 14);
	const int     extra4      = luaToIntOpt(L, 15);
	lua_pushnumber(L, runtime->windowCircleOp(name, action, left, top, right, bottom, penColour, penStyle,
	                                          penWidth, brushColour, brushStyle, extra1, extra2, extra3,
	                                          extra4));
	return 1;
}

static int luaWindowLine(lua_State *L)
{
	WorldRuntime *runtime = runtimeFromLua(L);
	if (!runtime)
	{
		lua_pushnumber(L, eNoSuchWindow);
		return 1;
	}
	const QString name      = QString::fromUtf8(luaL_checkstring(L, 1));
	const int     x1        = luaToInt(L, 2);
	const int     y1        = luaToInt(L, 3);
	const int     x2        = luaToInt(L, 4);
	const int     y2        = luaToInt(L, 5);
	const long    penColour = luaToLong(L, 6);
	const long    penStyle  = luaToLong(L, 7);
	const int     penWidth  = luaToInt(L, 8);
	lua_pushnumber(L, runtime->windowLine(name, x1, y1, x2, y2, penColour, penStyle, penWidth));
	return 1;
}

static int luaWindowArc(lua_State *L)
{
	WorldRuntime *runtime = runtimeFromLua(L);
	if (!runtime)
	{
		lua_pushnumber(L, eNoSuchWindow);
		return 1;
	}
	const QString name      = QString::fromUtf8(luaL_checkstring(L, 1));
	const int     left      = luaToInt(L, 2);
	const int     top       = luaToInt(L, 3);
	const int     right     = luaToInt(L, 4);
	const int     bottom    = luaToInt(L, 5);
	const int     x1        = luaToInt(L, 6);
	const int     y1        = luaToInt(L, 7);
	const int     x2        = luaToInt(L, 8);
	const int     y2        = luaToInt(L, 9);
	const long    penColour = luaToLong(L, 10);
	const long    penStyle  = luaToLong(L, 11);
	const int     penWidth  = luaToInt(L, 12);
	lua_pushnumber(
	    L, runtime->windowArc(name, left, top, right, bottom, x1, y1, x2, y2, penColour, penStyle, penWidth));
	return 1;
}

static int luaWindowBezier(lua_State *L)
{
	WorldRuntime *runtime = runtimeFromLua(L);
	if (!runtime)
	{
		lua_pushnumber(L, eNoSuchWindow);
		return 1;
	}
	const QString name      = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString points    = QString::fromUtf8(luaL_checkstring(L, 2));
	const long    penColour = luaToLong(L, 3);
	const long    penStyle  = luaToLong(L, 4);
	const int     penWidth  = luaToInt(L, 5);
	lua_pushnumber(L, runtime->windowBezier(name, points, penColour, penStyle, penWidth));
	return 1;
}

static int luaWindowPolygon(lua_State *L)
{
	WorldRuntime *runtime = runtimeFromLua(L);
	if (!runtime)
	{
		lua_pushnumber(L, eNoSuchWindow);
		return 1;
	}
	const QString name         = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString points       = QString::fromUtf8(luaL_checkstring(L, 2));
	const long    penColour    = luaToLong(L, 3);
	const long    penStyle     = luaToLong(L, 4);
	const int     penWidth     = luaToInt(L, 5);
	const long    brushColour  = luaToLong(L, 6);
	const long    brushStyle   = luaToLongOpt(L, 7, 0);
	const bool    closePolygon = optBool(L, 8, false);
	const bool    winding      = optBool(L, 9, false);
	lua_pushnumber(L, runtime->windowPolygon(name, points, penColour, penStyle, penWidth, brushColour,
	                                         brushStyle, closePolygon, winding));
	return 1;
}

static int luaWindowGradient(lua_State *L)
{
	WorldRuntime *runtime = runtimeFromLua(L);
	if (!runtime)
	{
		lua_pushnumber(L, eNoSuchWindow);
		return 1;
	}
	const QString name        = QString::fromUtf8(luaL_checkstring(L, 1));
	const int     left        = luaToInt(L, 2);
	const int     top         = luaToInt(L, 3);
	const int     right       = luaToInt(L, 4);
	const int     bottom      = luaToInt(L, 5);
	const long    startColour = luaToLong(L, 6);
	const long    endColour   = luaToLong(L, 7);
	const int     mode        = luaToInt(L, 8);
	lua_pushnumber(L, runtime->windowGradient(name, left, top, right, bottom, startColour, endColour, mode));
	return 1;
}

static int luaWindowFont(lua_State *L)
{
	WorldRuntime *runtime = runtimeFromLua(L);
	if (!runtime)
	{
		lua_pushnumber(L, eNoSuchWindow);
		return 1;
	}
	const QString name      = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString fontId    = QString::fromUtf8(luaL_checkstring(L, 2));
	const QString fontName  = QString::fromUtf8(luaL_checkstring(L, 3));
	const double  size      = luaL_checknumber(L, 4);
	const bool    bold      = optBool(L, 5, false);
	const bool    italic    = optBool(L, 6, false);
	const bool    underline = optBool(L, 7, false);
	const bool    strikeout = optBool(L, 8, false);
	const int     charset   = static_cast<int>(luaL_optinteger(L, 9, DEFAULT_CHARSET));
	const int     pitch     = static_cast<int>(luaL_optinteger(L, 10, kFontFamilyDontCare));
	lua_pushnumber(L, runtime->windowFont(name, fontId, fontName, size, bold, italic, underline, strikeout,
	                                      charset, pitch));
	return 1;
}

static int luaWindowFontInfo(lua_State *L)
{
	WorldRuntime *runtime = runtimeFromLua(L);
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const QString name     = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString fontId   = QString::fromUtf8(luaL_checkstring(L, 2));
	const int     infoType = luaToInt(L, 3);
	pushVariant(L, runtime->windowFontInfo(name, fontId, infoType));
	return 1;
}

static int luaWindowFontList(lua_State *L)
{
	WorldRuntime *runtime = runtimeFromLua(L);
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const QString     name  = QString::fromUtf8(luaL_checkstring(L, 1));
	const QStringList fonts = runtime->windowFontList(name);
	return pushOptionalStringList(L, fonts);
}

static bool decodeMiniWindowText(lua_State *L, const bool unicode, QString &out)
{
	constexpr int kTextArg = 3;
	if (!unicode)
	{
		out = QString::fromLocal8Bit(luaL_checkstring(L, kTextArg));
		return true;
	}
	const char      *raw = luaL_checkstring(L, kTextArg);
	const QByteArray bytes(raw);
	QStringDecoder   decoder(QStringConverter::Utf8);
	out = decoder.decode(bytes);
	return !decoder.hasError();
}

static int luaWindowText(lua_State *L)
{
	WorldRuntime *runtime = runtimeFromLua(L);
	if (!runtime)
	{
		lua_pushnumber(L, eNoSuchWindow);
		return 1;
	}
	const QString name    = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString fontId  = QString::fromUtf8(luaL_checkstring(L, 2));
	const bool    unicode = optBool(L, 9, false);
	QString       text;
	if (!decodeMiniWindowText(L, unicode, text))
	{
		lua_pushnumber(L, -3);
		return 1;
	}
	const int  left   = luaToInt(L, 4);
	const int  top    = luaToInt(L, 5);
	const int  right  = luaToInt(L, 6);
	const int  bottom = luaToInt(L, 7);
	const long colour = luaToLong(L, 8);
	lua_pushnumber(L, runtime->windowText(name, fontId, text, left, top, right, bottom, colour));
	return 1;
}

static int luaWindowTextWidth(lua_State *L)
{
	WorldRuntime *runtime = runtimeFromLua(L);
	if (!runtime)
	{
		lua_pushnumber(L, eNoSuchWindow);
		return 1;
	}
	const QString name    = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString fontId  = QString::fromUtf8(luaL_checkstring(L, 2));
	const bool    unicode = optBool(L, 4, false);
	QString       text;
	if (!decodeMiniWindowText(L, unicode, text))
	{
		lua_pushnumber(L, -3);
		return 1;
	}
	lua_pushnumber(L, runtime->windowTextWidth(name, fontId, text));
	return 1;
}

static int luaWindowSetPixel(lua_State *L)
{
	WorldRuntime *runtime = runtimeFromLua(L);
	if (!runtime)
	{
		lua_pushnumber(L, eNoSuchWindow);
		return 1;
	}
	const QString name   = QString::fromUtf8(luaL_checkstring(L, 1));
	const int     x      = luaToInt(L, 2);
	const int     y      = luaToInt(L, 3);
	const long    colour = luaToLong(L, 4);
	lua_pushnumber(L, runtime->windowSetPixel(name, x, y, colour));
	return 1;
}

static int luaWindowGetPixel(lua_State *L)
{
	WorldRuntime *runtime = runtimeFromLua(L);
	if (!runtime)
	{
		lua_pushnumber(L, eNoSuchWindow);
		return 1;
	}
	const QString name = QString::fromUtf8(luaL_checkstring(L, 1));
	const int     x    = luaToInt(L, 2);
	const int     y    = luaToInt(L, 3);
	pushVariant(L, runtime->windowGetPixel(name, x, y));
	return 1;
}

static int luaWindowLoadImage(lua_State *L)
{
	WorldRuntime *runtime = runtimeFromLua(L);
	if (!runtime)
	{
		lua_pushnumber(L, eNoSuchWindow);
		return 1;
	}
	const QString name     = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString imageId  = QString::fromUtf8(luaL_checkstring(L, 2));
	const QString filename = QString::fromUtf8(luaL_checkstring(L, 3));
	lua_pushnumber(L, runtime->windowLoadImage(name, imageId, filename));
	return 1;
}

static int luaWindowLoadImageMemory(lua_State *L)
{
	WorldRuntime *runtime = runtimeFromLua(L);
	if (!runtime)
	{
		lua_pushnumber(L, eNoSuchWindow);
		return 1;
	}
	const QString name     = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString imageId  = QString::fromUtf8(luaL_checkstring(L, 2));
	size_t        length   = 0;
	const char   *buffer   = luaL_checklstring(L, 3, &length);
	const bool    hasAlpha = optBool(L, 4, false);
	lua_pushnumber(L, runtime->windowLoadImageMemory(name, imageId,
	                                                 QByteArray(buffer, static_cast<int>(length)), hasAlpha));
	return 1;
}

static int luaWindowImageList(lua_State *L)
{
	WorldRuntime *runtime = runtimeFromLua(L);
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const QString     name   = QString::fromUtf8(luaL_checkstring(L, 1));
	const QStringList images = runtime->windowImageList(name);
	return pushOptionalStringList(L, images);
}

static int luaWindowImageInfo(lua_State *L)
{
	WorldRuntime *runtime = runtimeFromLua(L);
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const QString name     = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString imageId  = QString::fromUtf8(luaL_checkstring(L, 2));
	const int     infoType = static_cast<int>(luaL_checkinteger(L, 3));
	pushVariant(L, runtime->windowImageInfo(name, imageId, infoType));
	return 1;
}

static int luaWindowImageFromWindow(lua_State *L)
{
	WorldRuntime *runtime = runtimeFromLua(L);
	if (!runtime)
	{
		lua_pushnumber(L, eNoSuchWindow);
		return 1;
	}
	const QString name         = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString imageId      = QString::fromUtf8(luaL_checkstring(L, 2));
	const QString sourceWindow = QString::fromUtf8(luaL_checkstring(L, 3));
	lua_pushnumber(L, runtime->windowImageFromWindow(name, imageId, sourceWindow));
	return 1;
}

static int luaWindowDrawImage(lua_State *L)
{
	WorldRuntime *runtime = runtimeFromLua(L);
	if (!runtime)
	{
		lua_pushnumber(L, eNoSuchWindow);
		return 1;
	}
	const QString name      = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString imageId   = QString::fromUtf8(luaL_checkstring(L, 2));
	const int     left      = luaToInt(L, 3);
	const int     top       = luaToInt(L, 4);
	const int     right     = luaToInt(L, 5);
	const int     bottom    = luaToInt(L, 6);
	const int     mode      = luaToInt(L, 7);
	const int     srcLeft   = luaToIntOpt(L, 8);
	const int     srcTop    = luaToIntOpt(L, 9);
	const int     srcRight  = luaToIntOpt(L, 10);
	const int     srcBottom = luaToIntOpt(L, 11);
	lua_pushnumber(L, runtime->windowDrawImage(name, imageId, left, top, right, bottom, mode, srcLeft, srcTop,
	                                           srcRight, srcBottom));
	return 1;
}

static int luaWindowDrawImageAlpha(lua_State *L)
{
	WorldRuntime *runtime = runtimeFromLua(L);
	if (!runtime)
	{
		lua_pushnumber(L, eNoSuchWindow);
		return 1;
	}
	const QString name    = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString imageId = QString::fromUtf8(luaL_checkstring(L, 2));
	const int     left    = luaToInt(L, 3);
	const int     top     = luaToInt(L, 4);
	const int     right   = luaToInt(L, 5);
	const int     bottom  = luaToInt(L, 6);
	const double  opacity = luaL_optnumber(L, 7, 1.0);
	const int     srcLeft = luaToIntOpt(L, 8);
	const int     srcTop  = luaToIntOpt(L, 9);
	lua_pushnumber(
	    L, runtime->windowDrawImageAlpha(name, imageId, left, top, right, bottom, opacity, srcLeft, srcTop));
	return 1;
}

static int luaWindowImageOp(lua_State *L)
{
	WorldRuntime *runtime = runtimeFromLua(L);
	if (!runtime)
	{
		lua_pushnumber(L, eNoSuchWindow);
		return 1;
	}
	const QString name          = QString::fromUtf8(luaL_checkstring(L, 1));
	const int     action        = luaToInt(L, 2);
	const int     left          = luaToInt(L, 3);
	const int     top           = luaToInt(L, 4);
	const int     right         = luaToInt(L, 5);
	const int     bottom        = luaToInt(L, 6);
	const long    penColour     = luaToLong(L, 7);
	const long    penStyle      = luaToLong(L, 8);
	const int     penWidth      = luaToInt(L, 9);
	const long    brushColour   = luaToLong(L, 10);
	const QString imageId       = QString::fromUtf8(luaL_checkstring(L, 11));
	const int     ellipseWidth  = luaToIntOpt(L, 12);
	const int     ellipseHeight = luaToIntOpt(L, 13);
	lua_pushnumber(L, runtime->windowImageOp(name, action, left, top, right, bottom, penColour, penStyle,
	                                         penWidth, brushColour, imageId, ellipseWidth, ellipseHeight));
	return 1;
}

static int luaWindowMergeImageAlpha(lua_State *L)
{
	WorldRuntime *runtime = runtimeFromLua(L);
	if (!runtime)
	{
		lua_pushnumber(L, eNoSuchWindow);
		return 1;
	}
	const QString name      = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString imageId   = QString::fromUtf8(luaL_checkstring(L, 2));
	const QString maskId    = QString::fromUtf8(luaL_checkstring(L, 3));
	const int     left      = luaToInt(L, 4);
	const int     top       = luaToInt(L, 5);
	const int     right     = luaToInt(L, 6);
	const int     bottom    = luaToInt(L, 7);
	const int     mode      = luaToInt(L, 8);
	const double  opacity   = luaL_optnumber(L, 9, 1.0);
	const int     srcLeft   = luaToIntOpt(L, 10);
	const int     srcTop    = luaToIntOpt(L, 11);
	const int     srcRight  = luaToIntOpt(L, 12);
	const int     srcBottom = luaToIntOpt(L, 13);
	lua_pushnumber(L, runtime->windowMergeImageAlpha(name, imageId, maskId, left, top, right, bottom, mode,
	                                                 opacity, srcLeft, srcTop, srcRight, srcBottom));
	return 1;
}

static int luaWindowGetImageAlpha(lua_State *L)
{
	WorldRuntime *runtime = runtimeFromLua(L);
	if (!runtime)
	{
		lua_pushnumber(L, eNoSuchWindow);
		return 1;
	}
	const QString name    = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString imageId = QString::fromUtf8(luaL_checkstring(L, 2));
	const int     left    = luaToInt(L, 3);
	const int     top     = luaToInt(L, 4);
	const int     right   = luaToInt(L, 5);
	const int     bottom  = luaToInt(L, 6);
	const int     srcLeft = luaToIntOpt(L, 7);
	const int     srcTop  = luaToIntOpt(L, 8);
	lua_pushnumber(L, runtime->windowGetImageAlpha(name, imageId, left, top, right, bottom, srcLeft, srcTop));
	return 1;
}

static int luaWindowBlendImage(lua_State *L)
{
	WorldRuntime *runtime = runtimeFromLua(L);
	if (!runtime)
	{
		lua_pushnumber(L, eNoSuchWindow);
		return 1;
	}
	const QString name      = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString imageId   = QString::fromUtf8(luaL_checkstring(L, 2));
	const int     left      = luaToInt(L, 3);
	const int     top       = luaToInt(L, 4);
	const int     right     = luaToInt(L, 5);
	const int     bottom    = luaToInt(L, 6);
	const int     mode      = luaToInt(L, 7);
	const double  opacity   = luaL_optnumber(L, 8, 1.0);
	const int     srcLeft   = luaToIntOpt(L, 9);
	const int     srcTop    = luaToIntOpt(L, 10);
	const int     srcRight  = luaToIntOpt(L, 11);
	const int     srcBottom = luaToIntOpt(L, 12);
	lua_pushnumber(L, runtime->windowBlendImage(name, imageId, left, top, right, bottom, mode, opacity,
	                                            srcLeft, srcTop, srcRight, srcBottom));
	return 1;
}

static int luaWindowFilter(lua_State *L)
{
	WorldRuntime *runtime = runtimeFromLua(L);
	if (!runtime)
	{
		lua_pushnumber(L, eNoSuchWindow);
		return 1;
	}
	const QString name      = QString::fromUtf8(luaL_checkstring(L, 1));
	const int     left      = luaToInt(L, 2);
	const int     top       = luaToInt(L, 3);
	const int     right     = luaToInt(L, 4);
	const int     bottom    = luaToInt(L, 5);
	const int     operation = luaToInt(L, 6);
	const double  options   = luaL_checknumber(L, 7);
	const int     extra     = luaToIntOpt(L, 8);
	lua_pushnumber(L, runtime->windowFilter(name, left, top, right, bottom, operation, options, extra));
	return 1;
}

static int luaWindowTransformImage(lua_State *L)
{
	WorldRuntime *runtime = runtimeFromLua(L);
	if (!runtime)
	{
		lua_pushnumber(L, eNoSuchWindow);
		return 1;
	}
	const QString name    = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString imageId = QString::fromUtf8(luaL_checkstring(L, 2));
	const auto    left    = static_cast<float>(luaL_checknumber(L, 3));
	const auto    top     = static_cast<float>(luaL_checknumber(L, 4));
	const int     mode    = static_cast<int>(luaL_checkinteger(L, 5));
	const auto    mxx     = static_cast<float>(luaL_checknumber(L, 6));
	const auto    mxy     = static_cast<float>(luaL_checknumber(L, 7));
	const auto    myx     = static_cast<float>(luaL_checknumber(L, 8));
	const auto    myy     = static_cast<float>(luaL_checknumber(L, 9));
	lua_pushnumber(L, runtime->windowTransformImage(name, imageId, left, top, mode, mxx, mxy, myx, myy));
	return 1;
}

static int luaWindowWrite(lua_State *L)
{
	WorldRuntime *runtime = runtimeFromLua(L);
	if (!runtime)
	{
		lua_pushnumber(L, eNoSuchWindow);
		return 1;
	}
	const QString name     = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString fileName = QString::fromUtf8(luaL_checkstring(L, 2));
	lua_pushnumber(L, runtime->windowWrite(name, fileName));
	return 1;
}

static int luaWindowPosition(lua_State *L)
{
	WorldRuntime *runtime = runtimeFromLua(L);
	if (!runtime)
	{
		lua_pushnumber(L, eNoSuchWindow);
		return 1;
	}
	const QString name     = QString::fromUtf8(luaL_checkstring(L, 1));
	const int     left     = luaToInt(L, 2);
	const int     top      = luaToInt(L, 3);
	const int     position = luaToInt(L, 4);
	const int     flags    = luaToInt(L, 5);
	lua_pushnumber(L, runtime->windowPosition(name, left, top, position, flags));
	return 1;
}

static int luaWindowResize(lua_State *L)
{
	WorldRuntime *runtime = runtimeFromLua(L);
	if (!runtime)
	{
		lua_pushnumber(L, eNoSuchWindow);
		return 1;
	}
	const QString name   = QString::fromUtf8(luaL_checkstring(L, 1));
	const int     width  = luaToInt(L, 2);
	const int     height = luaToInt(L, 3);
	const long    colour = luaToLongOpt(L, 4, -1);
	lua_pushnumber(L, runtime->windowResize(name, width, height, colour));
	return 1;
}

static int luaWindowSetZOrder(lua_State *L)
{
	WorldRuntime *runtime = runtimeFromLua(L);
	if (!runtime)
	{
		lua_pushnumber(L, eNoSuchWindow);
		return 1;
	}
	const QString name   = QString::fromUtf8(luaL_checkstring(L, 1));
	const int     zOrder = luaToInt(L, 2);
	lua_pushnumber(L, runtime->windowSetZOrder(name, zOrder));
	return 1;
}

static int luaWindowAddHotspot(lua_State *L)
{
	WorldRuntime *runtime = runtimeFromLua(L);
	if (!runtime)
	{
		lua_pushnumber(L, eNoSuchWindow);
		return 1;
	}
	const QString name            = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString hotspotId       = QString::fromUtf8(luaL_checkstring(L, 2));
	const int     left            = luaToInt(L, 3);
	const int     top             = luaToInt(L, 4);
	const int     right           = luaToInt(L, 5);
	const int     bottom          = luaToInt(L, 6);
	const QString mouseOver       = QString::fromUtf8(luaL_optstring(L, 7, ""));
	const QString cancelMouseOver = QString::fromUtf8(luaL_optstring(L, 8, ""));
	const QString mouseDown       = QString::fromUtf8(luaL_optstring(L, 9, ""));
	const QString cancelMouseDown = QString::fromUtf8(luaL_optstring(L, 10, ""));
	const QString mouseUp         = QString::fromUtf8(luaL_optstring(L, 11, ""));
	const QString tooltip         = QString::fromUtf8(luaL_optstring(L, 12, ""));
	const int     cursor          = luaToIntOpt(L, 13);
	const int     flags           = luaToIntOpt(L, 14);
	lua_pushnumber(L, runtime->windowAddHotspot(name, hotspotId, left, top, right, bottom, mouseOver,
	                                            cancelMouseOver, mouseDown, cancelMouseDown, mouseUp, tooltip,
	                                            cursor, flags, pluginIdFromLua(L)));
	return 1;
}

static int luaWindowDeleteHotspot(lua_State *L)
{
	WorldRuntime *runtime = runtimeFromLua(L);
	if (!runtime)
	{
		lua_pushnumber(L, eNoSuchWindow);
		return 1;
	}
	const QString name      = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString hotspotId = QString::fromUtf8(luaL_checkstring(L, 2));
	lua_pushnumber(L, runtime->windowDeleteHotspot(name, hotspotId));
	return 1;
}

static int luaWindowDeleteAllHotspots(lua_State *L)
{
	WorldRuntime *runtime = runtimeFromLua(L);
	if (!runtime)
	{
		lua_pushnumber(L, eNoSuchWindow);
		return 1;
	}
	const QString name = QString::fromUtf8(luaL_checkstring(L, 1));
	lua_pushnumber(L, runtime->windowDeleteAllHotspots(name));
	return 1;
}

static int luaWindowHotspotList(lua_State *L)
{
	WorldRuntime *runtime = runtimeFromLua(L);
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const QString     name     = QString::fromUtf8(luaL_checkstring(L, 1));
	const QStringList hotspots = runtime->windowHotspotList(name);
	return pushOptionalStringList(L, hotspots);
}

static int luaWindowHotspotInfo(lua_State *L)
{
	WorldRuntime *runtime = runtimeFromLua(L);
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const QString name      = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString hotspotId = QString::fromUtf8(luaL_checkstring(L, 2));
	const int     infoType  = luaToInt(L, 3);
	pushVariant(L, runtime->windowHotspotInfo(name, hotspotId, infoType));
	return 1;
}

static int luaWindowHotspotTooltip(lua_State *L)
{
	WorldRuntime *runtime = runtimeFromLua(L);
	if (!runtime)
	{
		lua_pushnumber(L, eNoSuchWindow);
		return 1;
	}
	const QString name      = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString hotspotId = QString::fromUtf8(luaL_checkstring(L, 2));
	const QString tooltip   = QString::fromUtf8(luaL_checkstring(L, 3));
	lua_pushnumber(L, runtime->windowHotspotTooltip(name, hotspotId, tooltip));
	return 1;
}

static int luaWindowMoveHotspot(lua_State *L)
{
	WorldRuntime *runtime = runtimeFromLua(L);
	if (!runtime)
	{
		lua_pushnumber(L, eNoSuchWindow);
		return 1;
	}
	const QString name      = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString hotspotId = QString::fromUtf8(luaL_checkstring(L, 2));
	const int     left      = luaToInt(L, 3);
	const int     top       = luaToInt(L, 4);
	const int     right     = luaToInt(L, 5);
	const int     bottom    = luaToInt(L, 6);
	lua_pushnumber(L, runtime->windowMoveHotspot(name, hotspotId, left, top, right, bottom));
	return 1;
}

static int luaWindowDragHandler(lua_State *L)
{
	WorldRuntime *runtime = runtimeFromLua(L);
	if (!runtime)
	{
		lua_pushnumber(L, eNoSuchWindow);
		return 1;
	}
	const QString name            = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString hotspotId       = QString::fromUtf8(luaL_checkstring(L, 2));
	const QString moveCallback    = QString::fromUtf8(luaL_checkstring(L, 3));
	const QString releaseCallback = QString::fromUtf8(luaL_checkstring(L, 4));
	const int     flags           = luaToInt(L, 5);
	lua_pushnumber(L, runtime->windowDragHandler(name, hotspotId, moveCallback, releaseCallback, flags,
	                                             pluginIdFromLua(L)));
	return 1;
}

static int luaWindowScrollwheelHandler(lua_State *L)
{
	WorldRuntime *runtime = runtimeFromLua(L);
	if (!runtime)
	{
		lua_pushnumber(L, eNoSuchWindow);
		return 1;
	}
	const QString name      = QString::fromUtf8(luaL_checkstring(L, 1));
	const QString hotspotId = QString::fromUtf8(luaL_checkstring(L, 2));
	const QString callback  = QString::fromUtf8(luaL_checkstring(L, 3));
	lua_pushnumber(L, runtime->windowScrollwheelHandler(name, hotspotId, callback, pluginIdFromLua(L)));
	return 1;
}

static int luaWindowMenu(lua_State *L)
{
	WorldRuntime *runtime = runtimeFromLua(L);
	if (!runtime)
	{
		lua_pushnil(L);
		return 1;
	}
	const QString name   = QString::fromUtf8(luaL_checkstring(L, 1));
	const int     left   = luaToInt(L, 2);
	const int     top    = luaToInt(L, 3);
	const QString items  = QString::fromUtf8(luaL_checkstring(L, 4));
	const QString result = runtime->windowMenu(name, left, top, items, pluginIdFromLua(L));
	lua_pushstring(L, result.toLocal8Bit().constData());
	return 1;
}
#endif

bool LuaCallbackEngine::ensureState()
{
#ifdef QMUD_ENABLE_LUA_SCRIPTING
	if (!m_state)
	{
		m_ownedState.reset(luaL_newstate());
		m_state = m_ownedState.get();
		if (!m_state)
			return false;
		luaL_openlibs(m_state);
		QMudLuaSupport::applyLua51Compat(m_state);
		qmudLogLua51CompatState(m_state, "LuaCallbackEngine world state");
		if (m_worldRuntime)
		{
			if (const QString startupDir = m_worldRuntime->startupDirectory();
			    !startupDir.trimmed().isEmpty())
				extendLuaPackagePath(m_state, startupDir);
		}
		extendLuaPackagePath(m_state, QCoreApplication::applicationDirPath());
		m_worldBindingsReady         = false;
		m_packageRestrictionsApplied = false;
	}

	if (!m_worldBindingsReady)
		registerWorldBindings();

	if (!m_packageRestrictionsApplied || m_packageRestrictionsAppliedValue != m_allowPackage)
	{
		QMudLuaSupport::applyLuaPackageRestrictions(m_state, m_allowPackage);
		m_packageRestrictionsApplied      = true;
		m_packageRestrictionsAppliedValue = m_allowPackage;
	}

	if (!m_scriptLoaded)
	{
		QString globalPrelude;
		if (AppController *app = AppController::instance())
			globalPrelude = app->getGlobalOption(QStringLiteral("LuaScript")).toString();
		if (!globalPrelude.trimmed().isEmpty())
		{
			if (const QByteArray preludeBytes = globalPrelude.toUtf8();
			    luaL_dostring(m_state, preludeBytes.constData()) != 0)
			{
				const char *err = lua_tostring(m_state, -1);
				reportLuaError(*this, QStringLiteral("Lua prelude load error: %1")
				                          .arg(QString::fromUtf8(err ? err : "unknown")));
				lua_pop(m_state, 1);
				return false;
			}
		}

		if (m_script.isEmpty())
		{
			m_scriptLoaded = true;
			refreshLuaFunctionSetForState(m_state, m_luaFunctionsSet);
			return true;
		}
		if (const QByteArray scriptBytes = m_script.toUtf8();
		    luaL_dostring(m_state, scriptBytes.constData()) != 0)
		{
			const char *err = lua_tostring(m_state, -1);
			reportLuaError(
			    *this,
			    QStringLiteral("Lua script load error: %1").arg(QString::fromUtf8(err ? err : "unknown")));
			lua_pop(m_state, 1);
			return false;
		}
		m_scriptLoaded = true;
		refreshLuaFunctionSetForState(m_state, m_luaFunctionsSet);
	}

	return true;
#else
	return false;
#endif
}

#ifdef QMUD_ENABLE_LUA_SCRIPTING
static void refreshLuaFunctionSetForState(lua_State *L, QSet<QString> &out)
{
	out.clear();
	if (!L)
		return;

	lua_pushglobaltable(L);
	lua_pushnil(L);
	while (lua_next(L, -2) != 0)
	{
		if (lua_type(L, -2) == LUA_TSTRING && lua_type(L, -1) == LUA_TFUNCTION && !lua_iscfunction(L, -1))
		{
			size_t length = 0;
			if (const char *key = lua_tolstring(L, -2, &length); key && length > 0 && key[0] != '_')
				out.insert(QString::fromUtf8(key, static_cast<int>(length)));
		}
		lua_pop(L, 1);
	}
	lua_pop(L, 1);
}

static void extendLuaPackagePath(lua_State *L, const QString &appDir)
{
	if (!L)
		return;
	QString base = appDir;
	if (!base.endsWith('/') && !base.endsWith('\\'))
		base += '/';
#ifdef Q_OS_WIN
	base = QDir::toNativeSeparators(base);
#else
	base.replace(QLatin1Char('\\'), QLatin1Char('/'));
#endif
	const QString extraPath = base + QStringLiteral("lua/?.lua;") + base + QStringLiteral("lua/?/init.lua");
	const QString extraCPath =
#ifdef Q_OS_WIN
	    base + QStringLiteral("?.dll;") + base + QStringLiteral("lua/?.dll;") + base +
	    QStringLiteral("lua/?/core.dll");
#else
	    base + QStringLiteral("?.so;") + base + QStringLiteral("lua/?.so;") + base +
	    QStringLiteral("lua/?/core.so");
#endif
	lua_getglobal(L, LUA_LOADLIBNAME); // package table
	if (!lua_istable(L, -1))
	{
		lua_pop(L, 1);
		return;
	}
	lua_getfield(L, -1, "path");
	const char *existing = lua_tostring(L, -1);
	QString     current  = existing ? QString::fromUtf8(existing) : QString();
	lua_pop(L, 1);

	if (!current.contains(extraPath))
	{
		if (!current.isEmpty() && !current.endsWith(QLatin1Char(';')))
			current += QLatin1Char(';');
		current += extraPath;
		const QByteArray bytes = current.toUtf8();
		lua_pushlstring(L, bytes.constData(), bytes.size());
		lua_setfield(L, -2, "path");
	}

	lua_getfield(L, -1, "cpath");
	const char *existingCPath = lua_tostring(L, -1);
	QString     currentCPath  = existingCPath ? QString::fromUtf8(existingCPath) : QString();
	lua_pop(L, 1);

	if (!currentCPath.contains(extraCPath))
	{
		if (!currentCPath.isEmpty() && !currentCPath.endsWith(QLatin1Char(';')))
			currentCPath += QLatin1Char(';');
		currentCPath += extraCPath;
		const QByteArray bytes = currentCPath.toUtf8();
		lua_pushlstring(L, bytes.constData(), bytes.size());
		lua_setfield(L, -2, "cpath");
	}
	lua_pop(L, 1); // package
}
#endif

void LuaCallbackEngine::registerWorldBindings()
{
#ifdef QMUD_ENABLE_LUA_SCRIPTING
	if (!m_state)
		return;

	registerErrorDescriptions(m_state);
	registerColourNames(m_state);
	registerExtendedColours(m_state);
	registerFlagTables(m_state);
	registerSendToTable(m_state);
	registerMiniwinConstants(m_state);
	registerBitLibrary(m_state);
	registerRexLibrary(m_state);
	registerLuaPreloads(m_state);
	registerSqliteLibrary(m_state);
	registerBcLibrary(m_state);
	registerLpegLibrary(m_state);
	registerProgressLibrary(m_state);

	QSet<QString> registered;
	lua_newtable(m_state);
	auto registerWorldFn = [this, &registered](const char *name, const lua_CFunction fn)
	{
		lua_pushlightuserdata(m_state, this);
		lua_pushcclosure(m_state, fn, 1);
		lua_setfield(m_state, -2, name);
		lua_pushlightuserdata(m_state, this);
		lua_pushcclosure(m_state, fn, 1);
		lua_setglobal(m_state, name);
		registered.insert(QString::fromLatin1(name));
	};
	static const LuaBindingEntry kWorldBindings[] = {
	    {"Accelerator",               luaAccelerator              },
	    {"AcceleratorList",           luaAcceleratorList          },
	    {"AcceleratorTo",             luaAcceleratorTo            },
	    {"Activate",                  luaActivate                 },
	    {"ActivateClient",            luaActivateClient           },
	    {"ActivateNotepad",           luaActivateNotepad          },
	    {"AddAlias",                  luaAddAlias                 },
	    {"AddFont",                   luaAddFont                  },
	    {"AddMapperComment",          luaAddMapperComment         },
	    {"AddSpellCheckWord",         luaAddSpellCheckWord        },
	    {"AddTimer",                  luaAddTimer                 },
	    {"AddToMapper",               luaAddToMapper              },
	    {"AddTrigger",                luaAddTrigger               },
	    {"AddTriggerEx",              luaAddTriggerEx             },
	    {"AdjustColour",              luaAdjustColour             },
	    {"ANSI",	                  luaANSI                     },
	    {"AnsiNote",                  luaAnsiNote                 },
	    {"AppendToNotepad",           luaAppendToNotepad          },
	    {"ArrayClear",                luaArrayClear               },
	    {"ArrayCount",                luaArrayCount               },
	    {"ArrayCreate",               luaArrayCreate              },
	    {"ArrayDelete",               luaArrayDelete              },
	    {"ArrayDeleteKey",            luaArrayDeleteKey           },
	    {"ArrayExists",               luaArrayExists              },
	    {"ArrayExport",               luaArrayExport              },
	    {"ArrayExportKeys",           luaArrayExportKeys          },
	    {"ArrayGet",                  luaArrayGet                 },
	    {"ArrayGetFirstKey",          luaArrayGetFirstKey         },
	    {"ArrayGetLastKey",           luaArrayGetLastKey          },
	    {"ArrayImport",               luaArrayImport              },
	    {"ArrayKeyExists",            luaArrayKeyExists           },
	    {"ArrayList",                 luaArrayList                },
	    {"ArrayListAll",              luaArrayListAll             },
	    {"ArrayListKeys",             luaArrayListKeys            },
	    {"ArrayListValues",           luaArrayListValues          },
	    {"ArraySet",                  luaArraySet                 },
	    {"ArraySize",                 luaArraySize                },
	    {"Base64Decode",              luaBase64Decode             },
	    {"Base64Encode",              luaBase64Encode             },
	    {"BlendPixel",                luaBlendPixel               },
	    {"Bookmark",                  luaBookmark                 },
	    {"BroadcastPlugin",           luaBroadcastPlugin          },
	    {"CallPlugin",                luaCallPlugin               },
	    {"ChangeDir",                 luaChangeDir                },
	    {"ChatAcceptCalls",           luaChatAcceptCalls          },
	    {"ChatCall",                  luaChatCall                 },
	    {"ChatCallzChat",             luaChatCallzChat            },
	    {"ChatDisconnect",            luaChatDisconnect           },
	    {"ChatDisconnectAll",         luaChatDisconnectAll        },
	    {"ChatEverybody",             luaChatEverybody            },
	    {"ChatGetID",                 luaChatGetID                },
	    {"ChatGroup",                 luaChatGroup                },
	    {"ChatID",	                luaChatID                   },
	    {"ChatMessage",               luaChatMessage              },
	    {"ChatNameChange",            luaChatNameChange           },
	    {"ChatNote",                  luaChatNote                 },
	    {"ChatPasteEverybody",        luaChatPasteEverybody       },
	    {"ChatPasteText",             luaChatPasteText            },
	    {"ChatPeekConnections",       luaChatPeekConnections      },
	    {"ChatPersonal",              luaChatPersonal             },
	    {"ChatPing",                  luaChatPing                 },
	    {"ChatRequestConnections",    luaChatRequestConnections   },
	    {"ChatSendFile",              luaChatSendFile             },
	    {"ChatStopAcceptingCalls",    luaChatStopAcceptingCalls   },
	    {"ChatStopFileTransfer",      luaChatStopFileTransfer     },
	    {"CloseLog",                  luaCloseLog                 },
	    {"CloseNotepad",              luaCloseNotepad             },
	    {"ColourNameToRGB",           luaColourNameToRGB          },
	    {"ColourNote",                luaColourNote               },
	    {"ColourTell",                luaColourTell               },
	    {"Connect",                   luaConnect                  },
	    {"CreateGUID",                luaCreateGUID               },
	    {"DatabaseChanges",           luaDatabaseChanges          },
	    {"DatabaseClose",             luaDatabaseClose            },
	    {"DatabaseColumnName",        luaDatabaseColumnName       },
	    {"DatabaseColumnNames",       luaDatabaseColumnNames      },
	    {"DatabaseColumns",           luaDatabaseColumns          },
	    {"DatabaseColumnText",        luaDatabaseColumnText       },
	    {"DatabaseColumnType",        luaDatabaseColumnType       },
	    {"DatabaseColumnValue",       luaDatabaseColumnValue      },
	    {"DatabaseColumnValues",      luaDatabaseColumnValues     },
	    {"DatabaseError",             luaDatabaseError            },
	    {"DatabaseExec",              luaDatabaseExec             },
	    {"DatabaseFinalize",          luaDatabaseFinalize         },
	    {"DatabaseGetField",          luaDatabaseGetField         },
	    {"DatabaseInfo",              luaDatabaseInfo             },
	    {"DatabaseLastInsertRowid",   luaDatabaseLastInsertRowid  },
	    {"DatabaseList",              luaDatabaseList             },
	    {"DatabaseOpen",              luaDatabaseOpen             },
	    {"DatabasePrepare",           luaDatabasePrepare          },
	    {"DatabaseReset",             luaDatabaseReset            },
	    {"DatabaseStep",              luaDatabaseStep             },
	    {"DatabaseTotalChanges",      luaDatabaseTotalChanges     },
	    {"Debug",	                 luaDebug                    },
	    {"DeleteAlias",               luaDeleteAlias              },
	    {"DeleteAliasGroup",          luaDeleteAliasGroup         },
	    {"DeleteAllMapItems",         luaDeleteAllMapItems        },
	    {"DeleteCommandHistory",      luaDeleteCommandHistory     },
	    {"DeleteGroup",               luaDeleteGroup              },
	    {"DeleteLastMapItem",         luaDeleteLastMapItem        },
	    {"DeleteLines",               luaDeleteLines              },
	    {"DeleteOutput",              luaDeleteOutput             },
	    {"DeleteTemporaryAliases",    luaDeleteTemporaryAliases   },
	    {"DeleteTemporaryTimers",     luaDeleteTemporaryTimers    },
	    {"DeleteTemporaryTriggers",   luaDeleteTemporaryTriggers  },
	    {"DeleteTimer",               luaDeleteTimer              },
	    {"DeleteTimerGroup",          luaDeleteTimerGroup         },
	    {"DeleteTrigger",             luaDeleteTrigger            },
	    {"DeleteTriggerGroup",        luaDeleteTriggerGroup       },
	    {"DeleteVariable",            luaDeleteVariable           },
	    {"DiscardQueue",              luaDiscardQueue             },
	    {"Disconnect",                luaDisconnect               },
	    {"DoAfter",                   luaDoAfter                  },
	    {"DoAfterNote",               luaDoAfterNote              },
	    {"DoAfterSpecial",            luaDoAfterSpecial           },
	    {"DoAfterSpeedWalk",          luaDoAfterSpeedWalk         },
	    {"DoCommand",                 luaDoCommand                },
	    {"EditDistance",              luaEditDistance             },
	    {"EnableAlias",               luaEnableAlias              },
	    {"EnableAliasGroup",          luaEnableAliasGroup         },
	    {"EnableGroup",               luaEnableGroup              },
	    {"EnableMapping",             luaEnableMapping            },
	    {"EnablePlugin",              luaEnablePlugin             },
	    {"EnableTimer",               luaEnableTimer              },
	    {"EnableTimerGroup",          luaEnableTimerGroup         },
	    {"EnableTrigger",             luaEnableTrigger            },
	    {"EnableTriggerGroup",        luaEnableTriggerGroup       },
	    {"ErrorDesc",                 luaErrorDesc                },
	    {"EvaluateSpeedwalk",         luaEvaluateSpeedwalk        },
	    {"Execute",                   luaExecute                  },
	    {"ExportXML",                 luaExportXML                },
	    {"FilterPixel",               luaFilterPixel              },
	    {"FixupEscapeSequences",      luaFixupEscapeSequences     },
	    {"FixupHTML",                 luaFixupHTML                },
	    {"FlashIcon",                 luaFlashIcon                },
	    {"FlushLog",                  luaFlushLog                 },
	    {"GenerateName",              luaGenerateName             },
	    {"GetAlias",                  luaGetAlias                 },
	    {"GetAliasInfo",              luaGetAliasInfo             },
	    {"GetAliasList",              luaGetAliasList             },
	    {"GetAliasOption",            luaGetAliasOption           },
	    {"GetAliasWildcard",          luaGetAliasWildcard         },
	    {"GetAlphaOption",            luaGetAlphaOption           },
	    {"GetAlphaOptionList",        luaGetAlphaOptionList       },
	    {"GetBoldColour",             luaGetBoldColour            },
	    {"GetChatInfo",               luaGetChatInfo              },
	    {"GetChatList",               luaGetChatList              },
	    {"GetChatOption",             luaGetChatOption            },
	    {"GetClipboard",              luaGetClipboard             },
	    {"GetCommand",                luaGetCommand               },
	    {"GetCommandList",            luaGetCommandList           },
	    {"GetConnectDuration",        luaGetConnectDuration       },
	    {"GetConnectTime",            luaGetConnectTime           },
	    {"GetCurrentValue",           luaGetCurrentValue          },
	    {"GetCustomColourBackground", luaGetCustomColourBackground},
	    {"GetCustomColourName",       luaGetCustomColourName      },
	    {"GetCustomColourText",       luaGetCustomColourText      },
	    {"GetDefaultValue",           luaGetDefaultValue          },
	    {"GetDeviceCaps",             luaGetDeviceCaps            },
	    {"GetEchoInput",              luaGetEchoInput             },
	    {"GetEntity",                 luaGetEntity                },
	    {"GetFrame",                  luaGetFrame                 },
	    {"GetGlobalOption",           luaGetGlobalOption          },
	    {"GetGlobalOptionList",       luaGetGlobalOptionList      },
	    {"GetHostAddress",            luaGetHostAddress           },
	    {"GetHostName",               luaGetHostName              },
	    {"GetInfo",                   luaGetInfo                  },
	    {"GetInternalCommandsList",   luaGetInternalCommandsList  },
	    {"GetLineCount",              luaGetLineCount             },
	    {"GetLineInfo",               luaGetLineInfo              },
	    {"GetLinesInBufferCount",     luaGetLinesInBufferCount    },
	    {"GetLoadedValue",            luaGetLoadedValue           },
	    {"GetLogInput",               luaGetLogInput              },
	    {"GetLogNotes",               luaGetLogNotes              },
	    {"GetLogOutput",              luaGetLogOutput             },
	    {"GetMainWindowPosition",     luaGetMainWindowPosition    },
	    {"GetMapColour",              luaGetMapColour             },
	    {"GetMapping",                luaGetMapping               },
	    {"GetMappingCount",           luaGetMappingCount          },
	    {"GetMappingItem",            luaGetMappingItem           },
	    {"GetMappingString",          luaGetMappingString         },
	    {"GetNormalColour",           luaGetNormalColour          },
	    {"GetNoteColour",             luaGetNoteColour            },
	    {"GetNoteColourBack",         luaGetNoteColourBack        },
	    {"GetNoteColourFore",         luaGetNoteColourFore        },
	    {"GetNotepadLength",          luaGetNotepadLength         },
	    {"GetNotepadList",            luaGetNotepadList           },
	    {"GetNotepadText",            luaGetNotepadText           },
	    {"GetNotepadWindowPosition",  luaGetNotepadWindowPosition },
	    {"GetNotes",                  luaGetNotes                 },
	    {"GetNoteStyle",              luaGetNoteStyle             },
	    {"GetOption",                 luaGetOption                },
	    {"GetOptionList",             luaGetOptionList            },
	    {"GetPluginAliasInfo",        luaGetPluginAliasInfo       },
	    {"GetPluginAliasList",        luaGetPluginAliasList       },
	    {"GetPluginAliasOption",      luaGetPluginAliasOption     },
	    {"GetPluginID",               luaGetPluginID              },
	    {"GetPluginInfo",             luaGetPluginInfo            },
	    {"GetPluginList",             luaGetPluginList            },
	    {"GetPluginName",             luaGetPluginName            },
	    {"GetPluginTimerInfo",        luaGetPluginTimerInfo       },
	    {"GetPluginTimerList",        luaGetPluginTimerList       },
	    {"GetPluginTimerOption",      luaGetPluginTimerOption     },
	    {"GetPluginTriggerInfo",      luaGetPluginTriggerInfo     },
	    {"GetPluginTriggerList",      luaGetPluginTriggerList     },
	    {"GetPluginTriggerOption",    luaGetPluginTriggerOption   },
	    {"GetPluginVariable",         luaGetPluginVariable        },
	    {"GetPluginVariableList",     luaGetPluginVariableList    },
	    {"GetQueue",                  luaGetQueue                 },
	    {"GetReceivedBytes",          luaGetReceivedBytes         },
	    {"GetRecentLines",            luaGetRecentLines           },
	    {"GetRemoveMapReverses",      luaGetRemoveMapReverses     },
	    {"GetScriptTime",             luaGetScriptTime            },
	    {"GetSelectionEndColumn",     luaGetSelectionEndColumn    },
	    {"GetSelectionEndLine",       luaGetSelectionEndLine      },
	    {"GetSelectionStartColumn",   luaGetSelectionStartColumn  },
	    {"GetSelectionStartLine",     luaGetSelectionStartLine    },
	    {"GetSentBytes",              luaGetSentBytes             },
	    {"GetSoundStatus",            luaGetSoundStatus           },
	    {"GetSpeedWalkDelay",         luaGetSpeedWalkDelay        },
	    {"GetStatusTime",             luaGetStatusTime            },
	    {"GetStyleInfo",              luaGetStyleInfo             },
	    {"GetSysColor",               luaGetSysColor              },
	    {"GetSystemMetrics",          luaGetSystemMetrics         },
	    {"GetTimer",                  luaGetTimer                 },
	    {"GetTimerInfo",              luaGetTimerInfo             },
	    {"GetTimerList",              luaGetTimerList             },
	    {"GetTimerOption",            luaGetTimerOption           },
	    {"GetTrace",                  luaGetTrace                 },
	    {"GetTrigger",                luaGetTrigger               },
	    {"GetTriggerInfo",            luaGetTriggerInfo           },
	    {"GetTriggerList",            luaGetTriggerList           },
	    {"GetTriggerOption",          luaGetTriggerOption         },
	    {"GetTriggerWildcard",        luaGetTriggerWildcard       },
	    {"GetUdpPort",                luaGetUdpPort               },
	    {"GetUniqueID",               luaGetUniqueID              },
	    {"GetUniqueNumber",           luaGetUniqueNumber          },
	    {"GetVariable",               luaGetVariable              },
	    {"GetVariableList",           luaGetVariableList          },
	    {"GetWorld",                  luaGetWorld                 },
	    {"GetWorldById",              luaGetWorldById             },
	    {"GetWorldID",                luaGetWorldID               },
	    {"GetWorldIdList",            luaGetWorldIdList           },
	    {"GetWorldList",              luaGetWorldList             },
	    {"GetWorldWindowPosition",    luaGetWorldWindowPosition   },
	    {"GetXMLEntity",              luaGetXMLEntity             },
	    {"Hash",	                  luaHash                     },
	    {"Help",	                  luaHelp                     },
	    {"Hyperlink",                 luaHyperlink                },
	    {"ImportXML",                 luaImportXML                },
	    {"Info",	                  luaInfo                     },
	    {"InfoBackground",            luaInfoBackground           },
	    {"InfoClear",                 luaInfoClear                },
	    {"InfoColour",                luaInfoColour               },
	    {"InfoFont",                  luaInfoFont                 },
	    {"IsAlias",                   luaIsAlias                  },
	    {"IsConnected",               luaIsConnected              },
	    {"IsLogOpen",                 luaIsLogOpen                },
	    {"IsPluginInstalled",         luaIsPluginInstalled        },
	    {"IsTimer",                   luaIsTimer                  },
	    {"IsTrigger",                 luaIsTrigger                },
	    {"LoadPlugin",                luaLoadPlugin               },
	    {"LogSend",                   luaLogSend                  },
	    {"MakeRegularExpression",     luaMakeRegularExpression    },
	    {"MapColour",                 luaMapColour                },
	    {"MapColourList",             luaMapColourList            },
	    {"Menu",	                  luaMenu                     },
	    {"Metaphone",                 luaMetaphone                },
	    {"MoveMainWindow",            luaMoveMainWindow           },
	    {"MoveNotepadWindow",         luaMoveNotepadWindow        },
	    {"MoveWorldWindow",           luaMoveWorldWindow          },
	    {"MtRand",	                luaMtRand                   },
	    {"MtSrand",                   luaMtSrand                  },
	    {"Note",	                  luaNote                     },
	    {"NoteColourName",            luaNoteColourName           },
	    {"NoteColourRGB",             luaNoteColourRGB            },
	    {"NoteHr",	                luaNoteHr                   },
	    {"NotepadColour",             luaNotepadColour            },
	    {"NotepadFont",               luaNotepadFont              },
	    {"NotepadReadOnly",           luaNotepadReadOnly          },
	    {"NotepadSaveMethod",         luaNotepadSaveMethod        },
	    {"NoteStyle",                 luaNoteStyle                },
	    {"Open",	                  luaOpen                     },
	    {"OpenBrowser",               luaOpenBrowser              },
	    {"OpenLog",                   luaOpenLog                  },
	    {"PasteCommand",              luaPasteCommand             },
	    {"Pause",	                 luaPause                    },
	    {"PickColour",                luaPickColour               },
	    {"PlaySound",                 luaPlaySound                },
	    {"PlaySoundMemory",           luaPlaySoundMemory          },
	    {"PluginSupports",            luaPluginSupports           },
	    {"print",	                 luaPrint                    },
	    {"PushCommand",               luaPushCommand              },
	    {"Queue",	                 luaQueue                    },
	    {"ReadNamesFile",             luaReadNamesFile            },
	    {"Redraw",	                luaRedraw                   },
	    {"ReloadPlugin",              luaReloadPlugin             },
	    {"RemoveBacktracks",          luaRemoveBacktracks         },
	    {"Repaint",                   luaRepaint                  },
	    {"Replace",                   luaReplace                  },
	    {"ReplaceNotepad",            luaReplaceNotepad           },
	    {"Reset",	                 luaReset                    },
	    {"ResetIP",                   luaResetIP                  },
	    {"ResetStatusTime",           luaResetStatusTime          },
	    {"ResetTimer",                luaResetTimer               },
	    {"ResetTimers",               luaResetTimers              },
	    {"ReverseSpeedwalk",          luaReverseSpeedwalk         },
	    {"RGBColourToName",           luaRGBColourToName          },
	    {"Save",	                  luaSave                     },
	    {"SaveNotepad",               luaSaveNotepad              },
	    {"SaveState",                 luaSaveState                },
	    {"SelectCommand",             luaSelectCommand            },
	    {"Send",	                  luaSend                     },
	    {"SendImmediate",             luaSendImmediate            },
	    {"SendNoEcho",                luaSendNoEcho               },
	    {"SendPkt",                   luaSendPkt                  },
	    {"SendPush",                  luaSendPush                 },
	    {"SendSpecial",               luaSendSpecial              },
	    {"SendToNotepad",             luaSendToNotepad            },
	    {"SetAliasOption",            luaSetAliasOption           },
	    {"SetAlphaOption",            luaSetAlphaOption           },
	    {"SetBackgroundColour",       luaSetBackgroundColour      },
	    {"SetBackgroundImage",        luaSetBackgroundImage       },
	    {"SetBoldColour",             luaSetBoldColour            },
	    {"SetChanged",                luaSetChanged               },
	    {"SetChatOption",             luaSetChatOption            },
	    {"SetClipboard",              luaSetClipboard             },
	    {"SetCommand",                luaSetCommand               },
	    {"SetCommandSelection",       luaSetCommandSelection      },
	    {"SetCommandWindowHeight",    luaSetCommandWindowHeight   },
	    {"SetCursor",                 luaSetCursor                },
	    {"SetCustomColourBackground", luaSetCustomColourBackground},
	    {"SetCustomColourName",       luaSetCustomColourName      },
	    {"SetCustomColourText",       luaSetCustomColourText      },
	    {"SetEchoInput",              luaSetEchoInput             },
	    {"SetEntity",                 luaSetEntity                },
	    {"SetForegroundImage",        luaSetForegroundImage       },
	    {"SetFrameBackgroundColour",  luaSetFrameBackgroundColour },
	    {"SetInputFont",              luaSetInputFont             },
	    {"SetLogInput",               luaSetLogInput              },
	    {"SetLogNotes",               luaSetLogNotes              },
	    {"SetLogOutput",              luaSetLogOutput             },
	    {"SetMainTitle",              luaSetMainTitle             },
	    {"SetMapping",                luaSetMapping               },
	    {"SetNormalColour",           luaSetNormalColour          },
	    {"SetNoteColour",             luaSetNoteColour            },
	    {"SetNoteColourBack",         luaSetNoteColourBack        },
	    {"SetNoteColourFore",         luaSetNoteColourFore        },
	    {"SetNotes",                  luaSetNotes                 },
	    {"SetOption",                 luaSetOption                },
	    {"SetOutputFont",             luaSetOutputFont            },
	    {"SetRemoveMapReverses",      luaSetRemoveMapReverses     },
	    {"SetScroll",                 luaSetScroll                },
	    {"SetSelection",              luaSetSelection             },
	    {"SetSpeedWalkDelay",         luaSetSpeedWalkDelay        },
	    {"SetStatus",                 luaSetStatus                },
	    {"SetTimerOption",            luaSetTimerOption           },
	    {"SetTitle",                  luaSetTitle                 },
	    {"SetToolBarPosition",        luaSetToolBarPosition       },
	    {"SetTrace",                  luaSetTrace                 },
	    {"SetTriggerOption",          luaSetTriggerOption         },
	    {"SetUnseenLines",            luaSetUnseenLines           },
	    {"SetVariable",               luaSetVariable              },
	    {"SetWorldWindowStatus",      luaSetWorldWindowStatus     },
	    {"ShiftTabCompleteItem",      luaShiftTabCompleteItem     },
	    {"ShowInfoBar",               luaShowInfoBar              },
	    {"Simulate",                  luaSimulate                 },
	    {"Sound",	                 luaSound                    },
	    {"SpellCheck",                luaSpellCheck               },
	    {"SpellCheckCommand",         luaSpellCheckCommand        },
	    {"SpellCheckDlg",             luaSpellCheckDlg            },
	    {"StopEvaluatingTriggers",    luaStopEvaluatingTriggers   },
	    {"StopSound",                 luaStopSound                },
	    {"StripANSI",                 luaStripANSI                },
	    {"Tell",	                  luaTell                     },
	    {"TextRectangle",             luaTextRectangle            },
	    {"TraceOut",                  luaTraceOut                 },
	    {"TranslateDebug",            luaTranslateDebug           },
	    {"TranslateGerman",           luaTranslateGerman          },
	    {"Transparency",              luaTransparency             },
	    {"Trim",	                  luaTrim                     },
	    {"UdpListen",                 luaUdpListen                },
	    {"UdpPortList",               luaUdpPortList              },
	    {"UdpSend",                   luaUdpSend                  },
	    {"UnloadPlugin",              luaUnloadPlugin             },
	    {"Version",                   luaVersion                  },
	    {"WindowAddHotspot",          luaWindowAddHotspot         },
	    {"WindowArc",                 luaWindowArc                },
	    {"WindowBezier",              luaWindowBezier             },
	    {"WindowBlendImage",          luaWindowBlendImage         },
	    {"WindowCircleOp",            luaWindowCircleOp           },
	    {"WindowCreate",              luaWindowCreate             },
	    {"WindowCreateImage",         luaWindowCreateImage        },
	    {"WindowDelete",              luaWindowDelete             },
	    {"WindowDeleteAllHotspots",   luaWindowDeleteAllHotspots  },
	    {"WindowDeleteHotspot",       luaWindowDeleteHotspot      },
	    {"WindowDragHandler",         luaWindowDragHandler        },
	    {"WindowDrawImage",           luaWindowDrawImage          },
	    {"WindowDrawImageAlpha",      luaWindowDrawImageAlpha     },
	    {"WindowFilter",              luaWindowFilter             },
	    {"WindowFont",                luaWindowFont               },
	    {"WindowFontInfo",            luaWindowFontInfo           },
	    {"WindowFontList",            luaWindowFontList           },
	    {"WindowGetImageAlpha",       luaWindowGetImageAlpha      },
	    {"WindowGetPixel",            luaWindowGetPixel           },
	    {"WindowGradient",            luaWindowGradient           },
	    {"WindowHotspotInfo",         luaWindowHotspotInfo        },
	    {"WindowHotspotList",         luaWindowHotspotList        },
	    {"WindowHotspotTooltip",      luaWindowHotspotTooltip     },
	    {"WindowImageFromWindow",     luaWindowImageFromWindow    },
	    {"WindowImageInfo",           luaWindowImageInfo          },
	    {"WindowImageList",           luaWindowImageList          },
	    {"WindowImageOp",             luaWindowImageOp            },
	    {"WindowInfo",                luaWindowInfo               },
	    {"WindowLine",                luaWindowLine               },
	    {"WindowList",                luaWindowList               },
	    {"WindowLoadImage",           luaWindowLoadImage          },
	    {"WindowLoadImageMemory",     luaWindowLoadImageMemory    },
	    {"WindowMenu",                luaWindowMenu               },
	    {"WindowMergeImageAlpha",     luaWindowMergeImageAlpha    },
	    {"WindowMoveHotspot",         luaWindowMoveHotspot        },
	    {"WindowPolygon",             luaWindowPolygon            },
	    {"WindowPosition",            luaWindowPosition           },
	    {"WindowRectOp",              luaWindowRectOp             },
	    {"WindowResize",              luaWindowResize             },
	    {"WindowScrollwheelHandler",  luaWindowScrollwheelHandler },
	    {"WindowSetPixel",            luaWindowSetPixel           },
	    {"WindowSetZOrder",           luaWindowSetZOrder          },
	    {"WindowShow",                luaWindowShow               },
	    {"WindowText",                luaWindowText               },
	    {"WindowTextWidth",           luaWindowTextWidth          },
	    {"WindowTransformImage",      luaWindowTransformImage     },
	    {"WindowWrite",               luaWindowWrite              },
	    {"WorldAddress",              luaWorldAddress             },
	    {"WorldName",                 luaWorldName                },
	    {"WorldPort",                 luaWorldPort                },
	    {"WriteLog",                  luaWriteLog                 },
	};
	for (const auto &[name, function] : kWorldBindings)
		registerWorldFn(name, function);

	for (const char *name : kWorldLibNames)
	{
		if (!name)
			break;
		if (const QString fn = QString::fromLatin1(name); !registered.contains(fn))
			qWarning() << "Lua world function missing registration:" << fn;
	}

	lua_setglobal(m_state, "world");
	registerUtilsBindings(m_state, this);

	struct ErrorCodeEntry
	{
			const char *name;
			int         value;
	};
	static const ErrorCodeEntry kErrorCodes[] = {
	    {"eOK",	                     eOK	                    },
	    {"eWorldOpen",                  eWorldOpen                 },
	    {"eWorldClosed",                eWorldClosed               },
	    {"eNoNameSpecified",            eNoNameSpecified           },
	    {"eCannotPlaySound",            eCannotPlaySound           },
	    {"eTriggerNotFound",            eTriggerNotFound           },
	    {"eTriggerAlreadyExists",       eTriggerAlreadyExists      },
	    {"eTriggerCannotBeEmpty",       eTriggerCannotBeEmpty      },
	    {"eInvalidObjectLabel",         eInvalidObjectLabel        },
	    {"eScriptNameNotLocated",       eScriptNameNotLocated      },
	    {"eAliasNotFound",              eAliasNotFound             },
	    {"eAliasAlreadyExists",         eAliasAlreadyExists        },
	    {"eAliasCannotBeEmpty",         eAliasCannotBeEmpty        },
	    {"eCouldNotOpenFile",           eCouldNotOpenFile          },
	    {"eLogFileNotOpen",             eLogFileNotOpen            },
	    {"eLogFileAlreadyOpen",         eLogFileAlreadyOpen        },
	    {"eLogFileBadWrite",            eLogFileBadWrite           },
	    {"eTimerNotFound",              eTimerNotFound             },
	    {"eTimerAlreadyExists",         eTimerAlreadyExists        },
	    {"eVariableNotFound",           eVariableNotFound          },
	    {"eCommandNotEmpty",            eCommandNotEmpty           },
	    {"eBadRegularExpression",       eBadRegularExpression      },
	    {"eTimeInvalid",                eTimeInvalid               },
	    {"eBadMapItem",                 eBadMapItem                },
	    {"eNoMapItems",                 eNoMapItems                },
	    {"eUnknownOption",              eUnknownOption             },
	    {"eOptionOutOfRange",           eOptionOutOfRange          },
	    {"eTriggerSequenceOutOfRange",  eTriggerSequenceOutOfRange },
	    {"eTriggerSendToInvalid",       eTriggerSendToInvalid      },
	    {"eTriggerLabelNotSpecified",   eTriggerLabelNotSpecified  },
	    {"ePluginFileNotFound",         ePluginFileNotFound        },
	    {"eProblemsLoadingPlugin",      eProblemsLoadingPlugin     },
	    {"ePluginCannotSetOption",      ePluginCannotSetOption     },
	    {"ePluginCannotGetOption",      ePluginCannotGetOption     },
	    {"eNoSuchPlugin",               eNoSuchPlugin              },
	    {"eNotAPlugin",                 eNotAPlugin                },
	    {"eNoSuchRoutine",              eNoSuchRoutine             },
	    {"ePluginDoesNotSaveState",     ePluginDoesNotSaveState    },
	    {"ePluginCouldNotSaveState",    ePluginCouldNotSaveState   },
	    {"ePluginDisabled",             ePluginDisabled            },
	    {"eErrorCallingPluginRoutine",  eErrorCallingPluginRoutine },
	    {"eCommandsNestedTooDeeply",    eCommandsNestedTooDeeply   },
	    {"eCannotCreateChatSocket",     eCannotCreateChatSocket    },
	    {"eCannotLookupDomainName",     eCannotLookupDomainName    },
	    {"eNoChatConnections",          eNoChatConnections         },
	    {"eChatPersonNotFound",         eChatPersonNotFound        },
	    {"eBadParameter",               eBadParameter              },
	    {"eChatAlreadyListening",       eChatAlreadyListening      },
	    {"eChatIDNotFound",             eChatIDNotFound            },
	    {"eChatAlreadyConnected",       eChatAlreadyConnected      },
	    {"eClipboardEmpty",             eClipboardEmpty            },
	    {"eFileNotFound",               eFileNotFound              },
	    {"eAlreadyTransferringFile",    eAlreadyTransferringFile   },
	    {"eNotTransferringFile",        eNotTransferringFile       },
	    {"eNoSuchCommand",              eNoSuchCommand             },
	    {"eArrayAlreadyExists",         eArrayAlreadyExists        },
	    {"eArrayDoesNotExist",          eArrayDoesNotExist         },
	    {"eArrayNotEvenNumberOfValues", eArrayNotEvenNumberOfValues},
	    {"eImportedWithDuplicates",     eImportedWithDuplicates    },
	    {"eBadDelimiter",               eBadDelimiter              },
	    {"eSetReplacingExistingValue",  eSetReplacingExistingValue },
	    {"eKeyDoesNotExist",            eKeyDoesNotExist           },
	    {"eCannotImport",               eCannotImport              },
	    {"eItemInUse",                  eItemInUse                 },
	    {"eSpellCheckNotActive",        eSpellCheckNotActive       },
	    {"eCannotAddFont",              eCannotAddFont             },
	    {"ePenStyleNotValid",           ePenStyleNotValid          },
	    {"eUnableToLoadImage",          eUnableToLoadImage         },
	    {"eImageNotInstalled",          eImageNotInstalled         },
	    {"eInvalidNumberOfPoints",      eInvalidNumberOfPoints     },
	    {"eInvalidPoint",               eInvalidPoint              },
	    {"eHotspotPluginChanged",       eHotspotPluginChanged      },
	    {"eHotspotNotInstalled",        eHotspotNotInstalled       },
	    {"eNoSuchWindow",               eNoSuchWindow              },
	    {"eBrushStyleNotValid",         eBrushStyleNotValid        },
	    {nullptr,	                   0	                      }
    };

	lua_newtable(m_state);
	for (const ErrorCodeEntry *entry = kErrorCodes; entry->name; ++entry)
	{
		lua_pushstring(m_state, entry->name);
		lua_pushnumber(m_state, entry->value);
		lua_rawset(m_state, -3);
	}
	lua_setglobal(m_state, "error_code");

	for (const ErrorCodeEntry *entry = kErrorCodes; entry->name; ++entry)
	{
		lua_pushnumber(m_state, entry->value);
		lua_setglobal(m_state, entry->name);
	}

	registerCheckFunction(m_state);

	m_worldBindingsReady = true;
#endif
}

bool LuaCallbackEngine::callMxpError(const QString &functionName, int level, long messageNumber,
                                     int lineNumber, const QString &message)
{
#ifdef QMUD_ENABLE_LUA_SCRIPTING
	if (functionName.isEmpty())
		return false;
	if (!ensureState())
		return false;

	if (!pushLuaFunctionByName(m_state, functionName))
		return false;

	lua_pushinteger(m_state, level);
	lua_pushinteger(m_state, messageNumber);
	lua_pushinteger(m_state, lineNumber);
	const QByteArray msgBytes = message.toUtf8();
	lua_pushlstring(m_state, msgBytes.constData(), msgBytes.size());

	QElapsedTimer timer;
	timer.start();
	ScriptExecutionDepthGuard depthGuard(this);
	if (lua_pcall(m_state, 4, 1, 0) != 0)
	{
		const char *err = lua_tostring(m_state, -1);
		qWarning() << "Lua MXP error callback failed:" << (err ? err : "unknown");
		lua_pop(m_state, 1);
		return false;
	}
	addScriptTimeForEngine(*this, timer.nsecsElapsed());

	bool suppress = true;
	if (lua_gettop(m_state) > 0)
	{
		if (lua_isboolean(m_state, -1))
			suppress = lua_toboolean(m_state, -1) != 0;
		else
			suppress = lua_tonumber(m_state, -1) != 0.0;
	}
	lua_pop(m_state, 1);
	return suppress;
#else
	Q_UNUSED(functionName);
	Q_UNUSED(level);
	Q_UNUSED(messageNumber);
	Q_UNUSED(lineNumber);
	Q_UNUSED(message);
	return false;
#endif
}

void LuaCallbackEngine::callMxpStartUp(const QString &functionName)
{
#ifdef QMUD_ENABLE_LUA_SCRIPTING
	if (functionName.isEmpty())
		return;
	if (!ensureState())
		return;
	if (!pushLuaFunctionByName(m_state, functionName))
		return;
	QElapsedTimer timer;
	timer.start();
	ScriptExecutionDepthGuard depthGuard(this);
	if (lua_pcall(m_state, 0, 0, 0) != 0)
	{
		const char *err = lua_tostring(m_state, -1);
		qWarning() << "Lua MXP startup callback failed:" << (err ? err : "unknown");
		lua_pop(m_state, 1);
	}
	addScriptTimeForEngine(*this, timer.nsecsElapsed());
#else
	Q_UNUSED(functionName);
#endif
}

void LuaCallbackEngine::callMxpShutDown(const QString &functionName)
{
#ifdef QMUD_ENABLE_LUA_SCRIPTING
	if (functionName.isEmpty())
		return;
	if (!ensureState())
		return;
	if (!pushLuaFunctionByName(m_state, functionName))
		return;
	QElapsedTimer timer;
	timer.start();
	ScriptExecutionDepthGuard depthGuard(this);
	if (lua_pcall(m_state, 0, 0, 0) != 0)
	{
		const char *err = lua_tostring(m_state, -1);
		qWarning() << "Lua MXP shutdown callback failed:" << (err ? err : "unknown");
		lua_pop(m_state, 1);
	}
	addScriptTimeForEngine(*this, timer.nsecsElapsed());
#else
	Q_UNUSED(functionName);
#endif
}

bool LuaCallbackEngine::callMxpStartTag(const QString &functionName, const QString &name, const QString &args,
                                        const QMap<QString, QString> &table)
{
#ifdef QMUD_ENABLE_LUA_SCRIPTING
	if (functionName.isEmpty())
		return false;
	if (!ensureState())
		return false;

	if (!pushLuaFunctionByName(m_state, functionName))
		return false;

	const QByteArray nameBytes = name.toUtf8();
	lua_pushlstring(m_state, nameBytes.constData(), nameBytes.size());

	const QByteArray argBytes = args.toUtf8();
	lua_pushlstring(m_state, argBytes.constData(), argBytes.size());

	lua_newtable(m_state);
	for (auto it = table.constBegin(); it != table.constEnd(); ++it)
	{
		const QByteArray keyBytes   = it.key().toUtf8();
		const QByteArray valueBytes = it.value().toUtf8();
		lua_pushlstring(m_state, keyBytes.constData(), keyBytes.size());
		lua_pushlstring(m_state, valueBytes.constData(), valueBytes.size());
		lua_settable(m_state, -3);
	}

	QElapsedTimer timer;
	timer.start();
	ScriptExecutionDepthGuard depthGuard(this);
	if (lua_pcall(m_state, 3, 1, 0) != 0)
	{
		const char *err = lua_tostring(m_state, -1);
		qWarning() << "Lua MXP start tag callback failed:" << (err ? err : "unknown");
		lua_pop(m_state, 1);
		return false;
	}
	addScriptTimeForEngine(*this, timer.nsecsElapsed());

	bool suppress = true;
	if (lua_gettop(m_state) > 0)
	{
		if (lua_isboolean(m_state, -1))
			suppress = lua_toboolean(m_state, -1) != 0;
		else
			suppress = lua_tonumber(m_state, -1) != 0.0;
	}
	lua_pop(m_state, 1);
	return suppress;
#else
	Q_UNUSED(functionName);
	Q_UNUSED(name);
	Q_UNUSED(args);
	Q_UNUSED(table);
	return false;
#endif
}

void LuaCallbackEngine::callMxpEndTag(const QString &functionName, const QString &name, const QString &text)
{
#ifdef QMUD_ENABLE_LUA_SCRIPTING
	if (functionName.isEmpty())
		return;
	if (!ensureState())
		return;

	if (!pushLuaFunctionByName(m_state, functionName))
		return;

	const QByteArray nameBytes = name.toUtf8();
	lua_pushlstring(m_state, nameBytes.constData(), nameBytes.size());
	const QByteArray textBytes = text.toUtf8();
	lua_pushlstring(m_state, textBytes.constData(), textBytes.size());

	QElapsedTimer timer;
	timer.start();
	ScriptExecutionDepthGuard depthGuard(this);
	if (lua_pcall(m_state, 2, 0, 0) != 0)
	{
		const char *err = lua_tostring(m_state, -1);
		qWarning() << "Lua MXP end tag callback failed:" << (err ? err : "unknown");
		lua_pop(m_state, 1);
	}
	addScriptTimeForEngine(*this, timer.nsecsElapsed());
#else
	Q_UNUSED(functionName);
	Q_UNUSED(name);
	Q_UNUSED(text);
#endif
}

void LuaCallbackEngine::callMxpSetVariable(const QString &functionName, const QString &name,
                                           const QString &contents)
{
#ifdef QMUD_ENABLE_LUA_SCRIPTING
	if (functionName.isEmpty())
		return;
	if (!ensureState())
		return;

	if (!pushLuaFunctionByName(m_state, functionName))
		return;

	const QByteArray nameBytes = name.toUtf8();
	lua_pushlstring(m_state, nameBytes.constData(), nameBytes.size());
	const QByteArray valueBytes = contents.toUtf8();
	lua_pushlstring(m_state, valueBytes.constData(), valueBytes.size());

	QElapsedTimer timer;
	timer.start();
	ScriptExecutionDepthGuard depthGuard(this);
	if (lua_pcall(m_state, 2, 0, 0) != 0)
	{
		const char *err = lua_tostring(m_state, -1);
		reportLuaError(*this, QStringLiteral("Lua MXP set variable callback failed: %1")
		                          .arg(QString::fromUtf8(err ? err : "unknown")));
		lua_pop(m_state, 1);
	}
	addScriptTimeForEngine(*this, timer.nsecsElapsed());
#else
	Q_UNUSED(functionName);
	Q_UNUSED(name);
	Q_UNUSED(contents);
#endif
}

bool LuaCallbackEngine::callFunctionNoArgs(const QString &functionName, bool *hasFunction, bool defaultResult)
{
#ifdef QMUD_ENABLE_LUA_SCRIPTING
	if (hasFunction)
		*hasFunction = false;
	if (functionName.isEmpty())
		return defaultResult;
	if (!ensureState())
		return defaultResult;

	if (!pushLuaFunctionByName(m_state, functionName))
		return defaultResult;
	if (hasFunction)
		*hasFunction = true;

	QElapsedTimer timer;
	timer.start();
	ScriptExecutionDepthGuard depthGuard(this);
	if (lua_pcall(m_state, 0, 1, 0) != 0)
	{
		const char *err = lua_tostring(m_state, -1);
		reportLuaError(
		    *this, QStringLiteral("Lua callback failed: %1").arg(QString::fromUtf8(err ? err : "unknown")));
		lua_pop(m_state, 1);
		return false;
	}
	addScriptTimeForEngine(*this, timer.nsecsElapsed());

	bool result = defaultResult;
	if (lua_gettop(m_state) > 0)
	{
		if (lua_isboolean(m_state, -1))
			result = lua_toboolean(m_state, -1) != 0;
		else if (lua_isnumber(m_state, -1))
			result = lua_tonumber(m_state, -1) != 0.0;
	}
	lua_pop(m_state, 1);
	return result;
#else
	Q_UNUSED(functionName);
	Q_UNUSED(hasFunction);
	Q_UNUSED(defaultResult);
	return defaultResult;
#endif
}

bool LuaCallbackEngine::callFunctionWithString(const QString &functionName, const QString &arg,
                                               bool *hasFunction, bool defaultResult)
{
#ifdef QMUD_ENABLE_LUA_SCRIPTING
	if (hasFunction)
		*hasFunction = false;
	if (functionName.isEmpty())
		return defaultResult;
	if (!ensureState())
		return defaultResult;

	if (!pushLuaFunctionByName(m_state, functionName))
		return defaultResult;
	if (hasFunction)
		*hasFunction = true;

	const QByteArray argBytes = arg.toUtf8();
	lua_pushlstring(m_state, argBytes.constData(), argBytes.size());

	QElapsedTimer timer;
	timer.start();
	ScriptExecutionDepthGuard depthGuard(this);
	if (lua_pcall(m_state, 1, 1, 0) != 0)
	{
		const char *err = lua_tostring(m_state, -1);
		reportLuaError(
		    *this, QStringLiteral("Lua callback failed: %1").arg(QString::fromUtf8(err ? err : "unknown")));
		lua_pop(m_state, 1);
		return defaultResult;
	}
	addScriptTimeForEngine(*this, timer.nsecsElapsed());

	bool result = defaultResult;
	if (lua_gettop(m_state) > 0)
	{
		if (lua_isboolean(m_state, -1))
			result = lua_toboolean(m_state, -1) != 0;
		else if (lua_isnumber(m_state, -1))
			result = lua_tonumber(m_state, -1) != 0.0;
	}
	lua_pop(m_state, 1);
	return result;
#else
	Q_UNUSED(functionName);
	Q_UNUSED(arg);
	Q_UNUSED(hasFunction);
	Q_UNUSED(defaultResult);
	return defaultResult;
#endif
}

bool LuaCallbackEngine::callFunctionWithBytes(const QString &functionName, const QByteArray &arg,
                                              bool *hasFunction, bool defaultResult)
{
#ifdef QMUD_ENABLE_LUA_SCRIPTING
	if (hasFunction)
		*hasFunction = false;
	if (functionName.isEmpty())
		return defaultResult;
	if (!ensureState())
		return defaultResult;

	if (!pushLuaFunctionByName(m_state, functionName))
		return defaultResult;
	if (hasFunction)
		*hasFunction = true;

	lua_pushlstring(m_state, arg.constData(), arg.size());

	QElapsedTimer timer;
	timer.start();
	ScriptExecutionDepthGuard depthGuard(this);
	if (lua_pcall(m_state, 1, 1, 0) != 0)
	{
		const char *err = lua_tostring(m_state, -1);
		reportLuaError(
		    *this, QStringLiteral("Lua callback failed: %1").arg(QString::fromUtf8(err ? err : "unknown")));
		lua_pop(m_state, 1);
		return defaultResult;
	}
	addScriptTimeForEngine(*this, timer.nsecsElapsed());

	bool result = defaultResult;
	if (lua_gettop(m_state) > 0)
	{
		if (lua_isboolean(m_state, -1))
			result = lua_toboolean(m_state, -1) != 0;
		else if (lua_isnumber(m_state, -1))
			result = lua_tonumber(m_state, -1) != 0.0;
	}
	lua_pop(m_state, 1);
	return result;
#else
	Q_UNUSED(functionName);
	Q_UNUSED(arg);
	Q_UNUSED(hasFunction);
	Q_UNUSED(defaultResult);
	return defaultResult;
#endif
}

bool LuaCallbackEngine::callFunctionWithBytesInOut(const QString &functionName, QByteArray &arg,
                                                   bool *hasFunction)
{
#ifdef QMUD_ENABLE_LUA_SCRIPTING
	if (hasFunction)
		*hasFunction = false;
	if (functionName.isEmpty())
		return true;
	if (!ensureState())
		return true;

	if (!pushLuaFunctionByName(m_state, functionName))
		return true;
	if (hasFunction)
		*hasFunction = true;

	lua_pushlstring(m_state, arg.constData(), arg.size());

	QElapsedTimer timer;
	timer.start();
	ScriptExecutionDepthGuard depthGuard(this);
	if (lua_pcall(m_state, 1, 1, 0) != 0)
	{
		const char *err = lua_tostring(m_state, -1);
		reportLuaError(
		    *this, QStringLiteral("Lua callback failed: %1").arg(QString::fromUtf8(err ? err : "unknown")));
		lua_pop(m_state, 1);
		return false;
	}
	addScriptTimeForEngine(*this, timer.nsecsElapsed());

	if (lua_isstring(m_state, -1))
	{
		size_t outLen = 0;
		if (const char *out = lua_tolstring(m_state, -1, &outLen); out)
			arg = QByteArray(out, static_cast<int>(outLen));
		else
			arg.clear();
	}
	lua_pop(m_state, 1);
	return true;
#else
	Q_UNUSED(functionName);
	Q_UNUSED(arg);
	Q_UNUSED(hasFunction);
	return true;
#endif
}

bool LuaCallbackEngine::callFunctionWithStringInOut(const QString &functionName, QString &arg,
                                                    bool *hasFunction)
{
#ifdef QMUD_ENABLE_LUA_SCRIPTING
	if (hasFunction)
		*hasFunction = false;
	if (functionName.isEmpty())
		return true;
	if (!ensureState())
		return true;

	if (!pushLuaFunctionByName(m_state, functionName))
		return true;
	if (hasFunction)
		*hasFunction = true;

	const QByteArray in = arg.toUtf8();
	lua_pushlstring(m_state, in.constData(), in.size());

	QElapsedTimer timer;
	timer.start();
	ScriptExecutionDepthGuard depthGuard(this);
	if (lua_pcall(m_state, 1, 1, 0) != 0)
	{
		const char *err = lua_tostring(m_state, -1);
		reportLuaError(
		    *this, QStringLiteral("Lua callback failed: %1").arg(QString::fromUtf8(err ? err : "unknown")));
		lua_pop(m_state, 1);
		return false;
	}
	addScriptTimeForEngine(*this, timer.nsecsElapsed());

	if (lua_isstring(m_state, -1))
		arg = QString::fromUtf8(lua_tostring(m_state, -1));

	lua_pop(m_state, 1);
	return true;
#else
	Q_UNUSED(functionName);
	Q_UNUSED(arg);
	Q_UNUSED(hasFunction);
	return true;
#endif
}

bool LuaCallbackEngine::callFunctionWithNumberAndString(const QString &functionName, long arg1,
                                                        const QString &arg, bool *hasFunction,
                                                        bool defaultResult)
{
#ifdef QMUD_ENABLE_LUA_SCRIPTING
	if (hasFunction)
		*hasFunction = false;
	if (functionName.isEmpty())
		return defaultResult;
	if (!ensureState())
		return defaultResult;

	if (!pushLuaFunctionByName(m_state, functionName))
		return defaultResult;
	if (hasFunction)
		*hasFunction = true;

	lua_pushinteger(m_state, arg1);
	const QByteArray argBytes = arg.toUtf8();
	lua_pushlstring(m_state, argBytes.constData(), argBytes.size());

	QElapsedTimer timer;
	timer.start();
	ScriptExecutionDepthGuard depthGuard(this);
	if (lua_pcall(m_state, 2, 1, 0) != 0)
	{
		const char *err = lua_tostring(m_state, -1);
		reportLuaError(
		    *this, QStringLiteral("Lua callback failed: %1").arg(QString::fromUtf8(err ? err : "unknown")));
		lua_pop(m_state, 1);
		return defaultResult;
	}
	addScriptTimeForEngine(*this, timer.nsecsElapsed());

	bool result = defaultResult;
	if (lua_gettop(m_state) > 0)
	{
		if (lua_isboolean(m_state, -1))
			result = lua_toboolean(m_state, -1) != 0;
		else if (lua_isnumber(m_state, -1))
			result = lua_tonumber(m_state, -1) != 0.0;
	}
	lua_pop(m_state, 1);
	return result;
#else
	Q_UNUSED(functionName);
	Q_UNUSED(arg1);
	Q_UNUSED(arg);
	Q_UNUSED(hasFunction);
	Q_UNUSED(defaultResult);
	return defaultResult;
#endif
}

bool LuaCallbackEngine::callFunctionWithNumberAndStrings(const QString &functionName, long arg1,
                                                         const QString &arg2, const QString &arg3,
                                                         const QString &arg4, bool *hasFunction,
                                                         bool defaultResult)
{
#ifdef QMUD_ENABLE_LUA_SCRIPTING
	if (hasFunction)
		*hasFunction = false;
	if (functionName.isEmpty())
		return defaultResult;
	if (!ensureState())
		return defaultResult;

	if (!pushLuaFunctionByName(m_state, functionName))
		return defaultResult;
	if (hasFunction)
		*hasFunction = true;

	lua_pushinteger(m_state, arg1);
	const QByteArray arg2Bytes = arg2.toUtf8();
	lua_pushlstring(m_state, arg2Bytes.constData(), arg2Bytes.size());
	const QByteArray arg3Bytes = arg3.toUtf8();
	lua_pushlstring(m_state, arg3Bytes.constData(), arg3Bytes.size());
	const QByteArray arg4Bytes = arg4.toUtf8();
	lua_pushlstring(m_state, arg4Bytes.constData(), arg4Bytes.size());

	QElapsedTimer timer;
	timer.start();
	ScriptExecutionDepthGuard depthGuard(this);
	if (lua_pcall(m_state, 4, 1, 0) != 0)
	{
		const char *err = lua_tostring(m_state, -1);
		reportLuaError(
		    *this, QStringLiteral("Lua callback failed: %1").arg(QString::fromUtf8(err ? err : "unknown")));
		lua_pop(m_state, 1);
		return defaultResult;
	}
	addScriptTimeForEngine(*this, timer.nsecsElapsed());

	bool result = defaultResult;
	if (lua_gettop(m_state) > 0)
	{
		if (lua_isboolean(m_state, -1))
			result = lua_toboolean(m_state, -1) != 0;
		else if (lua_isnumber(m_state, -1))
			result = lua_tonumber(m_state, -1) != 0.0;
	}
	lua_pop(m_state, 1);
	return result;
#else
	Q_UNUSED(functionName);
	Q_UNUSED(arg1);
	Q_UNUSED(arg2);
	Q_UNUSED(arg3);
	Q_UNUSED(arg4);
	Q_UNUSED(hasFunction);
	Q_UNUSED(defaultResult);
	return defaultResult;
#endif
}

bool LuaCallbackEngine::callFunctionWithTwoNumbersAndString(const QString &functionName, long arg1, long arg2,
                                                            const QString &arg, bool *hasFunction,
                                                            bool defaultResult)
{
#ifdef QMUD_ENABLE_LUA_SCRIPTING
	if (hasFunction)
		*hasFunction = false;
	if (functionName.isEmpty())
		return defaultResult;
	if (!ensureState())
		return defaultResult;

	if (!pushLuaFunctionByName(m_state, functionName))
		return defaultResult;
	if (hasFunction)
		*hasFunction = true;

	lua_pushinteger(m_state, arg1);
	lua_pushinteger(m_state, arg2);
	const QByteArray argBytes = arg.toUtf8();
	lua_pushlstring(m_state, argBytes.constData(), argBytes.size());

	QElapsedTimer timer;
	timer.start();
	ScriptExecutionDepthGuard depthGuard(this);
	if (lua_pcall(m_state, 3, 1, 0) != 0)
	{
		const char *err = lua_tostring(m_state, -1);
		reportLuaError(
		    *this, QStringLiteral("Lua callback failed: %1").arg(QString::fromUtf8(err ? err : "unknown")));
		lua_pop(m_state, 1);
		return defaultResult;
	}
	addScriptTimeForEngine(*this, timer.nsecsElapsed());

	bool result = defaultResult;
	if (lua_gettop(m_state) > 0)
	{
		if (lua_isboolean(m_state, -1))
			result = lua_toboolean(m_state, -1) != 0;
		else if (lua_isnumber(m_state, -1))
			result = lua_tonumber(m_state, -1) != 0.0;
	}
	lua_pop(m_state, 1);
	return result;
#else
	Q_UNUSED(functionName);
	Q_UNUSED(arg1);
	Q_UNUSED(arg2);
	Q_UNUSED(arg);
	Q_UNUSED(hasFunction);
	Q_UNUSED(defaultResult);
	return defaultResult;
#endif
}

bool LuaCallbackEngine::callFunctionWithNumberAndBytes(const QString &functionName, long arg1,
                                                       const QByteArray &arg, bool *hasFunction,
                                                       bool defaultResult)
{
#ifdef QMUD_ENABLE_LUA_SCRIPTING
	if (hasFunction)
		*hasFunction = false;
	if (functionName.isEmpty())
		return defaultResult;
	if (!ensureState())
		return defaultResult;

	if (!pushLuaFunctionByName(m_state, functionName))
		return defaultResult;
	if (hasFunction)
		*hasFunction = true;

	lua_pushinteger(m_state, arg1);
	lua_pushlstring(m_state, arg.constData(), arg.size());

	QElapsedTimer timer;
	timer.start();
	ScriptExecutionDepthGuard depthGuard(this);
	if (lua_pcall(m_state, 2, 1, 0) != 0)
	{
		const char *err = lua_tostring(m_state, -1);
		reportLuaError(
		    *this, QStringLiteral("Lua callback failed: %1").arg(QString::fromUtf8(err ? err : "unknown")));
		lua_pop(m_state, 1);
		return defaultResult;
	}
	addScriptTimeForEngine(*this, timer.nsecsElapsed());

	bool result = defaultResult;
	if (lua_gettop(m_state) > 0)
	{
		if (lua_isboolean(m_state, -1))
			result = lua_toboolean(m_state, -1) != 0;
		else if (lua_isnumber(m_state, -1))
			result = lua_tonumber(m_state, -1) != 0.0;
	}
	lua_pop(m_state, 1);
	return result;
#else
	Q_UNUSED(functionName);
	Q_UNUSED(arg1);
	Q_UNUSED(arg);
	Q_UNUSED(hasFunction);
	Q_UNUSED(defaultResult);
	return defaultResult;
#endif
}

bool LuaCallbackEngine::callFunctionWithStringsAndWildcards(
    const QString &functionName, const QStringList &args, const QStringList &wildcards,
    const QMap<QString, QString> &namedWildcards, const QVector<LuaStyleRun> *styleRuns, bool *hasFunction)
{
#ifdef QMUD_ENABLE_LUA_SCRIPTING
	if (hasFunction)
		*hasFunction = false;
	if (functionName.isEmpty())
		return false;
	if (!ensureState())
		return false;

	if (!pushLuaFunctionByName(m_state, functionName))
		return false;
	if (hasFunction)
		*hasFunction = true;

	int paramCount = 0;
	for (const QString &arg : args)
	{
		const QByteArray argBytes = arg.toUtf8();
		lua_pushlstring(m_state, argBytes.constData(), argBytes.size());
		++paramCount;
	}

	if (!wildcards.isEmpty() || !namedWildcards.isEmpty())
	{
		lua_newtable(m_state);
		for (int i = 0; i < wildcards.size(); ++i)
		{
			const QByteArray wcBytes = wildcards.at(i).toUtf8();
			lua_pushlstring(m_state, wcBytes.constData(), wcBytes.size());
			lua_rawseti(m_state, -2, i);
		}
		for (auto it = namedWildcards.constBegin(); it != namedWildcards.constEnd(); ++it)
		{
			const QByteArray nameBytes  = it.key().toUtf8();
			const QByteArray valueBytes = it.value().toUtf8();
			lua_pushlstring(m_state, nameBytes.constData(), nameBytes.size());
			lua_pushlstring(m_state, valueBytes.constData(), valueBytes.size());
			lua_settable(m_state, -3);
		}
		++paramCount;
	}

	if (styleRuns)
	{
		lua_newtable(m_state);
		int index = 1;
		for (const auto &[text, textColour, backColour, style] : *styleRuns)
		{
			lua_newtable(m_state);
			const QByteArray textBytes = text.toUtf8();
			lua_pushstring(m_state, "text");
			lua_pushlstring(m_state, textBytes.constData(), textBytes.size());
			lua_settable(m_state, -3);
			lua_pushstring(m_state, "length");
			lua_pushnumber(m_state, static_cast<lua_Number>(text.size()));
			lua_settable(m_state, -3);
			lua_pushstring(m_state, "textcolour");
			lua_pushnumber(m_state, textColour);
			lua_settable(m_state, -3);
			lua_pushstring(m_state, "backcolour");
			lua_pushnumber(m_state, backColour);
			lua_settable(m_state, -3);
			lua_pushstring(m_state, "style");
			lua_pushnumber(m_state, style);
			lua_settable(m_state, -3);
			lua_rawseti(m_state, -2, index++);
		}
		++paramCount;
	}

	QElapsedTimer timer;
	timer.start();
	ScriptExecutionDepthGuard depthGuard(this);
	if (lua_pcall(m_state, paramCount, 0, 0) != 0)
	{
		const char *err = lua_tostring(m_state, -1);
		reportLuaError(
		    *this, QStringLiteral("Lua callback failed: %1").arg(QString::fromUtf8(err ? err : "unknown")));
		lua_pop(m_state, 1);
		return false;
	}
	addScriptTimeForEngine(*this, timer.nsecsElapsed());

	return true;
#else
	Q_UNUSED(functionName);
	Q_UNUSED(args);
	Q_UNUSED(wildcards);
	Q_UNUSED(namedWildcards);
	Q_UNUSED(styleRuns);
	Q_UNUSED(hasFunction);
	return false;
#endif
}

bool LuaCallbackEngine::executeScript(const QString &code, const QString &description,
                                      const QVector<LuaStyleRun> *styleRuns)
{
#ifdef QMUD_ENABLE_LUA_SCRIPTING
	if (code.trimmed().isEmpty())
		return true;
	if (!ensureState())
		return false;
	if (styleRuns)
	{
		lua_newtable(m_state);
		int index = 1;
		for (const auto &[text, textColour, backColour, style] : *styleRuns)
		{
			lua_newtable(m_state);
			const QByteArray textBytes = text.toUtf8();
			lua_pushstring(m_state, "text");
			lua_pushlstring(m_state, textBytes.constData(), textBytes.size());
			lua_settable(m_state, -3);
			lua_pushstring(m_state, "length");
			lua_pushnumber(m_state, static_cast<lua_Number>(text.size()));
			lua_settable(m_state, -3);
			lua_pushstring(m_state, "textcolour");
			lua_pushnumber(m_state, textColour);
			lua_settable(m_state, -3);
			lua_pushstring(m_state, "backcolour");
			lua_pushnumber(m_state, backColour);
			lua_settable(m_state, -3);
			lua_pushstring(m_state, "style");
			lua_pushnumber(m_state, style);
			lua_settable(m_state, -3);
			lua_rawseti(m_state, -2, index++);
		}
		lua_setglobal(m_state, "TriggerStyleRuns");
	}
	if (const QByteArray codeBytes = code.toUtf8(); luaL_loadstring(m_state, codeBytes.constData()) != 0)
	{
		const char *err = lua_tostring(m_state, -1);
		reportLuaError(*this, QStringLiteral("Lua script load error: %1 %2")
		                          .arg(description, QString::fromUtf8(err ? err : "unknown")));
		lua_pop(m_state, 1);
		return false;
	}
	QElapsedTimer timer;
	timer.start();
	unsigned short previousActionSource = WorldRuntime::eUnknownActionSource;
	bool           changedActionSource  = false;
	if (m_worldRuntime && m_worldRuntime->currentActionSource() == WorldRuntime::eUnknownActionSource)
	{
		previousActionSource = m_worldRuntime->currentActionSource();
		m_worldRuntime->setCurrentActionSource(WorldRuntime::eLuaSandbox);
		changedActionSource = true;
	}
	ScriptExecutionDepthGuard depthGuard(this);
	if (lua_pcall(m_state, 0, 0, 0) != 0)
	{
		const char *err = lua_tostring(m_state, -1);
		reportLuaError(*this, QStringLiteral("Lua script run error: %1 %2")
		                          .arg(description, QString::fromUtf8(err ? err : "unknown")));
		lua_pop(m_state, 1);
		if (changedActionSource)
			m_worldRuntime->setCurrentActionSource(previousActionSource);
		return false;
	}
	if (changedActionSource)
		m_worldRuntime->setCurrentActionSource(previousActionSource);
	addScriptTimeForEngine(*this, timer.nsecsElapsed());
	return true;
#else
	Q_UNUSED(code);
	Q_UNUSED(description);
	Q_UNUSED(styleRuns);
	return false;
#endif
}

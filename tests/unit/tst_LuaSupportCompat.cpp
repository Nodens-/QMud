/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_LuaSupportCompat.cpp
 * Role: Unit coverage for Lua 5.1 compatibility helpers used by Mushclient-style plugins.
 */

#include "LuaSupport.h"

#include <QtTest/QTest>

namespace
{
	/**
	 * @brief Executes a Lua chunk and returns the first result as UTF-8 string.
	 */
	struct LuaStringEvalResult
	{
			bool    ok{false};
			QString value;
			QString error;
	};

	/**
	 * @brief Creates a Lua state with standard libraries and Lua 5.1 compatibility shims.
	 */
	LuaStateOwner makeCompatLuaState()
	{
		LuaStateOwner state(QMudLuaSupport::makeLuaState());
		if (!state)
			return {};
		luaL_openlibs(state.get());
		QMudLuaSupport::applyLua51Compat(state.get());
		return state;
	}

	LuaStringEvalResult evaluateLuaToString(lua_State *L, const QByteArray &chunk)
	{
		LuaStringEvalResult result;
		if (!L)
		{
			result.error = QStringLiteral("Lua state is null.");
			return result;
		}
		if (luaL_loadbuffer(L, chunk.constData(), static_cast<size_t>(chunk.size()),
		                    "tst_LuaSupportCompat") != 0)
		{
			const char *err = lua_tostring(L, -1);
			result.error    = QString::fromUtf8(err ? err : "unknown load error");
			lua_pop(L, 1);
			return result;
		}
		if (lua_pcall(L, 0, 1, 0) != 0)
		{
			const char *err = lua_tostring(L, -1);
			result.error    = QString::fromUtf8(err ? err : "unknown runtime error");
			lua_pop(L, 1);
			return result;
		}
		if (!lua_isstring(L, -1))
		{
			result.error = QStringLiteral("Lua chunk did not return a string result.");
			lua_pop(L, 1);
			return result;
		}

		size_t      len   = 0;
		const char *bytes = lua_tolstring(L, -1, &len);
		result.ok         = true;
		result.value      = QString::fromUtf8(bytes ? bytes : "", static_cast<int>(len));
		lua_pop(L, 1);
		return result;
	}
} // namespace

/**
 * @brief QTest fixture covering Lua 5.1 compatibility shims.
 */
class tst_LuaSupportCompat : public QObject
{
		Q_OBJECT

		// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		void getfenvAndSetfenvWorkForFunctionClosures()
		{
			LuaStateOwner state = makeCompatLuaState();
			QVERIFY(state);

			const auto result = evaluateLuaToString(
			    state.get(),
			    QByteArrayLiteral("local f = assert(load(\"return test_value\"))\n"
			                      "setfenv(f, { test_value = \"compat\" })\n"
			                      "return tostring(getfenv(f).test_value) .. \":\" .. tostring(f())"));
			QVERIFY2(result.ok, qPrintable(result.error));
			QCOMPARE(result.value, QStringLiteral("compat:compat"));
		}

		void moduleShimCreatesAndUsesModuleEnvironment()
		{
			LuaStateOwner state = makeCompatLuaState();
			QVERIFY(state);

			const auto result = evaluateLuaToString(
			    state.get(), QByteArrayLiteral("module(\"compat_mod\")\n"
			                                   "value = 42\n"
			                                   "return tostring(compat_mod.value) .. \":\" .. "
			                                   "tostring(package.loaded.compat_mod.value) .. \":\" .. "
			                                   "tostring(value)"));
			QVERIFY2(result.ok, qPrintable(result.error));
			QCOMPARE(result.value, QStringLiteral("42:42:42"));
		}

		void unpackAliasUsesTableUnpackBehaviour()
		{
			LuaStateOwner state = makeCompatLuaState();
			QVERIFY(state);

			const auto result = evaluateLuaToString(
			    state.get(), QByteArrayLiteral("local a, b, c = unpack({1, 2, 3})\n"
			                                   "return string.format(\"%d,%d,%d\", a, b, c)"));
			QVERIFY2(result.ok, qPrintable(result.error));
			QCOMPARE(result.value, QStringLiteral("1,2,3"));
		}

		void loadstringAliasCompilesAndRunsChunks()
		{
			LuaStateOwner state = makeCompatLuaState();
			QVERIFY(state);

			const auto result = evaluateLuaToString(
			    state.get(), QByteArrayLiteral("local fn = loadstring(\"return 123\")\n"
			                                   "return tostring(type(fn)) .. \":\" .. tostring(fn())"));
			QVERIFY2(result.ok, qPrintable(result.error));
			QCOMPARE(result.value, QStringLiteral("function:123"));
		}

		void requireShimExportsSimpleModuleNamesToGlobals()
		{
			LuaStateOwner state = makeCompatLuaState();
			QVERIFY(state);

			const auto result = evaluateLuaToString(
			    state.get(), QByteArrayLiteral("package.preload[\"json\"] = function()\n"
			                                   "  return { decode = function() return \"ok\" end }\n"
			                                   "end\n"
			                                   "local m = require(\"json\")\n"
			                                   "return tostring(_G.json == m) .. \":\" .. "
			                                   "tostring(type(json.decode))"));
			QVERIFY2(result.ok, qPrintable(result.error));
			QCOMPARE(result.value, QStringLiteral("true:function"));
		}

		void requireShimDoesNotExportDottedModuleNames()
		{
			LuaStateOwner state = makeCompatLuaState();
			QVERIFY(state);

			const auto result = evaluateLuaToString(
			    state.get(), QByteArrayLiteral("package.preload[\"foo.bar\"] = function()\n"
			                                   "  return { value = 7 }\n"
			                                   "end\n"
			                                   "local m = require(\"foo.bar\")\n"
			                                   "return tostring(m.value) .. \":\" .. "
			                                   "tostring(rawget(_G, \"foo.bar\") == nil)"));
			QVERIFY2(result.ok, qPrintable(result.error));
			QCOMPARE(result.value, QStringLiteral("7:true"));
		}

		void stringFormatIntegerSpecifiersCoerceLua54NumbersLikeLua51()
		{
			LuaStateOwner state = makeCompatLuaState();
			QVERIFY(state);

			const auto result = evaluateLuaToString(
			    state.get(), QByteArrayLiteral("return string.format(\"%d|%i|%x\", 12.9, -3.2, 15.9)"));
			QVERIFY2(result.ok, qPrintable(result.error));
			QCOMPARE(result.value, QStringLiteral("12|-3|f"));
		}

		void stringFormatStillErrorsForNonNumericIntegerSpecifiers()
		{
			LuaStateOwner state = makeCompatLuaState();
			QVERIFY(state);

			const auto result =
			    evaluateLuaToString(state.get(), QByteArrayLiteral("return string.format(\"%d\", \"abc\")"));
			QVERIFY(!result.ok);
			QVERIFY(result.error.contains(QStringLiteral("number expected"), Qt::CaseInsensitive) ||
			        result.error.contains(QStringLiteral("bad argument"), Qt::CaseInsensitive));
		}

		void applyLua51CompatIsIdempotent()
		{
			LuaStateOwner state(QMudLuaSupport::makeLuaState());
			QVERIFY(state);
			luaL_openlibs(state.get());
			QMudLuaSupport::applyLua51Compat(state.get());
			QMudLuaSupport::applyLua51Compat(state.get());

			const auto result = evaluateLuaToString(
			    state.get(),
			    QByteArrayLiteral(
			        "local a = tostring(rawget(_G, \"__qmud_require_compat_wrapped\") == true)\n"
			        "local b = tostring(rawget(_G, \"__qmud_string_format_compat_wrapped\") == true)\n"
			        "local c = tostring(rawget(_G, \"__qmud_string_gsub_compat_wrapped\") == true)\n"
			        "local s = string.gsub(\"[x]\", \"%[x%]\", \"%[ok%]\")\n"
			        "return a .. \":\" .. b .. \":\" .. c .. \":\" .. s"));
			QVERIFY2(result.ok, qPrintable(result.error));
			QCOMPARE(result.value, QStringLiteral("true:true:true:[ok]"));
		}

		void stringGsubLenientReplacementMatchesMushclientBehaviour()
		{
			LuaStateOwner state = makeCompatLuaState();
			QVERIFY(state);

			const auto result = evaluateLuaToString(
			    state.get(),
			    QByteArrayLiteral("return (string.gsub(\"[CLAN Novice Adventurers] Friction: 'hax'\", "
			                      "\"%[CLAN Novice Adventurers%]\", \"%[Novice%]\"))"));
			QVERIFY2(result.ok, qPrintable(result.error));
			QCOMPARE(result.value, QStringLiteral("[Novice] Friction: 'hax'"));
		}

		void stringGsubCaptureReplacementStillWorks()
		{
			LuaStateOwner state = makeCompatLuaState();
			QVERIFY(state);

			const auto result = evaluateLuaToString(
			    state.get(),
			    QByteArrayLiteral("return (string.gsub(\"abc123\", \"(%a+)(%d+)\", \"%2-%1\"))"));
			QVERIFY2(result.ok, qPrintable(result.error));
			QCOMPARE(result.value, QStringLiteral("123-abc"));
		}

		void stringGsubPercentLiteralStillWorks()
		{
			LuaStateOwner state = makeCompatLuaState();
			QVERIFY(state);

			const auto result = evaluateLuaToString(
			    state.get(), QByteArrayLiteral("return (string.gsub(\"foo\", \"foo\", \"%%foo\"))"));
			QVERIFY2(result.ok, qPrintable(result.error));
			QCOMPARE(result.value, QStringLiteral("%foo"));
		}

		void socketHttpRequireShimRoutesHttpsThroughSslModuleWhenAvailable()
		{
			LuaStateOwner state = makeCompatLuaState();
			QVERIFY(state);

			const auto result = evaluateLuaToString(
			    state.get(), QByteArrayLiteral("package.preload[\"socket.http\"] = function()\n"
			                                   "  return {\n"
			                                   "    request = function(reqt, body)\n"
			                                   "      return \"plain:\" .. tostring(reqt), 480, { src = "
			                                   "\"plain\" }\n"
			                                   "    end\n"
			                                   "  }\n"
			                                   "end\n"
			                                   "package.preload[\"ssl.https\"] = function()\n"
			                                   "  return {\n"
			                                   "    request = function(reqt, body)\n"
			                                   "      if type(reqt) == \"table\" then\n"
			                                   "        if reqt.sink then reqt.sink(\"secure:\" .. "
			                                   "tostring(reqt.url)) end\n"
			                                   "        return 1, 200, { src = \"ssl\" }\n"
			                                   "      end\n"
			                                   "      return \"secure:\" .. tostring(reqt), 200, { src = "
			                                   "\"ssl\" }\n"
			                                   "    end\n"
			                                   "  }\n"
			                                   "end\n"
			                                   "local http = require(\"socket.http\")\n"
			                                   "local page, code, headers = "
			                                   "http.request(\"https://example.invalid/path\")\n"
			                                   "return tostring(page) .. \"|\" .. tostring(code) .. \"|\" "
			                                   ".. tostring(headers and headers.src)"));
			QVERIFY2(result.ok, qPrintable(result.error));
			QCOMPARE(result.value, QStringLiteral("secure:https://example.invalid/path|200|ssl"));
		}

		void socketHttpRequireShimPreservesHttpsPostBodySemantics()
		{
			LuaStateOwner state = makeCompatLuaState();
			QVERIFY(state);

			const auto result = evaluateLuaToString(
			    state.get(), QByteArrayLiteral("package.preload[\"socket.http\"] = function()\n"
			                                   "  return {\n"
			                                   "    request = function(reqt, body)\n"
			                                   "      return \"plain-fallback\", 599, { src = "
			                                   "\"plain\" }\n"
			                                   "    end\n"
			                                   "  }\n"
			                                   "end\n"
			                                   "package.preload[\"ltn12\"] = function()\n"
			                                   "  return {\n"
			                                   "    source = {\n"
			                                   "      string = function(value)\n"
			                                   "        local emitted = false\n"
			                                   "        return function()\n"
			                                   "          if emitted then return nil end\n"
			                                   "          emitted = true\n"
			                                   "          return value\n"
			                                   "        end\n"
			                                   "      end\n"
			                                   "    },\n"
			                                   "    sink = {\n"
			                                   "      table = function(target)\n"
			                                   "        return function(chunk)\n"
			                                   "          if chunk then target[#target + 1] = chunk "
			                                   "end\n"
			                                   "          return 1\n"
			                                   "        end\n"
			                                   "      end\n"
			                                   "    }\n"
			                                   "  }\n"
			                                   "end\n"
			                                   "package.preload[\"ssl.https\"] = function()\n"
			                                   "  return {\n"
			                                   "    request = function(req)\n"
			                                   "      local body = \"\"\n"
			                                   "      while true do\n"
			                                   "        local chunk = req.source and req.source()\n"
			                                   "        if chunk == nil then break end\n"
			                                   "        body = body .. chunk\n"
			                                   "      end\n"
			                                   "      if req.sink then req.sink(\"atlas-ok\") end\n"
			                                   "      return 1, 200, {\n"
			                                   "        method = req.method,\n"
			                                   "        ct = req.headers and "
			                                   "req.headers[\"content-type\"],\n"
			                                   "        cl = req.headers and "
			                                   "req.headers[\"content-length\"],\n"
			                                   "        body = body,\n"
			                                   "      }, \"HTTP/1.1 200 OK\"\n"
			                                   "    end\n"
			                                   "  }\n"
			                                   "end\n"
			                                   "local http = require(\"socket.http\")\n"
			                                   "local page, code, headers = "
			                                   "http.request(\"https://example.invalid/path\", "
			                                   "\"map=x\")\n"
			                                   "return tostring(page) .. \"|\" .. tostring(code) .. "
			                                   "\"|\" .. tostring(headers and headers.method) .. "
			                                   "\"|\" .. tostring(headers and headers.ct) .. \"|\" .. "
			                                   "tostring(headers and headers.cl) .. \"|\" .. "
			                                   "tostring(headers and headers.body)"));
			QVERIFY2(result.ok, qPrintable(result.error));
			QCOMPARE(result.value,
			         QStringLiteral("atlas-ok|200|POST|application/x-www-form-urlencoded|5|map=x"));
		}

		void socketHttpRequireShimFallsBackWhenSslModuleUnavailable()
		{
			LuaStateOwner state = makeCompatLuaState();
			QVERIFY(state);

			const auto result = evaluateLuaToString(
			    state.get(), QByteArrayLiteral("package.preload[\"socket.http\"] = function()\n"
			                                   "  return {\n"
			                                   "    request = function(reqt, body)\n"
			                                   "      return \"plain:\" .. tostring(reqt), 201, { src = "
			                                   "\"plain\" }\n"
			                                   "    end\n"
			                                   "  }\n"
			                                   "end\n"
			                                   "package.loaded[\"ssl.https\"] = nil\n"
			                                   "package.preload[\"ssl.https\"] = function()\n"
			                                   "  error(\"ssl disabled for test\")\n"
			                                   "end\n"
			                                   "local http = require(\"socket.http\")\n"
			                                   "local page, code, headers = "
			                                   "http.request(\"https://example.invalid/path\", \"map=x\")\n"
			                                   "return tostring(code) .. \"|\" .. "
			                                   "tostring(headers and headers.src)"));
			QVERIFY2(result.ok, qPrintable(result.error));
			QCOMPARE(result.value, QStringLiteral("201|plain"));
		}

		void socketHttpRequireShimFallsBackToRawWhenSslHttpsRejectsCreateFunction()
		{
			LuaStateOwner state = makeCompatLuaState();
			QVERIFY(state);

			const auto result = evaluateLuaToString(
			    state.get(), QByteArrayLiteral("package.preload[\"socket.http\"] = function()\n"
			                                   "  return {\n"
			                                   "    request = function(reqt, body)\n"
			                                   "      return \"raw:\" .. tostring(reqt), 206, { src = "
			                                   "\"raw\" }\n"
			                                   "    end\n"
			                                   "  }\n"
			                                   "end\n"
			                                   "package.preload[\"ssl.https\"] = function()\n"
			                                   "  return {\n"
			                                   "    request = function(reqt, body)\n"
			                                   "      return nil, \"create function not permitted\"\n"
			                                   "    end\n"
			                                   "  }\n"
			                                   "end\n"
			                                   "local http = require(\"socket.http\")\n"
			                                   "local page, code, headers = "
			                                   "http.request(\"https://example.invalid/path\", \"map=x\")\n"
			                                   "return tostring(page) .. \"|\" .. tostring(code) .. "
			                                   "\"|\" .. tostring(headers and headers.src)"));
			QVERIFY2(result.ok, qPrintable(result.error));
			QCOMPARE(result.value, QStringLiteral("raw:https://example.invalid/path|206|raw"));
		}

		void socketHttpRequireShimPatchesRealModulesForHttps()
		{
			LuaStateOwner state = makeCompatLuaState();
			QVERIFY(state);

			const auto result = evaluateLuaToString(
			    state.get(), QByteArrayLiteral("local ok_http, http = pcall(require, \"socket.http\")\n"
			                                   "if not ok_http then\n"
			                                   "  return \"no-http\"\n"
			                                   "end\n"
			                                   "local ok_https, https = pcall(require, \"ssl.https\")\n"
			                                   "if not ok_https then\n"
			                                   "  return \"no-https\"\n"
			                                   "end\n"
			                                   "if type(http) ~= \"table\" or type(http.request) ~= "
			                                   "\"function\" then\n"
			                                   "  return \"bad-http\"\n"
			                                   "end\n"
			                                   "if type(https) ~= \"table\" or type(https.request) ~= "
			                                   "\"function\" then\n"
			                                   "  return \"bad-https\"\n"
			                                   "end\n"
			                                   "local called = false\n"
			                                   "local original = https.request\n"
			                                   "https.request = function(reqt, body)\n"
			                                   "  called = true\n"
			                                   "  if type(reqt) == \"table\" then\n"
			                                   "    if reqt.sink then reqt.sink(\"shim-ok\") end\n"
			                                   "    return 1, 299, { src = \"ssl-real\" }\n"
			                                   "  end\n"
			                                   "  return \"shim-ok\", 299, { src = \"ssl-real\" }\n"
			                                   "end\n"
			                                   "local page, code, headers = "
			                                   "http.request(\"https://example.invalid/path\")\n"
			                                   "https.request = original\n"
			                                   "return tostring(called) .. \"|\" .. tostring(page) .. \"|\" "
			                                   ".. tostring(code) .. \"|\" .. tostring(headers and "
			                                   "headers.src)"));

			QVERIFY2(result.ok, qPrintable(result.error));
			if (result.value == QStringLiteral("no-http") || result.value == QStringLiteral("no-https"))
				QSKIP("System Lua modules socket.http/ssl.https not available in this environment");
			QCOMPARE(result.value, QStringLiteral("true|shim-ok|299|ssl-real"));
		}
		// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_APPLESS_MAIN(tst_LuaSupportCompat)

#if __has_include("tst_LuaSupportCompat.moc")
#include "tst_LuaSupportCompat.moc"
#endif

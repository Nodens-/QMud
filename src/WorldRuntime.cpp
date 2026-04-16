/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: WorldRuntime.cpp
 * Role: Primary world runtime engine implementation handling socket traffic, telnet/MXP processing, rendering,
 * automation, and Lua callbacks.
 */

#include "WorldRuntime.h"

#include "AnsiSgrParseUtils.h"
#include "AppController.h"
#include "Blending.h"
#include "ColorUtils.h"
#include "CommandMappingTypes.h"
#include "DoubleMetaphone.h"
#include "EncodingUtils.h"
#include "ErrorDescriptions.h"
#include "FontUtils.h"
#include "LogCompressionUtils.h"
#include "LuaCallbackEngine.h"
#include "LuaHeaders.h"
#include "MainFrame.h"
#include "MainFrameActionUtils.h"
#include "MiniWindowBrushUtils.h"
#include "MxpDiagnostics.h"
#include "NameGeneration.h"
#include "PluginCallbackCatalogUtils.h"
#include "ReloadUtils.h"
#include "SqliteCompat.h"
#include "StartTlsFallbackUtils.h"
#include "StringUtils.h"
#include "TimeFormatUtils.h"
#include "Version.h"
#include "WorldChildWindow.h"
#include "WorldCommandProcessor.h"
#include "WorldDocument.h"
#include "WorldOptionDefaults.h"
#include "WorldOptions.h"
#include "WorldRuntimeAttributeUtils.h"
#include "WorldSocket.h"
#include "WorldView.h"
#include "scripting/ScriptingErrors.h"

#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QColor>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHash>
#include <QHostInfo>
#include <QImageReader>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QMetaType>
#include <QNetworkInterface>
#include <QPainter>
#include <QPainterPath>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopeGuard>
#include <QScreen>
#include <QStack>
#include <QStringConverter>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTextBoundaryFinder>
#include <QTextStream>
#include <QThreadPool>
#include <QTimeZone>
#include <QTimer>
#include <QUdpSocket>
#include <QtSql/QSqlError>
#include <QtSql/QSqlRecord>
#if QMUD_ENABLE_SOUND
#include <QSoundEffect>
#include <QTemporaryFile>
#endif
#include <QPointer>
#include <QUrl>
#include <QtMath>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <exception>
#include <iterator>
#include <limits>
#include <memory>
#include <zlib.h>

static long        colorToLong(const QColor &color);
static QString     convertToRegularExpression(const QString &text);
static void        buildCustomColours(const QList<WorldRuntime::Colour> &colours, QVector<QColor> &normalAnsi,
                                      QVector<QColor> &customText, QVector<QColor> &customBack);
static QStringList macroDescriptionList();
static QStringList keypadNameList();

constexpr int      kChatLoopDiscardSeconds           = 5;
constexpr int      ADJUST_COLOUR_INVERT              = 1;
constexpr int      ADJUST_COLOUR_LIGHTER             = 2;
constexpr int      ADJUST_COLOUR_DARKER              = 3;
constexpr int      ADJUST_COLOUR_LESS_COLOUR         = 4;
constexpr int      ADJUST_COLOUR_MORE_COLOUR         = 5;
constexpr int      kPacketDebugChars                 = 16;
constexpr int      kMaxMxpTextBufferBytes            = 256 * 1024;
constexpr int      kMaxMxpStackDepth                 = 512;
constexpr int      kMemoryImageDecodeCacheMaxEntries = 48;
constexpr qint64   kMemoryImageDecodeCacheMaxBytes   = 64LL * 1024LL * 1024LL;

namespace
{
	bool isAsciiPrintableByte(const unsigned char c)
	{
		return c >= 0x20 && c < 0x7F;
	}

	bool isAsciiAlnumByte(const unsigned char c)
	{
		return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
	}

	int safeQSizeToInt(const qsizetype size)
	{
		if (size <= 0)
			return 0;
		constexpr qsizetype kMaxInt = std::numeric_limits<int>::max();
		return size > kMaxInt ? std::numeric_limits<int>::max() : static_cast<int>(size);
	}

	void flashTaskbarForView(QWidget *view)
	{
		if (const AppController *app = AppController::instance())
		{
			if (MainWindow *mainWindow = app->mainWindow())
			{
				(void)mainWindow->requestBackgroundTaskbarFlash(view);
				return;
			}
		}
		if (!view)
			return;
		QWidget *alertTarget = view->window();
		if (!alertTarget)
			alertTarget = view;
		QApplication::alert(alertTarget, 0);
	}

	int boundedQSizeToInt(const qsizetype size)
	{
		constexpr qsizetype kMaxInt = std::numeric_limits<int>::max();
		constexpr qsizetype kMinInt = std::numeric_limits<int>::min();
		if (size > kMaxInt)
			return std::numeric_limits<int>::max();
		if (size < kMinInt)
			return std::numeric_limits<int>::min();
		return static_cast<int>(size);
	}

	bool worldAttributeAffectsCommandProcessor(const QString &key)
	{
		return key.compare(QStringLiteral("speed_walk_delay"), Qt::CaseInsensitive) == 0 ||
		       key.compare(QStringLiteral("speed_walk_filler"), Qt::CaseInsensitive) == 0 ||
		       key.compare(QStringLiteral("translate_german"), Qt::CaseInsensitive) == 0 ||
		       key.compare(QStringLiteral("translate_backslash_sequences"), Qt::CaseInsensitive) == 0 ||
		       key.compare(QStringLiteral("enable_spam_prevention"), Qt::CaseInsensitive) == 0 ||
		       key.compare(QStringLiteral("spam_line_count"), Qt::CaseInsensitive) == 0 ||
		       key.compare(QStringLiteral("spam_message"), Qt::CaseInsensitive) == 0 ||
		       key.compare(QStringLiteral("do_not_translate_iac_to_iac_iac"), Qt::CaseInsensitive) == 0 ||
		       key.compare(QStringLiteral("regexp_match_empty"), Qt::CaseInsensitive) == 0 ||
		       key.compare(QStringLiteral("utf_8"), Qt::CaseInsensitive) == 0;
	}

	int safeQInt64ToInt(const qint64 value)
	{
		if (value <= 0)
			return 0;
		constexpr qint64 kMaxInt = std::numeric_limits<int>::max();
		return static_cast<int>(value > kMaxInt ? kMaxInt : value);
	}

	[[nodiscard]] bool textRectangleSettingsEqual(const WorldRuntime::TextRectangleSettings &lhs,
	                                              const WorldRuntime::TextRectangleSettings &rhs)
	{
		return lhs.left == rhs.left && lhs.top == rhs.top && lhs.right == rhs.right &&
		       lhs.bottom == rhs.bottom && lhs.borderOffset == rhs.borderOffset &&
		       lhs.borderColour == rhs.borderColour && lhs.borderWidth == rhs.borderWidth &&
		       lhs.outsideFillColour == rhs.outsideFillColour && lhs.outsideFillStyle == rhs.outsideFillStyle;
	}

	bool hasValidPluginId(const WorldRuntime::Plugin &plugin)
	{
		return !plugin.attributes.value(QStringLiteral("id")).trimmed().isEmpty();
	}

	bool canExecutePlugin(const WorldRuntime::Plugin &plugin)
	{
		return hasValidPluginId(plugin) && plugin.enabled && plugin.lua && !plugin.installPending;
	}

	long roundedToLong(const double value)
	{
		return std::lround(value);
	}

	int lowByteToInt(const long value)
	{
		return static_cast<int>(value & 0xFFL);
	}

	double longToDouble(const long value)
	{
		return static_cast<double>(value);
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

	void appendHexByte(QByteArray &out, const unsigned char c)
	{
		constexpr char kHex[] = "0123456789abcdef";
		out.append(kHex[(c >> 4) & 0x0F]);
		out.append(kHex[c & 0x0F]);
	}

	class ContextMenuDismissReplayFilter final : public QObject
	{
		public:
			explicit ContextMenuDismissReplayFilter(const QMenu *menu) : m_menu(menu)
			{
			}

			[[nodiscard]] bool hasReplayPoint() const
			{
				return m_hasReplayPoint;
			}

			[[nodiscard]] QPoint replayPoint() const
			{
				return m_replayPoint;
			}

			bool eventFilter(QObject *watched, QEvent *event) override
			{
				if (auto *watchedWidget = qobject_cast<QWidget *>(watched);
				    m_menu && watchedWidget &&
				    (watchedWidget == m_menu || m_menu->isAncestorOf(watchedWidget)))
					return false;

				if (event->type() == QEvent::MouseButtonPress)
				{
					if (auto *mouse = dynamic_cast<QMouseEvent *>(event);
					    mouse && mouse->button() == Qt::RightButton)
					{
						m_hasReplayPoint = true;
						m_replayPoint    = mouse->globalPosition().toPoint();
					}
				}
				else if (event->type() == QEvent::ContextMenu)
				{
					if (auto *contextEvent = dynamic_cast<QContextMenuEvent *>(event);
					    contextEvent && contextEvent->reason() == QContextMenuEvent::Mouse)
					{
						m_hasReplayPoint = true;
						m_replayPoint    = contextEvent->globalPos();
					}
				}
				return false;
			}

		private:
			const QMenu *m_menu{nullptr};
			bool         m_hasReplayPoint{false};
			QPoint       m_replayPoint;
	};
} // namespace

static double runtimeRandomUnit()
{
	if (AppController *app = AppController::instance())
		return app->nextRandomUnit();
	return QRandomGenerator::global()->generateDouble();
}

constexpr int kChatNameChange         = 1;
constexpr int kChatRequestConnections = 2;
constexpr int kChatConnectionList     = 3;
constexpr int kChatTextEverybody      = 4;
constexpr int kChatTextPersonal       = 5;
constexpr int kChatTextGroup          = 6;
constexpr int kChatMessage            = 7;
constexpr int kChatDoNotDisturb       = 8;
constexpr int kChatSendAction         = 9;
constexpr int kChatSendAlias          = 10;
constexpr int kChatSendMacro          = 11;
constexpr int kChatSendVariable       = 12;
constexpr int kChatSendEvent          = 13;
constexpr int kChatSendGag            = 14;
constexpr int kChatSendHighlight      = 15;
constexpr int kChatSendList           = 16;
constexpr int kChatSendArray          = 17;
constexpr int kChatSendBaritem        = 18;
constexpr int kChatVersion            = 19;
constexpr int kChatFileStart          = 20;
constexpr int kChatFileDeny           = 21;
constexpr int kChatFileBlockRequest   = 22;
constexpr int kChatFileBlock          = 23;
constexpr int kChatFileEnd            = 24;
constexpr int kChatFileCancel         = 25;
constexpr int kChatPingRequest        = 26;
constexpr int kChatPingResponse       = 27;
constexpr int kChatPeekConnections    = 28;

struct MxpSupportTag
{
		const char *name;
		const char *args;
};

static const MxpSupportTag *mxpSupportedTags()
{
	static const MxpSupportTag kSupportedTags[] = {
	    {"send",             "href,hint,prompt,xch_cmd,xch_hint"},
	    {"a",	            "href,xch_cmd,xch_hint"            },
	    {"bold",             ""	                             },
	    {"b",	            ""	                             },
	    {"strong",           ""	                             },
	    {"underline",        ""	                             },
	    {"u",	            ""	                             },
	    {"italic",           ""	                             },
	    {"i",	            ""	                             },
	    {"em",	           ""	                             },
	    {"color",            "fore,back"                        },
	    {"c",	            "fore,back"                        },
	    {"font",             "color,back,fgcolor,bgcolor"       },
	    {"high",             ""	                             },
	    {"h",	            ""	                             },
	    {"br",	           ""	                             },
	    {"p",	            ""	                             },
	    {"hr",	           ""	                             },
	    {"pre",              ""	                             },
	    {"ul",	           ""	                             },
	    {"ol",	           ""	                             },
	    {"li",	           ""	                             },
	    {"samp",             ""	                             },
	    {"reset",            ""	                             },
	    {"mxp",              "off"	                          },
	    {"version",          ""	                             },
	    {"support",          ""	                             },
	    {"option",           ""	                             },
	    {"recommend_option", ""	                             },
	    {"afk",              "challenge"                        },
	    {"user",             ""	                             },
	    {"username",         ""	                             },
	    {"password",         ""	                             },
	    {"pass",             ""	                             },
	    {"var",              ""	                             },
	    {"v",	            ""	                             },
	    {"body",             ""	                             },
	    {"head",             ""	                             },
	    {"html",             ""	                             },
	    {"title",            ""	                             },
	    {"img",              "src,xch_mode"                     },
	    {"xch_page",         ""	                             },
	    {nullptr,            nullptr	                        }
    };
	return kSupportedTags;
}

static int mxpBuiltinTagCount()
{
	int         count     = 0;
	const auto *supported = mxpSupportedTags();
	for (int i = 0; supported[i].name; ++i)
		count++;
	return count;
}

constexpr int     kChatPeekList  = 29;
constexpr int     kChatSnoop     = 30;
constexpr int     kChatSnoopData = 31;

constexpr int     kChatIcon          = 100;
constexpr int     kChatStatus        = 101;
constexpr int     kChatEmailAddress  = 102;
constexpr int     kChatRequestPgpKey = 103;
constexpr int     kChatPgpKey        = 104;
constexpr int     kChatSendCommand   = 105;
constexpr int     kChatStamp         = 106;

constexpr char    kChatEndOfCommand = static_cast<char>(0xFF);

constexpr int     kChatTypeMudMaster = 0;
constexpr int     kChatTypeZMud      = 1;

constexpr int     kChatStatusClosed                    = 0;
constexpr int     kChatStatusConnecting                = 1;
constexpr int     kChatStatusAwaitingConnectConfirm    = 2;
constexpr int     kChatStatusAwaitingConnectionRequest = 3;
constexpr int     kChatStatusConnected                 = 4;

constexpr int     kDefaultChatPort       = 4050;
constexpr int     kDefaultChatBlockSize  = 500;
constexpr int     kDefaultZChatBlockSize = 1024;

static QByteArray makeChatStamp(long stamp)
{
	QByteArray payload(4, '\0');
	payload[0] = static_cast<char>(stamp & 0xFF);
	payload[1] = static_cast<char>((stamp >> 8) & 0xFF);
	payload[2] = static_cast<char>((stamp >> 16) & 0xFF);
	payload[3] = static_cast<char>((stamp >> 24) & 0xFF);
	return payload;
}

static long extractChatStamp(QByteArray &payload)
{
	if (payload.size() < 4)
		return 0;
	const auto *data  = reinterpret_cast<const unsigned char *>(payload.constData());
	const long  stamp = static_cast<long>(data[0]) | (static_cast<long>(data[1]) << 8) |
	                   (static_cast<long>(data[2]) << 16) | (static_cast<long>(data[3]) << 24);
	payload = payload.mid(4);
	return stamp;
}

static quint32 sha1Word(const QByteArray &digest, int wordIndex)
{
	const int offset = wordIndex * 4;
	if (digest.size() < offset + 4)
		return 0;
	const auto *data = reinterpret_cast<const unsigned char *>(digest.constData() + offset);
	return (static_cast<quint32>(data[0]) << 24) | (static_cast<quint32>(data[1]) << 16) |
	       (static_cast<quint32>(data[2]) << 8) | static_cast<quint32>(data[3]);
}

static QString formatSha1Words(const QByteArray &digest)
{
	return QStringLiteral("%1 %2 %3 %4 %5")
	    .arg(sha1Word(digest, 0), 8, 16, QLatin1Char('0'))
	    .arg(sha1Word(digest, 1), 8, 16, QLatin1Char('0'))
	    .arg(sha1Word(digest, 2), 8, 16, QLatin1Char('0'))
	    .arg(sha1Word(digest, 3), 8, 16, QLatin1Char('0'))
	    .arg(sha1Word(digest, 4), 8, 16, QLatin1Char('0'));
}

static QByteArray ansiCode(int code)
{
	QByteArray payload("\x1b[");
	payload.append(QByteArray::number(code));
	payload.append('m');
	return payload;
}

namespace
{
	constexpr int kDbErrorIdNotFound            = -1;
	constexpr int kDbErrorNotOpen               = -2;
	constexpr int kDbErrorHavePreparedStatement = -3;
	constexpr int kDbErrorNoPreparedStatement   = -4;
	constexpr int kDbErrorNoValidRow            = -5;
	constexpr int kDbErrorDatabaseAlreadyExists = -6;
	constexpr int kDbErrorColumnOutOfRange      = -7;

	constexpr int kEdgeRaised   = 5;
	constexpr int kEdgeEtched   = 6;
	constexpr int kEdgeBump     = 9;
	constexpr int kEdgeSunken   = 10;
	constexpr int kAnsiBold     = 1;
	constexpr int kAnsiTextRed  = 31;
	constexpr int kAnsiTextCyan = 36;

	constexpr int kBfTopLeft = 0x0003;

} // namespace

class WorldRuntime::ChatConnection : public QObject
{
	public:
		friend class WorldRuntime;

		explicit ChatConnection(WorldRuntime *runtime, QTcpSocket *socket)
		    : QObject(runtime), m_runtime(runtime), m_socket(socket)
		{
			if (m_socket)
			{
				m_socket->setParent(this);
				connect(m_socket, &QTcpSocket::readyRead, this, &ChatConnection::onReadyRead);
				connect(m_socket, &QTcpSocket::connected, this, &ChatConnection::onConnected);
				connect(m_socket, &QTcpSocket::disconnected, this, &ChatConnection::onDisconnected);
				connect(m_socket, &QTcpSocket::bytesWritten, this, &ChatConnection::onBytesWritten);
				connect(m_socket, &QTcpSocket::errorOccurred, this, &ChatConnection::onError);
			}

			if (m_runtime)
			{
				if (++m_runtime->m_nextChatId > std::numeric_limits<long>::max())
					m_runtime->m_nextChatId = 1;
				m_chatId = m_runtime->m_nextChatId;
			}

			m_zChatStamp         = makeRandomStamp();
			m_zChatStatus        = 1;
			m_whenStarted        = QDateTime::currentDateTime();
			m_lastIncoming       = QDateTime();
			m_lastOutgoing       = QDateTime();
			m_chatStatus         = kChatStatusClosed;
			m_chatConnectionType = kChatTypeMudMaster;
			m_fileBlockSize      = kDefaultChatBlockSize;
		}

		~ChatConnection() override
		{
			stopFileTransfer(true);
			if (m_socket)
			{
				static_cast<void>(disconnect(m_socket, nullptr, this, nullptr));
				if (m_socket->state() != QAbstractSocket::UnconnectedState)
					m_socket->disconnectFromHost();
			}
			if (m_runtime && m_wasConnected)
			{
				m_runtime->callPluginCallbacksWithNumberAndString(
				    QStringLiteral("OnPluginChatUserDisconnect"), m_chatId, m_remoteUserName);
			}
		}

		[[nodiscard]] long id() const
		{
			return m_chatId;
		}
		[[nodiscard]] bool isConnected() const
		{
			return m_chatStatus == kChatStatusConnected;
		}
		[[nodiscard]] bool isIncoming() const
		{
			return m_incoming;
		}
		[[nodiscard]] const QString &allegedAddress() const
		{
			return m_allegedAddress;
		}
		[[nodiscard]] int allegedPort() const
		{
			return m_allegedPort;
		}

		void startOutgoing(const QString &server, int port, bool zChat)
		{
			if (!m_socket || !m_runtime)
				return;
			m_serverName         = server;
			m_chatConnectionType = zChat ? kChatTypeZMud : kChatTypeMudMaster;
			m_fileBlockSize      = zChat ? kDefaultZChatBlockSize : kDefaultChatBlockSize;
			m_chatStatus         = kChatStatusConnecting;

			if (port == 0)
				port = kDefaultChatPort;

			m_socket->connectToHost(server, static_cast<quint16>(port));
		}

		void startIncoming()
		{
			if (!m_socket)
				return;
			m_incoming   = true;
			m_chatStatus = kChatStatusAwaitingConnectionRequest;
			if (m_socket->peerAddress().protocol() == QAbstractSocket::IPv4Protocol)
				m_serverName = m_socket->peerAddress().toString();
			else
				m_serverName = m_socket->peerAddress().toString();
		}

		void sendChatMessage(int message, const QString &text, long stamp = 0)
		{
			if (!m_socket || m_deleteMe || m_chatStatus != kChatStatusConnected)
				return;

			if (!m_runtime->callPluginCallbacksStopOnFalseWithTwoNumbersAndString(
			        QStringLiteral("OnPluginChatMessageOut"), m_chatId, message, text))
				return;

			if (message == kChatSnoop && m_youAreSnooping)
				m_youAreSnooping = false;

			QByteArray payload;
			if (m_chatConnectionType == kChatTypeMudMaster)
			{
				payload.append(static_cast<char>(message & 0xFF));
				payload.append(text.toLatin1());
				if (message != kChatFileBlock)
					payload.replace(kChatEndOfCommand, 'y');
				payload.append(kChatEndOfCommand);
			}
			else
			{
				QByteArray messageBytes;
				if (message == kChatTextEverybody || message == kChatTextPersonal ||
				    message == kChatTextGroup)
					messageBytes = makeChatStamp(stamp ? stamp : m_zChatStamp) + text.toLatin1();
				else
					messageBytes = text.toLatin1();

				const int length = safeQSizeToInt(messageBytes.size());
				payload.append(static_cast<char>(message & 0xFF));
				payload.append(static_cast<char>((message >> 8) & 0xFF));
				payload.append(static_cast<char>(length & 0xFF));
				payload.append(static_cast<char>((length >> 8) & 0xFF));
				payload.append(messageBytes);
			}

			sendData(payload);
		}

		void stopFileTransfer(bool abort)
		{
			if (!m_doingFileTransfer)
				return;

			if (m_file)
			{
				m_file->close();
				m_file.reset();
			}
			m_fileBuffer.clear();

			if (abort && m_chatStatus == kChatStatusConnected)
				sendChatMessage(kChatFileCancel, QString());

			if (abort)
			{
				if (m_sendFile)
					m_runtime->chatNote(14, QStringLiteral("Aborted sending file %1").arg(m_ourFileName));
				else
				{
					m_runtime->chatNote(14, QStringLiteral("Aborted receiving file %1").arg(m_ourFileName));
					m_runtime->chatNote(14, QStringLiteral("File %1 deleted.").arg(m_ourFileName));
					QFile::remove(m_ourFileName);
				}
			}

			m_doingFileTransfer   = false;
			m_sendFile            = false;
			m_fileSize            = 0;
			m_fileBlocks          = 0;
			m_blocksTransferred   = 0;
			m_startedFileTransfer = QDateTime();
		}

		void sendData(const QByteArray &data)
		{
			if (m_deleteMe || !m_socket)
				return;
			m_lastOutgoing = QDateTime::currentDateTime();
			m_outstandingOutput.append(data);
			flushOutput();
		}

		void close()
		{
			if (m_chatStatus == kChatStatusConnected)
				m_runtime->chatNote(1, QStringLiteral("Chat session to %1 closed.").arg(m_remoteUserName));
			m_chatStatus = kChatStatusClosed;
			m_deleteMe   = true;
			if (m_socket)
				m_socket->disconnectFromHost();
		}

	private slots:
		void onReadyRead()
		{
			if (!m_socket)
				return;
			m_lastIncoming            = QDateTime::currentDateTime();
			const QByteArray incoming = m_socket->readAll();
			if (incoming.isEmpty())
				return;
			m_outstandingInput.append(incoming);
			processInput();
		}

		void onConnected()
		{
			if (!m_runtime)
				return;
			if (m_chatStatus != kChatStatusConnecting)
				return;

			m_serverAddress  = m_socket->peerAddress();
			m_serverPort     = m_socket->peerPort();
			m_allegedAddress = m_serverAddress.toString();
			m_allegedPort    = m_serverPort;

			if (m_runtime->isChatAlreadyConnected(m_allegedAddress, m_allegedPort, this))
			{
				m_runtime->chatNote(0, QStringLiteral("You are already connected to %1 port %2")
				                           .arg(m_allegedAddress)
				                           .arg(m_allegedPort));
				close();
				return;
			}

			m_runtime->chatNote(
			    0,
			    QStringLiteral("Calling chat server at %1 port %2").arg(m_allegedAddress).arg(m_allegedPort));

			m_runtime->chatNote(1, QStringLiteral("Session established to %1.").arg(m_serverName));

			const QString hostAddress  = chatLocalAddress();
			const int     incomingPort = m_runtime->chatIncomingPort();
			if (m_chatConnectionType == kChatTypeZMud)
				sendData(QStringLiteral("ZCHAT:%1\t\n%2%3")
				             .arg(m_runtime->chatOurName(), hostAddress,
				                  QStringLiteral("%1").arg(incomingPort, 5, 10, QLatin1Char('0')))
				             .toLatin1());
			else
				sendData(QStringLiteral("CHAT:%1\n%2%3")
				             .arg(m_runtime->chatOurName(), hostAddress,
				                  QStringLiteral("%1").arg(incomingPort, -5, 10, QLatin1Char(' ')))
				             .toLatin1());

			m_chatStatus = kChatStatusAwaitingConnectConfirm;
		}

		void onDisconnected()
		{
			close();
			if (m_runtime)
				m_runtime->removeChatConnection(this);
		}

		void onBytesWritten(qint64)
		{
			flushOutput();
		}

		void onError(QAbstractSocket::SocketError)
		{
			if (!m_socket || !m_runtime)
				return;
			if (m_chatStatus == kChatStatusConnecting || m_chatStatus == kChatStatusAwaitingConnectConfirm)
			{
				const QString error = m_socket->errorString();
				m_runtime->chatNote(0, QStringLiteral("Unable to connect to \"%1\", code = %2 (%3)")
				                           .arg(m_serverName)
				                           .arg(static_cast<int>(m_socket->error()))
				                           .arg(error));
				close();
			}
		}

	private:
		void flushOutput()
		{
			if (!m_socket || m_outstandingOutput.isEmpty())
				return;
			const qint64 sent = m_socket->write(m_outstandingOutput);
			if (sent > 0)
			{
				m_outstandingOutput = m_outstandingOutput.mid(static_cast<int>(sent));
			}
		}

		void processInput()
		{
			if (m_deleteMe || !m_runtime)
				return;

			if (m_chatStatus == kChatStatusAwaitingConnectConfirm)
			{
				if (m_outstandingInput.size() < 4)
					return;
				if (!m_outstandingInput.startsWith("YES:"))
				{
					m_runtime->chatNote(0, QStringLiteral("Server rejected chat session attempt."));
					close();
					return;
				}

				m_outstandingInput.remove(0, 4);
				const qsizetype newline = m_outstandingInput.indexOf('\n');
				if (newline == -1)
				{
					m_remoteUserName = QString::fromLatin1(m_outstandingInput);
					m_outstandingInput.clear();
				}
				else
				{
					m_remoteUserName = QString::fromLatin1(m_outstandingInput.left(newline));
					m_outstandingInput.remove(0, newline + 1);
				}

				m_runtime->chatNote(
				    1, QStringLiteral("Chat session accepted, remote server: \"%1\"").arg(m_remoteUserName));

				m_chatStatus   = kChatStatusConnected;
				m_wasConnected = true;
				sendChatMessage(kChatVersion, QStringLiteral("QMud v") + QString::fromLatin1(kVersionString));
				if (m_chatConnectionType == kChatTypeZMud)
				{
					sendChatMessage(kChatStatus, QStringLiteral("\x01"));
					sendChatMessage(kChatStamp, QString::fromLatin1(makeChatStamp(m_zChatStamp)));
				}
				m_runtime->callPluginCallbacksWithNumberAndString(QStringLiteral("OnPluginChatNewUser"),
				                                                  m_chatId, m_remoteUserName);
			}

			if (m_chatStatus == kChatStatusAwaitingConnectionRequest)
			{
				if (m_outstandingInput.size() < 7)
					return;
				const bool isChat  = m_outstandingInput.startsWith("CHAT:");
				const bool isZChat = m_outstandingInput.startsWith("ZCHAT:");
				if (!isChat && !isZChat)
				{
					m_runtime->chatNote(0, QStringLiteral("Unexpected chat negotiation."));
					sendData("NO");
					close();
					return;
				}

				if (isChat)
				{
					m_outstandingInput.remove(0, 5);
					m_chatConnectionType = kChatTypeMudMaster;
				}
				else
				{
					m_outstandingInput.remove(0, 6);
					m_chatConnectionType = kChatTypeZMud;
					m_fileBlockSize      = kDefaultZChatBlockSize;
				}

				const qsizetype newline = m_outstandingInput.indexOf('\n');
				if (newline == -1)
				{
					m_remoteUserName = QString::fromLatin1(m_outstandingInput);
					m_outstandingInput.clear();
				}
				else
				{
					m_remoteUserName = QString::fromLatin1(m_outstandingInput.left(newline));
					m_outstandingInput.remove(0, newline + 1);
				}

				const qsizetype tabIndex = m_remoteUserName.indexOf('\t');
				if (tabIndex != -1)
					m_remoteUserName = m_remoteUserName.left(tabIndex);

				QString         rest        = QString::fromLatin1(m_outstandingInput);
				const qsizetype restNewline = rest.indexOf('\n');
				if (restNewline != -1)
					rest = rest.left(restNewline);

				if (rest.size() > 5)
				{
					m_allegedPort    = rest.right(5).toInt();
					m_allegedAddress = rest.left(rest.size() - 5);
				}

				m_outstandingInput.clear();

				const QString acceptPayload =
				    QStringLiteral("%1,%2").arg(m_serverAddress.toString(), m_remoteUserName);
				if (!m_runtime->callPluginCallbacksStopOnFalse(QStringLiteral("OnPluginChatAccept"),
				                                               acceptPayload))
				{
					sendData("NO");
					close();
					return;
				}

				if (m_runtime->validateIncomingChatCalls())
				{
					const QString question =
					    QStringLiteral(
					        "Incoming chat call to world %1 from %2, IP address: %3.\n\nAccept it?")
					        .arg(m_runtime->worldName(), m_remoteUserName, m_serverAddress.toString());
					if (QMessageBox::question(nullptr, QStringLiteral("Chat"), question) != QMessageBox::Yes)
					{
						sendData("NO");
						close();
						return;
					}
				}

				m_runtime->chatNote(
				    1, QStringLiteral("Chat session accepted, remote user: \"%1\"").arg(m_remoteUserName));
				sendData(QStringLiteral("YES:%1\n").arg(m_runtime->chatOurName()).toLatin1());
				m_chatStatus   = kChatStatusConnected;
				m_wasConnected = true;
				sendChatMessage(kChatVersion,
				                QStringLiteral("MUSHclient v") + QString::fromLatin1(kVersionString));
				if (m_chatConnectionType == kChatTypeZMud)
				{
					sendChatMessage(kChatStatus, QStringLiteral("\x01"));
					sendChatMessage(kChatStamp, QString::fromLatin1(makeChatStamp(m_zChatStamp)));
				}

				m_runtime->callPluginCallbacksWithNumberAndString(QStringLiteral("OnPluginChatNewUser"),
				                                                  m_chatId, m_remoteUserName);
			}

			while (!m_deleteMe && !m_outstandingInput.isEmpty())
			{
				if (m_chatConnectionType == kChatTypeMudMaster)
				{
					if (static_cast<unsigned char>(m_outstandingInput.at(0)) ==
					    static_cast<unsigned char>(kChatFileBlock))
					{
						const int required = m_fileBlockSize + 2;
						if (m_outstandingInput.size() < required)
							return;
						const QByteArray block = m_outstandingInput.mid(1, m_fileBlockSize);
						m_outstandingInput.remove(0, required);
						processChatMessage(kChatFileBlock, QString::fromLatin1(block));
					}
					else
					{
						const qsizetype terminator = m_outstandingInput.indexOf(kChatEndOfCommand);
						if (terminator == -1)
							return;
						const QByteArray message = m_outstandingInput.left(terminator);
						m_outstandingInput.remove(0, terminator + 1);
						if (message.isEmpty())
							continue;
						const int code = static_cast<unsigned char>(message.at(0));
						processChatMessage(code, QString::fromLatin1(message.mid(1)));
					}
				}
				else
				{
					if (m_outstandingInput.size() < 4)
						return;
					const auto *data =
					    reinterpret_cast<const unsigned char *>(m_outstandingInput.constData());
					const int code   = data[0] | (data[1] << 8);
					const int length = data[2] | (data[3] << 8);
					if (m_outstandingInput.size() < length + 4)
						return;
					const QByteArray message = m_outstandingInput.mid(4, length);
					m_outstandingInput.remove(0, length + 4);
					processChatMessage(code, QString::fromLatin1(message));
				}
			}
		}

		void processChatMessage(int message, const QString &text)
		{
			if (m_deleteMe || m_chatStatus != kChatStatusConnected)
				return;

			if (!m_runtime->callPluginCallbacksStopOnFalseWithTwoNumbersAndString(
			        QStringLiteral("OnPluginChatMessage"), m_chatId, message, text))
				return;

			switch (message)
			{
			case kChatTextEverybody:
				processTextEverybody(text);
				break;
			case kChatTextPersonal:
				processTextPersonal(text);
				break;
			case kChatMessage:
				processMessage(text);
				break;
			case kChatTextGroup:
				processTextGroup(text);
				break;
			case kChatPingRequest:
				processPingRequest(text);
				break;
			case kChatPingResponse:
				processPingResponse(text);
				break;
			case kChatVersion:
				m_remoteVersion = text;
				break;
			case kChatRequestConnections:
				processRequestConnections();
				break;
			case kChatConnectionList:
				processConnectionList(text);
				break;
			case kChatPeekConnections:
				processPeekConnections();
				break;
			case kChatPeekList:
				processPeekList(text);
				break;
			case kChatSnoop:
				processSnoop(text);
				break;
			case kChatSnoopData:
				processSnoopData(text);
				break;
			case kChatNameChange:
				processNameChange(text);
				break;
			case kChatSendCommand:
				processSendCommand(text);
				break;
			case kChatFileStart:
				processFileStart(text);
				break;
			case kChatFileDeny:
				processFileDeny(text);
				break;
			case kChatFileBlockRequest:
				processFileBlockRequest();
				break;
			case kChatFileBlock:
				processFileBlock(text);
				break;
			case kChatFileEnd:
				processFileEnd();
				break;
			case kChatFileCancel:
				processFileCancel();
				break;
			case kChatIcon:
				break;
			case kChatStatus:
				if (!text.isEmpty())
					m_zChatStatus = static_cast<unsigned char>(text.at(0).toLatin1());
				break;
			case kChatEmailAddress:
				m_emailAddress = text;
				break;
			case kChatStamp:
				processStamp(text);
				break;
			case kChatRequestPgpKey:
				sendChatMessage(kChatMessage, QStringLiteral("Cannot send PGP key."));
				break;
			case kChatPgpKey:
				m_pgpKey = text;
				break;
			default:
				sendChatMessage(kChatMessage, QStringLiteral("\n%1 does not support the chat command %2.\n")
				                                  .arg(m_runtime->chatOurName())
				                                  .arg(message));
				m_runtime->chatNote(13, QStringLiteral("Received unsupported chat command %1 from %2")
				                            .arg(message)
				                            .arg(m_remoteUserName));
				break;
			}
		}

		void processNameChange(const QString &message)
		{
			const QString oldName = m_remoteUserName;
			m_remoteUserName      = message;
			m_runtime->chatNote(
			    2, QStringLiteral("%1 has changed his/her name to %2.").arg(oldName, m_remoteUserName));
		}

		void processRequestConnections()
		{
			QStringList parts;
			for (ChatConnection const *connection : m_runtime->chatConnections())
			{
				if (!connection->isConnected() || connection->m_private)
					continue;
				if (connection == this)
					continue;
				parts << connection->m_allegedAddress;
				parts << QString::number(connection->m_allegedPort);
			}

			m_runtime->chatNote(
			    13, QStringLiteral("%1 has requested your public connections").arg(m_remoteUserName));
			sendChatMessage(kChatConnectionList, parts.join(QLatin1Char(',')));
		}

		void processConnectionList(const QString &message) const
		{
			const QStringList parts = message.split(QLatin1Char(','), Qt::KeepEmptyParts);
			const int         count = safeQSizeToInt(parts.size() / 2);
			m_runtime->chatNote(11, QStringLiteral("Found %1 connection%2 to %3")
			                            .arg(count)
			                            .arg(count == 1 ? QString() : QStringLiteral("s"))
			                            .arg(m_remoteUserName));
			for (int i = 0; i < count; ++i)
			{
				const QString &address = parts.at(i * 2);
				const int      port    = parts.at(i * 2 + 1).toInt();
				m_runtime->chatCall(address, port, false);
			}
		}

		void processTextEverybody(const QString &message)
		{
			if (m_ignore)
				return;

			QByteArray raw   = message.toLatin1();
			long       stamp = 0;
			if (m_chatConnectionType == kChatTypeZMud)
				stamp = extractChatStamp(raw);
			const QString fixedMessage = QString::fromLatin1(raw);

			if (fixedMessage == m_runtime->m_lastChatMessageSent &&
			    m_runtime->m_lastChatMessageTime.isValid() &&
			    m_runtime->m_lastChatMessageTime.secsTo(QDateTime::currentDateTime()) <
			        kChatLoopDiscardSeconds)
				return;

			if (m_chatConnectionType == kChatTypeZMud && m_zChatStamp == stamp)
				return;

			m_countIncomingAll++;
			m_runtime->chatNote(5, fixedMessage);
			if (m_incoming)
				m_runtime->sendChatMessageToAll(kChatTextEverybody, fixedMessage, true, false, false,
				                                m_chatId, QString(), stamp);
			else
				m_runtime->sendChatMessageToAll(kChatTextEverybody, fixedMessage, true, true, false, m_chatId,
				                                QString(), stamp);
		}

		void processTextPersonal(const QString &message)
		{
			if (m_ignore)
				return;
			QByteArray raw   = message.toLatin1();
			long       stamp = 0;
			if (m_chatConnectionType == kChatTypeZMud)
				stamp = extractChatStamp(raw);
			const QString fixedMessage = QString::fromLatin1(raw);
			if (m_chatConnectionType == kChatTypeZMud && m_zChatStamp == stamp)
				return;
			m_countIncomingPersonal++;
			m_runtime->chatNote(4, fixedMessage);
		}

		void processTextGroup(const QString &message)
		{
			if (m_ignore)
				return;

			if (message == m_runtime->m_lastChatGroupMessageSent &&
			    m_runtime->m_lastChatGroupMessageTime.isValid() &&
			    m_runtime->m_lastChatGroupMessageTime.secsTo(QDateTime::currentDateTime()) <
			        kChatLoopDiscardSeconds)
				return;

			QByteArray raw   = message.toLatin1();
			long       stamp = 0;
			if (m_chatConnectionType == kChatTypeZMud)
				stamp = extractChatStamp(raw);
			if (m_chatConnectionType == kChatTypeZMud && m_zChatStamp == stamp)
				return;

			const QString fixedMessage = QString::fromLatin1(raw);
			if (fixedMessage.size() <= 15)
				return;

			m_countIncomingGroup++;
			const QString group = fixedMessage.left(15).trimmed();
			const QString body  = fixedMessage.mid(15);
			m_runtime->chatNote(6, body);
			if (!m_incoming)
				m_runtime->sendChatMessageToAll(kChatTextGroup, fixedMessage, true, true, false, m_chatId,
				                                group, stamp);
		}

		void processMessage(const QString &message)
		{
			if (m_ignore)
				return;
			m_countMessages++;
			m_runtime->chatNote(3, message);
		}

		void processPingRequest(const QString &message)
		{
			sendChatMessage(kChatPingResponse, message);
		}

		void processPingResponse(const QString &message)
		{
			if (m_pingTimer.isValid())
			{
				m_lastPingTime = static_cast<double>(m_pingTimer.elapsed()) / 1000.0;
				m_pingTimer.invalidate();
				m_runtime->chatNote(12, QStringLiteral("Ping time to %1: %2 seconds")
				                            .arg(m_remoteUserName)
				                            .arg(QString::number(m_lastPingTime, 'f', 3)));
			}
			else
			{
				m_runtime->chatNote(12, QStringLiteral("Ping response: %1").arg(message));
			}
		}

		void processPeekConnections()
		{
			QString result;
			for (ChatConnection const *connection : m_runtime->chatConnections())
			{
				if (!connection->isConnected() || connection->m_private)
					continue;
				if (connection == this)
					continue;
				if (m_chatConnectionType == kChatTypeZMud)
				{
					result += connection->m_allegedAddress;
					result += ",";
					result += QString::number(connection->m_allegedPort);
					result += ",";
				}
				else
				{
					result += connection->m_allegedAddress;
					result += "~";
					result += QString::number(connection->m_allegedPort);
					result += "~";
					result += connection->m_remoteUserName;
					result += "~";
				}
			}
			sendChatMessage(kChatPeekList, result);
			m_runtime->chatNote(13,
			                    QStringLiteral("%1 is peeking at your connections").arg(m_remoteUserName));
		}

		void processPeekList(const QString &message) const
		{
			if (m_chatConnectionType == kChatTypeZMud)
			{
				const QStringList parts = message.split(QLatin1Char(','), Qt::KeepEmptyParts);
				const int         count = safeQSizeToInt(parts.size() / 2);
				m_runtime->chatNote(10, QStringLiteral("Peek found %1 connection%2 to %3")
				                            .arg(count)
				                            .arg(count == 1 ? QString() : QStringLiteral("s"))
				                            .arg(m_remoteUserName));
				if (count > 0)
				{
					m_runtime->chatNote(10, QStringLiteral("Address                    Port"));
					m_runtime->chatNote(10, QStringLiteral("=========================  ====="));
				}
				for (int i = 0; i < count; ++i)
				{
					const QString &ip   = parts.at(i * 2);
					const QString &port = parts.at(i * 2 + 1);
					m_runtime->chatNote(10, QStringLiteral("%1 %2")
					                            .arg(ip.leftJustified(26, QLatin1Char(' ')))
					                            .arg(port.leftJustified(5, QLatin1Char(' '))));
				}
			}
			else
			{
				const QStringList parts = message.split(QLatin1Char('~'), Qt::KeepEmptyParts);
				const int         count = safeQSizeToInt(parts.size() / 3);
				m_runtime->chatNote(10, QStringLiteral("Peek found %1 connection%2 to %3")
				                            .arg(count)
				                            .arg(count == 1 ? QString() : QStringLiteral("s"))
				                            .arg(m_remoteUserName));
				if (count > 0)
				{
					m_runtime->chatNote(
					    10, QStringLiteral("Name                    Address                    Port"));
					m_runtime->chatNote(
					    10, QStringLiteral("======================  =========================  ====="));
				}
				for (int i = 0; i < count; ++i)
				{
					const QString &ip   = parts.at(i * 3);
					const QString &port = parts.at(i * 3 + 1);
					const QString &name = parts.at(i * 3 + 2);
					m_runtime->chatNote(10, QStringLiteral("%1 %2 %3")
					                            .arg(name.leftJustified(23, QLatin1Char(' ')))
					                            .arg(ip.leftJustified(26, QLatin1Char(' ')))
					                            .arg(port.leftJustified(5, QLatin1Char(' '))));
				}
			}
		}

		void processSnoop(const QString &)
		{
			if (m_heIsSnooping)
			{
				sendChatMessage(
				    kChatMessage,
				    QStringLiteral("\nYou are no longer snooping %1.\n").arg(m_runtime->chatOurName()));
				m_runtime->chatNote(13, QStringLiteral("%1 has stopped snooping you.").arg(m_remoteUserName));
				m_heIsSnooping = false;
				return;
			}

			if (m_canSnoop)
			{
				if (!m_runtime->autoAllowSnooping())
				{
					const QString question = QStringLiteral("%1 wishes to start snooping you.\n\nPermit it?")
					                             .arg(m_remoteUserName);
					if (QMessageBox::question(nullptr, QStringLiteral("Chat"), question) != QMessageBox::Yes)
					{
						sendChatMessage(kChatMessage,
						                QStringLiteral("\n%1 does not want you to snoop just now.\n")
						                    .arg(m_runtime->chatOurName()));
						return;
					}
				}
				sendChatMessage(kChatMessage,
				                QStringLiteral("\nYou are now snooping %1.\n").arg(m_runtime->chatOurName()));
				m_runtime->chatNote(13, QStringLiteral("%1 is now snooping you.").arg(m_remoteUserName));
				m_heIsSnooping = true;
			}
			else
			{
				sendChatMessage(kChatMessage, QStringLiteral("\n%1 has not given you permission to snoop.\n")
				                                  .arg(m_runtime->chatOurName()));
				m_runtime->chatNote(13, QStringLiteral("%1 attempted to snoop you.").arg(m_remoteUserName));
			}
		}

		void processSnoopData(const QString &message)
		{
			const bool oldNotesRgb = m_runtime->notesInRgb();
			const long oldFore     = m_runtime->noteColourFore();
			const long oldBack     = m_runtime->noteColourBack();

			m_runtime->setNoteColourFore(colorToLong(QColor(QStringLiteral("springgreen"))));
			m_runtime->setNoteColourBack(colorToLong(QColor(Qt::black)));

			const QString &body = message;
			if (body.size() >= 4 && body.left(4).trimmed().size() == 4 && body.left(4).toInt() >= 0)
			{
				const int fore = body.left(2).toInt();
				const int back = body.mid(2, 2).toInt();
				if (fore >= 0 && fore <= 15)
				{
					const QColor foreColour = m_runtime->ansiColour(fore > 7, (fore % 8) + 1);
					m_runtime->setNoteColourFore(colorToLong(foreColour));
					if (back >= 0 && back <= 7)
					{
						const QColor backColour = m_runtime->ansiColour(false, back + 1);
						m_runtime->setNoteColourBack(colorToLong(backColour));
					}
				}
				m_runtime->outputAnsiText(body.mid(4), true);
			}
			else
			{
				m_runtime->outputAnsiText(body, true);
			}

			if (oldNotesRgb)
			{
				m_runtime->setNoteColourFore(oldFore);
				m_runtime->setNoteColourBack(oldBack);
			}
			else
			{
				m_runtime->m_notesInRgb = false;
			}

			m_youAreSnooping = true;
		}

		void processSendCommand(const QString &command)
		{
			if (m_canSendCommands)
			{
				sendChatMessage(
				    kChatMessage,
				    QStringLiteral("\nYou command %1 to '%2'.\n").arg(m_runtime->chatOurName(), command));
				m_runtime->chatNote(
				    16, QStringLiteral("%1 commands you to '%2'.").arg(m_remoteUserName, command));
				static_cast<void>(m_runtime->executeCommand(command));
			}
			else
			{
				sendChatMessage(kChatMessage,
				                QStringLiteral("\n%1 has not given you permission to send commands.\n")
				                    .arg(m_runtime->chatOurName()));
				m_runtime->chatNote(
				    16, QStringLiteral("%1 attempted to send you a command.").arg(m_remoteUserName));
			}
		}

		void processFileStart(const QString &message)
		{
			if (!m_canSendFiles)
			{
				sendChatMessage(kChatFileDeny, QStringLiteral("%1 is not allowing file transfers from you.")
				                                   .arg(m_runtime->chatOurName()));
				return;
			}

			const QStringList parts = message.split(QLatin1Char(','), Qt::KeepEmptyParts);
			if (parts.size() != 2)
			{
				sendChatMessage(kChatFileDeny,
				                QStringLiteral("Expected \"filename,filesize\" but did not get that."));
				return;
			}

			m_senderFileName = parts.at(0);
			if (m_senderFileName.contains('/') || m_senderFileName.contains('\\'))
			{
				sendChatMessage(kChatFileDeny,
				                QStringLiteral("Supplied file name of \"%1\" may not contain slashes.")
				                    .arg(m_senderFileName));
				return;
			}

			bool ok    = false;
			m_fileSize = parts.at(1).toLong(&ok);
			if (!ok)
			{
				sendChatMessage(kChatFileDeny, QStringLiteral("File size was not numeric."));
				return;
			}

			bool         wanted = true;
			const double kb     = static_cast<double>(m_fileSize) / 1024.0;

			if (!m_runtime->autoAllowFiles())
			{
				const QString question =
				    QStringLiteral(
				        "%1 wishes to send you the file \"%2\", size %3 bytes (%4 Kb).\n\nAccept it?")
				        .arg(m_remoteUserName, m_senderFileName)
				        .arg(m_fileSize)
				        .arg(kb, 0, 'f', 1);
				if (QMessageBox::question(nullptr, QStringLiteral("Chat"), question) != QMessageBox::Yes)
					wanted = false;
			}

			if (wanted)
			{
				const QString saveDir = m_runtime->chatSaveDirectory();
				if (saveDir.isEmpty() || !m_runtime->autoAllowFiles())
				{
					const QString title =
					    QStringLiteral("Chat: Save file from %1 as ...").arg(m_remoteUserName);
					const QString path = QFileDialog::getSaveFileName(
					    nullptr, title,
					    saveDir.isEmpty() ? m_senderFileName : saveDir + "/" + m_senderFileName,
					    QStringLiteral("All files (*.*)"));
					if (path.isEmpty())
						wanted = false;
					else
						m_ourFileName = path;
				}
				else
				{
					QString dir = saveDir;
					dir.replace("\\", "/");
					if (!dir.endsWith('/'))
						dir += "/";
					m_ourFileName = dir + m_senderFileName;
				}
			}

			if (!wanted)
			{
				sendChatMessage(
				    kChatFileDeny,
				    QStringLiteral("%1 does not want that particular file.").arg(m_runtime->chatOurName()));
				return;
			}

			m_file = std::make_unique<QFile>(m_ourFileName);
			if (!m_file->open(QIODevice::WriteOnly))
			{
				sendChatMessage(kChatFileDeny,
				                QStringLiteral("%1 can not open that file.").arg(m_runtime->chatOurName()));
				m_runtime->chatNote(14, QStringLiteral("File %1 cannot be opened.").arg(m_ourFileName));
				m_file.reset();
				m_ourFileName.clear();
				m_fileSize = 0;
				return;
			}

			sendChatMessage(kChatFileBlockRequest, QString());
			m_runtime->chatNote(
			    14, QStringLiteral("Receiving a file from %1 -- Filename: %2, Length: %3 bytes (%4 Kb).")
			            .arg(m_remoteUserName, m_ourFileName)
			            .arg(m_fileSize)
			            .arg(kb, 0, 'f', 1));

			m_startedFileTransfer = QDateTime::currentDateTime();
			m_sendFile            = false;
			m_doingFileTransfer   = true;
			m_blocksTransferred   = 0;
			m_fileBlocks          = (m_fileSize + m_fileBlockSize - 1) / m_fileBlockSize;
			m_fileSha1.reset();
		}

		void processFileDeny(const QString &message)
		{
			m_runtime->chatNote(14, message);
			stopFileTransfer(true);
		}

		void processFileBlockRequest()
		{
			if (!m_doingFileTransfer)
				return;
			if (!m_sendFile)
			{
				sendChatMessage(kChatMessage, QStringLiteral("We are supposed to be receiving a file."));
				return;
			}

			if (!m_file)
				return;

			if (m_fileBuffer.size() != m_fileBlockSize)
				m_fileBuffer.resize(m_fileBlockSize);
			m_fileBuffer.fill('\0');

			const qint64 bytesRead = m_file->read(m_fileBuffer.data(), m_fileBlockSize);
			m_blocksTransferred++;

			qint64 expected = m_fileBlockSize;
			if (m_blocksTransferred == m_fileBlocks)
				expected = m_fileSize % m_fileBlockSize;
			if (expected == 0)
				expected = m_fileBlockSize;

			if (bytesRead != expected)
			{
				sendChatMessage(kChatMessage,
				                QStringLiteral("File transfer aborted due to read error by sender."));
				m_runtime->chatNote(
				    14, QStringLiteral("Send of file \"%1\" aborted due to read error.").arg(m_ourFileName));
				stopFileTransfer(true);
				return;
			}

			m_fileSha1.addData(QByteArrayView(m_fileBuffer.constData(), safeQInt64ToInt(bytesRead)));

			sendChatMessage(kChatFileBlock, QString::fromLatin1(m_fileBuffer));
			m_countFileBytesOut += static_cast<long>(bytesRead);

			if (m_blocksTransferred >= m_fileBlocks)
			{
				sendChatMessage(kChatFileEnd, QString());
				m_runtime->chatNote(14, QStringLiteral("Send of file \"%1\" complete.").arg(m_ourFileName));
				const QString sumText = formatSha1Words(m_fileSha1.result());
				sendChatMessage(kChatMessage, QStringLiteral("Sumcheck from sender was: %1").arg(sumText));
				m_runtime->chatNote(14, QStringLiteral("Sumcheck we calculated:   %1").arg(sumText));
				stopFileTransfer(false);
			}
		}

		void processFileBlock(const QString &message)
		{
			if (!m_doingFileTransfer)
				return;
			if (m_sendFile)
			{
				sendChatMessage(kChatMessage, QStringLiteral("We are supposed to be sending a file."));
				return;
			}

			if (!m_file)
				return;

			m_blocksTransferred++;
			qint64 expected = m_fileBlockSize;
			if (m_blocksTransferred == m_fileBlocks)
				expected = m_fileSize % m_fileBlockSize;
			if (expected == 0)
				expected = m_fileBlockSize;

			const QByteArray buffer = message.toLatin1();
			if (m_file->write(buffer.constData(), expected) != expected)
			{
				sendChatMessage(kChatMessage,
				                QStringLiteral("File transfer aborted due to write error by receiver."));
				m_runtime->chatNote(
				    14,
				    QStringLiteral("Receive of file \"%1\" aborted due to write error.").arg(m_ourFileName));
				stopFileTransfer(true);
				return;
			}

			m_fileSha1.addData(QByteArrayView(buffer.constData(), safeQInt64ToInt(expected)));
			m_countFileBytesIn += static_cast<long>(expected);

			sendChatMessage(kChatFileBlockRequest, QString());
			if (m_blocksTransferred >= m_fileBlocks)
			{
				m_runtime->chatNote(14,
				                    QStringLiteral("Receive of file \"%1\" complete.").arg(m_ourFileName));
				const QString sumText = formatSha1Words(m_fileSha1.result());
				m_runtime->chatNote(14, QStringLiteral("Sumcheck as written was:  %1").arg(sumText));
				sendChatMessage(kChatMessage, QStringLiteral("Sumcheck as received was: %1").arg(sumText));
				stopFileTransfer(false);
			}
		}

		void processFileEnd()
		{
			if (!m_doingFileTransfer)
				return;
			if (m_fileBlocks != m_blocksTransferred)
			{
				m_runtime->chatNote(
				    14, QStringLiteral("Transfer of file \"%1\" stopped prematurely.").arg(m_ourFileName));
				stopFileTransfer(true);
			}
			else
				stopFileTransfer(false);
		}

		void processFileCancel()
		{
			stopFileTransfer(true);
		}

		void processStamp(const QString &message)
		{
			if (m_chatConnectionType != kChatTypeZMud)
				return;
			if (message.size() != 4)
				return;
			QByteArray payload = message.toLatin1();
			long const stamp   = extractChatStamp(payload);
			if (stamp == m_zChatStamp)
			{
				m_zChatStamp = makeRandomStamp();
				for (ChatConnection *connection : m_runtime->chatConnections())
				{
					if (!connection->isConnected() || connection->m_chatConnectionType != kChatTypeZMud)
						continue;
					connection->sendChatMessage(kChatStamp, QString::fromLatin1(makeChatStamp(m_zChatStamp)));
				}
			}
		}

		long makeRandomStamp()
		{
			const QString seed = QStringLiteral("%1 %2 %3 %4 %5")
			                         .arg(m_runtime ? m_runtime->worldName() : QString())
			                         .arg(QRandomGenerator::global()->generate())
			                         .arg(QCoreApplication::applicationPid())
			                         .arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODate))
			                         .arg(reinterpret_cast<quintptr>(this));
			const QByteArray seedBytes = seed.toLatin1();
			const QByteArray digest    = QCryptographicHash::hash(seedBytes, QCryptographicHash::Sha1);
			return sha1Word(digest, 0);
		}

		WorldRuntime          *m_runtime{nullptr};
		QTcpSocket            *m_socket{nullptr};
		QByteArray             m_outstandingOutput;
		QByteArray             m_outstandingInput;
		QString                m_serverName;
		QHostAddress           m_serverAddress;
		int                    m_serverPort{0};
		QString                m_remoteUserName;
		QString                m_group;
		QString                m_remoteVersion;
		QString                m_allegedAddress{QStringLiteral("<Unknown>")};
		int                    m_allegedPort{kDefaultChatPort};
		int                    m_chatStatus{kChatStatusClosed};
		int                    m_chatConnectionType{kChatTypeMudMaster};
		bool                   m_deleteMe{false};
		long                   m_chatId{0};
		QDateTime              m_whenStarted;
		QDateTime              m_lastIncoming;
		QDateTime              m_lastOutgoing;
		bool                   m_incoming{false};
		bool                   m_ignore{false};
		bool                   m_canSnoop{false};
		bool                   m_youAreSnooping{false};
		bool                   m_heIsSnooping{false};
		bool                   m_canSendCommands{false};
		bool                   m_private{false};
		bool                   m_canSendFiles{false};
		bool                   m_wasConnected{false};
		QElapsedTimer          m_pingTimer;
		double                 m_lastPingTime{0.0};
		long                   m_zChatStamp{0};
		QString                m_emailAddress;
		QString                m_pgpKey;
		short                  m_zChatStatus{1};
		long                   m_userOption{0};

		bool                   m_doingFileTransfer{false};
		bool                   m_sendFile{false};
		QString                m_senderFileName;
		QString                m_ourFileName;
		long                   m_fileSize{0};
		long                   m_fileBlocks{0};
		long                   m_blocksTransferred{0};
		std::unique_ptr<QFile> m_file;
		QDateTime              m_startedFileTransfer;
		int                    m_fileBlockSize{kDefaultChatBlockSize};
		QByteArray             m_fileBuffer;
		QCryptographicHash     m_fileSha1{QCryptographicHash::Sha1};

		long                   m_countIncomingPersonal{0};
		long                   m_countIncomingAll{0};
		long                   m_countIncomingGroup{0};
		long                   m_countOutgoingPersonal{0};
		long                   m_countOutgoingAll{0};
		long                   m_countOutgoingGroup{0};
		long                   m_countMessages{0};
		long                   m_countFileBytesIn{0};
		long                   m_countFileBytesOut{0};
};

namespace
{
	constexpr int kBfTopRight    = 0x0006;
	constexpr int kBfBottomLeft  = 0x0009;
	constexpr int kBfBottomRight = 0x000C;
	constexpr int kBfRect        = 0x000F;
	constexpr int kBfMiddle      = 0x0800;

	constexpr int kPsStyleMask  = 0x0000000F;
	constexpr int kPsEndCapMask = 0x00000F00;
	constexpr int kPsJoinMask   = 0x0000F000;

	constexpr int kPsSolid       = 0;
	constexpr int kPsDash        = 1;
	constexpr int kPsDot         = 2;
	constexpr int kPsDashDot     = 3;
	constexpr int kPsDashDotDot  = 4;
	constexpr int kPsNull        = 5;
	constexpr int kPsInsideFrame = 6;

	constexpr int kPsEndCapRound  = 0x00000000;
	constexpr int kPsEndCapSquare = 0x00000100;
	constexpr int kPsEndCapFlat   = 0x00000200;

	constexpr int kPsJoinRound = 0x00000000;
	constexpr int kPsJoinBevel = 0x00001000;
	constexpr int kPsJoinMiter = 0x00002000;

	QColor        colorFromRef(long value)
	{
		return MiniWindowBrushUtils::colorFromRef(value);
	}

	long colorToRef(const QColor &color)
	{
		return static_cast<long>(color.red()) | (static_cast<long>(color.green()) << 8) |
		       (static_cast<long>(color.blue()) << 16);
	}

	Qt::PenStyle mapPenStyle(long style)
	{
		switch (static_cast<int>(style & 0xFF))
		{
		case 1:
			return Qt::DashLine;
		case 2:
			return Qt::DotLine;
		case 3:
			return Qt::DashDotLine;
		case 4:
			return Qt::DashDotDotLine;
		case 5:
			return Qt::NoPen;
		default:
			return Qt::SolidLine;
		}
	}

	Qt::PenCapStyle mapPenCap(long style)
	{
		if (style & 0x00000100)
			return Qt::SquareCap;
		if (style & 0x00000200)
			return Qt::FlatCap;
		return Qt::RoundCap;
	}

	Qt::PenJoinStyle mapPenJoin(long style)
	{
		if (style & 0x00001000)
			return Qt::BevelJoin;
		if (style & 0x00002000)
			return Qt::MiterJoin;
		return Qt::RoundJoin;
	}

	unsigned char clampByte(long value)
	{
		if (value < 0)
			return 0;
		if (value > 255)
			return 255;
		return static_cast<unsigned char>(value);
	}

	int validatePenStyle(long penStyle, int penWidth)
	{
		switch (penStyle & kPsStyleMask)
		{
		case kPsSolid:
		case kPsNull:
		case kPsInsideFrame:
			break;
		case kPsDash:
		case kPsDot:
		case kPsDashDot:
		case kPsDashDotDot:
			if (penWidth > 1)
				return ePenStyleNotValid;
			break;
		default:
			return ePenStyleNotValid;
		}

		switch (penStyle & kPsEndCapMask)
		{
		case kPsEndCapRound:
		case kPsEndCapSquare:
		case kPsEndCapFlat:
			break;
		default:
			return ePenStyleNotValid;
		}

		switch (penStyle & kPsJoinMask)
		{
		case kPsJoinRound:
		case kPsJoinBevel:
		case kPsJoinMiter:
			break;
		default:
			return ePenStyleNotValid;
		}

		return eOK;
	}

	int validateBrushStyle(long brushStyle)
	{
		switch (brushStyle)
		{
		case 0:
		case 1:
		case 2:
		case 3:
		case 4:
		case 5:
		case 6:
		case 7:
		case 8:
		case 9:
		case 10:
		case 11:
		case 12:
			return eOK;
		default:
			return eBrushStyleNotValid;
		}
	}

	int bytesPerLine(int width, int bitsPerPixel)
	{
		return ((width * bitsPerPixel + 31) / 32) * 4;
	}

	int bytesPerLine24(int width)
	{
		return bytesPerLine(width, 24);
	}

	void imageToBgrBuffer(const QImage &image, QByteArray &buffer, int width, int height, int bpl)
	{
		buffer.resize(bpl * height);
		buffer.fill(0);
		for (int y = 0; y < height; ++y)
		{
			auto *line = reinterpret_cast<unsigned char *>(buffer.data() + y * bpl);
			for (int x = 0; x < width; ++x)
			{
				const QRgb pixel  = image.pixel(x, y);
				const int  offset = x * 3;
				line[offset]      = static_cast<unsigned char>(qBlue(pixel));
				line[offset + 1]  = static_cast<unsigned char>(qGreen(pixel));
				line[offset + 2]  = static_cast<unsigned char>(qRed(pixel));
			}
		}
	}

	void bgrBufferToImage(const QByteArray &buffer, QImage &image, int width, int height, int bpl)
	{
		for (int y = 0; y < height; ++y)
		{
			const auto *line = reinterpret_cast<const unsigned char *>(buffer.constData() + y * bpl);
			for (int x = 0; x < width; ++x)
			{
				const int offset = x * 3;
				const int b      = line[offset];
				const int g      = line[offset + 1];
				const int r      = line[offset + 2];
				image.setPixel(x, y, qRgba(r, g, b, 0xFF));
			}
		}
	}

	void noise(unsigned char *buffer, long width, long height, long bpl, double options)
	{
		Q_UNUSED(width);
		const long   count     = bpl * height;
		const double threshold = options / 100.0;
		for (long i = 0; i < count; ++i)
		{
			long c = buffer[i];
			c += roundedToLong((128.0 - runtimeRandomUnit() * 256.0) * threshold);
			buffer[i] = clampByte(c);
		}
	}

	void monoNoise(unsigned char *buffer, long width, long height, long bpl, double options)
	{
		Q_UNUSED(width);
		const double threshold = options / 100.0;
		for (long row = 0; row < height; ++row)
		{
			unsigned char *line = buffer + row * bpl;
			for (long i = 0; i < width; ++i)
			{
				const long j = roundedToLong((128.0 - runtimeRandomUnit() * 256.0) * threshold);
				for (int channel = 0; channel < 3; ++channel)
				{
					const long c  = line[channel] + j;
					line[channel] = clampByte(c);
				}
				line += 3;
			}
		}
	}

	void brightness(unsigned char *buffer, long width, long height, long bpl, double options)
	{
		Q_UNUSED(width);
		const long count = bpl * height;
		for (long i = 0; i < count; ++i)
		{
			long c = buffer[i];
			c += roundedToLong(options);
			buffer[i] = clampByte(c);
		}
	}

	void contrast(unsigned char *buffer, long width, long height, long bpl, double options)
	{
		Q_UNUSED(width);
		const long count = bpl * height;
		for (long i = 0; i < count; ++i)
		{
			double c = buffer[i] - 128;
			c *= options;
			buffer[i] = clampByte(static_cast<long>(c + 128));
		}
	}

	void gammaAdjust(unsigned char *buffer, long width, long height, long bpl, double options)
	{
		Q_UNUSED(width);
		if (options < 0.0)
			options = 0.0;

		const long count = bpl * height;
		for (long i = 0; i < count; ++i)
		{
			double c  = static_cast<double>(buffer[i]) / 255.0;
			c         = qPow(c, options);
			buffer[i] = clampByte(static_cast<long>(c * 255));
		}
	}

	void colourBrightness(unsigned char *buffer, long width, long height, long bpl, double options,
	                      long channel)
	{
		for (long row = 0; row < height; ++row)
		{
			unsigned char *line = buffer + row * bpl + channel;
			for (long i = 0; i < width; ++i)
			{
				long c = *line;
				c += roundedToLong(options);
				*line = clampByte(c);
				line += 3;
			}
		}
	}

	void colourBrightnessMultiply(unsigned char *buffer, long width, long height, long bpl, double options,
	                              long channel)
	{
		for (long row = 0; row < height; ++row)
		{
			unsigned char *line = buffer + row * bpl + channel;
			for (long i = 0; i < width; ++i)
			{
				long c = *line;
				c      = roundedToLong(static_cast<double>(c) * options);
				*line  = clampByte(c);
				line += 3;
			}
		}
	}

	void colourContrast(unsigned char *buffer, long width, long height, long bpl, double options,
	                    long channel)
	{
		unsigned char lookup[256];
		for (int i = 0; i < 256; ++i)
		{
			double c = i - 128;
			c *= options;
			lookup[i] = clampByte(static_cast<long>(c + 128));
		}

		for (long row = 0; row < height; ++row)
		{
			unsigned char *line = buffer + row * bpl + channel;
			for (long i = 0; i < width; ++i)
			{
				*line = lookup[*line];
				line += 3;
			}
		}
	}

	void colourGamma(unsigned char *buffer, long width, long height, long bpl, double options, long channel)
	{
		if (options < 0.0)
			options = 0.0;
		unsigned char lookup[256];
		for (int i = 0; i < 256; ++i)
		{
			double c  = static_cast<double>(i) / 255.0;
			c         = qPow(c, options);
			lookup[i] = clampByte(static_cast<long>(c * 255));
		}

		for (long row = 0; row < height; ++row)
		{
			unsigned char *line = buffer + row * bpl + channel;
			for (long i = 0; i < width; ++i)
			{
				*line = lookup[*line];
				line += 3;
			}
		}
	}

	void makeGreyscale(unsigned char *buffer, long width, long height, long bpl, double options, bool linear)
	{
		Q_UNUSED(options);
		for (long row = 0; row < height; ++row)
		{
			unsigned char *line = buffer + row * bpl;
			for (long i = 0; i < width; ++i)
			{
				double c;
				if (linear)
					c = (line[0] + line[1] + line[2]) / 3.0;
				else
					c = line[0] * 0.11 + line[1] * 0.59 + line[2] * 0.30;
				const unsigned char value = clampByte(static_cast<long>(c));
				line[0]                   = value;
				line[1]                   = value;
				line[2]                   = value;
				line += 3;
			}
		}
	}

	void averageBuffer(unsigned char *buffer, long width, long height, long bpl)
	{
		Q_UNUSED(bpl);
		qint64    r         = 0;
		qint64    g         = 0;
		qint64    b         = 0;
		qint64    count     = 0;
		const int increment = bytesPerLine24(static_cast<int>(width));
		for (long col = 0; col < width; ++col)
		{
			unsigned char const *p = buffer + col * 3;
			for (long row = 0; row < height; ++row)
			{
				b += p[0];
				g += p[1];
				r += p[2];
				++count;
				p += increment;
			}
		}

		if (count == 0)
			return;

		b /= count;
		g /= count;
		r /= count;

		for (long col = 0; col < width; ++col)
		{
			unsigned char *p = buffer + col * 3;
			for (long row = 0; row < height; ++row)
			{
				p[0] = clampByte(b);
				p[1] = clampByte(g);
				p[2] = clampByte(r);
				p += increment;
			}
		}
	}

	void brightnessMultiply(unsigned char *buffer, long width, long height, long bpl, double options)
	{
		Q_UNUSED(width);
		const long count = bpl * height;
		for (long i = 0; i < count; ++i)
		{
			long c    = buffer[i];
			c         = roundedToLong(static_cast<double>(c) * options);
			buffer[i] = clampByte(c);
		}
	}

	void generalFilter(unsigned char *buffer, long width, long height, long bpl, double options,
	                   const double *matrix, double divisor)
	{
		if (options != 2)
		{
			for (long row = 0; row < height; ++row)
			{
				unsigned char *line     = buffer + bpl * row;
				const int      lastByte = static_cast<int>(width * 3);
				for (long rgb = 0; rgb < 3; ++rgb)
				{
					long window[5];
					for (long col = 0; col < 4; ++col)
						window[col + 1] =
						    line[qBound(0, static_cast<int>(rgb + (col * 3) - 6), lastByte - 1)];

					for (long col = 0; col < lastByte - 4; col += 3)
					{
						window[0] = window[1];
						window[1] = window[2];
						window[2] = window[3];
						window[3] = window[4];
						window[4] = line[qBound(0, static_cast<int>(col + rgb + 6), lastByte - 1)];

						long total = 0;
						for (int i = 0; i < 5; ++i)
							total += roundedToLong(static_cast<double>(window[i]) * matrix[i]);

						line[col + rgb] = clampByte(roundedToLong(static_cast<double>(total) / divisor));
					}
				}
			}
		}

		if (options != 1)
		{
			const int lastByte = static_cast<int>(width * 3);
			for (long col = 0; col < lastByte; ++col)
			{
				long window[5];
				for (long row = 0; row < 4; ++row)
				{
					const long           from = qBound(0L, row - 2, height - 1);
					unsigned char const *line = buffer + col + (from * bpl);
					window[row + 1]           = *line;
				}

				for (long row = 0; row < height; ++row)
				{
					const long     from = qBound(0L, row + 3, height - 1);
					unsigned char *line = buffer + col + (from * bpl);
					window[0]           = window[1];
					window[1]           = window[2];
					window[2]           = window[3];
					window[3]           = window[4];
					window[4]           = *line;

					long total = 0;
					for (int i = 0; i < 5; ++i)
						total += roundedToLong(static_cast<double>(window[i]) * matrix[i]);

					line  = buffer + col + (row * bpl);
					*line = clampByte(roundedToLong(static_cast<double>(total) / divisor));
				}
			}
		}
	}

	bool blendPixelInternal(long blend, long base, short mode, double opacity, long &out)
	{
		long rA = blend & 0xFF;
		long gA = (blend >> 8) & 0xFF;
		long bA = (blend >> 16) & 0xFF;
		long rB = base & 0xFF;
		long gB = (base >> 8) & 0xFF;
		long bB = (base >> 16) & 0xFF;

		long r = 0;
		long g = 0;
		long b = 0;

		if (opacity < 0.0 || opacity > 1.0)
			return false;

		static const quint8 *cosTable = []
		{
			static quint8 table[256]  = {};
			static bool   initialized = false;
			if (!initialized)
			{
				constexpr double pi_div255 = 3.1415926535898 / 255.0;
				for (int i = 0; i < 256; ++i)
				{
					const double a = 64.0 - qCos(static_cast<double>(i) * pi_div255) * 64.0;
					table[i]       = static_cast<quint8>(std::lround(a));
				}
				initialized = true;
			}
			return table;
		}();

		auto applyBlendOp = [&](auto blendOp)
		{
			if (opacity < 1.0)
			{
				r = QMudBlend::withOpacity(rA, rB, blendOp, opacity);
				g = QMudBlend::withOpacity(gA, gB, blendOp, opacity);
				b = QMudBlend::withOpacity(bA, bB, blendOp, opacity);
			}
			else
			{
				r = blendOp(rA, rB);
				g = blendOp(gA, gB);
				b = blendOp(bA, bB);
			}
		};

		auto applyRgb = [&](const long outR, const long outG, const long outB)
		{
			if (opacity < 1.0)
			{
				r = QMudBlend::simpleOpacity(static_cast<double>(rB), static_cast<double>(outR), opacity);
				g = QMudBlend::simpleOpacity(static_cast<double>(gB), static_cast<double>(outG), opacity);
				b = QMudBlend::simpleOpacity(static_cast<double>(bB), static_cast<double>(outB), opacity);
			}
			else
			{
				r = outR;
				g = outG;
				b = outB;
			}
		};

		switch (mode)
		{
		case 1:
			applyBlendOp(QMudBlend::normal);
			break;
		case 2:
			applyBlendOp(QMudBlend::average);
			break;
		case 3:
			applyBlendOp([](const long blendChannel, const long baseChannel)
			             { return QMudBlend::interpolate(blendChannel, baseChannel, cosTable); });
			break;
		case 4:
		{
			const double rnd = runtimeRandomUnit();
			r                = (rnd < opacity) ? rA : rB;
			g                = (rnd < opacity) ? gA : gB;
			b                = (rnd < opacity) ? bA : bB;
		}
		break;
		case 5:
			applyBlendOp(QMudBlend::darken);
			break;
		case 6:
			applyBlendOp(QMudBlend::multiply);
			break;
		case 7:
			applyBlendOp(QMudBlend::colorBurn);
			break;
		case 8:
			applyBlendOp(QMudBlend::linearBurn);
			break;
		case 9:
			applyBlendOp(QMudBlend::inverseColorBurn);
			break;
		case 10:
			applyBlendOp(QMudBlend::subtract);
			break;
		case 11:
			applyBlendOp(QMudBlend::lighten);
			break;
		case 12:
			applyBlendOp(QMudBlend::screen);
			break;
		case 13:
			applyBlendOp(QMudBlend::colorDodge);
			break;
		case 14:
			applyBlendOp(QMudBlend::linearDodge);
			break;
		case 15:
			applyBlendOp(QMudBlend::inverseColorDodge);
			break;
		case 16:
			applyBlendOp(QMudBlend::add);
			break;
		case 17:
			applyBlendOp(QMudBlend::overlay);
			break;
		case 18:
			applyBlendOp(QMudBlend::softLight);
			break;
		case 19:
			applyBlendOp(QMudBlend::hardLight);
			break;
		case 20:
			applyBlendOp(QMudBlend::vividLight);
			break;
		case 21:
			applyBlendOp(QMudBlend::linearLight);
			break;
		case 22:
			applyBlendOp(QMudBlend::pinLight);
			break;
		case 23:
			applyBlendOp(QMudBlend::hardMix);
			break;
		case 24:
			applyBlendOp(QMudBlend::difference);
			break;
		case 25:
			applyBlendOp(QMudBlend::exclusion);
			break;
		case 26:
			applyBlendOp(QMudBlend::reflect);
			break;
		case 27:
			applyBlendOp(QMudBlend::glow);
			break;
		case 28:
			applyBlendOp(QMudBlend::freeze);
			break;
		case 29:
			applyBlendOp(QMudBlend::heat);
			break;
		case 30:
			applyBlendOp(QMudBlend::negation);
			break;
		case 31:
			applyBlendOp(QMudBlend::phoenix);
			break;
		case 32:
			applyBlendOp(QMudBlend::stamp);
			break;
		case 33:
			applyBlendOp(QMudBlend::bitXor);
			break;
		case 34:
			applyBlendOp(QMudBlend::bitAnd);
			break;
		case 35:
			applyBlendOp(QMudBlend::bitOr);
			break;
		case 36:
			applyRgb(rA, gB, bB);
			break;
		case 37:
			applyRgb(rB, gA, bB);
			break;
		case 38:
			applyRgb(rB, gB, bA);
			break;
		case 39:
			applyRgb(rA, gA, bB);
			break;
		case 40:
			applyRgb(rB, gA, bA);
			break;
		case 41:
			applyRgb(rA, gB, bA);
			break;
		case 42:
			applyRgb(rA, (gA > rA) ? rA : gA, bA);
			break;
		case 43:
			applyRgb(rA, (gA > bA) ? bA : gA, bA);
			break;
		case 44:
			applyRgb(rA, (gA > ((rA + bA) / 2)) ? ((rA + bA) / 2) : gA, bA);
			break;
		case 45:
			applyRgb(rA, gA, (bA > rA) ? rA : bA);
			break;
		case 46:
			applyRgb(rA, gA, (bA > gA) ? gA : bA);
			break;
		case 47:
			applyRgb(rA, gA, (bA > ((rA + gA) / 2)) ? ((rA + gA) / 2) : bA);
			break;
		case 48:
			applyRgb((rA > gA) ? gA : rA, gA, bA);
			break;
		case 49:
			applyRgb((rA > bA) ? bA : rA, gA, bA);
			break;
		case 50:
			applyRgb((rA > ((gA + bA) / 2)) ? ((gA + bA) / 2) : rA, gA, bA);
			break;
		case 51:
			applyRgb(rA, 0, 0);
			break;
		case 52:
			applyRgb(0, gA, 0);
			break;
		case 53:
			applyRgb(0, 0, bA);
			break;
		case 54:
			applyRgb(0, gA, bA);
			break;
		case 55:
			applyRgb(rA, 0, bA);
			break;
		case 56:
			applyRgb(rA, gA, 0);
			break;
		case 57:
			applyRgb(rA, rA, rA);
			break;
		case 58:
			applyRgb(gA, gA, gA);
			break;
		case 59:
			applyRgb(bA, bA, bA);
			break;
		case 60:
		{
			const QColor cA(lowByteToInt(rA), lowByteToInt(gA), lowByteToInt(bA));
			const QColor cB(lowByteToInt(rB), lowByteToInt(gB), lowByteToInt(bB));
			const int    hue      = (cA.hslHue() < 0) ? 0 : cA.hslHue();
			QColor const outColor = QColor::fromHsl(hue, cB.hslSaturation(), cB.lightness());
			r                     = QMudBlend::simpleOpacity(longToDouble(rB), outColor.red(), opacity);
			g                     = QMudBlend::simpleOpacity(longToDouble(gB), outColor.green(), opacity);
			b                     = QMudBlend::simpleOpacity(longToDouble(bB), outColor.blue(), opacity);
		}
		break;
		case 61:
		{
			const QColor cA(lowByteToInt(rA), lowByteToInt(gA), lowByteToInt(bA));
			const QColor cB(lowByteToInt(rB), lowByteToInt(gB), lowByteToInt(bB));
			const int    sat      = cA.hslSaturation();
			QColor const outColor = QColor::fromHsl(cB.hslHue() < 0 ? 0 : cB.hslHue(), sat, cB.lightness());
			r                     = QMudBlend::simpleOpacity(longToDouble(rB), outColor.red(), opacity);
			g                     = QMudBlend::simpleOpacity(longToDouble(gB), outColor.green(), opacity);
			b                     = QMudBlend::simpleOpacity(longToDouble(bB), outColor.blue(), opacity);
		}
		break;
		case 62:
		{
			const QColor cA(lowByteToInt(rA), lowByteToInt(gA), lowByteToInt(bA));
			const QColor cB(lowByteToInt(rB), lowByteToInt(gB), lowByteToInt(bB));
			const int    hue      = (cA.hslHue() < 0) ? 0 : cA.hslHue();
			const int    sat      = cA.hslSaturation();
			QColor const outColor = QColor::fromHsl(hue, sat, cB.lightness());
			r                     = QMudBlend::simpleOpacity(longToDouble(rB), outColor.red(), opacity);
			g                     = QMudBlend::simpleOpacity(longToDouble(gB), outColor.green(), opacity);
			b                     = QMudBlend::simpleOpacity(longToDouble(bB), outColor.blue(), opacity);
		}
		break;
		case 63:
		{
			const QColor cA(lowByteToInt(rA), lowByteToInt(gA), lowByteToInt(bA));
			const QColor cB(lowByteToInt(rB), lowByteToInt(gB), lowByteToInt(bB));
			QColor const outColor =
			    QColor::fromHsl(cB.hslHue() < 0 ? 0 : cB.hslHue(), cB.hslSaturation(), cA.lightness());
			r = QMudBlend::simpleOpacity(longToDouble(rB), outColor.red(), opacity);
			g = QMudBlend::simpleOpacity(longToDouble(gB), outColor.green(), opacity);
			b = QMudBlend::simpleOpacity(longToDouble(bB), outColor.blue(), opacity);
		}
		break;
		case 64:
		{
			const QColor cA(lowByteToInt(rA), lowByteToInt(gA), lowByteToInt(bA));
			const qreal  hue       = cA.hslHueF();
			const qreal  sat       = cA.hslSaturationF();
			const qreal  lum       = cA.lightnessF();
			const double hueScaled = (hue < 0.0) ? 0.0 : hue * 255.0;
			r                      = QMudBlend::simpleOpacity(longToDouble(rB), hueScaled, opacity);
			g                      = QMudBlend::simpleOpacity(longToDouble(gB), sat * 255.0, opacity);
			b                      = QMudBlend::simpleOpacity(longToDouble(bB), lum * 255.0, opacity);
		}
		break;
		default:
			return false;
		}

		out = static_cast<long>(clampByte(r)) | (static_cast<long>(clampByte(g)) << 8) |
		      (static_cast<long>(clampByte(b)) << 16);
		return true;
	}

	QRect rectFromCoords(const MiniWindow &window, long left, long top, long right, long bottom)
	{
		const int fixedRight  = window.fixRight(static_cast<int>(right));
		const int fixedBottom = window.fixBottom(static_cast<int>(bottom));
		const int width       = fixedRight - static_cast<int>(left);
		const int height      = fixedBottom - static_cast<int>(top);
		return {static_cast<int>(left), static_cast<int>(top), width, height};
	}

	bool isValidScriptLabel(const QString &label)
	{
		const QString trimmed = label.trimmed();
		if (trimmed.isEmpty())
			return false;
		return std::ranges::all_of(
		    trimmed, [](const QChar ch)
		    { return ch == QLatin1Char('_') || ch == QLatin1Char('.') || ch.isLetterOrNumber(); });
	}

	struct AtomicTagInfo
	{
			bool open{false};
			bool command{false};
			bool noReset{false};
			bool puebloOnly{false};
	};

	const QHash<QByteArray, AtomicTagInfo> &atomicTagInfoMap()
	{
		static const QHash<QByteArray, AtomicTagInfo> kMap = []
		{
			QHash<QByteArray, AtomicTagInfo> map;
			auto                             add =
			    [&map](const QByteArray &name, bool open, bool command, bool noReset, bool puebloOnly = false)
			{
				AtomicTagInfo info;
				info.open       = open;
				info.command    = command;
				info.noReset    = noReset;
				info.puebloOnly = puebloOnly;
				map.insert(name, info);
			};

			// open tags
			add("bold", true, false, false);
			add("b", true, false, false);
			add("high", true, false, false);
			add("h", true, false, false);
			add("underline", true, false, false);
			add("u", true, false, false);
			add("italic", true, false, false);
			add("i", true, false, false);
			add("em", true, false, false);
			add("color", true, false, false);
			add("c", true, false, false);
			add("s", true, false, false);
			add("strike", true, false, false);
			add("strong", true, false, false);
			add("small", true, false, false);
			add("tt", true, false, false);
			add("font", true, false, false);

			// secure tags
			add("frame", false, false, false);
			add("dest", false, false, false);
			add("image", false, true, false);
			add("filter", false, false, false);
			add("a", false, false, false);
			add("h1", false, false, false);
			add("h2", false, false, false);
			add("h3", false, false, false);
			add("h4", false, false, false);
			add("h5", false, false, false);
			add("h6", false, false, false);
			add("hr", false, true, false);
			add("nobr", false, false, false);
			add("p", false, false, false);
			add("script", false, false, false);
			add("send", false, false, false);
			add("ul", false, false, false);
			add("ol", false, false, false);
			add("samp", false, false, false);
			add("center", false, false, false);
			add("var", false, false, false);
			add("v", false, false, false);
			add("gauge", false, false, false);
			add("stat", false, false, false);
			add("expire", false, false, false);
			add("li", false, true, false);

			// secure, command tags
			add("sound", false, true, false);
			add("music", false, true, false);
			add("br", false, true, false);
			add("username", false, true, false);
			add("user", false, true, false);
			add("password", false, true, false);
			add("pass", false, true, false);
			add("relocate", false, true, false);
			add("version", false, true, false);

			// extension tags
			add("reset", false, true, false);
			add("mxp", false, true, false);
			add("support", false, true, false);
			add("option", false, true, false);
			add("afk", false, true, false);
			add("recommend_option", false, true, false);

			// Pueblo tags
			add("pre", false, false, false, true);
			add("body", false, false, true, true);
			add("head", false, false, true, true);
			add("html", false, false, true, true);
			add("title", false, false, false, true);
			add("img", false, true, false, true);
			add("xch_page", false, true, false, true);
			add("xch_pane", false, true, false, true);

			return map;
		}();
		return kMap;
	}

	bool lookupAtomicTagInfo(const QByteArray &tag, AtomicTagInfo &info)
	{
		const auto &map = atomicTagInfoMap();
		const auto  it  = map.find(tag);
		if (it == map.end())
			return false;
		info = it.value();
		return true;
	}

	[[nodiscard]] bool mxpQuotedTokenEndsHere(const char *afterQuote)
	{
		const char *p = afterQuote;
		while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
			++p;
		if (*p == '\0' || *p == '>' || *p == '/')
			return true;
		if (*p == '|' || *p == ',' || *p == ';')
			return false;

		const auto isAttrChar = [](const unsigned char ch)
		{ return std::isalnum(ch) != 0 || ch == '_' || ch == '-' || ch == ':'; };

		if (!isAttrChar(static_cast<unsigned char>(*p)))
			return false;
		while (isAttrChar(static_cast<unsigned char>(*p)))
			++p;
		while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
			++p;
		return *p == '=' || *p == '\0' || *p == '>' || *p == '/';
	}

	bool mxpGetWord(QByteArray &result, QByteArray &input)
	{
		const char *p = input.constData();
		const char *pStart;
		int         len = 0;

		// skip leading spaces
		while (*p == ' ')
			p++;

		// is it quoted string?
		if (*p == '\'' || *p == '\"')
		{
			const char quote = *p;
			pStart           = ++p; // bypass opening quote
			for (; *p; ++p)
			{
				if (*p == quote)
				{
					// Be forgiving with malformed MXP values that embed quote chars
					// without entity escaping (e.g. href='... 'harm' | ...'). Treat a
					// quote as closing only at real token boundaries.
					if (mxpQuotedTokenEndsHere(p + 1))
						break;
				}
				++len;
			}
			result = QByteArray(pStart, len);
			if (*p == quote)
				++p;
			input = QByteArray(p); // return rest of line
			return false;          // not end, even if empty
		}

		// where word starts
		pStart = p;

		// is it a word or number?
		if (isAsciiAlnumByte(static_cast<unsigned char>(*p)) || *p == '+' || *p == '-')
		{
			// skip initial character, and then look for terminator
			for (p++, len++; *p; p++, len++)
			{
				if (isAsciiAlnumByte(static_cast<unsigned char>(*p)))
					continue;
				if (*p == '_' || // underscore
				    *p == '-' || // hyphen
				    *p == '.' || // period
				    *p == ','    // comma
				)
					continue;

				break; // stop if not alpha, digit, or above special characters
			}
		}
		else
			// is it a colour?  ie. #xxxxxx
			if (*p == '#')
			{
				for (p++, len++; *p; p++, len++)
				{
					if (asciiHexValue(static_cast<unsigned char>(*p)) >= 0)
						continue;
					break; // stop if not hex digit
				}
			}
			else
				// is it an argument?  i.e. &xxx;
				if (*p == '&')
				{
					for (p++, len++; *p && *p != ';'; p++, len++)
						; // keep going until ;
					      // include the ; in the word
					if (*p == ';')
					{
						len++;
						p++;
					}
				}
				else if (*p) // provided not end of line
				// assume single character, e.g. '=' or ',' or something
				{
					len = 1;
					p++;
				}

		result = QByteArray(pStart, len);
		input  = QByteArray(p); // return rest of line
		return len == 0;        // true if nothing found
	}

	struct MxpArgument
	{
			QString name;
			QString value;
			int     position{0};
	};

	struct ParsedMxpArguments
	{
			QString            rawArguments;
			QList<MxpArgument> args;
	};

	ParsedMxpArguments parseMxpArguments(const QByteArray &rawTag)
	{
		ParsedMxpArguments parsed;
		QByteArray         input = rawTag;
		QByteArray         tagName;
		mxpGetWord(tagName, input);
		parsed.rawArguments = QString::fromLocal8Bit(input);

		int        position = 0;
		QByteArray pending;
		bool       hasPending = false;

		while (true)
		{
			QByteArray argName;
			if (hasPending)
			{
				argName    = pending;
				hasPending = false;
			}
			else
			{
				if (mxpGetWord(argName, input))
					break;
			}

			if (argName.isEmpty())
				break;

			if (argName == "/")
				break;

			QByteArray equals;
			const bool atEnd = mxpGetWord(equals, input);

			if (equals == "=")
			{
				QByteArray value;
				if (mxpGetWord(value, input))
					break;
				MxpArgument arg;
				arg.name     = QString::fromLocal8Bit(argName).toLower();
				arg.value    = QString::fromLocal8Bit(value);
				arg.position = 0;
				parsed.args.push_back(arg);
				if (atEnd)
					break;
			}
			else
			{
				MxpArgument arg;
				arg.name.clear();
				arg.value    = QString::fromLocal8Bit(argName);
				arg.position = ++position;
				parsed.args.push_back(arg);
				if (atEnd)
					break;
				pending    = equals;
				hasPending = true;
			}
		}

		return parsed;
	}

	QMap<QString, QString>       buildArgumentTable(const QList<MxpArgument> &args);
	QMap<QByteArray, QByteArray> buildArgumentTableBytes(const QList<MxpArgument> &args);

	QMap<QByteArray, QByteArray> mergeAttributeDefaultsBytes(const QByteArray         &defaults,
	                                                         const QList<MxpArgument> &provided)
	{
		QMap<QByteArray, QByteArray> merged = buildArgumentTableBytes(provided);
		if (defaults.isEmpty())
			return merged;

		const QByteArray         tag            = QByteArray("tag ") + defaults;
		ParsedMxpArguments const parsedDefaults = parseMxpArguments(tag);
		int                      sequence       = 0;

		for (const MxpArgument &attribute : parsedDefaults.args)
		{
			++sequence;
			QByteArray nameKey;
			QByteArray defaultValue;
			if (attribute.name.isEmpty())
			{
				nameKey = attribute.value.toLocal8Bit().trimmed().toLower();
			}
			else
			{
				nameKey      = attribute.name.toLocal8Bit().trimmed().toLower();
				defaultValue = attribute.value.toLocal8Bit();
			}
			if (nameKey.isEmpty())
				continue;

			QByteArray positionalValue;
			QByteArray namedValue;
			bool       hasPositionalValue = false;
			bool       hasNamedValue      = false;
			for (const MxpArgument &providedArg : provided)
			{
				if (!providedArg.name.isEmpty() &&
				    providedArg.name.compare(QString::fromLocal8Bit(nameKey), Qt::CaseInsensitive) == 0)
				{
					namedValue    = providedArg.value.toLocal8Bit();
					hasNamedValue = true;
					break;
				}
				if (!hasPositionalValue && providedArg.position == sequence)
				{
					positionalValue    = providedArg.value.toLocal8Bit();
					hasPositionalValue = true;
				}
			}

			QByteArray replacement = hasNamedValue        ? namedValue
			                         : hasPositionalValue ? positionalValue
			                                              : defaultValue;

			merged.insert(nameKey, replacement);
			merged.insert(QByteArray::number(sequence), replacement);
		}

		return merged;
	}

	QByteArray resolveDefinitionEntities(const QByteArray &source, const QMap<QByteArray, QByteArray> &values,
	                                     const TelnetProcessor &telnet)
	{
		auto decodeNumericEntity = [](const QByteArray &name, QByteArray &decoded) -> bool
		{
			if (!name.startsWith('#'))
				return false;
			bool     ok   = false;
			uint32_t code = 0;
			if (name.size() > 2 && (name.at(1) == 'x' || name.at(1) == 'X'))
				code = name.mid(2).toUInt(&ok, 16);
			else
				code = name.mid(1).toUInt(&ok, 10);
			if (!ok || code > 0x10FFFFu)
				return false;
			QString out;
			out.reserve(2);
			out.append(QChar::fromUcs4(code));
			decoded = out.toUtf8();
			return !decoded.isEmpty();
		};

		QByteArray output;
		output.reserve(source.size());
		for (qsizetype i = 0; i < source.size(); ++i)
		{
			const char ch = source.at(i);
			if (ch != '&')
			{
				output.append(ch);
				continue;
			}
			const qsizetype semi = source.indexOf(';', i + 1);
			if (semi < 0)
			{
				output.append(ch);
				continue;
			}
			const QByteArray name = source.mid(i + 1, semi - i - 1);
			if (name == "text")
			{
				output.append('&');
				output.append(name);
				output.append(';');
				i = semi;
				continue;
			}
			const QByteArray key = name.toLower();
			if (values.contains(key))
				output.append(values.value(key));
			else
			{
				QByteArray resolved;
				if (decodeNumericEntity(name, resolved) || telnet.resolveEntityValue(name, resolved))
					output.append(resolved);
				else
				{
					output.append('&');
					output.append(name);
					output.append(';');
				}
			}
			i = semi;
		}
		return output;
	}

	bool parseDefinitionAlias(const QByteArray &definition, QByteArray &aliasTag,
	                          QMap<QByteArray, QByteArray> &aliasAttributes)
	{
		if (definition.isEmpty())
			return false;
		QByteArray trimmed = definition.trimmed();
		if (!trimmed.startsWith('<') || !trimmed.endsWith('>'))
			return false;
		trimmed                         = trimmed.mid(1, trimmed.size() - 2);
		ParsedMxpArguments const parsed = parseMxpArguments(trimmed);
		QByteArray               name;
		QByteArray               temp = trimmed;
		mxpGetWord(name, temp);
		if (name.isEmpty())
			return false;
		aliasTag        = name.toLower();
		aliasAttributes = buildArgumentTableBytes(parsed.args);
		return true;
	}

	QVector<QByteArray> extractDefinitionTags(const QByteArray &definition)
	{
		QVector<QByteArray> tags;
		qsizetype           i = 0;
		while (i < definition.size())
		{
			const qsizetype start = definition.indexOf('<', i);
			if (start < 0)
				break;
			bool      inQuote = false;
			char      quote   = 0;
			qsizetype end     = -1;
			for (qsizetype j = start + 1; j < definition.size(); ++j)
			{
				const char ch = definition.at(j);
				if ((ch == '\'' || ch == '\"') && (!inQuote || ch == quote))
				{
					if (!inQuote)
					{
						inQuote = true;
						quote   = ch;
					}
					else
					{
						inQuote = false;
						quote   = 0;
					}
					continue;
				}
				if (!inQuote && ch == '>')
				{
					end = j;
					break;
				}
			}
			if (end < 0)
				break;
			const QByteArray body = definition.mid(start + 1, end - start - 1);
			tags.push_back(body);
			i = end + 1;
		}
		return tags;
	}

	QMap<QString, QString> buildArgumentTable(const QList<MxpArgument> &args)
	{
		QMap<QString, QString> table;
		for (const auto &arg : args)
		{
			if (arg.name.isEmpty())
				table.insert(QString::number(arg.position), arg.value);
			else
				table.insert(arg.name, arg.value);
		}
		return table;
	}

	QMap<QByteArray, QByteArray> buildArgumentTableBytes(const QList<MxpArgument> &args)
	{
		QMap<QByteArray, QByteArray> table;
		for (const auto &arg : args)
		{
			QByteArray key = arg.name.isEmpty() ? QByteArray::number(arg.position) : arg.name.toLocal8Bit();
			if (!arg.name.isEmpty())
				key = key.toLower();
			table.insert(key, arg.value.toLocal8Bit());
		}
		return table;
	}

	QString buildArgumentString(const QList<MxpArgument> &args)
	{
		QStringList parts;
		for (const auto &arg : args)
		{
			if (arg.name.isEmpty())
				parts << QStringLiteral("'%1'").arg(arg.value);
			else
				parts << QStringLiteral("%1='%2'").arg(arg.name, arg.value);
		}
		return parts.join(' ');
	}

	bool isEnabledFlag(const QString &value)
	{
		return value == QStringLiteral("1") || value.compare(QStringLiteral("y"), Qt::CaseInsensitive) == 0 ||
		       value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
	}

	struct FixedColumnWrapConfig
	{
			bool enabled{false};
			int  wrapColumn{0};
			bool indentParas{true};
	};

	int wrapColumnFromWorldSettings(const QMap<QString, QString> &attrs)
	{
		return attrs.value(QStringLiteral("wrap_column")).toInt();
	}

	int wrapColumnWithWorldMinimum(const int calculatedColumns, const int worldWrapColumn)
	{
		if (worldWrapColumn <= 0)
			return calculatedColumns;
		if (calculatedColumns <= 0)
			return worldWrapColumn;
		return qMax(calculatedColumns, worldWrapColumn);
	}

	int localWrapColumnForOutput(const QMap<QString, QString> &attrs, const bool nawsNegotiated,
	                             const int windowColumns)
	{
		if (!isEnabledFlag(attrs.value(QStringLiteral("wrap"))))
			return 0;

		const int  worldWrapColumn = wrapColumnFromWorldSettings(attrs);
		const bool autoWrapWindow  = isEnabledFlag(attrs.value(QStringLiteral("auto_wrap_window_width")));

		if (nawsNegotiated)
			return wrapColumnWithWorldMinimum(windowColumns, worldWrapColumn);

		if (autoWrapWindow)
			return wrapColumnWithWorldMinimum(windowColumns, worldWrapColumn);

		return worldWrapColumn;
	}

	bool indentParasEnabled(const QMap<QString, QString> &attrs)
	{
		const QString indentValue = attrs.value(QStringLiteral("indent_paras"));
		return !(indentValue == QStringLiteral("0") ||
		         indentValue.compare(QStringLiteral("n"), Qt::CaseInsensitive) == 0 ||
		         indentValue.compare(QStringLiteral("false"), Qt::CaseInsensitive) == 0);
	}

	FixedColumnWrapConfig localOutputWrapConfig(const QMap<QString, QString> &attrs,
	                                            const bool nawsNegotiated, const int windowColumns)
	{
		FixedColumnWrapConfig config;
		config.indentParas = indentParasEnabled(attrs);
		config.wrapColumn  = localWrapColumnForOutput(attrs, nawsNegotiated, windowColumns);
		config.enabled     = config.wrapColumn > 0;
		return config;
	}

	bool sameStyleForWrap(const WorldRuntime::StyleSpan &a, const WorldRuntime::StyleSpan &b)
	{
		return a.fore == b.fore && a.back == b.back && a.bold == b.bold && a.underline == b.underline &&
		       a.italic == b.italic && a.blink == b.blink && a.strike == b.strike && a.inverse == b.inverse &&
		       a.changed == b.changed && a.actionType == b.actionType && a.action == b.action &&
		       a.hint == b.hint && a.variable == b.variable && a.startTag == b.startTag;
	}

	bool isSingleCharGrapheme(const QStringView grapheme, const QChar ch)
	{
		return grapheme.size() == 1 && grapheme.at(0) == ch;
	}

	bool isWhitespaceGrapheme(const QStringView grapheme)
	{
		for (const QChar ch : grapheme)
		{
			if (!ch.isSpace())
				return false;
		}
		return !grapheme.isEmpty();
	}

	bool isZeroWidthWrapCodeUnit(const QChar ch)
	{
		switch (ch.category())
		{
		case QChar::Mark_NonSpacing:
		case QChar::Mark_SpacingCombining:
		case QChar::Mark_Enclosing:
		case QChar::Other_Format:
			return true;
		default:
			break;
		}
		return false;
	}

	int graphemeColumnWidthForWrap(const QStringView grapheme)
	{
		if (grapheme.isEmpty())
			return 0;
		bool hasVisibleCodeUnit = false;
		for (const QChar ch : grapheme)
		{
			if (isZeroWidthWrapCodeUnit(ch))
				continue;
			hasVisibleCodeUnit = true;
			break;
		}
		return hasVisibleCodeUnit ? 1 : 0;
	}

	int trailingLineColumnWidthForWrap(const QString &text)
	{
		if (text.isEmpty())
			return 0;

		constexpr auto      kNoBoundary = qsizetype{-1};
		QTextBoundaryFinder boundary(QTextBoundaryFinder::Grapheme, text);
		const QStringView   textView{text};
		int                 column = 0;
		for (qsizetype graphemeStart = 0, graphemeEnd = boundary.toNextBoundary(); graphemeEnd != kNoBoundary;
		     graphemeStart = graphemeEnd, graphemeEnd = boundary.toNextBoundary())
		{
			const QStringView grapheme = textView.sliced(graphemeStart, graphemeEnd - graphemeStart);
			if (isSingleCharGrapheme(grapheme, QLatin1Char('\n')))
			{
				column = 0;
				continue;
			}
			column += graphemeColumnWidthForWrap(grapheme);
		}
		return column;
	}

	void wrapPlainLineForColumn(QString &text, const int wrapColumn, const bool indentParas,
	                            const int firstLinePrefixColumns = 0)
	{
		if (wrapColumn <= 0 || text.isEmpty())
		{
			return;
		}
		constexpr auto kNoBoundary = qsizetype{-1};

		QString        wrappedText;
		wrappedText.reserve(safeQSizeToInt(text.size() + text.size() / qMax(1, wrapColumn)));

		int  column             = qMax(0, firstLinePrefixColumns);
		int  lineStart          = 0;
		int  lastSpace          = -1;
		bool lineHasVisibleChar = column > 0;

		auto recomputeLineState = [&]
		{
			column             = 0;
			lineStart          = 0;
			lastSpace          = -1;
			lineHasVisibleChar = false;
			for (int i = safeQSizeToInt(wrappedText.size()) - 1; i >= 0; --i)
			{
				if (wrappedText.at(i) == QLatin1Char('\n'))
				{
					lineStart = i + 1;
					break;
				}
			}
			QTextBoundaryFinder boundary(QTextBoundaryFinder::Grapheme, wrappedText);
			boundary.setPosition(lineStart);
			const QStringView wrappedView{wrappedText};
			for (qsizetype graphemeStart = static_cast<qsizetype>(lineStart),
			               graphemeEnd   = boundary.toNextBoundary();
			     graphemeEnd != kNoBoundary;
			     graphemeStart = graphemeEnd, graphemeEnd = boundary.toNextBoundary())
			{
				const QStringView grapheme = wrappedView.sliced(graphemeStart, graphemeEnd - graphemeStart);
				if (isSingleCharGrapheme(grapheme, QLatin1Char('\n')))
				{
					lineStart          = boundedQSizeToInt(graphemeEnd);
					column             = 0;
					lastSpace          = -1;
					lineHasVisibleChar = false;
					continue;
				}
				if (isSingleCharGrapheme(grapheme, QLatin1Char(' ')))
					lastSpace = boundedQSizeToInt(graphemeEnd - 1);
				if (!isWhitespaceGrapheme(grapheme))
					lineHasVisibleChar = true;
				column += graphemeColumnWidthForWrap(grapheme);
			}
		};

		QTextBoundaryFinder boundary(QTextBoundaryFinder::Grapheme, text);
		const QStringView   textView{text};
		for (qsizetype graphemeStart = 0, graphemeEnd = boundary.toNextBoundary(); graphemeEnd != kNoBoundary;
		     graphemeStart = graphemeEnd, graphemeEnd = boundary.toNextBoundary())
		{
			const QStringView grapheme = textView.sliced(graphemeStart, graphemeEnd - graphemeStart);
			wrappedText.append(grapheme);

			if (isSingleCharGrapheme(grapheme, QLatin1Char('\n')))
			{
				column             = 0;
				lineStart          = safeQSizeToInt(wrappedText.size());
				lastSpace          = -1;
				lineHasVisibleChar = false;
				continue;
			}

			if (isSingleCharGrapheme(grapheme, QLatin1Char(' ')))
				lastSpace = safeQSizeToInt(wrappedText.size()) - 1;
			if (!isWhitespaceGrapheme(grapheme))
				lineHasVisibleChar = true;

			column += graphemeColumnWidthForWrap(grapheme);
			if (column < wrapColumn)
				continue;

			if (!lineHasVisibleChar)
				continue;

			int insertPos = safeQSizeToInt(wrappedText.size());
			if (lastSpace >= lineStart)
			{
				insertPos = indentParas ? lastSpace : lastSpace + 1;

				bool onlyWhitespace = true;
				for (int j = lineStart, size = safeQSizeToInt(wrappedText.size()); j < insertPos && j < size;
				     ++j)
				{
					if (!wrappedText.at(j).isSpace())
					{
						onlyWhitespace = false;
						break;
					}
				}
				if (onlyWhitespace)
					insertPos = safeQSizeToInt(wrappedText.size());
			}

			wrappedText.insert(insertPos, QLatin1Char('\n'));
			recomputeLineState();
		}

		text = wrappedText;
	}

	void wrapStyledLineForColumn(QString &text, QVector<WorldRuntime::StyleSpan> &spans, const int wrapColumn,
	                             const bool indentParas, const int firstLinePrefixColumns = 0)
	{
		if (wrapColumn <= 0 || text.isEmpty())
			return;

		constexpr auto                   kNoBoundary = qsizetype{-1};

		QVector<WorldRuntime::StyleSpan> expandedStyles;
		expandedStyles.reserve(text.size());
		WorldRuntime::StyleSpan lastStyle;
		int                     covered = 0;
		for (const WorldRuntime::StyleSpan &span : spans)
		{
			lastStyle     = span;
			const int len = qMax(0, span.length);
			for (int i = 0; i < len && covered < text.size(); ++i, ++covered)
				expandedStyles.push_back(span);
			if (covered >= text.size())
				break;
		}
		while (expandedStyles.size() < text.size())
			expandedStyles.push_back(lastStyle);

		QString wrappedText;
		wrappedText.reserve(safeQSizeToInt(text.size() + text.size() / qMax(1, wrapColumn)));
		QVector<WorldRuntime::StyleSpan> wrappedStyles;
		wrappedStyles.reserve(
		    safeQSizeToInt(expandedStyles.size() + expandedStyles.size() / qMax(1, wrapColumn)));

		int  column             = qMax(0, firstLinePrefixColumns);
		int  lineStart          = 0;
		int  lastSpace          = -1;
		bool lineHasVisibleChar = column > 0;

		auto recomputeLineState = [&]
		{
			column             = 0;
			lineStart          = 0;
			lastSpace          = -1;
			lineHasVisibleChar = false;
			for (int i = safeQSizeToInt(wrappedText.size()) - 1; i >= 0; --i)
			{
				if (wrappedText.at(i) == QLatin1Char('\n'))
				{
					lineStart = i + 1;
					break;
				}
			}
			QTextBoundaryFinder boundary(QTextBoundaryFinder::Grapheme, wrappedText);
			boundary.setPosition(lineStart);
			const QStringView wrappedView{wrappedText};
			for (qsizetype graphemeStart = static_cast<qsizetype>(lineStart),
			               graphemeEnd   = boundary.toNextBoundary();
			     graphemeEnd != kNoBoundary;
			     graphemeStart = graphemeEnd, graphemeEnd = boundary.toNextBoundary())
			{
				const QStringView grapheme = wrappedView.sliced(graphemeStart, graphemeEnd - graphemeStart);
				if (isSingleCharGrapheme(grapheme, QLatin1Char('\n')))
				{
					lineStart          = boundedQSizeToInt(graphemeEnd);
					column             = 0;
					lastSpace          = -1;
					lineHasVisibleChar = false;
					continue;
				}
				if (isSingleCharGrapheme(grapheme, QLatin1Char(' ')))
					lastSpace = boundedQSizeToInt(graphemeEnd - 1);
				if (!isWhitespaceGrapheme(grapheme))
					lineHasVisibleChar = true;
				column += graphemeColumnWidthForWrap(grapheme);
			}
		};

		QTextBoundaryFinder boundary(QTextBoundaryFinder::Grapheme, text);
		const QStringView   textView{text};
		for (qsizetype graphemeStart = 0, graphemeEnd = boundary.toNextBoundary(); graphemeEnd != kNoBoundary;
		     graphemeStart = graphemeEnd, graphemeEnd = boundary.toNextBoundary())
		{
			const QStringView grapheme = textView.sliced(graphemeStart, graphemeEnd - graphemeStart);
			wrappedText.append(grapheme);
			for (qsizetype i = graphemeStart; i < graphemeEnd; ++i)
				wrappedStyles.push_back(expandedStyles.value(boundedQSizeToInt(i)));
			const WorldRuntime::StyleSpan style = expandedStyles.value(boundedQSizeToInt(graphemeEnd - 1));

			if (isSingleCharGrapheme(grapheme, QLatin1Char('\n')))
			{
				column             = 0;
				lineStart          = safeQSizeToInt(wrappedText.size());
				lastSpace          = -1;
				lineHasVisibleChar = false;
				continue;
			}

			if (isSingleCharGrapheme(grapheme, QLatin1Char(' ')))
				lastSpace = safeQSizeToInt(wrappedText.size()) - 1;
			if (!isWhitespaceGrapheme(grapheme))
				lineHasVisibleChar = true;

			column += graphemeColumnWidthForWrap(grapheme);
			if (column < wrapColumn)
				continue;

			if (!lineHasVisibleChar)
				continue;

			int insertPos = safeQSizeToInt(wrappedText.size());
			if (lastSpace >= lineStart)
			{
				insertPos = indentParas ? lastSpace : lastSpace + 1;

				bool onlyWhitespace = true;
				for (int j = lineStart, size = safeQSizeToInt(wrappedText.size()); j < insertPos && j < size;
				     ++j)
				{
					if (!wrappedText.at(j).isSpace())
					{
						onlyWhitespace = false;
						break;
					}
				}
				if (onlyWhitespace)
					insertPos = safeQSizeToInt(wrappedText.size());
			}

			const WorldRuntime::StyleSpan newlineStyle = insertPos > 0 && insertPos - 1 < wrappedStyles.size()
			                                                 ? wrappedStyles.at(insertPos - 1)
			                                                 : style;
			wrappedText.insert(insertPos, QLatin1Char('\n'));
			wrappedStyles.insert(insertPos, newlineStyle);
			recomputeLineState();
		}

		QVector<WorldRuntime::StyleSpan> rebuilt;
		rebuilt.reserve(wrappedStyles.size());
		for (const WorldRuntime::StyleSpan &oneStyle : wrappedStyles)
		{
			WorldRuntime::StyleSpan span = oneStyle;
			span.length                  = 1;
			if (!rebuilt.isEmpty() && sameStyleForWrap(rebuilt.last(), span))
				rebuilt.last().length++;
			else
				rebuilt.push_back(span);
		}

		text  = wrappedText;
		spans = rebuilt;
	}

	int currentMxpDebugLevel(const QMap<QString, QString> &worldAttributes)
	{
		int           debugLevel = DBG_NONE;
		bool          ok         = false;
		const QString rawDebug   = worldAttributes.value(QStringLiteral("mxp_debug_level"));
		debugLevel               = rawDebug.toInt(&ok);
		if (!ok)
		{
			const WorldNumericOption *opt =
			    QMudWorldOptions::findWorldNumericOption(QStringLiteral("mxp_debug_level"));
			if (opt)
				debugLevel = static_cast<int>(opt->defaultValue);
		}
		return debugLevel;
	}

	void applyMappingFailureIfMatched(WorldRuntime &runtime, const QString &line)
	{
		if (!runtime.isMapping())
			return;

		const QMap<QString, QString> &attrs          = runtime.worldAttributes();
		const QString                 mappingFailure = attrs.value(QStringLiteral("mapping_failure"));
		if (mappingFailure.isEmpty())
			return;

		const bool useRegexp = isEnabledFlag(attrs.value(QStringLiteral("map_failure_regexp")));
		bool       matched   = false;
		if (useRegexp)
		{
			const QRegularExpression re(mappingFailure);
			if (re.isValid())
				matched = re.match(line).hasMatch();
		}
		else
		{
			matched = (line == mappingFailure);
		}

		if (matched)
			runtime.deleteLastMapItem();
	}

	QTime timeFromParts(int hour, int minute, double second)
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

	qint64 intervalMsFromParts(int hour, int minute, double second)
	{
		const int    secInt = qFloor(second);
		const qint64 baseMs = static_cast<qint64>(hour) * 3600 * 1000 +
		                      static_cast<qint64>(minute) * 60 * 1000 + static_cast<qint64>(secInt) * 1000;
		const qint64 fracMs = qRound((second - secInt) * 1000.0);
		return baseMs + fracMs;
	}

	void resetTimerFields(WorldRuntime::Timer &timer)
	{
		if (!isEnabledFlag(timer.attributes.value(QStringLiteral("enabled"))))
			return;

		const bool      atTime       = isEnabledFlag(timer.attributes.value(QStringLiteral("at_time")));
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
			QTime const at = timeFromParts(hour, minute, second);
			if (!at.isValid())
				return;
			QDateTime fire(QDate::currentDate(), at);
			if (fire < now)
				fire = fire.addDays(1);
			timer.nextFireTime = fire;
			return;
		}

		const qint64 intervalMs = intervalMsFromParts(hour, minute, second);
		const qint64 offsetMs   = intervalMsFromParts(offsetHour, offsetMinute, offsetSecond);
		timer.nextFireTime      = now.addMSecs(intervalMs - offsetMs);
	}

	QDateTime parsePluginDate(const QString &value)
	{
		if (value.trimmed().isEmpty())
			return {};
		QDateTime parsed = QDateTime::fromString(value, QStringLiteral("yyyy-MM-dd HH:mm:ss"));
		if (!parsed.isValid())
			parsed = QDateTime::fromString(value, QStringLiteral("yyyy-MM-dd HH:mm"));
		if (!parsed.isValid())
			parsed = QDateTime::fromString(value, QStringLiteral("yyyy-MM-dd"));
		if (!parsed.isValid())
			parsed = QDateTime::fromString(value, Qt::ISODate);
		return parsed;
	}

	constexpr int kDefaultPluginSequence = 5000;

	int           pluginSequenceFromAttributes(const QMap<QString, QString> &attributes)
	{
		bool      ok  = false;
		const int seq = attributes.value(QStringLiteral("sequence")).toInt(&ok);
		return ok ? seq : kDefaultPluginSequence;
	}

	double toOleDate(const QDateTime &time)
	{
		if (!time.isValid())
			return 0.0;
		const QDateTime base(QDate(1899, 12, 30), QTime(0, 0), QTimeZone::systemTimeZone());
		const qint64    seconds = base.secsTo(time);
		return static_cast<double>(seconds) / 86400.0;
	}

	int findPluginIndex(const QList<WorldRuntime::Plugin> &plugins, const QString &pluginId)
	{
		const QString key = pluginId.trimmed().toLower();
		if (key.isEmpty())
			return -1;
		for (int i = 0; i < plugins.size(); ++i)
		{
			const QString id = plugins.at(i).attributes.value(QStringLiteral("id")).trimmed().toLower();
			if (id == key)
				return i;
		}
		return -1;
	}

	QString fixHtmlString(const QString &source)
	{
		QString cleaned;
		cleaned.reserve(source.size());
		for (const QChar ch : source)
		{
			const ushort code = ch.unicode();
			if (code < 0x20 && code != 0x09 && code != 0x0A && code != 0x0D)
				continue;
			cleaned += ch;
		}

		QString   strOldString = cleaned;
		QString   strNewString;

		qsizetype i;

		while ((i = strOldString.indexOf(QRegularExpression(QStringLiteral("[<>&\\\"]")))) != -1)
		{
			strNewString += strOldString.left(i);

			switch (strOldString.at(i).unicode())
			{
			case '<':
				strNewString += QStringLiteral("&lt;");
				break;
			case '>':
				strNewString += QStringLiteral("&gt;");
				break;
			case '&':
				strNewString += QStringLiteral("&amp;");
				break;
			case '\"':
				strNewString += QStringLiteral("&quot;");
				break;
			default:
				break;
			}

			strOldString = strOldString.mid(i + 1);
		}

		strNewString += strOldString;

		return strNewString;
	}

	QString fixHtmlMultilineString(const QString &source)
	{
		QString cleaned;
		cleaned.reserve(source.size());
		for (const QChar ch : source)
		{
			const ushort code = ch.unicode();
			if (code < 0x20 && code != 0x09 && code != 0x0A && code != 0x0D)
				continue;
			cleaned += ch;
		}

		QString   strOldString = cleaned;
		QString   strNewString;

		qsizetype i;

		while ((i = strOldString.indexOf(QRegularExpression(QStringLiteral("[<>&\\t]")))) != -1)
		{
			strNewString += strOldString.left(i);

			switch (strOldString.at(i).unicode())
			{
			case '<':
				strNewString += QStringLiteral("&lt;");
				break;
			case '>':
				strNewString += QStringLiteral("&gt;");
				break;
			case '&':
				strNewString += QStringLiteral("&amp;");
				break;
			case '\t':
				strNewString += QStringLiteral("&#9;");
				break;
			default:
				break;
			}

			strOldString = strOldString.mid(i + 1);
		}

		strNewString += strOldString;

		return strNewString;
	}

	QString replaceNewlines(const QString &value)
	{
		QString result = value;
		result.replace(QStringLiteral("\r\n"), QStringLiteral(" "));
		result.replace(QLatin1Char('\n'), QLatin1Char(' '));
		result.replace(QLatin1Char('\r'), QLatin1Char(' '));
		return result;
	}

	long parseColorRef(const QString &value)
	{
		if (value.isEmpty())
			return 0;
		QColor const color(value);
		if (color.isValid())
			return colorToLong(color);
		bool       ok      = false;
		const long numeric = value.toLong(&ok);
		return ok ? numeric : 0;
	}

	bool isLikelyPathAttributeName(const QString &name)
	{
		const QString key = name.trimmed().toLower();
		if (key.isEmpty())
			return false;
		return key.contains(QStringLiteral("file")) || key.contains(QStringLiteral("directory")) ||
		       key.contains(QStringLiteral("path")) || key == QStringLiteral("sound") ||
		       key.endsWith(QStringLiteral("_sound"));
	}

	bool isCanonicalizablePathAttributeName(const QString &name)
	{
		const QString key = name.trimmed().toLower();
		if (key.isEmpty())
			return false;
		if (key == QStringLiteral("script_filename") || key == QStringLiteral("auto_log_file_name") ||
		    key == QStringLiteral("chat_file_save_directory") || key == QStringLiteral("sound") ||
		    key == QStringLiteral("beep_sound") || key == QStringLiteral("new_activity_sound"))
		{
			return true;
		}
		return key.endsWith(QStringLiteral("_directory")) || key.endsWith(QStringLiteral("_file")) ||
		       key.endsWith(QStringLiteral("_filename")) || key.endsWith(QStringLiteral("_file_name")) ||
		       key.endsWith(QStringLiteral("_path")) || key.endsWith(QStringLiteral("_sound"));
	}

	QString normalizePathForStorage(const QString &value)
	{
		QString normalized = value;
		normalized.replace(QLatin1Char('\\'), QLatin1Char('/'));
		return normalized;
	}

	QString normalizePathForRuntime(const QString &value)
	{
#ifdef Q_OS_WIN
		return QDir::toNativeSeparators(value);
#else
		return normalizePathForStorage(value);
#endif
	}

	bool isAbsolutePathLike(const QString &value)
	{
		if (value.isEmpty())
			return false;
		const QChar first      = value.at(0);
		const bool  isDrive    = value.size() > 1 && value.at(1) == QLatin1Char(':') && first.isLetter();
		const bool  isUncShare = value.startsWith(QStringLiteral("//"));
		return isDrive || isUncShare || first == QLatin1Char('/') || first == QLatin1Char('\\');
	}

	QString normalizeAutoLogFileNameValue(const QString &value)
	{
		QString normalized = normalizePathForStorage(value.trimmed());
		if (normalized.isEmpty() || isAbsolutePathLike(normalized))
			return normalizePathForRuntime(normalized);

		while (normalized.startsWith(QStringLiteral("./")))
			normalized.remove(0, 2);

		return normalizePathForRuntime(normalized);
	}

	void saveXmlNumber(QTextStream &out, const QString &nl, const char *name, long long number,
	                   bool sameLine = false)
	{
		if (number)
			out << (sameLine ? "" : "   ") << name << "=\"" << number << "\"" << (sameLine ? " " : nl);
	}

	void saveXmlBoolean(QTextStream &out, const QString &nl, const char *name, bool value,
	                    bool sameLine = false)
	{
		if (value)
			out << (sameLine ? "" : "   ") << name << "=\"y\"" << (sameLine ? " " : nl);
	}

	void saveXmlString(QTextStream &out, const QString &nl, const char *name, const QString &value,
	                   bool sameLine = false)
	{
		QString output = value;
		if (isLikelyPathAttributeName(QString::fromLatin1(name)))
			output = normalizePathForStorage(output);
		if (!output.isEmpty())
			out << (sameLine ? "" : "   ") << name << "=\"" << fixHtmlString(replaceNewlines(output)) << "\""
			    << (sameLine ? " " : nl);
	}

	void saveXmlDate(QTextStream &out, const QString &nl, const char *name, const QDateTime &value)
	{
		if (value.isValid())
			saveXmlString(out, nl, name, value.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
	}

	void saveXmlDouble(QTextStream &out, const QString &nl, const char *name, double value)
	{
		QString number = QString::asprintf("%.2f", value);
		number.replace(QLatin1Char(','), QLatin1Char('.'));
		saveXmlString(out, nl, name, number, true);
	}

	void saveXmlColour(QTextStream &out, const QString &nl, const char *name, long colour,
	                   bool sameLine = false)
	{
		saveXmlString(out, nl, name, qmudColourToName(colour), sameLine);
	}

	void saveXmlMulti(QTextStream &out, const QString &nl, const char *name, const QString &value)
	{
		if (!value.isEmpty())
			out << "  <" << name << ">" << fixHtmlMultilineString(value) << "</" << name << ">" << nl;
	}

	constexpr int OPT_MULTLINE      = 0x000001;
	constexpr int OPT_PASSWORD      = 0x000004;
	constexpr int OPT_KEEP_SPACES   = 0x000002;
	constexpr int OPT_COMMAND_STACK = 0x000008;
	constexpr int OPT_WORLD_ID      = 0x000010;

	struct WorldNumericSaveOption
	{
			constexpr WorldNumericSaveOption(const char *name, const long long defaultValue)
			    : name(name), defaultValue(defaultValue), min(0), max(0), flags(0)
			{
			}

			constexpr WorldNumericSaveOption(const char *name, const long long defaultValue,
			                                 const long long min, const long long max)
			    : name(name), defaultValue(defaultValue), min(min), max(max), flags(0)
			{
			}

			constexpr WorldNumericSaveOption(const char *name, const long long defaultValue,
			                                 const long long min, const long long max, const int flags)
			    : name(name), defaultValue(defaultValue), min(min), max(max), flags(flags)
			{
			}

			const char *name;
			long long   defaultValue;
			long long   min;
			long long   max;
			int         flags;
	};

	struct WorldAlphaSaveOption
	{
			constexpr WorldAlphaSaveOption(const char *name, const char *) : name(name), flags(0)
			{
			}

			constexpr WorldAlphaSaveOption(const char *name, const char *, int flags)
			    : name(name), flags(flags)
			{
			}

			const char *name;
			int         flags;
	};
	constexpr char kNoSoundLiteral[] = "(No sound)";

#ifdef RGB
#undef RGB
#endif
	constexpr long long RGB(const int red, const int green, const int blue)
	{
		return qmudRgb(red, green, blue);
	}
	const WorldNumericSaveOption kNumericSaveOptions[] = {
#define QMUD_INCLUDE_WORLD_OPTION_DEFAULTS_NUMERIC_ROWS
#include "WorldOptionDefaultsNumeric.inc"

#undef QMUD_INCLUDE_WORLD_OPTION_DEFAULTS_NUMERIC_ROWS

	};

	const WorldAlphaSaveOption kAlphaSaveOptions[] = {
#define QMUD_INCLUDE_WORLD_OPTION_DEFAULTS_ALPHA_ROWS
#include "WorldOptionDefaultsAlpha.inc"

#undef QMUD_INCLUDE_WORLD_OPTION_DEFAULTS_ALPHA_ROWS

	};

	QString resolveWorkingDir(const QString &startupDir)
	{
		if (!startupDir.isEmpty())
			return startupDir;
		QString dir = QCoreApplication::applicationDirPath();
		if (!dir.endsWith('/'))
			dir += '/';
		return dir;
	}

	QString normalizeSeparators(const QString &input)
	{
#ifdef Q_OS_WIN
		return QDir::fromNativeSeparators(input);
#else
		QString output = input;
		output.replace(QLatin1Char('\\'), QLatin1Char('/'));
		return output;
#endif
	}

	QString appendNumberBeforeExtension(const QString &filePath, int number)
	{
		const QFileInfo info(filePath);
		const QString   dir    = info.path();
		const QString   suffix = info.completeSuffix();
		QString         stem   = info.completeBaseName();
		if (stem.isEmpty())
			stem = info.fileName();
		QString numbered = QStringLiteral("%1-%2").arg(stem).arg(number);
		if (!suffix.isEmpty())
			numbered += QStringLiteral(".%1").arg(suffix);
		if (dir.isEmpty() || dir == QStringLiteral("."))
			return numbered;
		return QDir(dir).filePath(numbered);
	}

	QString makeAbsolutePath(const QString &fileName, const QString &workingDir)
	{
		if (fileName.isEmpty())
			return fileName;

		QString normalized;
#ifdef Q_OS_WIN
		normalized = QDir::fromNativeSeparators(fileName);
#else
		normalized = normalizeSeparators(fileName);
#endif
		const bool hadTrailingSeparator =
		    normalized.endsWith(QLatin1Char('/')) || normalized.endsWith(QLatin1Char('\\'));
		const QChar first      = normalized.at(0);
		const bool  isDrive    = normalized.size() > 1 && normalized.at(1) == QChar(':') && first.isLetter();
		const bool  isAbsolute = isDrive || first == QChar('\\') || first == QChar('/');

		if (!isAbsolute)
		{
			QString relative = normalized;
			if (relative.startsWith(QStringLiteral("./")) || relative.startsWith(QStringLiteral(".\\")))
				relative = relative.mid(2);
			QString resolved = QDir(workingDir).filePath(relative);
#ifndef Q_OS_WIN
			resolved = QDir::cleanPath(resolved);
#endif
			if (hadTrailingSeparator && !resolved.endsWith(QLatin1Char('/')))
				resolved += QLatin1Char('/');
			return resolved;
		}

#ifndef Q_OS_WIN
		normalized = QDir::cleanPath(normalized);
#endif
		if (hadTrailingSeparator && !normalized.endsWith(QLatin1Char('/')))
			normalized += QLatin1Char('/');
		return normalized;
	}

	QString canonicalPortableRootName(const QString &segment)
	{
		static constexpr std::array<const char *, 8> kPortableRoots = {"worlds", "lua",   "logs",   "plugins",
		                                                               "sounds", "state", "backup", "docs"};
		for (const char *root : kPortableRoots)
		{
			if (segment.compare(QLatin1String(root), Qt::CaseInsensitive) == 0)
				return QString::fromLatin1(root);
		}
		return {};
	}

	QString collapseLeadingDuplicatePortableRoot(const QString &relativePath)
	{
		QString     cleaned  = QDir::cleanPath(relativePath);
		QStringList segments = cleaned.split(QLatin1Char('/'), Qt::SkipEmptyParts);
		if (segments.size() < 2)
			return cleaned;

		const QString root = canonicalPortableRootName(segments.at(0));
		if (root.isEmpty())
			return cleaned;

		// Guard against accidentally duplicated portable-root prefixes such as
		// "worlds/worlds/..." or "worlds/plugins/worlds/plugins/..."
		// introduced by legacy path rewrites.
		auto prefixesMatch = [&segments](const int startA, const int startB, const int count) -> bool
		{
			for (int i = 0; i < count; ++i)
			{
				if (segments.at(startA + i).compare(segments.at(startB + i), Qt::CaseInsensitive) != 0)
					return false;
			}
			return true;
		};

		bool collapsed = true;
		while (collapsed)
		{
			collapsed              = false;
			const int maxPrefixLen = safeQSizeToInt(segments.size() / 2);
			for (int prefixLen = maxPrefixLen; prefixLen >= 1; --prefixLen)
			{
				if (!prefixesMatch(0, prefixLen, prefixLen))
					continue;
				for (int removeIndex = 0; removeIndex < prefixLen; ++removeIndex)
					segments.removeAt(prefixLen);
				collapsed = true;
				break;
			}
		}
		return QDir::cleanPath(segments.join(QLatin1Char('/')));
	}

	QString extractPortableRootRelativePath(const QString &path)
	{
		QString normalized = normalizePathForStorage(path.trimmed());
		if (normalized.isEmpty())
			return {};
		if (normalized.startsWith(QLatin1Char('/')) && normalized.size() > 3 && normalized.at(1).isLetter() &&
		    normalized.at(2) == QLatin1Char(':'))
		{
			normalized.remove(0, 1);
		}

		const QStringList segments = normalized.split(QLatin1Char('/'), Qt::SkipEmptyParts);
		if (segments.isEmpty())
			return {};

		for (qsizetype i = 0; i < segments.size(); ++i)
		{
			const QString root = canonicalPortableRootName(segments.at(i));
			if (root.isEmpty())
				continue;
			QStringList relativeSegments;
			relativeSegments.reserve(segments.size() - i);
			relativeSegments.push_back(root);
			for (qsizetype j = i + 1; j < segments.size(); ++j)
				relativeSegments.push_back(segments.at(j));
			return collapseLeadingDuplicatePortableRoot(relativeSegments.join(QLatin1Char('/')));
		}
		return {};
	}

	bool hasUrlScheme(const QString &value)
	{
		const QUrl url(value);
		return url.isValid() && !url.scheme().isEmpty() && value.contains(QStringLiteral("://"));
	}

	QString toDotRelativePath(const QString &relativePath)
	{
		QString cleaned = normalizePathForStorage(relativePath.trimmed());
		if (cleaned.isEmpty())
			return {};
		cleaned = QDir::cleanPath(cleaned);
		while (cleaned.startsWith(QStringLiteral("./")))
			cleaned.remove(0, 2);
		if (cleaned.isEmpty() || cleaned == QStringLiteral("."))
			return QStringLiteral("./");
		if (cleaned.startsWith(QStringLiteral("../")))
			return cleaned;
		return QStringLiteral("./") + cleaned;
	}

	QString canonicalizePathForStorage(const QString &path, const QString &workingDir)
	{
		QString normalized = normalizePathForStorage(path.trimmed());
		if (normalized.isEmpty() || hasUrlScheme(normalized))
			return normalized;

		if (const QString portableRelative = extractPortableRootRelativePath(normalized);
		    !portableRelative.isEmpty())
		{
			return toDotRelativePath(portableRelative);
		}

		if (!isAbsolutePathLike(normalized))
			return toDotRelativePath(normalized);

		if (normalized.startsWith(QLatin1Char('/')) && normalized.size() > 3 && normalized.at(1).isLetter() &&
		    normalized.at(2) == QLatin1Char(':'))
		{
			normalized.remove(0, 1);
		}

		QString cleanedAbsolute = QDir::cleanPath(normalized);
		if (!workingDir.isEmpty())
		{
			const QString relative = QDir(workingDir).relativeFilePath(cleanedAbsolute);
			if (!relative.isEmpty() && relative != QStringLiteral("..") &&
			    !relative.startsWith(QStringLiteral("../")) && !QDir::isAbsolutePath(relative))
			{
				return toDotRelativePath(relative);
			}
		}

		return cleanedAbsolute;
	}

	QString canonicalizePathForRuntime(const QString &path, const QString &workingDir)
	{
		return normalizePathForRuntime(canonicalizePathForStorage(path, workingDir));
	}

	void normalizePathAttributeMapForStorage(QMap<QString, QString> &attributes, const QString &workingDir)
	{
		for (auto it = attributes.begin(); it != attributes.end(); ++it)
		{
			if (!isCanonicalizablePathAttributeName(it.key()))
				continue;
			if (it.value().trimmed() == QString::fromLatin1(kNoSoundLiteral))
				continue;
			it.value() = canonicalizePathForStorage(it.value(), workingDir);
			if (it.key() == QStringLiteral("auto_log_file_name"))
				it.value() = normalizeAutoLogFileNameValue(it.value());
		}
	}

	bool patternUsesPlayerDirective(const QString &pattern)
	{
		for (int i = 0; i < pattern.size(); ++i)
		{
			if (pattern.at(i) != QLatin1Char('%'))
				continue;
			if (i + 1 >= pattern.size())
				break;
			QChar next = pattern.at(++i);
			if (next == QLatin1Char('%'))
				continue;
			if (next == QLatin1Char('#') && i + 1 < pattern.size())
				next = pattern.at(++i);
			if (next == QLatin1Char('P'))
				return true;
		}
		return false;
	}

	bool isLuaScriptingEnabled(const QMap<QString, QString> &attrs)
	{
		if (!isEnabledFlag(attrs.value(QStringLiteral("enable_scripts"))))
			return false;
		const QString language = attrs.value(QStringLiteral("script_language"));
		return language.compare(QStringLiteral("Lua"), Qt::CaseInsensitive) == 0;
	}
} // namespace

WorldRuntime::WorldRuntime(QObject *parent) : QObject(parent)
{
	m_luaCallbacks = new LuaCallbackEngine();
	m_luaCallbacks->setWorldRuntime(this);
	m_socket         = new WorldSocket(this);
	m_statusTime     = QDateTime::currentDateTime();
	m_lastFlushTime  = QDateTime::currentDateTime();
	m_worldStartTime = QDateTime::currentDateTime();
	m_lineTimer.start();
	m_nextLineNumber = 1;
	m_soundBuffers.resize(kMaxSoundBuffers);
	m_scriptWatcher = new QFileSystemWatcher(this);
	connect(m_scriptWatcher, &QFileSystemWatcher::fileChanged, this,
	        [this](const QString &)
	        {
		        m_scriptFileChanged = true;
		        emit scriptFileChangedDetected();
		        if (m_scriptWatcher)
		        {
			        const QString scriptFile = m_worldAttributes.value(QStringLiteral("script_filename"));
			        if (!scriptFile.isEmpty())
			        {
				        const QString workingDir = resolveWorkingDir(m_startupDirectory);
				        const QString path       = makeAbsolutePath(scriptFile, workingDir);
				        if (!path.isEmpty() && !m_scriptWatcher->files().contains(path))
					        m_scriptWatcher->addPath(path);
			        }
		        }
	        });
	TelnetProcessor::Callbacks callbacks;
	callbacks.onTelnetRequest = [this](int number, const QString &type)
	{ return callPluginCallbacksStopOnTrue(QStringLiteral("OnPluginTelnetRequest"), number, type); };
	callbacks.onTelnetSubnegotiation = [this](int number, const QByteArray &data)
	{
		m_lastTelnetSubnegotiation = QString::fromLatin1(data);
		callPluginCallbacksWithNumberAndBytes(QStringLiteral("OnPluginTelnetSubnegotiation"), number, data);
	};
	callbacks.onTelnetOption = [this](const QByteArray &data)
	{ callPluginCallbacksWithBytes(QStringLiteral("OnPluginTelnetOption"), data); };
	callbacks.onIacGa = [this]
	{
		m_lastLineWithIacGa = m_linesReceived;
		callPluginCallbacksNoArgs(QStringLiteral("OnPluginIacGa"));
	};
	callbacks.onMxpStart = [this](bool pueblo, bool manual)
	{
		Q_UNUSED(manual);
		mxpStartUp();
		if (pueblo)
			mxpError(DBG_INFO, infoMXP_on, QStringLiteral("Pueblo turned on."));
		else
			mxpError(DBG_INFO, infoMXP_on, QStringLiteral("MXP turned on."));
	};
	callbacks.onMxpReset = [this]
	{
		if (m_mxpActive)
		{
			m_mxpTagStack.clear();
			m_mxpOpenTags.clear();
			m_mxpTextBuffer.clear();
			resetMxpRenderState();
			clearAnsiActionContext();
		}
		mxpError(DBG_INFO, infoMXP_ResetReceived, QStringLiteral("MXP reset."));
	};
	callbacks.onMxpStop = [this](bool completely, bool puebloActive)
	{
		if (!completely)
			return;
		mxpShutDown();
		if (puebloActive)
			mxpError(DBG_INFO, infoMXP_off, QStringLiteral("Pueblo turned off."));
		else
			mxpError(DBG_INFO, infoMXP_off, QStringLiteral("MXP turned off."));
	};
	callbacks.onMxpModeChange = [this](int oldMode, int newMode, bool shouldLog)
	{
		auto isLockedMode = [](const int mode) { return mode == 2 || mode == 7; };
		// Defensive state barrier: when MXP transitions to a locked/reset mode,
		// drop open render/tag state so secure link/style spans cannot leak into
		// subsequent plain text.
		if (newMode == 3 || isLockedMode(newMode))
		{
			m_mxpTagStack.clear();
			m_mxpOpenTags.clear();
			m_mxpTextBuffer.clear();
			resetMxpRenderState();
			clearAnsiActionContext();
		}

		if (!shouldLog)
			return;
		auto modeLabel = [](int mode) -> QString
		{
			switch (mode)
			{
			case 0:
				return QStringLiteral("open");
			case 1:
				return QStringLiteral("secure");
			case 2:
				return QStringLiteral("locked");
			case 3:
				return QStringLiteral("reset");
			case 4:
				return QStringLiteral("secure next tag only");
			case 5:
				return QStringLiteral("permanently open");
			case 6:
				return QStringLiteral("permanently secure");
			case 7:
				return QStringLiteral("permanently locked");
			default:
				break;
			}
			return QStringLiteral("unknown mode %1").arg(mode);
		};
		const QString from = modeLabel(oldMode);
		const QString to   = modeLabel(newMode);
		mxpError(DBG_INFO, infoMXP_ModeChange,
		         QStringLiteral("MXP mode change from '%1' to '%2'").arg(from, to));
	};
	callbacks.onMxpDiagnosticNeeded = [this](int level)
	{
		if (isLuaScriptingEnabled(m_worldAttributes) && m_luaCallbacks &&
		    !m_worldAttributes.value(QStringLiteral("on_mxp_error")).trimmed().isEmpty())
		{
			return true;
		}
		return level <= currentMxpDebugLevel(m_worldAttributes);
	};
	callbacks.onMxpDiagnostic = [this](int level, long messageNumber, const QString &message)
	{ mxpError(level, messageNumber, message); };
	callbacks.onNoEchoChanged      = [this](bool enabled) { setNoCommandEcho(enabled); };
	callbacks.onFatalProtocolError = [this](const QString &message)
	{
#ifndef NDEBUG
		const QString worldName = m_worldAttributes.value(QStringLiteral("name")).trimmed();
		qWarning().noquote() << QStringLiteral("[QMud][MCCP] fatal protocol error in world \"%1\": %2")
		                            .arg(worldName.isEmpty() ? QStringLiteral("<unnamed>") : worldName,
		                                 message.isEmpty()
		                                     ? QStringLiteral(
		                                           "Cannot process compressed output. World closed.")
		                                     : message);
#endif
		emit socketError(message.isEmpty() ? QStringLiteral("Cannot process compressed output. World closed.")
		                                   : message);
		disconnectFromWorld();
	};
	m_telnet.setCallbacks(callbacks);
	connect(m_socket, &WorldSocketService::rawDataReceived, this, &WorldRuntime::receiveRawData);
	connect(m_socket, &WorldSocketService::socketError, this,
	        [this](const QString &message)
	        {
		        // Preserve behavior: intentional disconnects should not
		        // surface connection-failure messages through UI/Lua paths.
		        if (m_disconnectOk || m_connectPhase == eConnectDisconnecting)
			        return;
		        emit socketError(message);
	        });
	connect(m_socket, &WorldSocketService::socketStateChanged, this,
	        [this](QAbstractSocket::SocketState state)
	        {
		        switch (state)
		        {
		        case QAbstractSocket::HostLookupState:
			        m_connectPhase = m_connectViaProxy ? eConnectProxyNameLookup : eConnectMudNameLookup;
			        break;
		        case QAbstractSocket::ConnectingState:
			        m_connectPhase = m_connectViaProxy ? eConnectConnectingToProxy : eConnectConnectingToMud;
			        break;
		        case QAbstractSocket::UnconnectedState:
			        if (m_connectPhase != eConnectConnectedToMud && m_connectPhase != eConnectDisconnecting)
			        {
				        m_connectViaProxy = false;
				        m_connectPhase    = eConnectNotConnected;
			        }
			        break;
		        default:
			        break;
		        }
	        });
	connect(m_socket, &WorldSocketService::connected, this,
	        [this]
	        {
		        m_mxpActive = false;
		        m_mxpTagStack.clear();
		        m_mxpOpenTags.clear();
		        m_mxpTextBuffer.clear();
		        resetAnsiRenderState();
		        m_connectViaProxy = false;
		        m_hasCachedIp     = true;
		        m_connectTime     = QDateTime::currentDateTime();
		        if (!m_tlsEncryptionEnabled)
		        {
			        finalizeSocketConnectedState();
			        return;
		        }
		        if (m_tlsMethod == eTlsDirect)
		        {
			        finalizeSocketConnectedState();
			        return;
		        }
		        if (m_tlsMethod == eTlsStartTls)
		        {
			        m_telnet.queueStartTlsNegotiation();
			        if (const QByteArray outbound = m_telnet.takeOutboundData(); !outbound.isEmpty())
				        sendToWorld(outbound);
			        startStartTlsFallbackTimer();
			        return;
		        }
	        });
	connect(m_socket, &WorldSocketService::tlsEncrypted, this,
	        [this]
	        {
		        cancelStartTlsFallbackTimer();
		        if (m_tlsEncryptionEnabled && m_tlsMethod == eTlsStartTls)
			        m_telnet.setStartTlsActive(true);
		        finalizeSocketConnectedState();
	        });
	connect(m_socket, &WorldSocketService::disconnected, this,
	        [this]
	        {
		        cancelStartTlsFallbackTimer();
		        cancelReloadMccpProbeTimeout();
		        m_reloadReattachMccpProbePending        = false;
		        m_reloadReattachMccpResumePending       = false;
		        m_reloadReattachLookProbeSent           = false;
		        m_reloadReattachMccpProbeDecisionOffset = 0;
		        m_reloadMccpProbeTimeoutPass            = 0;
		        m_reloadReattachMccpProbeBuffer.clear();
		        m_reloadReattachUseDeferredMxpReplay = false;
		        m_reloadReattachMxpProbeEvents.clear();
		        m_reloadReattachMxpProbeModeChanges.clear();
		        const bool wasConnected = m_socketReadyForWorld || m_connectPhase == eConnectDisconnecting;
		        mxpShutDown();
		        m_connectViaProxy     = false;
		        m_connectPhase        = eConnectNotConnected;
		        m_socketReadyForWorld = false;
		        m_telnet.resetConnectionState();
		        resetAnsiRenderState();
		        if (wasConnected)
			        emit disconnected();
	        });
}

WorldRuntime::~WorldRuntime()
{
	if (m_viewDestroyedConnection)
	{
		QObject::disconnect(m_viewDestroyedConnection);
		m_viewDestroyedConnection = QMetaObject::Connection{};
	}
	m_view = nullptr;

	for (auto &plugin : m_plugins)
	{
		if (plugin.lua && hasValidPluginId(plugin))
			plugin.lua->callFunctionNoArgs(QStringLiteral("OnPluginClose"));
		savePluginStateForPlugin(plugin, false, nullptr);
	}

	const QStringList dbNames = m_databases.keys();
	for (const QString &name : dbNames)
		databaseClose(name);

#if QMUD_ENABLE_SOUND
	for (SoundBuffer &buffer : m_soundBuffers)
	{
		if (buffer.effect)
		{
			buffer.effect->stop();
			delete buffer.effect;
			buffer.effect = nullptr;
		}
		if (buffer.tempFile)
		{
			delete buffer.tempFile;
			buffer.tempFile = nullptr;
		}
	}
#endif

	for (int const fontId : std::as_const(m_specialFontIds))
		QFontDatabase::removeApplicationFont(fontId);
	m_specialFontIds.clear();
	m_specialFontPaths.clear();
	m_specialFontPathOrder.clear();

	for (UdpListener &listener : m_udpListeners)
	{
		if (listener.socket)
		{
			listener.socket->close();
			delete listener.socket;
			listener.socket = nullptr;
		}
	}
	m_udpListeners.clear();
}

void WorldRuntime::addScriptTime(qint64 nanos)
{
	if (nanos <= 0)
		return;
	m_scriptTimeNanos += nanos;
}

double WorldRuntime::scriptTimeSeconds() const
{
	return static_cast<double>(m_scriptTimeNanos) / 1000000000.0;
}

void WorldRuntime::resetAnsiRenderState()
{
	m_ansiStreamState = QMudAnsiStreamState{};
	m_partialLineText.clear();
	m_partialLineSpans.clear();
	m_pendingCarriageReturnOverwrite = false;
	m_ansiRenderState                = AnsiRenderState{};
	m_streamUtf8Carry.clear();
	m_streamUtf8DecoderEnabled = false;
	resetMxpRenderState();
}

void WorldRuntime::resetMxpRenderState()
{
	m_mxpRenderStyle = MxpStyleState{};
	m_mxpRenderStack.clear();
	m_mxpRenderBlockStack.clear();
	m_mxpRenderLinkOpen = false;
	m_mxpRenderPreDepth = 0;
}

void WorldRuntime::clearAnsiActionContext()
{
	m_ansiRenderState.actionType = ActionNone;
	m_ansiRenderState.action.clear();
	m_ansiRenderState.hint.clear();
	m_ansiRenderState.variable.clear();
	m_ansiRenderState.startTag = false;
}

void WorldRuntime::receiveRawData(const QByteArray &data)
{
	if (m_incomingSocketDataPaused)
		return;

	if (data.isEmpty())
		return;

	const bool simulatedInput = m_doingSimulate;

	if (!simulatedInput)
	{
		if (AppController const *app = AppController::instance())
		{
			if (MainWindow *main = app->mainWindow())
				main->checkTimerFallback();
		}
	}

	const unsigned short previousActionSource = m_currentActionSource;
	m_currentActionSource                     = eInputFromServer;

	const QString utf8Flag  = m_worldAttributes.value(QStringLiteral("utf_8"));
	const bool    useUtf8   = isEnabledFlag(utf8Flag);
	const QString gaFlag    = m_worldAttributes.value(QStringLiteral("convert_ga_to_newline"));
	const bool    convertGA = isEnabledFlag(gaFlag);
	const bool    carriageReturnClears =
	    isEnabledFlag(m_worldAttributes.value(QStringLiteral("carriage_return_clears_line")));
	const QString noEchoOffFlag          = m_worldAttributes.value(QStringLiteral("no_echo_off"));
	const bool    noEchoOff              = isEnabledFlag(noEchoOffFlag);
	const QString disableCompressionFlag = m_worldAttributes.value(QStringLiteral("disable_compression"));
	const bool    disableCompression     = isEnabledFlag(disableCompressionFlag);
	const bool    negotiateOptionsOnce =
	    isEnabledFlag(m_worldAttributes.value(QStringLiteral("only_negotiate_telnet_options_once")));
	const int     useMxp     = m_worldAttributes.value(QStringLiteral("use_mxp")).toInt();
	const QString terminalId = m_worldAttributes.value(QStringLiteral("terminal_identification"));

	m_telnet.setUseUtf8(useUtf8);
	m_telnet.setConvertGAtoNewline(convertGA);
	m_telnet.setNoEchoOff(noEchoOff);
	m_telnet.setDisableCompression(disableCompression);
	m_telnet.setNegotiateOptionsOnce(negotiateOptionsOnce);
	if (useMxp >= 0)
		m_telnet.setUseMxp(useMxp);
	updateTelnetWindowSizeForNaws();
	if (!terminalId.isEmpty())
		m_telnet.setTerminalIdentification(terminalId);

	if (!simulatedInput && m_reloadReattachMccpProbePending)
	{
		// Probe quarantine mode: keep telnet state/protocol negotiation current,
		// but defer all output/plugin processing until keep/reconnect decision.
		QByteArray                            processed                 = m_telnet.processBytes(data);
		QList<TelnetProcessor::MxpEvent>      mxpEventsDuringProbe      = m_telnet.takeMxpEvents();
		QList<TelnetProcessor::MxpModeChange> mxpModeChangesDuringProbe = m_telnet.takeMxpModeChanges();
		QByteArray const                      outbound                  = m_telnet.takeOutboundData();
		if (!outbound.isEmpty())
			sendToWorld(outbound);
		if (m_tlsEncryptionEnabled && m_tlsMethod == eTlsStartTls &&
		    m_telnet.takeStartTlsNegotiationRejected() && !m_socketReadyForWorld)
		{
			cancelStartTlsFallbackTimer();
			outputText(QStringLiteral("TLS Encryption NOT enabled: Server denied."), true, true);
			m_telnet.setStartTlsEnabled(false);
			finalizeSocketConnectedState();
		}
		if (m_telnet.takeStartTlsUpgradeRequest())
		{
			cancelStartTlsFallbackTimer();
			if (!beginStartTlsUpgrade())
			{
				m_currentActionSource = previousActionSource;
				return;
			}
		}
		if (!mxpEventsDuringProbe.isEmpty() || !mxpModeChangesDuringProbe.isEmpty())
		{
			const qsizetype baseOffset = m_reloadReattachMccpProbeBuffer.size();
			for (TelnetProcessor::MxpEvent &ev : mxpEventsDuringProbe)
				ev.offset = safeQSizeToInt(baseOffset + static_cast<qsizetype>(qMax(ev.offset, 0)));
			for (TelnetProcessor::MxpModeChange &change : mxpModeChangesDuringProbe)
				change.offset = safeQSizeToInt(baseOffset + static_cast<qsizetype>(qMax(change.offset, 0)));
			m_reloadReattachMxpProbeEvents.append(mxpEventsDuringProbe);
			m_reloadReattachMxpProbeModeChanges.append(mxpModeChangesDuringProbe);
			m_reloadReattachUseDeferredMxpReplay = true;
		}
		const ReloadMccpProbeIngressInput ingressInput{
		    simulatedInput,
		    m_reloadReattachMccpProbePending,
		    data.size(),
		    processed,
		};
		(void)ingestReloadMccpProbeChunk(ingressInput, &m_bytesIn, &m_inputPacketCount,
		                                 &m_reloadReattachMccpProbeBuffer);
		m_currentActionSource = previousActionSource;
		return;
	}

	if (!simulatedInput)
	{
		m_bytesIn += data.size();
		m_inputPacketCount++;
	}
	if (m_debugIncomingPackets)
	{
		auto appendPacketDebug = [this](const QString &caption, const QByteArray &packet, qint64 number)
		{
			const QString worldName = m_worldAttributes.value(QStringLiteral("name"));
			const QString title     = QStringLiteral("Packet debug - %1")
			                          .arg(worldName.isEmpty() ? QStringLiteral("World") : worldName);
			const QDateTime now = QDateTime::currentDateTime();
			const QString   timestamp =
			    QLocale::system().toString(now, QStringLiteral("dddd, MMMM dd, yyyy, h:mm:ss AP"));
			QString const header = QStringLiteral("\r\n%1 packet: %2 (%3 bytes) at %4\r\n\r\n")
			                           .arg(caption)
			                           .arg(number)
			                           .arg(packet.size())
			                           .arg(timestamp);
			if (!callPluginCallbacksStopOnTrueWithString(QStringLiteral("OnPluginPacketDebug"), header))
			{
				if (AppController const *app = AppController::instance())
					if (MainWindow *main = app->mainWindow())
						main->appendToNotepad(title, header, false, this);
			}

			int remaining = safeQSizeToInt(packet.size());
			int offset    = 0;
			while (remaining > 0)
			{
				const int count = qMin(remaining, kPacketDebugChars);
				QString   ascii;
				QString   hex;
				ascii.reserve(kPacketDebugChars);
				hex.reserve(kPacketDebugChars * 3);
				for (int i = 0; i < count; ++i)
				{
					const auto byte = static_cast<unsigned char>(packet.at(offset + i));
					ascii += (isAsciiPrintableByte(byte) ? QChar(byte) : QChar('.'));
					hex += QString::asprintf(" %02x", byte);
				}
				const QString line = QStringLiteral("%1%2%3\r\n")
				                         .arg(ascii.leftJustified(kPacketDebugChars, QLatin1Char(' ')))
				                         .arg(QLatin1Char(' '))
				                         .arg(hex);
				if (!callPluginCallbacksStopOnTrueWithString(QStringLiteral("OnPluginPacketDebug"), line))
				{
					if (AppController const *app = AppController::instance())
						if (MainWindow *main = app->mainWindow())
							main->appendToNotepad(title, line, false, this);
				}
				remaining -= count;
				offset += count;
			}
		};
		appendPacketDebug(QStringLiteral("Incoming"), data, m_inputPacketCount);
	}

	// Legacy behavior: packet callbacks run on telnet-processed bytes, not on the
	// raw compressed stream.
	QByteArray processed = simulatedInput ? data : m_telnet.processBytes(data);
	callPluginCallbacksTransformBytes(QStringLiteral("OnPluginPacketReceived"), processed);
	if (processed.contains('\0'))
	{
		QByteArray sanitized;
		sanitized.reserve(processed.size());
		for (const char ch : processed)
		{
			if (ch != '\0')
				sanitized.append(ch);
		}
		processed.swap(sanitized);
	}

	// Port of the ReceiveMsg/DisplayMsg line handling:
	// - pass data through telnet/MCCP phase processing
	// - accumulate raw bytes
	// - split on LF
	// - trim CR
	if (const bool detectPueblo = isEnabledFlag(m_worldAttributes.value(QStringLiteral("detect_pueblo")));
	    detectPueblo && !m_mxpActive && !m_telnet.isPuebloActive())
	{
		static const auto kPuebloStart = QByteArrayLiteral("</xch_mudtext>");
		if (const qsizetype marker = processed.indexOf(kPuebloStart); marker >= 0)
		{
			m_telnet.activatePuebloMode();
			processed.remove(marker, kPuebloStart.size());
			if (marker < processed.size())
			{
				if (marker + 1 < processed.size() && processed.at(marker) == '\r' &&
				    processed.at(marker + 1) == '\n')
					processed.remove(marker, 2);
				else if (processed.at(marker) == '\n')
					processed.remove(marker, 1);
			}
		}
	}
	int bellCount = 0;
	for (char const ch : processed)
	{
		if (static_cast<unsigned char>(ch) == 0x07)
			++bellCount;
	}
	if (bellCount > 0)
	{
		QByteArray filtered;
		filtered.reserve(processed.size());
		for (char const ch : processed)
		{
			if (static_cast<unsigned char>(ch) != 0x07)
				filtered.append(ch);
		}
		processed = filtered;

		if (const bool enableBeeps = isEnabledFlag(m_worldAttributes.value(QStringLiteral("enable_beeps")));
		    enableBeeps)
		{
			const QString beepSound = m_worldAttributes.value(QStringLiteral("beep_sound")).trimmed();
			if (!beepSound.isEmpty())
			{
				for (int i = 0; i < bellCount; ++i)
					playSound(0, beepSound, false, 0.0, 0.0);
			}
			else
			{
				for (int i = 0; i < bellCount; ++i)
					QApplication::beep();
			}
		}
	}
	QByteArray const outbound = m_telnet.takeOutboundData();
	if (!outbound.isEmpty())
		sendToWorld(outbound);
	if (m_tlsEncryptionEnabled && m_tlsMethod == eTlsStartTls && m_telnet.takeStartTlsNegotiationRejected() &&
	    !m_socketReadyForWorld)
	{
		cancelStartTlsFallbackTimer();
		outputText(QStringLiteral("TLS Encryption NOT enabled: Server denied."), true, true);
		m_telnet.setStartTlsEnabled(false);
		finalizeSocketConnectedState();
	}
	if (m_telnet.takeStartTlsUpgradeRequest())
	{
		cancelStartTlsFallbackTimer();
		if (!beginStartTlsUpgrade())
		{
			m_currentActionSource = previousActionSource;
			return;
		}
	}
	if (processed.isEmpty())
	{
		if (simulatedInput && m_reloadReattachUseDeferredMxpReplay)
		{
			m_reloadReattachUseDeferredMxpReplay = false;
			m_reloadReattachMxpProbeEvents.clear();
			m_reloadReattachMxpProbeModeChanges.clear();
		}
		m_currentActionSource = previousActionSource;
		return;
	}

	if (useUtf8 != m_streamUtf8DecoderEnabled)
	{
		m_streamUtf8DecoderEnabled = useUtf8;
		m_streamUtf8Carry.clear();
	}
	auto decodeIncomingDisplayBytes = [&](const QByteArrayView bytes) -> QString
	{
		if (bytes.isEmpty())
			return {};
		if (useUtf8)
		{
			bool          hadInvalidBytes = false;
			const QString decoded =
			    qmudDecodeUtf8WithWindows1252Fallback(bytes, m_streamUtf8Carry, &hadInvalidBytes);
			if (hadInvalidBytes)
				++m_utf8ErrorCount;
			return decoded;
		}
		return qmudDecodeWindows1252(bytes);
	};
	auto decodeIncomingIsolatedBytes = [&](const QByteArray &bytes) -> QString
	{
		if (bytes.isEmpty())
			return {};
		if (useUtf8)
		{
			QStringDecoder decoder(QStringConverter::Utf8);
			const QString  decoded = decoder.decode(bytes);
			if (decoder.hasError())
				++m_utf8ErrorCount;
			return decoded;
		}
		return qmudDecodeWindows1252(bytes);
	};

	QList<TelnetProcessor::MxpEvent>      events;
	QList<TelnetProcessor::MxpModeChange> modeChanges;
	if (simulatedInput && m_reloadReattachUseDeferredMxpReplay)
	{
		events.swap(m_reloadReattachMxpProbeEvents);
		modeChanges.swap(m_reloadReattachMxpProbeModeChanges);
		m_reloadReattachUseDeferredMxpReplay = false;
	}
	else
	{
		events      = m_telnet.takeMxpEvents();
		modeChanges = m_telnet.takeMxpModeChanges();
	}
	auto resetMxpTrackingState = [this]
	{
		m_mxpTagStack.clear();
		m_mxpOpenTags.clear();
		m_mxpTextBuffer.clear();
		resetMxpRenderState();
		clearAnsiActionContext();
	};
	const bool hasActiveMxpParserContext = !m_mxpTagStack.isEmpty() || !m_mxpRenderStack.isEmpty() ||
	                                       !m_mxpRenderBlockStack.isEmpty() || m_mxpRenderLinkOpen;
	const bool keepMxpTextBuffer = !events.isEmpty() || !modeChanges.isEmpty() || hasActiveMxpParserContext;
	int        mxpBaseOffset     = 0;
	if (keepMxpTextBuffer)
	{
		mxpBaseOffset = safeQSizeToInt(m_mxpTextBuffer.size());
		m_mxpTextBuffer.append(processed);
		if (m_mxpTextBuffer.size() > kMaxMxpTextBufferBytes)
		{
			mxpError(DBG_WARNING, wrnMXP_TextBufferLimitExceeded,
			         QStringLiteral("MXP text buffer exceeded %1 bytes; resetting MXP context.")
			             .arg(kMaxMxpTextBufferBytes));
			resetMxpTrackingState();
			events.clear();
			mxpBaseOffset = 0;
		}
	}
	else
	{
		// No active MXP frames and no MXP events in this packet: avoid unbounded
		// growth from plain-text traffic.
		m_mxpTextBuffer.clear();
	}
	if (!events.isEmpty() || !modeChanges.isEmpty())
		mxpStartUp();
	const bool luaEnabled = isLuaScriptingEnabled(m_worldAttributes);

	auto       parseColorValue = [](const QString &value) -> QColor
	{
		if (value.isEmpty())
			return {};
		QColor color(value);
		if (color.isValid())
			return color;
		bool      ok      = false;
		const int numeric = value.toInt(&ok);
		if (!ok)
			return {};
		const int r = numeric & 0xFF;
		const int g = (numeric >> 8) & 0xFF;
		const int b = (numeric >> 16) & 0xFF;
		return {r, g, b};
	};

	QVector<QColor> normalAnsi(8);
	QVector<QColor> boldAnsi(8);
	QVector<QColor> customText(16);
	QVector<QColor> customBack(16);
	normalAnsi[0] = QColor(0, 0, 0);
	normalAnsi[1] = QColor(128, 0, 0);
	normalAnsi[2] = QColor(0, 128, 0);
	normalAnsi[3] = QColor(128, 128, 0);
	normalAnsi[4] = QColor(0, 0, 128);
	normalAnsi[5] = QColor(128, 0, 128);
	normalAnsi[6] = QColor(0, 128, 128);
	normalAnsi[7] = QColor(192, 192, 192);
	boldAnsi[0]   = QColor(128, 128, 128);
	boldAnsi[1]   = QColor(255, 0, 0);
	boldAnsi[2]   = QColor(0, 255, 0);
	boldAnsi[3]   = QColor(255, 255, 0);
	boldAnsi[4]   = QColor(0, 0, 255);
	boldAnsi[5]   = QColor(255, 0, 255);
	boldAnsi[6]   = QColor(0, 255, 255);
	boldAnsi[7]   = QColor(255, 255, 255);
	for (int i = 0; i < customText.size(); ++i)
	{
		customText[i] = QColor(255, 255, 255);
		customBack[i] = QColor(0, 0, 0);
	}
	customText[0]  = QColor(255, 128, 128);
	customText[1]  = QColor(255, 255, 128);
	customText[2]  = QColor(128, 255, 128);
	customText[3]  = QColor(128, 255, 255);
	customText[4]  = QColor(0, 128, 255);
	customText[5]  = QColor(255, 128, 192);
	customText[6]  = QColor(255, 0, 0);
	customText[7]  = QColor(0, 128, 192);
	customText[8]  = QColor(255, 0, 255);
	customText[9]  = QColor(128, 64, 64);
	customText[10] = QColor(255, 128, 64);
	customText[11] = QColor(0, 128, 128);
	customText[12] = QColor(0, 64, 128);
	customText[13] = QColor(255, 0, 128);
	customText[14] = QColor(0, 128, 0);
	customText[15] = QColor(0, 0, 255);

	for (const auto &colour : m_colours)
	{
		const QString group = colour.group.trimmed().toLower();
		bool          ok    = false;
		const int     seq   = colour.attributes.value(QStringLiteral("seq")).toInt(&ok);
		const int     index = ok ? seq - 1 : -1;
		if (index < 0)
			continue;
		if (group == QStringLiteral("ansi/normal") && index < normalAnsi.size())
		{
			const QColor rgb = parseColorValue(colour.attributes.value(QStringLiteral("rgb")));
			if (rgb.isValid())
				normalAnsi[index] = rgb;
		}
		else if (group == QStringLiteral("ansi/bold") && index < boldAnsi.size())
		{
			const QColor rgb = parseColorValue(colour.attributes.value(QStringLiteral("rgb")));
			if (rgb.isValid())
				boldAnsi[index] = rgb;
		}
		else if ((group == QStringLiteral("custom/custom") || group == QStringLiteral("custom")) &&
		         index < customText.size())
		{
			const QColor text = parseColorValue(colour.attributes.value(QStringLiteral("text")));
			const QColor back = parseColorValue(colour.attributes.value(QStringLiteral("back")));
			if (text.isValid())
				customText[index] = text;
			if (back.isValid())
				customBack[index] = back;
		}
	}

	const bool custom16Default =
	    isEnabledFlag(m_worldAttributes.value(QStringLiteral("custom_16_is_default_colour")));
	const bool ignoreMxpColourChanges =
	    isEnabledFlag(m_worldAttributes.value(QStringLiteral("ignore_mxp_colour_changes")));
	QColor defaultForeColor = parseColorValue(m_worldAttributes.value(QStringLiteral("output_text_colour")));
	QColor defaultBackColor =
	    parseColorValue(m_worldAttributes.value(QStringLiteral("output_background_colour")));
	if (!defaultForeColor.isValid())
		defaultForeColor = custom16Default ? customText.value(15) : normalAnsi.value(7);
	if (!defaultBackColor.isValid())
		defaultBackColor = custom16Default ? customBack.value(15) : normalAnsi.value(0);

	const QString defaultFore = defaultForeColor.isValid() ? defaultForeColor.name() : QString();
	const QString defaultBack = defaultBackColor.isValid() ? defaultBackColor.name() : QString();

	auto          emitCompletedLine = [&](QString &lineText, QVector<StyleSpan> &lineSpans)
	{
		applyMappingFailureIfMatched(*this, lineText);
		if (!m_active)
			incrementNewLines();
		emit incomingStyledLineReceived(lineText, lineSpans);
		emit incomingLineReceived(lineText);
		m_linesReceived++;
		m_newlinesReceived++;
		lineText.clear();
		lineSpans.clear();
	};

	const bool hasActiveMxpRenderContext =
	    !m_mxpRenderStack.isEmpty() || !m_mxpRenderBlockStack.isEmpty() || m_mxpRenderLinkOpen;

	if (events.isEmpty() && modeChanges.isEmpty() && !hasActiveMxpRenderContext)
	{
		AnsiRenderState current = m_ansiRenderState;
		if (current.fore.isEmpty())
			current.fore = defaultFore;
		if (current.back.isEmpty())
			current.back = defaultBack;

		QString            lineText              = m_partialLineText;
		QVector<StyleSpan> lineSpans             = m_partialLineSpans;
		bool               carriageReturnPending = m_pendingCarriageReturnOverwrite;

		auto               colorFromIndex = [](int idx) -> QString
		{
			if (idx < 0 || idx >= 256)
				return {};
			const AppController *app = AppController::instance();
			const QMudColorRef   ref = app ? app->xtermColorAt(idx) : qmudRgb(0, 0, 0);
			return QColor(qmudRed(ref), qmudGreen(ref), qmudBlue(ref)).name();
		};

		auto appendDecodedSegment = [&](const QString &decoded, const QMudStyledTextState &segmentState)
		{
			QString rawSegment = decoded;
			// Avoid treating Unicode separator controls as visual line breaks.
			rawSegment.replace(QChar(0x0085), QLatin1Char(' ')); // NEL
			rawSegment.replace(QChar(0x2028), QLatin1Char(' ')); // LINE SEPARATOR
			rawSegment.replace(QChar(0x2029), QLatin1Char(' ')); // PARAGRAPH SEPARATOR
			if (rawSegment.isEmpty())
			{
				return;
			}

			const int segmentLength = safeQSizeToInt(rawSegment.size());
			bool      startTag      = segmentState.startTag;
			auto      backspaceOne  = [&]
			{
				if (lineText.isEmpty())
					return;
				lineText.chop(1);
				while (!lineSpans.isEmpty())
				{
					StyleSpan &lastSpan = lineSpans.last();
					if (lastSpan.length > 1)
					{
						lastSpan.length -= 1;
						return;
					}
					lineSpans.pop_back();
				}
			};
			auto appendRun = [&](int start, int length)
			{
				if (length <= 0)
					return;
				lineText += rawSegment.mid(start, length);
				StyleSpan span;
				span.length     = length;
				span.fore       = segmentState.fore.isEmpty() ? QColor() : QColor(segmentState.fore);
				span.back       = segmentState.back.isEmpty() ? QColor() : QColor(segmentState.back);
				span.bold       = segmentState.bold;
				span.italic     = segmentState.italic;
				span.blink      = segmentState.blink;
				span.underline  = segmentState.underline;
				span.inverse    = segmentState.inverse;
				span.strike     = segmentState.strike;
				span.actionType = segmentState.actionType;
				span.action     = segmentState.action;
				span.hint       = segmentState.hint;
				span.variable   = segmentState.variable;
				span.startTag   = startTag;
				startTag        = false;
				if (!lineSpans.isEmpty())
				{
					StyleSpan &lastSpan = lineSpans.last();
					if (lastSpan.fore == span.fore && lastSpan.back == span.back &&
					    lastSpan.bold == span.bold && lastSpan.italic == span.italic &&
					    lastSpan.blink == span.blink && lastSpan.underline == span.underline &&
					    lastSpan.inverse == span.inverse && lastSpan.strike == span.strike &&
					    lastSpan.changed == span.changed && lastSpan.actionType == span.actionType &&
					    lastSpan.action == span.action && lastSpan.hint == span.hint &&
					    lastSpan.variable == span.variable && lastSpan.startTag == span.startTag)
						lastSpan.length += span.length;
					else
						lineSpans.push_back(span);
				}
				else
					lineSpans.push_back(span);
			};

			int runStart = -1;
			for (int i = 0; i < segmentLength; ++i)
			{
				const QChar ch = rawSegment.at(i);
				if (carriageReturnPending && ch != QLatin1Char('\n') && ch != QLatin1Char('\r'))
				{
					if (carriageReturnClears)
					{
						lineText.clear();
						lineSpans.clear();
					}
					carriageReturnPending = false;
				}
				if (ch == QLatin1Char('\n') || ch == QLatin1Char('\r'))
				{
					if (runStart >= 0)
					{
						appendRun(runStart, i - runStart);
						runStart = -1;
					}

					if (ch == QLatin1Char('\r'))
					{
						const bool nextIsNewline =
						    (i + 1 < segmentLength && rawSegment.at(i + 1) == QLatin1Char('\n'));
						if (!nextIsNewline)
							carriageReturnPending = carriageReturnClears;
						continue;
					}

					carriageReturnPending = false;
					emitCompletedLine(lineText, lineSpans);
					continue;
				}
				if (ch == QLatin1Char('\b'))
				{
					if (runStart >= 0)
					{
						appendRun(runStart, i - runStart);
						runStart = -1;
					}
					backspaceOne();
					continue;
				}

				if (runStart < 0)
					runStart = i;
			}

			if (runStart >= 0)
				appendRun(runStart, segmentLength - runStart);
		};

		auto normalAnsiColorFromIndex = [&](const int idx) -> QString
		{
			if (idx < 0 || idx >= normalAnsi.size())
				return {};
			return normalAnsi.at(idx).name();
		};
		auto boldAnsiColorFromIndex = [&](const int idx) -> QString
		{
			if (idx < 0 || idx >= boldAnsi.size())
				return {};
			return boldAnsi.at(idx).name();
		};

		QMudStyledTextState ansiState;
		ansiState.bold       = current.bold;
		ansiState.underline  = current.underline;
		ansiState.italic     = current.italic;
		ansiState.blink      = current.blink;
		ansiState.strike     = current.strike;
		ansiState.inverse    = current.inverse;
		ansiState.fore       = current.fore;
		ansiState.back       = current.back;
		ansiState.actionType = current.actionType;
		ansiState.action     = current.action;
		ansiState.hint       = current.hint;
		ansiState.variable   = current.variable;
		ansiState.startTag   = current.startTag;
		ansiState.monospace  = current.monospace;

		constexpr QMudOscActionIds     oscActionIds{ActionNone, ActionSend, ActionPrompt, ActionHyperlink};
		const QVector<QMudStyledChunk> chunks = qmudParseAnsiSgrChunks(
		    processed, m_ansiStreamState, defaultFore, defaultBack, normalAnsiColorFromIndex,
		    boldAnsiColorFromIndex, colorFromIndex, decodeIncomingDisplayBytes, ansiState, oscActionIds);
		for (const QMudStyledChunk &chunk : chunks)
			appendDecodedSegment(chunk.text, chunk.state);

		current.bold       = ansiState.bold;
		current.underline  = ansiState.underline;
		current.italic     = ansiState.italic;
		current.blink      = ansiState.blink;
		current.strike     = ansiState.strike;
		current.inverse    = ansiState.inverse;
		current.fore       = ansiState.fore;
		current.back       = ansiState.back;
		current.actionType = ansiState.actionType;
		current.action     = ansiState.action;
		current.hint       = ansiState.hint;
		current.variable   = ansiState.variable;
		current.startTag   = ansiState.startTag;
		current.monospace  = ansiState.monospace;

		m_ansiRenderState                = current;
		m_partialLineText                = lineText;
		m_partialLineSpans               = lineSpans;
		m_pendingCarriageReturnOverwrite = carriageReturnPending;
		emit incomingStyledLinePartialReceived(lineText, lineSpans);
		if (m_mxpTagStack.isEmpty() && !hasActiveMxpRenderContext)
			m_mxpTextBuffer.clear();
		m_currentActionSource = previousActionSource;
		return;
	}
	const QString openTagCallback =
	    luaEnabled ? m_worldAttributes.value(QStringLiteral("on_mxp_open_tag")).trimmed() : QString();
	const QString closeTagCallback =
	    luaEnabled ? m_worldAttributes.value(QStringLiteral("on_mxp_close_tag")).trimmed() : QString();
	const QString setVarCallback =
	    luaEnabled ? m_worldAttributes.value(QStringLiteral("on_mxp_set_variable")).trimmed() : QString();
	{
		QList<TelnetProcessor::MxpEvent> sorted = events;
		std::ranges::sort(sorted,
		                  [](const TelnetProcessor::MxpEvent &a, const TelnetProcessor::MxpEvent &b)
		                  {
			                  if (a.offset != b.offset)
				                  return a.offset < b.offset;
			                  return a.sequence < b.sequence;
		                  });
		QList<TelnetProcessor::MxpModeChange> sortedModeChanges = modeChanges;
		std::ranges::sort(sortedModeChanges,
		                  [](const TelnetProcessor::MxpModeChange &a, const TelnetProcessor::MxpModeChange &b)
		                  {
			                  if (a.offset != b.offset)
				                  return a.offset < b.offset;
			                  return a.sequence < b.sequence;
		                  });

		MxpStyleState          current               = m_mxpRenderStyle;
		QVector<MxpStyleFrame> stack                 = m_mxpRenderStack;
		QVector<QByteArray>    blockStack            = m_mxpRenderBlockStack;
		bool                   linkOpen              = m_mxpRenderLinkOpen;
		int                    preDepth              = m_mxpRenderPreDepth;
		QString                lineText              = m_partialLineText;
		QVector<StyleSpan>     lineSpans             = m_partialLineSpans;
		bool                   carriageReturnPending = m_pendingCarriageReturnOverwrite;
		bool                   mxpTrackingReset{false};
		auto                   restoreCurrentFromAnsiState = [&]
		{
			current.bold       = m_ansiRenderState.bold;
			current.underline  = m_ansiRenderState.underline;
			current.italic     = m_ansiRenderState.italic;
			current.blink      = m_ansiRenderState.blink;
			current.strike     = m_ansiRenderState.strike;
			current.monospace  = m_ansiRenderState.monospace;
			current.inverse    = m_ansiRenderState.inverse;
			current.fore       = m_ansiRenderState.fore;
			current.back       = m_ansiRenderState.back;
			current.actionType = m_ansiRenderState.actionType;
			current.action     = m_ansiRenderState.action;
			current.hint       = m_ansiRenderState.hint;
			current.variable   = m_ansiRenderState.variable;
			current.startTag   = m_ansiRenderState.startTag;
			if (current.fore.isEmpty())
				current.fore = defaultFore;
			if (current.back.isEmpty())
				current.back = defaultBack;
		};
		auto resetMxpTrackingForOverflow = [&](const QString &reason)
		{
			if (mxpTrackingReset)
				return;
			mxpError(DBG_WARNING, wrnMXP_TagStackLimitExceeded, reason);
			m_mxpOpenTags.clear();
			m_mxpTagStack.clear();
			m_mxpTextBuffer.clear();
			clearAnsiActionContext();
			restoreCurrentFromAnsiState();
			stack.clear();
			blockStack.clear();
			linkOpen         = false;
			preDepth         = 0;
			mxpTrackingReset = true;
		};
		auto ensureOpenTagCapacity = [&]()
		{
			if (m_mxpOpenTags.size() >= kMaxMxpStackDepth)
			{
				resetMxpTrackingForOverflow(
				    QStringLiteral("MXP open-tag stack exceeded %1 entries; resetting MXP context.")
				        .arg(kMaxMxpStackDepth));
				return false;
			}
			return true;
		};
		auto ensureTagFrameCapacity = [&]()
		{
			if (m_mxpTagStack.size() >= kMaxMxpStackDepth)
			{
				resetMxpTrackingForOverflow(
				    QStringLiteral("MXP tag frame stack exceeded %1 entries; resetting MXP context.")
				        .arg(kMaxMxpStackDepth));
				return false;
			}
			return true;
		};
		auto ensureRenderStackCapacity = [&](const QByteArray &tag)
		{
			if (stack.size() >= kMaxMxpStackDepth)
			{
				resetMxpTrackingForOverflow(
				    QStringLiteral("MXP render stack exceeded %1 entries near <%2>; resetting MXP context.")
				        .arg(kMaxMxpStackDepth)
				        .arg(QString::fromLatin1(tag)));
				return false;
			}
			return true;
		};
		auto ensureRenderBlockCapacity = [&](const QByteArray &tag)
		{
			if (blockStack.size() >= kMaxMxpStackDepth)
			{
				resetMxpTrackingForOverflow(
				    QStringLiteral(
				        "MXP block-tag stack exceeded %1 entries near <%2>; resetting MXP context.")
				        .arg(kMaxMxpStackDepth)
				        .arg(QString::fromLatin1(tag)));
				return false;
			}
			return true;
		};
		const bool hasPersistedMxpRenderContext =
		    !stack.isEmpty() || !blockStack.isEmpty() || linkOpen || preDepth > 0;
		if (!hasPersistedMxpRenderContext)
			restoreCurrentFromAnsiState();

		static const QRegularExpression kHexColor6(QStringLiteral("^[0-9A-Fa-f]{6}$"));
		auto                            normalizeColorBytes = [](const QByteArray &value) -> QString
		{
			QString name = QString::fromLocal8Bit(value).trimmed();
			if (name.isEmpty())
				return {};
			if (kHexColor6.match(name).hasMatch())
				name.prepend(QLatin1Char('#'));
			QColor const color(name);
			if (!color.isValid())
				return {};
			return color.name();
		};

		auto normalizeColorText = [](const QString &value) -> QString
		{
			QString name = value.trimmed();
			if (name.isEmpty())
				return {};
			if (kHexColor6.match(name).hasMatch())
				name.prepend(QLatin1Char('#'));
			QColor const color(name);
			if (!color.isValid())
				return {};
			return color.name();
		};
		auto mxpTagsEquivalent = [](const QByteArray &lhs, const QByteArray &rhs)
		{
			if (lhs == rhs)
				return true;
			return (lhs == "send" && rhs == "a") || (lhs == "a" && rhs == "send");
		};

		int  last = 0;

		auto applyStartTag =
		    [&](const QByteArray &activeTag, const QMap<QByteArray, QByteArray> &activeAttributes)
		{
			const QByteArray unnamedForeLocal = activeAttributes.value("1");
			const QByteArray unnamedBackLocal = activeAttributes.value("2");

			if (activeTag == "send" || activeTag == "a")
			{
				QString href = QString::fromLocal8Bit(activeAttributes.value("href"));
				if (href.isEmpty())
					href = QString::fromLocal8Bit(activeAttributes.value("xch_cmd"));
				QString xchPrompt = QString::fromLocal8Bit(activeAttributes.value("xch_prompt"));
				if (xchPrompt.isEmpty())
					xchPrompt = QString::fromLocal8Bit(activeAttributes.value("prompt"));
				QString hint = QString::fromLocal8Bit(activeAttributes.value("hint"));
				if (hint.isEmpty())
					hint = QString::fromLocal8Bit(activeAttributes.value("xch_hint"));
				linkOpen = true;
				if (!ensureRenderStackCapacity(activeTag))
					return;
				stack.push_back({activeTag, current});
				if (activeTag == "a")
					current.actionType = ActionHyperlink;
				else if (!xchPrompt.isEmpty())
					current.actionType = ActionPrompt;
				else
					current.actionType = ActionSend;
				current.action   = href;
				current.hint     = hint.isEmpty() ? xchPrompt : hint;
				QString variable = QString::fromLocal8Bit(activeAttributes.value("xch_set"));
				if (variable.isEmpty())
					variable = QString::fromLocal8Bit(activeAttributes.value("variable"));
				if (variable.isEmpty())
					variable = QString::fromLocal8Bit(activeAttributes.value("set"));
				current.variable = variable;
				current.startTag = true;
			}
			else if (activeTag == "bold" || activeTag == "b" || activeTag == "strong" || activeTag == "h1" ||
			         activeTag == "h2" || activeTag == "h3" || activeTag == "h4" || activeTag == "h5" ||
			         activeTag == "h6")
			{
				if (!ensureRenderStackCapacity(activeTag))
					return;
				stack.push_back({activeTag, current});
				current.bold = true;
			}
			else if (activeTag == "underline" || activeTag == "u")
			{
				if (!ensureRenderStackCapacity(activeTag))
					return;
				stack.push_back({activeTag, current});
				current.underline = true;
			}
			else if (activeTag == "italic" || activeTag == "i" || activeTag == "em")
			{
				if (!ensureRenderStackCapacity(activeTag))
					return;
				stack.push_back({activeTag, current});
				current.italic = true;
			}
			else if (activeTag == "strike" || activeTag == "s")
			{
				if (!ensureRenderStackCapacity(activeTag))
					return;
				stack.push_back({activeTag, current});
				current.strike = true;
			}
			else if (activeTag == "color")
			{
				if (!ensureRenderStackCapacity(activeTag))
					return;
				stack.push_back({activeTag, current});
				if (!ignoreMxpColourChanges)
				{
					QString fore = normalizeColorBytes(activeAttributes.value("fore"));
					if (fore.isEmpty())
						fore = normalizeColorBytes(unnamedForeLocal);
					QString back = normalizeColorBytes(activeAttributes.value("back"));
					if (back.isEmpty())
						back = normalizeColorBytes(unnamedBackLocal);
					if (!fore.isEmpty())
						current.fore = fore;
					if (!back.isEmpty())
						current.back = back;
				}
			}
			else if (activeTag == "c")
			{
				if (!ensureRenderStackCapacity(activeTag))
					return;
				stack.push_back({activeTag, current});
				if (!ignoreMxpColourChanges)
				{
					QString const fore = normalizeColorBytes(unnamedForeLocal);
					QString const back = normalizeColorBytes(unnamedBackLocal);
					if (!fore.isEmpty())
						current.fore = fore;
					if (!back.isEmpty())
						current.back = back;
				}
			}
			else if (activeTag == "font")
			{
				if (!ensureRenderStackCapacity(activeTag))
					return;
				stack.push_back({activeTag, current});
				QString colorSpec = QString::fromLocal8Bit(activeAttributes.value("color"));
				if (colorSpec.isEmpty())
					colorSpec = QString::fromLocal8Bit(activeAttributes.value("fgcolor"));
				QStringList const parts = colorSpec.split(',', Qt::SkipEmptyParts);
				for (QString part : parts)
				{
					part = part.trimmed();
					if (part.compare(QStringLiteral("bold"), Qt::CaseInsensitive) == 0)
						current.bold = true;
					else if (part.compare(QStringLiteral("italic"), Qt::CaseInsensitive) == 0)
						current.italic = true;
					else if (part.compare(QStringLiteral("underline"), Qt::CaseInsensitive) == 0)
						current.underline = true;
					else if (part.compare(QStringLiteral("blink"), Qt::CaseInsensitive) == 0)
						current.blink = true;
					else if (part.compare(QStringLiteral("inverse"), Qt::CaseInsensitive) == 0)
					{
						current.inverse = true;
						if (!current.fore.isEmpty() || !current.back.isEmpty())
							qSwap(current.fore, current.back);
					}
					else
					{
						if (!ignoreMxpColourChanges)
						{
							QString const color = normalizeColorText(part);
							if (!color.isEmpty())
								current.fore = color;
						}
					}
				}
				QString back = QString::fromLocal8Bit(activeAttributes.value("back"));
				if (back.isEmpty())
					back = QString::fromLocal8Bit(activeAttributes.value("bgcolor"));
				if (!ignoreMxpColourChanges)
				{
					QString const backColor = normalizeColorText(back);
					if (!backColor.isEmpty())
						current.back = backColor;
				}
			}
			else if (activeTag == "high" || activeTag == "h")
			{
				if (!ensureRenderStackCapacity(activeTag))
					return;
				stack.push_back({activeTag, current});
				if (!current.fore.isEmpty())
				{
					QColor const color(current.fore);
					if (color.isValid())
						current.fore = color.lighter(115).name();
				}
			}
			else if (activeTag == "tt" || activeTag == "samp")
			{
				if (!ensureRenderStackCapacity(activeTag))
					return;
				stack.push_back({activeTag, current});
				current.monospace = true;
			}
			else if (activeTag == "pre")
			{
				if (!ensureRenderBlockCapacity(activeTag))
					return;
				blockStack.push_back(activeTag);
				preDepth++;
			}
			else if (activeTag == "center" || activeTag == "ul" || activeTag == "ol" || activeTag == "li")
			{
				if (!ensureRenderBlockCapacity(activeTag))
					return;
				blockStack.push_back(activeTag);
			}
			else if (activeTag == "reset")
			{
				if (linkOpen)
				{
					linkOpen = false;
				}
				while (!blockStack.isEmpty())
				{
					const QByteArray closeTag = blockStack.takeLast();
					if (closeTag == "pre")
					{
						if (preDepth > 0)
							preDepth--;
					}
				}
				current = MxpStyleState();
				stack.clear();
			}
		};

		auto applyEndTag = [&](const QByteArray &closeTag)
		{
			if (closeTag == "send" || closeTag == "a")
			{
				if (linkOpen)
				{
					linkOpen = false;
				}
				for (int i = safeQSizeToInt(stack.size()) - 1; i >= 0; --i)
				{
					if (mxpTagsEquivalent(stack.at(i).tag, closeTag))
					{
						current = stack.at(i).state;
						stack.removeAt(i);
						break;
					}
				}
			}
			else if (closeTag == "pre" || closeTag == "center" || closeTag == "ul" || closeTag == "ol" ||
			         closeTag == "li")
			{
				for (int i = safeQSizeToInt(blockStack.size()) - 1; i >= 0; --i)
				{
					if (blockStack.at(i) == closeTag)
					{
						for (int j = safeQSizeToInt(blockStack.size()) - 1; j >= i; --j)
						{
							const QByteArray &innerTag = blockStack.at(j);
							if (innerTag == "pre")
							{
								if (preDepth > 0)
									preDepth--;
							}
						}
						blockStack.resize(i);
						break;
					}
				}
			}
			else
			{
				for (int i = safeQSizeToInt(stack.size()) - 1; i >= 0; --i)
				{
					if (stack.at(i).tag == closeTag)
					{
						current = stack.at(i).state;
						stack.removeAt(i);
						break;
					}
				}
			}
		};

		auto applyMxpModeBarrier = [&](const TelnetProcessor::MxpModeChange &modeChange)
		{
			auto       isLockedMode = [](const int mode) { return mode == 2 || mode == 7; };
			auto       isOpenMode   = [](const int mode) { return mode == 0 || mode == 5; };
			const bool closeOpenTagsForModeTransition =
			    isOpenMode(modeChange.oldMode) && !isOpenMode(modeChange.newMode);
			const bool resetOrLockedTransition = modeChange.newMode == 3 || isLockedMode(modeChange.newMode);

			if (!closeOpenTagsForModeTransition && !resetOrLockedTransition)
				return;

			m_mxpOpenTags.clear();
			m_mxpTagStack.clear();

			if (!stack.isEmpty())
				current = stack.front().state;
			stack.clear();
			blockStack.clear();
			linkOpen = false;
			preDepth = 0;

			current.actionType = ActionNone;
			current.action.clear();
			current.hint.clear();
			current.variable.clear();
			current.startTag = false;
		};

		auto appendStyled = [&](const QByteArray &bytes)
		{
			if (bytes.isEmpty())
				return;
			auto appendDecodedSegment = [&](const QString &decoded, const QMudStyledTextState &segmentState)
			{
				QString rawSegment = decoded;
				if (rawSegment.isEmpty())
				{
					return;
				}

				const int segmentLength = safeQSizeToInt(rawSegment.size());
				bool      startTag      = segmentState.startTag;
				auto      backspaceOne  = [&]
				{
					if (lineText.isEmpty())
						return;
					lineText.chop(1);
					while (!lineSpans.isEmpty())
					{
						StyleSpan &lastSpan = lineSpans.last();
						if (lastSpan.length > 1)
						{
							lastSpan.length -= 1;
							return;
						}
						lineSpans.pop_back();
					}
				};
				auto appendRun = [&](int start, int length)
				{
					if (length <= 0)
						return;
					lineText += rawSegment.mid(start, length);
					StyleSpan span;
					span.length     = length;
					span.fore       = segmentState.fore.isEmpty() ? QColor() : QColor(segmentState.fore);
					span.back       = segmentState.back.isEmpty() ? QColor() : QColor(segmentState.back);
					span.bold       = segmentState.bold;
					span.italic     = segmentState.italic;
					span.blink      = segmentState.blink;
					span.underline  = segmentState.underline;
					span.inverse    = segmentState.inverse;
					span.actionType = segmentState.actionType;
					span.action     = segmentState.action;
					span.hint       = segmentState.hint;
					span.variable   = segmentState.variable;
					span.startTag   = startTag;
					startTag        = false;
					if (!lineSpans.isEmpty())
					{
						StyleSpan &lastSpan = lineSpans.last();
						if (lastSpan.fore == span.fore && lastSpan.back == span.back &&
						    lastSpan.bold == span.bold && lastSpan.italic == span.italic &&
						    lastSpan.blink == span.blink && lastSpan.underline == span.underline &&
						    lastSpan.inverse == span.inverse && lastSpan.actionType == span.actionType &&
						    lastSpan.action == span.action && lastSpan.hint == span.hint &&
						    lastSpan.variable == span.variable && lastSpan.startTag == span.startTag)
						{
							lastSpan.length += span.length;
						}
						else
							lineSpans.push_back(span);
					}
					else
						lineSpans.push_back(span);
				};
				int runStart = -1;
				for (int i = 0; i < segmentLength; ++i)
				{
					const QChar ch = rawSegment.at(i);
					if (carriageReturnPending && ch != QLatin1Char('\n') && ch != QLatin1Char('\r'))
					{
						if (carriageReturnClears)
						{
							lineText.clear();
							lineSpans.clear();
						}
						carriageReturnPending = false;
					}
					if (ch == QLatin1Char('\n') || ch == QLatin1Char('\r'))
					{
						if (runStart >= 0)
						{
							appendRun(runStart, i - runStart);
							runStart = -1;
						}

						if (ch == QLatin1Char('\r'))
						{
							const bool nextIsNewline =
							    (i + 1 < segmentLength && rawSegment.at(i + 1) == QLatin1Char('\n'));
							if (!nextIsNewline)
								carriageReturnPending = carriageReturnClears;
							continue;
						}

						carriageReturnPending = false;
						emitCompletedLine(lineText, lineSpans);
						continue;
					}
					if (ch == QLatin1Char('\b'))
					{
						if (runStart >= 0)
						{
							appendRun(runStart, i - runStart);
							runStart = -1;
						}
						backspaceOne();
						continue;
					}
					if (runStart < 0)
						runStart = i;
				}
				if (runStart >= 0)
					appendRun(runStart, segmentLength - runStart);
			};

			auto colorFromIndex = [](int idx) -> QString
			{
				if (idx < 0 || idx >= 256)
					return {};
				const AppController *app = AppController::instance();
				const QMudColorRef   ref = app ? app->xtermColorAt(idx) : qmudRgb(0, 0, 0);
				return QColor(qmudRed(ref), qmudGreen(ref), qmudBlue(ref)).name();
			};
			auto normalAnsiColorFromIndex = [&](const int idx) -> QString
			{
				if (idx < 0 || idx >= normalAnsi.size())
					return {};
				return normalAnsi.at(idx).name();
			};
			auto boldAnsiColorFromIndex = [&](const int idx) -> QString
			{
				if (idx < 0 || idx >= boldAnsi.size())
					return {};
				return boldAnsi.at(idx).name();
			};

			QMudStyledTextState ansiState;
			ansiState.bold       = current.bold;
			ansiState.underline  = current.underline;
			ansiState.italic     = current.italic;
			ansiState.blink      = current.blink;
			ansiState.strike     = current.strike;
			ansiState.inverse    = current.inverse;
			ansiState.fore       = current.fore;
			ansiState.back       = current.back;
			ansiState.actionType = current.actionType;
			ansiState.action     = current.action;
			ansiState.hint       = current.hint;
			ansiState.variable   = current.variable;
			ansiState.startTag   = current.startTag;
			ansiState.monospace  = current.monospace;

			constexpr QMudOscActionIds oscActionIds{ActionNone, ActionSend, ActionPrompt, ActionHyperlink};
			const QVector<QMudStyledChunk> chunks = qmudParseAnsiSgrChunks(
			    bytes, m_ansiStreamState, defaultFore, defaultBack, normalAnsiColorFromIndex,
			    boldAnsiColorFromIndex, colorFromIndex, decodeIncomingDisplayBytes, ansiState, oscActionIds);
			for (const QMudStyledChunk &chunk : chunks)
				appendDecodedSegment(chunk.text, chunk.state);

			current.bold       = ansiState.bold;
			current.underline  = ansiState.underline;
			current.italic     = ansiState.italic;
			current.blink      = ansiState.blink;
			current.strike     = ansiState.strike;
			current.inverse    = ansiState.inverse;
			current.fore       = ansiState.fore;
			current.back       = ansiState.back;
			current.actionType = ansiState.actionType;
			current.action     = ansiState.action;
			current.hint       = ansiState.hint;
			current.variable   = ansiState.variable;
			current.startTag   = ansiState.startTag;
			current.monospace  = ansiState.monospace;
		};

		auto isAtomicTag = [](const QByteArray &tag)
		{
			AtomicTagInfo info;
			return lookupAtomicTagInfo(tag, info);
		};

		int modeChangeIndex = 0;

		for (const TelnetProcessor::MxpEvent &ev : sorted)
		{
			if (mxpTrackingReset)
				break;

			while (modeChangeIndex < sortedModeChanges.size())
			{
				const TelnetProcessor::MxpModeChange &modeChange = sortedModeChanges.at(modeChangeIndex);
				const bool                            boundaryComesBeforeEvent =
				    (modeChange.offset < ev.offset) ||
				    (modeChange.offset == ev.offset && modeChange.sequence < ev.sequence);
				if (!boundaryComesBeforeEvent)
					break;
				if (modeChange.offset > last)
				{
					appendStyled(processed.mid(last, modeChange.offset - last));
					last = modeChange.offset;
				}
				applyMxpModeBarrier(modeChange);
				++modeChangeIndex;
			}

			if (ev.offset > last)
			{
				appendStyled(processed.mid(last, ev.offset - last));
				last = ev.offset;
			}

			const QByteArray                   tag          = ev.name.toLower();
			const QString                      tagText      = QString::fromLatin1(tag);
			QMap<QByteArray, QByteArray>       attributes   = ev.attributes;
			QByteArray                         effectiveTag = tag;
			TelnetProcessor::CustomElementInfo customInfo;
			AtomicTagInfo                      atomicInfo;
			bool                               hasCustomElement = false;
			bool                               hasAtomicTag     = false;
			bool                               tagIsCommand     = false;
			bool                               mxpSecure        = false;
			if (ev.type == TelnetProcessor::MxpEvent::StartTag)
			{
				hasCustomElement = m_telnet.getCustomElementInfo(tag, customInfo);
				hasAtomicTag     = lookupAtomicTagInfo(tag, atomicInfo);
				if (hasCustomElement)
				{
					const ParsedMxpArguments           providedArgs = parseMxpArguments(ev.raw);
					const QMap<QByteArray, QByteArray> mergedDefaults =
					    mergeAttributeDefaultsBytes(customInfo.attributes, providedArgs.args);
					QByteArray const resolvedDefinition =
					    resolveDefinitionEntities(customInfo.definition, mergedDefaults, m_telnet);
					QMap<QByteArray, QByteArray> aliasAttributes;
					QByteArray                   aliasTag;
					if (parseDefinitionAlias(resolvedDefinition, aliasTag, aliasAttributes))
					{
						effectiveTag = aliasTag;
						if (!aliasAttributes.isEmpty())
							attributes = aliasAttributes;
					}
				}
				if (!hasCustomElement && !hasAtomicTag)
				{
					mxpError(DBG_ERROR, errMXP_UnknownElement,
					         QStringLiteral("Unknown MXP element: <%1> (custom elements loaded: %2)")
					             .arg(tagText)
					             .arg(m_telnet.customElementCount()));
					continue;
				}
				if (hasAtomicTag && atomicInfo.puebloOnly && !m_telnet.isPuebloActive())
				{
					mxpError(DBG_ERROR, errMXP_PuebloOnly,
					         QStringLiteral("Using Pueblo-only element in MXP mode: <%1>").arg(tagText));
					continue;
				}
				const bool tagIsOpen = hasCustomElement ? customInfo.open : atomicInfo.open;
				tagIsCommand         = hasCustomElement ? customInfo.command : atomicInfo.command;
				mxpSecure            = ev.secure;
				if (!tagIsOpen && !mxpSecure)
				{
					mxpError(
					    DBG_ERROR, errMXP_ElementWhenNotSecure,
					    QStringLiteral("Secure MXP tag ignored when not in secure mode: <%1>").arg(tagText));
					continue;
				}
			}
			const QByteArray unnamedFore = attributes.value("1");
			const QByteArray unnamedBack = attributes.value("2");

			if (ev.type == TelnetProcessor::MxpEvent::Definition)
			{
				QByteArray definition = ev.raw;
				if (definition.startsWith('!'))
					definition = definition.mid(1);
				QByteArray defType;
				mxpGetWord(defType, definition);
				defType = defType.toLower();
				if (defType == "entity")
				{
					QByteArray name;
					mxpGetWord(name, definition);
					if (!name.isEmpty())
					{
						QByteArray value;
						if (m_telnet.getCustomEntityValue(name, value))
						{
							QByteArray payload = name.toLower();
							payload.append('=');
							payload.append(value);
							callPluginCallbacksWithBytes(QStringLiteral("OnPluginMXPsetEntity"), payload);
						}
					}
				}
				continue;
			}

			if (ev.type == TelnetProcessor::MxpEvent::StartTag)
			{
				const ParsedMxpArguments parsed        = parseMxpArguments(ev.raw);
				QString                  argsForScript = parsed.rawArguments;
				if (isAtomicTag(tag))
					argsForScript = buildArgumentString(parsed.args);

				// MUSHclient parity: MXP open-tag callbacks are only invoked for
				// user-defined tags, not built-in atomic command/style tags.
				if (hasCustomElement)
				{
					const QString pluginOpenPayload = QStringLiteral("%1,%2").arg(tagText, argsForScript);
					if (!callPluginCallbacksStopOnFalse(QStringLiteral("OnPluginMXPopenTag"),
					                                    pluginOpenPayload))
						continue;
					if (!openTagCallback.isEmpty() && m_luaCallbacks && tag != "afk")
					{
						const QMap<QString, QString> argumentTable = buildArgumentTable(parsed.args);
						const bool suppress = m_luaCallbacks->callMxpStartTag(openTagCallback, tagText,
						                                                      argsForScript, argumentTable);
						if (suppress)
							continue;
					}
				}

				if (!tagIsCommand)
				{
					if (!ensureOpenTagCapacity())
						continue;
					MxpOpenTag openTag;
					openTag.tag          = tag;
					openTag.openedSecure = mxpSecure;
					openTag.noReset      = hasAtomicTag ? atomicInfo.noReset : false;
					m_mxpOpenTags.push_back(openTag);
				}

				QVector<QByteArray>                   customTagSequence;
				QVector<QMap<QByteArray, QByteArray>> customTagAttributes;
				if (hasCustomElement)
				{
					const QMap<QByteArray, QByteArray> mergedDefaults =
					    mergeAttributeDefaultsBytes(customInfo.attributes, parsed.args);
					const QVector<QByteArray> rawTags = extractDefinitionTags(customInfo.definition);
					for (const QByteArray &rawTag : rawTags)
					{
						const QByteArray resolved =
						    resolveDefinitionEntities(rawTag, mergedDefaults, m_telnet);
						ParsedMxpArguments const defParsed = parseMxpArguments(resolved);
						QByteArray               tagName;
						QByteArray               temp = resolved;
						mxpGetWord(tagName, temp);
						tagName = tagName.toLower();
						if (tagName.isEmpty())
							continue;
						if (!isAtomicTag(tagName))
							continue;
						QMap<QByteArray, QByteArray> const attrMap = buildArgumentTableBytes(defParsed.args);
						customTagSequence.push_back(tagName);
						customTagAttributes.push_back(attrMap);
					}
				}

				bool const trackable = !tagIsCommand;
				if (tagIsCommand)
				{
					customTagSequence.clear();
					customTagAttributes.clear();
				}

				if (!customTagSequence.isEmpty())
				{
					QVector<QByteArray> appliedCloseTags;
					for (int i = 0; i < customTagSequence.size(); ++i)
					{
						const QByteArray                  &childTag   = customTagSequence.at(i);
						const QMap<QByteArray, QByteArray> childAttrs = customTagAttributes.value(i);
						applyStartTag(childTag, childAttrs);
						if (mxpTrackingReset)
							break;
						appliedCloseTags.push_back(childTag);
					}
					if (mxpTrackingReset)
						continue;

					if (trackable)
					{
						if (!ensureTagFrameCapacity())
							continue;
						MxpTagFrame frame;
						frame.tag          = tag;
						frame.contentStart = mxpBaseOffset + ev.offset;
						if (frame.variableName.isEmpty() && !customInfo.flag.isEmpty())
							frame.variableName = QString::fromLocal8Bit(customInfo.flag).toLower();
						frame.closeTags = appliedCloseTags;
						m_mxpTagStack.push_back(frame);
					}
					continue;
				}

				if (trackable)
				{
					if (!ensureTagFrameCapacity())
						continue;
					MxpTagFrame frame;
					frame.tag          = tag;
					frame.contentStart = mxpBaseOffset + ev.offset;
					if (tag == "var" || tag == "v")
					{
						for (const auto &arg : parsed.args)
						{
							if (arg.name.isEmpty() && arg.position == 1)
							{
								frame.variableName = arg.value.toLower();
								break;
							}
						}
					}
					if (frame.variableName.isEmpty() && hasCustomElement && !customInfo.flag.isEmpty())
						frame.variableName = QString::fromLocal8Bit(customInfo.flag).toLower();
					m_mxpTagStack.push_back(frame);
				}

				applyStartTag(effectiveTag, attributes);
				if (mxpTrackingReset)
					continue;

				if (effectiveTag == "version" || effectiveTag == "afk" || effectiveTag == "support" ||
				    effectiveTag == "option" || effectiveTag == "recommend_option" ||
				    effectiveTag == "user" || effectiveTag == "username" || effectiveTag == "password" ||
				    effectiveTag == "pass")
				{
					// Port of MXP_OpenAtomicTag actions that generate responses.
					const QByteArray endLine("\r\n");
					auto             sendMxpPacket = [&](const QString &payload)
					{
						if (payload.isEmpty())
							return;
						sendToWorld(payload.toLocal8Bit());
					};

					auto extractArgumentTokens = [&](const QMap<QByteArray, QByteArray> &attrs)
					{
						QList<int>  numericKeys;
						QStringList tokens;
						for (auto it = attrs.constBegin(); it != attrs.constEnd(); ++it)
						{
							if (it.key() == "_tag")
								continue;
							bool      ok    = false;
							const int index = QString::fromLocal8Bit(it.key()).toInt(&ok);
							if (ok)
							{
								numericKeys.append(index);
								continue;
							}
							const QString key   = QString::fromLocal8Bit(it.key());
							const QString value = QString::fromLocal8Bit(it.value());
							if (value.isEmpty())
								tokens.append(key);
							else
								tokens.append(QStringLiteral("%1=%2").arg(key, value));
						}
						std::ranges::sort(numericKeys);
						for (int const idx : numericKeys)
						{
							const QByteArray key   = QByteArray::number(idx);
							const QString    value = QString::fromLocal8Bit(attrs.value(key));
							if (!value.isEmpty())
								tokens.append(value);
						}
						return tokens;
					};

					auto getWorldAttribute = [&](const QString &key) { return m_worldAttributes.value(key); };

					auto parseOptionValue = [&](const QString &raw, bool *okOut) -> long long
					{
						const QString trimmed = raw.trimmed();
						if (trimmed.compare(QStringLiteral("y"), Qt::CaseInsensitive) == 0 ||
						    trimmed.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0)
						{
							if (okOut)
								*okOut = true;
							return 1;
						}
						if (trimmed.compare(QStringLiteral("n"), Qt::CaseInsensitive) == 0 ||
						    trimmed.compare(QStringLiteral("false"), Qt::CaseInsensitive) == 0)
						{
							if (okOut)
								*okOut = true;
							return 0;
						}
						bool            ok    = false;
						const long long value = trimmed.toLongLong(&ok);
						if (okOut)
							*okOut = ok;
						return ok ? value : 0;
					};

					auto currentOptionValue = [&](const QString            &key,
					                              const WorldNumericOption *option) -> long long
					{
						const QString   raw   = getWorldAttribute(key);
						bool            ok    = false;
						const long long value = parseOptionValue(raw, &ok);
						if (ok)
							return value;
						if (option)
							return option->defaultValue;
						return 0;
					};

					auto getWorldPassword = [&]
					{
						QString password = getWorldAttribute(QStringLiteral("password"));
						if (!password.isEmpty())
							return password;
						const QString base64 = getWorldAttribute(QStringLiteral("password_base64"));
						if (!base64.isEmpty())
						{
							const QByteArray decoded = QByteArray::fromBase64(base64.toLocal8Bit());
							return QString::fromLocal8Bit(decoded);
						}
						return QString();
					};

					auto isConnectMxp = [&]
					{
						const int connectMethod = getWorldAttribute(QStringLiteral("connect_method")).toInt();
						return connectMethod == 3; // eConnectMXP
					};

					auto mxpSupports = [&] { return mxpSupportedTags(); };

					auto isValidMxpName = [](const QString &name)
					{
						if (name.isEmpty())
							return false;
						const QChar first = name.at(0);
						if (!first.isLetter())
							return false;
						for (int i = 1; i < name.size(); ++i)
						{
							const QChar ch = name.at(i);
							if (ch.isLetterOrNumber() || ch == QLatin1Char('_') || ch == QLatin1Char('-') ||
							    ch == QLatin1Char('.'))
								continue;
							return false;
						}
						return true;
					};

					if (effectiveTag == "version")
					{
						// From mxpOpenAtomic.cpp: <VERSION MXP="0.5" CLIENT=MUSHclient VERSION="..." REGISTERED=YES>
						const QString version = QString::fromLatin1(kVersionString);
						const QString packet =
						    QStringLiteral("\x1B[1z<VERSION MXP=\"0.5\" CLIENT=QMud VERSION=\"%1\" "
						                   "REGISTERED=YES>%2")
						        .arg(version, QString::fromLatin1(endLine));
						sendMxpPacket(packet);
						mxpError(DBG_INFO, infoMXP_VersionSent,
						         QStringLiteral("Sent version response: %1").arg(packet.mid(4)));
					}
					else if (effectiveTag == "afk")
					{
						const QString sendFlag = getWorldAttribute(QStringLiteral("send_mxp_afk_response"));
						const bool    allowResponse =
						    !(sendFlag == QStringLiteral("0") ||
						      sendFlag.compare(QStringLiteral("false"), Qt::CaseInsensitive) == 0);
						if (allowResponse)
						{
							const QStringList args      = extractArgumentTokens(attributes);
							const QString     challenge = args.isEmpty() ? QString() : args.first();
							const QDateTime   now       = QDateTime::currentDateTime();
							const qint64      seconds =
							    (m_lastUserInput.isValid() ? m_lastUserInput.secsTo(now) : 0);
							const QString packet = QStringLiteral("\x1B[1z<AFK %1 %2>%3")
							                           .arg(seconds)
							                           .arg(challenge, QString::fromLatin1(endLine));
							sendMxpPacket(packet);
							mxpError(DBG_INFO, infoMXP_AFKSent,
							         QStringLiteral("Sent AFK response: %1").arg(packet.mid(4)));
						}
					}
					else if (effectiveTag == "support")
					{
						// From mxpOpenAtomic.cpp: reply with supported tags and arguments.
						const QStringList args = extractArgumentTokens(attributes);
						QString           supports;
						const auto       *supported              = mxpSupports();
						bool              invalidSupportArgument = false;

						if (args.isEmpty())
						{
							for (int i = 0; supported[i].name; i++)
							{
								supports +=
								    QStringLiteral("+%1 ").arg(QString::fromLatin1(supported[i].name));
								const QString argList = QString::fromLatin1(supported[i].args);
								if (!argList.isEmpty())
								{
									const QStringList items = argList.split(',', Qt::SkipEmptyParts);
									for (const QString &item : items)
										supports += QStringLiteral("+%1.%2 ").arg(
										    QString::fromLatin1(supported[i].name), item);
								}
							}
						}
						else
						{
							for (const QString &raw : args)
							{
								const QStringList parts = raw.split('.', Qt::SkipEmptyParts);
								if (parts.size() > 2)
								{
									mxpError(DBG_ERROR, errMXP_InvalidSupportArgument,
									         QStringLiteral("Invalid <support> argument: %1").arg(raw));
									invalidSupportArgument = true;
									break;
								}
								const QString tagName = parts.value(0).toLower();
								const QString subTag  = parts.value(1).toLower();
								if (!isValidMxpName(tagName) ||
								    (!subTag.isEmpty() && subTag != "*" && !isValidMxpName(subTag)))
								{
									mxpError(DBG_ERROR, errMXP_InvalidSupportArgument,
									         QStringLiteral("Invalid <support> argument: %1").arg(raw));
									invalidSupportArgument = true;
									break;
								}
								bool found = false;
								for (int i = 0; supported[i].name; i++)
								{
									if (tagName == QString::fromLatin1(supported[i].name))
									{
										found = true;
										if (subTag.isEmpty())
										{
											supports += QStringLiteral("+%1 ").arg(tagName);
										}
										else if (subTag == "*")
										{
											const QString argList = QString::fromLatin1(supported[i].args);
											if (!argList.isEmpty())
											{
												const QStringList items =
												    argList.split(',', Qt::SkipEmptyParts);
												for (const QString &item : items)
													supports += QStringLiteral("+%1.%2 ").arg(tagName, item);
											}
										}
										else
										{
											const QString argList   = QString::fromLatin1(supported[i].args);
											const QStringList items = argList.split(',', Qt::SkipEmptyParts);
											if (items.contains(subTag))
												supports += QStringLiteral("+%1.%2 ").arg(tagName, subTag);
											else
												supports += QStringLiteral("-%1.%2 ").arg(tagName, subTag);
										}
										break;
									}
								}
								if (!found)
									supports += QStringLiteral("-%1 ").arg(tagName);
							}
						}

						if (invalidSupportArgument)
							continue;

						const QString packet = QStringLiteral("\x1B[1z<SUPPORTS %1>%2")
						                           .arg(supports.trimmed(), QString::fromLatin1(endLine));
						sendMxpPacket(packet);
						mxpError(DBG_INFO, infoMXP_SupportsSent,
						         QStringLiteral("Sent supports response: %1").arg(packet.mid(4)));
					}
					else if (effectiveTag == "option")
					{
						// From mxpOpenAtomic.cpp: reply with options requested (if any).
						const QStringList args = extractArgumentTokens(attributes);
						QString           options;
						if (args.isEmpty())
						{
							const WorldNumericOption *list = worldNumericOptions();
							for (int i = 0; list[i].name; i++)
							{
								const QString   key   = QString::fromLatin1(list[i].name);
								const long long value = currentOptionValue(key, &list[i]);
								options += QStringLiteral("%1=%2 ").arg(key).arg(QString::number(value));
							}
						}
						else
						{
							for (const QString &raw : args)
							{
								const QString key = raw.section('=', 0, 0).trimmed();
								if (key.isEmpty())
									continue;
								const WorldNumericOption *opt = QMudWorldOptions::findWorldNumericOption(key);
								const long long           value = opt ? currentOptionValue(key, opt) : 0;
								options += QStringLiteral("%1=%2 ").arg(key).arg(QString::number(value));
							}
						}
						const QString packet = QStringLiteral("\x1B[1z<OPTIONS %1>%2")
						                           .arg(options.trimmed(), QString::fromLatin1(endLine));
						sendMxpPacket(packet);
						mxpError(DBG_INFO, infoMXP_OptionsSent,
						         QStringLiteral("Sent options response: %1").arg(packet.mid(4)));
					}
					else if (effectiveTag == "recommend_option")
					{
						// Server-suggested options (only applies to server-writable options).
						if (!isEnabledFlag(m_worldAttributes.value(QStringLiteral("mud_can_change_options"))))
							continue;

						const QStringList args = extractArgumentTokens(ev.attributes);
						for (const QString &raw : args)
						{
							const QString key       = raw.section('=', 0, 0).trimmed();
							const QString valueText = raw.section('=', 1).trimmed();
							if (key.isEmpty() || valueText.isEmpty())
								continue;
							if (!isValidMxpName(key))
							{
								mxpError(DBG_ERROR, errMXP_InvalidOptionArgument,
								         QStringLiteral("Option named '%1' not known.").arg(key));
								continue;
							}
							const WorldNumericOption *opt = QMudWorldOptions::findWorldNumericOption(key);
							if (!opt)
							{
								mxpError(DBG_ERROR, errMXP_InvalidOptionArgument,
								         QStringLiteral("Option named '%1' not known.").arg(key));
								continue;
							}
							if ((opt->flags & OPT_SERVER_CAN_WRITE) == 0)
							{
								mxpError(DBG_ERROR, errMXP_CannotChangeOption,
								         QStringLiteral("Option named '%1' cannot be changed.").arg(key));
								continue;
							}
							bool            ok    = false;
							const long long value = parseOptionValue(valueText, &ok);
							if (!ok)
								continue;
							if (key.compare(QStringLiteral("hyperlink_colour"), Qt::CaseInsensitive) == 0 &&
							    !isEnabledFlag(
							        m_worldAttributes.value(QStringLiteral("mud_can_change_link_colour"))))
							{
								mxpError(DBG_ERROR, errMXP_CannotChangeOption,
								         QStringLiteral("Option named '%1' cannot be changed.").arg(key));
								continue;
							}
							if (key.compare(QStringLiteral("underline_hyperlinks"), Qt::CaseInsensitive) ==
							        0 &&
							    value == 0 &&
							    !isEnabledFlag(
							        m_worldAttributes.value(QStringLiteral("mud_can_remove_underline"))))
							{
								mxpError(DBG_ERROR, errMXP_CannotChangeOption,
								         QStringLiteral("Option named '%1' cannot be changed.").arg(key));
								continue;
							}
							long long const minValue = opt->minValue;
							long long       maxValue = opt->maxValue;
							if (minValue == 0 && maxValue == 0)
								maxValue = 1;
							if (value < minValue || value > maxValue)
							{
								mxpError(DBG_ERROR, errMXP_OptionOutOfRange,
								         QStringLiteral(
								             "Option named '%1' could not be changed to '%2' (out of range).")
								             .arg(key, valueText));
								continue;
							}
							m_worldAttributes.insert(key, QString::number(value));
							mxpError(
							    DBG_INFO, infoMXP_OptionChanged,
							    QStringLiteral("Option named '%1' changed to '%2'.").arg(key, valueText));
						}
					}
					else if (effectiveTag == "mxp")
					{
						const QStringList args       = extractArgumentTokens(attributes);
						auto              hasKeyword = [&](const QString &keyword)
						{
							return std::ranges::any_of(
							    args,
							    [&](const QString &arg)
							    {
								    const QString token = arg.section('=', 0, 0).trimmed();
								    return token.compare(keyword, Qt::CaseInsensitive) == 0;
							    });
						};

						if (hasKeyword(QStringLiteral("off")))
						{
							m_telnet.disableMxp();
							continue;
						}
						if (hasKeyword(QStringLiteral("default_open")))
							m_telnet.setMxpDefaultMode(TelnetProcessor::MxpDefaultMode::Open);
						if (hasKeyword(QStringLiteral("default_secure")))
							m_telnet.setMxpDefaultMode(TelnetProcessor::MxpDefaultMode::Secure);
						if (hasKeyword(QStringLiteral("default_locked")))
							m_telnet.setMxpDefaultMode(TelnetProcessor::MxpDefaultMode::Locked);
					}
					else if (effectiveTag == "user" || effectiveTag == "username")
					{
						const QString name = getWorldAttribute(QStringLiteral("player"));
						if (!name.isEmpty() && isConnectMxp())
						{
							const QString packet =
							    QStringLiteral("%1%2").arg(name, QString::fromLatin1(endLine));
							sendMxpPacket(packet);
							mxpError(DBG_INFO, infoMXP_CharacterNameSent,
							         QStringLiteral("Sent character name: %1").arg(name));
						}
						else if (!isConnectMxp())
							mxpError(
							    DBG_WARNING, wrnMXP_CharacterNameRequestedButNotDefined,
							    QStringLiteral("Character name requested but auto-connect not set to MXP."));
						else
							mxpError(DBG_WARNING, wrnMXP_CharacterNameRequestedButNotDefined,
							         QStringLiteral("Character name requested but none defined."));
					}
					else if (effectiveTag == "password" || effectiveTag == "pass")
					{
						if (m_totalLinesSent > 10)
						{
							mxpError(DBG_WARNING, wrnMXP_PasswordNotSent,
							         QStringLiteral("Too many lines sent to MUD - password not sent."));
						}
						else if (isConnectMxp())
						{
							const QString password = getWorldPassword();
							if (!password.isEmpty())
							{
								const QString packet =
								    QStringLiteral("%1%2").arg(password, QString::fromLatin1(endLine));
								sendMxpPacket(packet);
								mxpError(DBG_INFO, infoMXP_PasswordSent,
								         QStringLiteral("Sent password to world."));
							}
							else
								mxpError(DBG_WARNING, wrnMXP_PasswordRequestedButNotDefined,
								         QStringLiteral("Password requested but none defined."));
						}
						else
							mxpError(DBG_WARNING, wrnMXP_PasswordRequestedButNotDefined,
							         QStringLiteral("Password requested but auto-connect not set to MXP."));
					}
				}
			}
			else if (ev.type == TelnetProcessor::MxpEvent::EndTag)
			{
				const QByteArray &closeTag     = tag;
				const bool        endMxpSecure = ev.secure;

				int               openMatchIndex       = -1;
				bool              closeBlockedBySecure = false;
				for (int i = safeQSizeToInt(m_mxpOpenTags.size()) - 1; i >= 0; --i)
				{
					const MxpOpenTag &openTag = m_mxpOpenTags.at(i);
					if (mxpTagsEquivalent(openTag.tag, closeTag))
					{
						openMatchIndex = i;
						break;
					}
					if (!endMxpSecure && openTag.openedSecure)
					{
						mxpError(DBG_WARNING, wrnMXP_OpenTagBlockedBySecureTag,
						         QStringLiteral("Cannot close open MXP tag <%1> - blocked by secure tag <%2>")
						             .arg(tagText, QString::fromLatin1(openTag.tag)));
						closeBlockedBySecure = true;
						break;
					}
				}
				if (closeBlockedBySecure)
					continue;

				if (openMatchIndex < 0)
				{
					mxpError(DBG_WARNING, wrnMXP_OpenTagNotThere,
					         QStringLiteral("Closing MXP tag </%1> does not have corresponding opening tag")
					             .arg(tagText));
					continue;
				}

				if (!endMxpSecure && m_mxpOpenTags.at(openMatchIndex).openedSecure)
				{
					mxpError(DBG_WARNING, wrnMXP_TagOpenedInSecureMode,
					         QStringLiteral("Cannot close open MXP tag <%1> - it was opened in secure mode.")
					             .arg(tagText));
					continue;
				}

				while (m_mxpOpenTags.size() - 1 >= openMatchIndex)
				{
					const MxpOpenTag openTag = m_mxpOpenTags.takeLast();
					if (!mxpTagsEquivalent(openTag.tag, closeTag))
					{
						mxpError(DBG_WARNING, wrnMXP_ClosingOutOfSequenceTag,
						         QStringLiteral("Closing out-of-sequence MXP tag: <%1>")
						             .arg(QString::fromLatin1(openTag.tag)));
					}
				}

				int                 matchIndex = -1;
				QVector<QByteArray> closeTagsToApply;
				for (int i = safeQSizeToInt(m_mxpTagStack.size()) - 1; i >= 0; --i)
				{
					if (mxpTagsEquivalent(m_mxpTagStack.at(i).tag, closeTag))
					{
						matchIndex = i;
						break;
					}
				}

				if (matchIndex >= 0)
				{
					const int eventOffset = mxpBaseOffset + ev.offset;
					while (m_mxpTagStack.size() - 1 >= matchIndex)
					{
						const MxpTagFrame frame = m_mxpTagStack.takeLast();
						QByteArray        textBytes;
						if (eventOffset > frame.contentStart && frame.contentStart >= 0)
							textBytes =
							    m_mxpTextBuffer.mid(frame.contentStart, eventOffset - frame.contentStart);
						if (textBytes.size() > 1000)
							textBytes = textBytes.left(1000);
						QString const text          = decodeIncomingIsolatedBytes(textBytes);
						QString const sanitizedText = qmudStripAnsiEscapeCodes(text);

						if (!closeTagCallback.isEmpty() && m_luaCallbacks)
						{
							m_luaCallbacks->callMxpEndTag(closeTagCallback, QString::fromLatin1(frame.tag),
							                              sanitizedText);
						}

						const QString pluginClosePayload =
						    QStringLiteral("%1,%2").arg(QString::fromLatin1(frame.tag), sanitizedText);
						callPluginCallbacks(QStringLiteral("OnPluginMXPcloseTag"), pluginClosePayload);

						if (!frame.variableName.isEmpty())
						{
							const QString variableName = QStringLiteral("mxp_%1").arg(frame.variableName);
							setVariable(variableName, sanitizedText);
							if (!setVarCallback.isEmpty() && m_luaCallbacks)
								m_luaCallbacks->callMxpSetVariable(setVarCallback, variableName,
								                                   sanitizedText);

							const QString pluginVarPayload =
							    QStringLiteral("%1=%2").arg(variableName, sanitizedText);
							callPluginCallbacks(QStringLiteral("OnPluginMXPsetVariable"), pluginVarPayload);
							if (frame.tag == "var" || frame.tag == "v")
								callPluginCallbacks(QStringLiteral("OnPluginMXPsetEntity"), pluginVarPayload);
						}

						if (mxpTagsEquivalent(frame.tag, closeTag) && closeTagsToApply.isEmpty() &&
						    !frame.closeTags.isEmpty())
							closeTagsToApply = frame.closeTags;
					}
				}
				if (!closeTagsToApply.isEmpty())
				{
					for (int i = safeQSizeToInt(closeTagsToApply.size()) - 1; i >= 0; --i)
						applyEndTag(closeTagsToApply.at(i));
				}
				else
				{
					applyEndTag(closeTag);
				}
			}
		}

		while (modeChangeIndex < sortedModeChanges.size())
		{
			const TelnetProcessor::MxpModeChange &modeChange = sortedModeChanges.at(modeChangeIndex++);
			if (modeChange.offset > last)
			{
				appendStyled(processed.mid(last, modeChange.offset - last));
				last = modeChange.offset;
			}
			applyMxpModeBarrier(modeChange);
		}

		if (last < processed.size())
			appendStyled(processed.mid(last));

		const bool hasActiveMxpRenderContextAfterProcessing =
		    !stack.isEmpty() || !blockStack.isEmpty() || linkOpen || preDepth > 0;
		if (m_mxpTagStack.isEmpty() && !hasActiveMxpRenderContextAfterProcessing)
			m_mxpTextBuffer.clear();

		m_mxpRenderStyle             = current;
		m_mxpRenderStack             = stack;
		m_mxpRenderBlockStack        = blockStack;
		m_mxpRenderLinkOpen          = linkOpen;
		m_mxpRenderPreDepth          = preDepth;
		m_ansiRenderState.bold       = current.bold;
		m_ansiRenderState.underline  = current.underline;
		m_ansiRenderState.italic     = current.italic;
		m_ansiRenderState.blink      = current.blink;
		m_ansiRenderState.strike     = current.strike;
		m_ansiRenderState.monospace  = current.monospace;
		m_ansiRenderState.inverse    = current.inverse;
		m_ansiRenderState.fore       = current.fore;
		m_ansiRenderState.back       = current.back;
		m_ansiRenderState.actionType = current.actionType;
		m_ansiRenderState.action     = current.action;
		m_ansiRenderState.hint       = current.hint;
		m_ansiRenderState.variable   = current.variable;
		m_ansiRenderState.startTag   = current.startTag;

		m_partialLineText                = lineText;
		m_partialLineSpans               = lineSpans;
		m_pendingCarriageReturnOverwrite = carriageReturnPending;
		emit incomingStyledLinePartialReceived(lineText, lineSpans);
		m_currentActionSource = previousActionSource;
	}
}

void WorldRuntime::finalizeSocketConnectedState()
{
	if (m_socketReadyForWorld)
		return;
	m_connectPhase       = eConnectConnectedToMud;
	const bool convertGA = isEnabledFlag(m_worldAttributes.value(QStringLiteral("convert_ga_to_newline")));
	const int  useMxp    = m_worldAttributes.value(QStringLiteral("use_mxp")).toInt();
	m_telnet.setConvertGAtoNewline(convertGA);
	if (useMxp >= 0)
		m_telnet.setUseMxp(useMxp);
	m_telnet.queueInitialNegotiation(true, convertGA);
	if (const QByteArray outbound = m_telnet.takeOutboundData(); !outbound.isEmpty())
		sendToWorld(outbound);
	m_socketReadyForWorld = true;
	emit connected();
}

bool WorldRuntime::beginStartTlsUpgrade()
{
	if (!m_socket)
		return false;
	QString error;
	if (!m_socket->startClientEncryption(&error))
	{
		if (error.isEmpty())
			error = QStringLiteral("START-TLS upgrade failed.");
		emit socketError(error);
		disconnectFromWorld();
		return false;
	}
	return true;
}

void WorldRuntime::startStartTlsFallbackTimer()
{
	if (!(m_tlsEncryptionEnabled && m_tlsMethod == eTlsStartTls))
		return;
	const quint64 generation = ++m_startTlsFallbackGeneration;
	QTimer::singleShot(500, this,
	                   [this, generation]
	                   {
		                   const StartTlsFallbackContext context = {generation,
		                                                            m_startTlsFallbackGeneration,
		                                                            m_socket != nullptr,
		                                                            m_socket && m_socket->isConnected(),
		                                                            m_socketReadyForWorld,
		                                                            m_tlsEncryptionEnabled,
		                                                            m_tlsMethod};
		                   if (!shouldFallbackToPlainOnStartTlsTimeout(context))
			                   return;
		                   outputText(QStringLiteral("TLS Encryption NOT enabled: Server did not respond to "
		                                             "START-TLS"),
		                              true, true);
		                   m_telnet.setStartTlsEnabled(false);
		                   finalizeSocketConnectedState();
	                   });
}

void WorldRuntime::cancelStartTlsFallbackTimer()
{
	++m_startTlsFallbackGeneration;
}

bool WorldRuntime::connectToWorld(const QString &host, quint16 port)
{
	if (!m_socket)
		return false;

	cancelStartTlsFallbackTimer();
	cancelReloadMccpProbeTimeout();
	m_reloadReattachMccpProbePending        = false;
	m_reloadReattachMccpResumePending       = false;
	m_reloadReattachLookProbeSent           = false;
	m_reloadReattachMccpProbeDecisionOffset = 0;
	m_reloadMccpProbeTimeoutPass            = 0;
	m_reloadReattachMccpProbeBuffer.clear();
	m_reloadReattachUseDeferredMxpReplay = false;
	m_reloadReattachMxpProbeEvents.clear();
	m_reloadReattachMxpProbeModeChanges.clear();
	m_proxyAddressString.clear();
	m_proxyAddressV4      = 0;
	m_disconnectOk        = false;
	m_socketReadyForWorld = false;
	m_telnet.resetConnectionState();
	resetAnsiRenderState();
	m_tlsEncryptionEnabled = isEnabledFlag(m_worldAttributes.value(QStringLiteral("tls_encryption")));
	if (m_tlsEncryptionEnabled)
	{
		const int configuredTlsMethod = m_worldAttributes.value(QStringLiteral("tls_method")).toInt();
		if (configuredTlsMethod == eTlsStartTls)
			m_tlsMethod = eTlsStartTls;
		else
			m_tlsMethod = eTlsDirect;
		m_tlsDisableCertificateValidation =
		    isEnabledFlag(m_worldAttributes.value(QStringLiteral("tls_disable_certificate_validation")));
	}
	else
	{
		m_tlsMethod                       = eTlsDirect;
		m_tlsDisableCertificateValidation = false;
	}
	m_telnet.setStartTlsEnabled(m_tlsEncryptionEnabled && m_tlsMethod == eTlsStartTls);
#if !QT_CONFIG(ssl)
	if (m_tlsEncryptionEnabled)
	{
		const QString message =
		    QStringLiteral("World has TLS option enabled but TLS is not enabled in this build.");
		outputText(message, true, true);
		m_connectViaProxy     = false;
		m_connectPhase        = eConnectNotConnected;
		m_socketReadyForWorld = false;
		return false;
	}
#endif
	const bool    useUtf8   = isEnabledFlag(m_worldAttributes.value(QStringLiteral("utf_8")));
	const bool    keepAlive = isEnabledFlag(m_worldAttributes.value(QStringLiteral("send_keep_alives")));
	const int     configuredProxyType = m_worldAttributes.value(QStringLiteral("proxy_type")).toInt();
	const QString proxyServer         = m_worldAttributes.value(QStringLiteral("proxy_server")).trimmed();
	const quint16 proxyPort =
	    static_cast<quint16>(m_worldAttributes.value(QStringLiteral("proxy_port")).toUInt());
	const QString proxyUsername = m_worldAttributes.value(QStringLiteral("proxy_username"));
	const QString proxyPassword = m_worldAttributes.value(QStringLiteral("proxy_password"));
	m_connectViaProxy =
	    (configuredProxyType == eProxyServerSocks4 || configuredProxyType == eProxyServerSocks5) &&
	    !proxyServer.isEmpty() && proxyPort != 0;
	WorldSocketConnectionSettings connectionSettings;
	connectionSettings.useUtf8                         = useUtf8;
	connectionSettings.keepAlive                       = keepAlive;
	connectionSettings.tlsEncryption                   = m_tlsEncryptionEnabled;
	connectionSettings.tlsMethod                       = m_tlsMethod;
	connectionSettings.disableTlsCertificateValidation = m_tlsDisableCertificateValidation;
	connectionSettings.tlsServerName                   = host.trimmed();
	connectionSettings.proxyType     = m_connectViaProxy ? configuredProxyType : eProxyServerNone;
	connectionSettings.proxyServer   = m_connectViaProxy ? proxyServer : QString();
	connectionSettings.proxyPort     = m_connectViaProxy ? proxyPort : 0;
	connectionSettings.proxyUsername = m_connectViaProxy ? proxyUsername : QString();
	connectionSettings.proxyPassword = m_connectViaProxy ? proxyPassword : QString();
	m_socket->applyConnectionSettings(connectionSettings);
	if (m_connectViaProxy)
	{
		QHostAddress proxyAddr;
		if (proxyAddr.setAddress(proxyServer) && proxyAddr.protocol() == QAbstractSocket::IPv4Protocol)
		{
			m_proxyAddressString = proxyAddr.toString();
			m_proxyAddressV4     = proxyAddr.toIPv4Address();
		}
		else
		{
			const QHostInfo info = QHostInfo::fromName(proxyServer);
			if (info.error() == QHostInfo::NoError)
			{
				for (const QHostAddress &candidate : info.addresses())
				{
					if (candidate.protocol() == QAbstractSocket::IPv4Protocol)
					{
						m_proxyAddressString = candidate.toString();
						m_proxyAddressV4     = candidate.toIPv4Address();
						break;
					}
				}
			}
		}
		m_connectPhase = eConnectConnectingToProxy;
	}
	else
	{
		m_connectPhase = eConnectConnectingToMud;
	}
	QString targetHost = host;
	if (m_connectViaProxy && connectionSettings.proxyType == eProxyServerSocks4)
	{
		// SOCKS4 does not support hostnames; resolve to IPv4 first.
		QHostAddress hostAddr;
		if (hostAddr.setAddress(host) && hostAddr.protocol() == QAbstractSocket::IPv4Protocol)
			targetHost = hostAddr.toString();
		else
		{
			const QHostInfo info = QHostInfo::fromName(host);
			if (info.error() == QHostInfo::NoError)
			{
				for (const QHostAddress &candidate : info.addresses())
				{
					if (candidate.protocol() == QAbstractSocket::IPv4Protocol)
					{
						targetHost = candidate.toString();
						break;
					}
				}
			}
		}
		QHostAddress resolvedTarget;
		if (!resolvedTarget.setAddress(targetHost) ||
		    resolvedTarget.protocol() != QAbstractSocket::IPv4Protocol)
		{
			m_connectViaProxy = false;
			m_connectPhase    = eConnectNotConnected;
			emit socketError(
			    QStringLiteral("Unable to resolve an IPv4 address for SOCKS4 connection target."));
			return false;
		}
	}

	const bool ok = m_socket->connectToHost(targetHost, port);
	if (!ok)
	{
		m_connectViaProxy = false;
		m_connectPhase    = eConnectNotConnected;
	}
	return ok;
}

void WorldRuntime::disconnectFromWorld()
{
	if (!m_socket)
		return;
	if (m_connectPhase == eConnectNotConnected || m_connectPhase == eConnectDisconnecting)
		return;
	cancelStartTlsFallbackTimer();
	cancelReloadMccpProbeTimeout();
	m_reloadReattachMccpProbePending        = false;
	m_reloadReattachMccpResumePending       = false;
	m_reloadReattachLookProbeSent           = false;
	m_reloadReattachMccpProbeDecisionOffset = 0;
	m_reloadMccpProbeTimeoutPass            = 0;
	m_reloadReattachMccpProbeBuffer.clear();
	m_reloadReattachUseDeferredMxpReplay = false;
	m_reloadReattachMxpProbeEvents.clear();
	m_reloadReattachMxpProbeModeChanges.clear();
	m_disconnectOk = true;
	m_connectPhase = eConnectDisconnecting;
	m_socket->disconnectFromHost();
}

int WorldRuntime::nativeSocketDescriptor() const
{
	if (!m_socket)
		return -1;
	const qintptr descriptor = m_socket->nativeSocketDescriptor();
	return descriptor < 0 ? -1 : static_cast<int>(descriptor);
}

bool WorldRuntime::adoptConnectedSocketDescriptor(const int descriptor, QString *errorMessage)
{
	if (errorMessage)
		errorMessage->clear();
	if (!m_socket)
	{
		if (errorMessage)
			*errorMessage = QStringLiteral("Socket service is unavailable.");
		return false;
	}
	if (descriptor < 0)
	{
		if (errorMessage)
			*errorMessage = QStringLiteral("Socket descriptor is invalid.");
		return false;
	}

	const QList<TelnetProcessor::CustomElementInfo> preservedCustomMxpElements =
	    m_telnet.customElementInfos();
	const TelnetProcessor::MxpSessionState preservedMxpSessionState = m_telnet.mxpSessionState();
	const bool                             preservedMxpActive       = m_mxpActive;

	cancelStartTlsFallbackTimer();
	cancelReloadMccpProbeTimeout();
	m_reloadReattachMccpProbePending        = false;
	m_reloadReattachMccpResumePending       = false;
	m_reloadReattachLookProbeSent           = false;
	m_reloadReattachMccpProbeDecisionOffset = 0;
	m_reloadMccpProbeTimeoutPass            = 0;
	m_reloadReattachMccpProbeBuffer.clear();
	m_reloadReattachUseDeferredMxpReplay = false;
	m_reloadReattachMxpProbeEvents.clear();
	m_reloadReattachMxpProbeModeChanges.clear();
	m_disconnectOk        = false;
	m_socketReadyForWorld = false;
	m_telnet.resetConnectionState();
	m_telnet.setStartTlsEnabled(false);
	m_telnet.setCustomElementInfos(preservedCustomMxpElements);
	m_telnet.setMxpSessionState(preservedMxpSessionState);
	m_mxpActive = preservedMxpActive && preservedMxpSessionState.enabled;
	resetAnsiRenderState();
	m_mxpTagStack.clear();
	m_mxpOpenTags.clear();
	m_mxpTextBuffer.clear();
	m_incomingSocketDataPaused = false;

	QString adoptionError;
	if (!m_socket->adoptConnectedSocketDescriptor(static_cast<qintptr>(descriptor), &adoptionError))
	{
		if (errorMessage)
			*errorMessage = adoptionError;
		m_connectViaProxy = false;
		m_connectPhase    = eConnectNotConnected;
		return false;
	}

	m_connectViaProxy                 = false;
	m_connectPhase                    = eConnectConnectedToMud;
	m_tlsEncryptionEnabled            = false;
	m_tlsMethod                       = eTlsDirect;
	m_tlsDisableCertificateValidation = false;
	m_hasCachedIp                     = true;
	m_connectTime                     = QDateTime::currentDateTime();
	const bool convertGA = isEnabledFlag(m_worldAttributes.value(QStringLiteral("convert_ga_to_newline")));
	m_telnet.setConvertGAtoNewline(convertGA);
	m_socketReadyForWorld = true;
	emit connected();
	return true;
}

void WorldRuntime::closeSocketForReloadReconnect()
{
	if (!m_socket)
		return;
	cancelStartTlsFallbackTimer();
	cancelReloadMccpProbeTimeout();
	m_reloadReattachMccpProbePending        = false;
	m_reloadReattachMccpResumePending       = false;
	m_reloadReattachLookProbeSent           = false;
	m_reloadReattachMccpProbeDecisionOffset = 0;
	m_reloadMccpProbeTimeoutPass            = 0;
	m_reloadReattachMccpProbeBuffer.clear();
	m_reloadReattachUseDeferredMxpReplay = false;
	m_reloadReattachMxpProbeEvents.clear();
	m_reloadReattachMxpProbeModeChanges.clear();
	m_incomingSocketDataPaused        = false;
	m_disconnectOk                    = true;
	m_connectViaProxy                 = false;
	m_connectPhase                    = eConnectNotConnected;
	m_tlsDisableCertificateValidation = false;
	m_socketReadyForWorld             = false;
	m_socket->abortSocket();
	m_telnet.resetConnectionState();
	m_telnet.setStartTlsEnabled(false);
	resetAnsiRenderState();
}

void WorldRuntime::setIncomingSocketDataPaused(const bool paused)
{
	m_incomingSocketDataPaused = paused;
}

bool WorldRuntime::incomingSocketDataPaused() const
{
	return m_incomingSocketDataPaused;
}

void WorldRuntime::markReloadReattachConnectActionsSuppressed()
{
	m_reloadReattachSuppressConnectActions = true;
}

bool WorldRuntime::reloadReattachConnectActionsSuppressed() const
{
	return m_reloadReattachSuppressConnectActions;
}

bool WorldRuntime::consumeReloadReattachConnectActionsSuppressed()
{
	const bool suppressed                  = m_reloadReattachSuppressConnectActions;
	m_reloadReattachSuppressConnectActions = false;
	return suppressed;
}

void WorldRuntime::requestMccpResumeAfterReloadReattach()
{
	if (!m_socket || !m_socket->isConnected())
		return;
	if (isCompressing() || mccpType() != 0)
		return;
	if (const bool disableCompression =
	        isEnabledFlag(m_worldAttributes.value(QStringLiteral("disable_compression")));
	    disableCompression)
	{
		return;
	}
	m_telnet.queueEnableCompression2Negotiation();
	if (const QByteArray outbound = m_telnet.takeOutboundData(); !outbound.isEmpty())
		sendToWorld(outbound);
}

void WorldRuntime::configureReloadMccpReattachProbe(const bool enabled)
{
	cancelReloadMccpProbeTimeout();
	m_reloadReattachMccpProbePending        = enabled;
	m_reloadReattachMccpResumePending       = enabled;
	m_reloadReattachLookProbeSent           = false;
	m_reloadReattachMccpProbeDecisionOffset = 0;
	m_reloadMccpProbeTimeoutPass            = 0;
	m_reloadReattachMccpProbeBuffer.clear();
	m_reloadReattachUseDeferredMxpReplay = false;
	m_reloadReattachMxpProbeEvents.clear();
	m_reloadReattachMxpProbeModeChanges.clear();
	if (enabled)
		armReloadMccpProbeTimeout();
}

void WorldRuntime::sendReloadReattachLookProbe()
{
	if (!m_socket || !m_socket->isConnected())
		return;
	if (m_reloadReattachMccpProbePending)
	{
		m_reloadReattachLookProbeSent           = true;
		m_reloadReattachMccpProbeDecisionOffset = m_reloadReattachMccpProbeBuffer.size();
	}
	sendToWorld(QByteArrayLiteral("look\r\n"));
}

void WorldRuntime::armReloadMccpProbeTimeout()
{
	const quint64 generation     = ++m_reloadMccpProbeGeneration;
	m_reloadMccpProbeTimeoutPass = 0;
	armReloadMccpProbeTimeoutPass(generation, 0);
}

void WorldRuntime::armReloadMccpProbeTimeoutPass(const quint64 generation, const int pass)
{
	QTimer::singleShot(
	    500, this,
	    [this, generation, pass]
	    {
		    if (generation != m_reloadMccpProbeGeneration)
			    return;
		    if (!m_reloadReattachMccpProbePending)
			    return;
		    const ReloadMccpProbePayloads payloads =
		        makeReloadMccpProbePayloads(m_reloadReattachMccpProbeBuffer, m_reloadReattachLookProbeSent,
		                                    m_reloadReattachMccpProbeDecisionOffset);
		    const ReloadMccpProbeTimeoutAction timeoutAction =
		        resolveReloadMccpProbeTimeoutAction(payloads.decisionPayload, pass);
		    if (timeoutAction == ReloadMccpProbeTimeoutAction::WaitSecondPass)
		    {
			    m_reloadMccpProbeTimeoutPass = 1;
			    armReloadMccpProbeTimeoutPass(generation, 1);
			    return;
		    }

		    m_reloadReattachMccpProbePending        = false;
		    m_reloadMccpProbeTimeoutPass            = 0;
		    const QByteArray decisionPayload        = payloads.decisionPayload;
		    m_reloadReattachLookProbeSent           = false;
		    m_reloadReattachMccpProbeDecisionOffset = 0;
		    if (timeoutAction == ReloadMccpProbeTimeoutAction::Reconnect)
		    {
			    m_reloadReattachMccpResumePending = false;
			    m_reloadReattachMccpProbeBuffer.clear();
			    m_reloadReattachUseDeferredMxpReplay = false;
			    m_reloadReattachMxpProbeEvents.clear();
			    m_reloadReattachMxpProbeModeChanges.clear();
			    if (decisionPayload.isEmpty())
			    {
				    outputText(QStringLiteral("Reload reattach probe timed out without response. "
				                              "Reconnecting."),
				               true, true);
			    }
			    else
			    {
				    outputText(QStringLiteral("Reload reattach detected active MCCP stream. "
				                              "Reconnecting."),
				               true, true);
			    }
			    const QString host = m_worldAttributes.value(QStringLiteral("site")).trimmed();
			    const quint16 port = m_worldAttributes.value(QStringLiteral("port")).toUShort();
			    closeSocketForReloadReconnect();
			    if (!host.isEmpty() && port != 0)
			    {
				    if (!connectToWorld(host, port))
					    outputText(QStringLiteral("Reload reconnect failed to start after MCCP "
					                              "probe fallback."),
					               true, true);
			    }
			    else
			    {
				    outputText(QStringLiteral("Reload reconnect skipped after MCCP probe fallback "
				                              "due to missing host/port."),
				               true, true);
			    }
			    return;
		    }

		    const bool resumeAfterReplay      = m_reloadReattachMccpResumePending;
		    m_reloadReattachMccpResumePending = false;
		    const QByteArray buffered = takeDeferredReloadMccpProbePayload(&m_reloadReattachMccpProbeBuffer);
		    if (!buffered.isEmpty())
		    {
			    const bool wasSimulated = m_doingSimulate;
			    m_doingSimulate         = true;
			    receiveRawData(buffered);
			    m_doingSimulate = wasSimulated;
		    }
		    if (m_reloadReattachUseDeferredMxpReplay)
		    {
			    m_reloadReattachUseDeferredMxpReplay = false;
			    m_reloadReattachMxpProbeEvents.clear();
			    m_reloadReattachMxpProbeModeChanges.clear();
		    }
		    if (resumeAfterReplay)
			    requestMccpResumeAfterReloadReattach();
	    });
}

void WorldRuntime::cancelReloadMccpProbeTimeout()
{
	++m_reloadMccpProbeGeneration;
	m_reloadMccpProbeTimeoutPass = 0;
}

void WorldRuntime::queueMccpDisableForReload()
{
	if (isMccpDisableCompleteForReload())
		return;

	m_telnet.queueDisableCompressionNegotiation();
	const QByteArray outbound = m_telnet.takeOutboundData();
	if (!outbound.isEmpty())
		sendToWorld(outbound);
}

bool WorldRuntime::isMccpDisableCompleteForReload() const
{
	return !isCompressing() && mccpType() == 0;
}

bool WorldRuntime::requestMccpDisableForReload(const int timeoutMs)
{
	queueMccpDisableForReload();

	QElapsedTimer timer;
	timer.start();
	const int boundedTimeoutMs = qMax(0, timeoutMs);
	while (timer.elapsed() < boundedTimeoutMs)
	{
		QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
		if (isMccpDisableCompleteForReload())
			return true;
	}
	return isMccpDisableCompleteForReload();
}

void WorldRuntime::sendToWorld(const QByteArray &payload)
{
	if (!m_socket)
		return;

	const qint64 written = m_socket->sendPacket(payload);
	if (written <= 0)
		return;

	m_bytesOut += written;
	m_outputPacketCount++;
	if (m_debugIncomingPackets)
	{
		auto appendPacketDebug = [this](const QString &caption, const QByteArray &packet, qint64 number)
		{
			const QString worldName = m_worldAttributes.value(QStringLiteral("name"));
			const QString title     = QStringLiteral("Packet debug - %1")
			                          .arg(worldName.isEmpty() ? QStringLiteral("World") : worldName);
			const QDateTime now = QDateTime::currentDateTime();
			const QString   timestamp =
			    QLocale::system().toString(now, QStringLiteral("dddd, MMMM dd, yyyy, h:mm:ss AP"));
			QString const header = QStringLiteral("\r\n%1 packet: %2 (%3 bytes) at %4\r\n\r\n")
			                           .arg(caption)
			                           .arg(number)
			                           .arg(packet.size())
			                           .arg(timestamp);
			if (!callPluginCallbacksStopOnTrueWithString(QStringLiteral("OnPluginPacketDebug"), header))
			{
				if (AppController const *app = AppController::instance())
					if (MainWindow *main = app->mainWindow())
						main->appendToNotepad(title, header, false, this);
			}

			int remaining = safeQSizeToInt(packet.size());
			int offset    = 0;
			while (remaining > 0)
			{
				const int count = qMin(remaining, kPacketDebugChars);
				QString   ascii;
				QString   hex;
				ascii.reserve(kPacketDebugChars);
				hex.reserve(kPacketDebugChars * 3);
				for (int i = 0; i < count; ++i)
				{
					const auto byte = static_cast<unsigned char>(packet.at(offset + i));
					ascii += (isAsciiPrintableByte(byte) ? QChar(byte) : QChar('.'));
					hex += QString::asprintf(" %02x", byte);
				}
				const QString line = QStringLiteral("%1%2%3\r\n")
				                         .arg(ascii.leftJustified(kPacketDebugChars, QLatin1Char(' ')))
				                         .arg(QLatin1Char(' '))
				                         .arg(hex);
				if (!callPluginCallbacksStopOnTrueWithString(QStringLiteral("OnPluginPacketDebug"), line))
				{
					if (AppController const *app = AppController::instance())
						if (MainWindow *main = app->mainWindow())
							main->appendToNotepad(title, line, false, this);
				}
				remaining -= count;
				offset += count;
			}
		};
		appendPacketDebug(QStringLiteral("Sent "), payload, m_outputPacketCount);
	}
}

void WorldRuntime::incrementLinesSent()
{
	m_totalLinesSent++;
}

int WorldRuntime::totalLinesSent() const
{
	return m_totalLinesSent;
}

int WorldRuntime::totalLinesReceived() const
{
	return m_linesReceived;
}

int WorldRuntime::newLines() const
{
	return m_newLines;
}

void WorldRuntime::setNewLines(int value)
{
	m_newLines = value;
}

void WorldRuntime::incrementNewLines()
{
	++m_newLines;
	const bool qtAppFocused  = QGuiApplication::applicationState() == Qt::ApplicationActive;
	bool       windowFocused = qtAppFocused;
	if (const AppController *app = AppController::instance())
	{
		if (const MainWindow *mainWindow = app->mainWindow())
			windowFocused = mainWindow->isApplicationFocused();
	}
	const bool appFocusedForFlash =
	    QMudMainFrameActionUtils::resolveIncomingLineFocusForFlash(qtAppFocused, windowFocused);
	const bool appFocusedForSound =
	    QMudMainFrameActionUtils::resolveIncomingLineFocusForActivitySound(qtAppFocused, windowFocused);
	const bool worldFlashEnabled =
	    isEnabledFlag(m_worldAttributes.value(QStringLiteral("flash_taskbar_icon")));
	if (QMudMainFrameActionUtils::shouldAttemptIncomingLineTaskbarFlash(worldFlashEnabled,
	                                                                    appFocusedForFlash))
	{
		flashTaskbarForView(m_view);
	}

	const bool worldInactive          = !m_active;
	const bool backgroundWindowActive = !appFocusedForSound;
	if (worldInactive || backgroundWindowActive)
	{
		const QString sound = m_worldAttributes.value(QStringLiteral("new_activity_sound")).trimmed();
		if (!sound.isEmpty() && sound.compare(QStringLiteral("(No sound)"), Qt::CaseInsensitive) != 0)
		{
			const bool allowBackground =
			    isEnabledFlag(m_worldAttributes.value(QStringLiteral("play_sounds_in_background")));
			if (appFocusedForSound || allowBackground)
				playSound(0, sound, false, 0.0, 0.0);
		}
	}
}

void WorldRuntime::clearNewLines()
{
	m_newLines = 0;
}

void WorldRuntime::setActive(bool active)
{
	m_active = active;
	if (active)
		clearNewLines();
}

bool WorldRuntime::isActive() const
{
	return m_active;
}

bool WorldRuntime::doNotShowOutstandingLines() const
{
	const QString flag = m_worldAttributes.value(QStringLiteral("do_not_show_outstanding_lines"));
	return flag == QStringLiteral("1") || flag.compare(QStringLiteral("y"), Qt::CaseInsensitive) == 0 ||
	       flag.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
}

void WorldRuntime::setCurrentActionSource(unsigned short source)
{
	m_currentActionSource = source;
}

unsigned short WorldRuntime::currentActionSource() const
{
	return m_currentActionSource;
}

void WorldRuntime::incrementUtf8ErrorCount()
{
	++m_utf8ErrorCount;
}

int WorldRuntime::utf8ErrorCount() const
{
	return m_utf8ErrorCount;
}

void WorldRuntime::setLastLineWithIacGa(int lineNumber)
{
	m_lastLineWithIacGa = lineNumber;
}

int WorldRuntime::lastLineWithIacGa() const
{
	return m_lastLineWithIacGa;
}

void WorldRuntime::incrementTriggersMatched()
{
	m_triggersMatchedThisSession++;
}

void WorldRuntime::incrementTriggersEvaluated()
{
	m_triggersEvaluatedCount++;
}

void WorldRuntime::incrementAliasesMatched()
{
	m_aliasesMatchedThisSession++;
}

void WorldRuntime::incrementAliasesEvaluated()
{
	m_aliasesEvaluatedCount++;
}

void WorldRuntime::incrementTimersFired()
{
	m_timersFiredThisSession++;
}

int WorldRuntime::triggersEvaluatedCount() const
{
	return m_triggersEvaluatedCount;
}

int WorldRuntime::triggersMatchedThisSession() const
{
	return m_triggersMatchedThisSession;
}

int WorldRuntime::aliasesEvaluatedCount() const
{
	return m_aliasesEvaluatedCount;
}

int WorldRuntime::aliasesMatchedThisSession() const
{
	return m_aliasesMatchedThisSession;
}

int WorldRuntime::timersFiredThisSession() const
{
	return m_timersFiredThisSession;
}

void WorldRuntime::setLastUserInput(const QDateTime &when)
{
	m_lastUserInput = when;
}

QDateTime WorldRuntime::lastUserInput() const
{
	return m_lastUserInput;
}

int WorldRuntime::addFontFromFile(const QString &path)
{
	const QString trimmed = path.trimmed();
	if (trimmed.isEmpty())
		return eBadParameter;

	QFileInfo const info(trimmed);
	if (!info.exists())
		return eFileNotFound;

	const QString key = info.absoluteFilePath();
	if (m_specialFontPaths.contains(key))
		return eOK;

	const int fontId = QFontDatabase::addApplicationFont(key);
	if (fontId < 0)
		return eFileNotFound;

	m_specialFontPaths.insert(key);
	m_specialFontPathOrder.append(key);
	m_specialFontIds.append(fontId);
	return eOK;
}

QString WorldRuntime::firstSpecialFontPath() const
{
	if (m_specialFontPathOrder.isEmpty())
		return {};
	return m_specialFontPathOrder.first();
}

bool WorldRuntime::isConnected() const
{
	return m_connectPhase == eConnectConnectedToMud;
}

bool WorldRuntime::isNawsNegotiated() const
{
	return m_telnet.isNawsNegotiated();
}

int WorldRuntime::outputWrapColumns() const
{
	return m_telnet.windowColumns();
}

int WorldRuntime::connectPhase() const
{
	return m_connectPhase;
}

int WorldRuntime::lastPreferencesPage() const
{
	return m_lastPreferencesPage;
}

void WorldRuntime::setLastPreferencesPage(int page)
{
	m_lastPreferencesPage = page;
}

QString WorldRuntime::lastTriggerTreeExpandedGroup() const
{
	return m_lastTriggerTreeExpandedGroup;
}

void WorldRuntime::setLastTriggerTreeExpandedGroup(const QString &group)
{
	m_lastTriggerTreeExpandedGroup = group;
}

QString WorldRuntime::lastAliasTreeExpandedGroup() const
{
	return m_lastAliasTreeExpandedGroup;
}

void WorldRuntime::setLastAliasTreeExpandedGroup(const QString &group)
{
	m_lastAliasTreeExpandedGroup = group;
}

QString WorldRuntime::lastTimerTreeExpandedGroup() const
{
	return m_lastTimerTreeExpandedGroup;
}

void WorldRuntime::setLastTimerTreeExpandedGroup(const QString &group)
{
	m_lastTimerTreeExpandedGroup = group;
}

QDateTime WorldRuntime::connectTime() const
{
	return m_connectTime;
}

void WorldRuntime::setDisconnectOk(bool ok)
{
	m_disconnectOk = ok;
}

bool WorldRuntime::disconnectOk() const
{
	return m_disconnectOk;
}

void WorldRuntime::setReconnectOnLinkFailure(bool enabled)
{
	m_reconnectOnLinkFailure = enabled;
}

bool WorldRuntime::reconnectOnLinkFailure() const
{
	return m_reconnectOnLinkFailure;
}

QDateTime WorldRuntime::statusTime() const
{
	return m_statusTime;
}

void WorldRuntime::resetStatusTime()
{
	m_statusTime = QDateTime::currentDateTime();
}

QDateTime WorldRuntime::lastFlushTime() const
{
	return m_lastFlushTime;
}

QDateTime WorldRuntime::scriptFileModTime() const
{
	const QString scriptFile = m_worldAttributes.value(QStringLiteral("script_filename"));
	if (scriptFile.isEmpty())
		return {};
	const QString   workingDir = resolveWorkingDir(m_startupDirectory);
	const QString   path       = makeAbsolutePath(scriptFile, workingDir);
	QFileInfo const info(path);
	if (!info.exists())
		return {};
	return info.lastModified();
}

QDateTime WorldRuntime::clientStartTime() const
{
	return m_clientStartTime;
}

QDateTime WorldRuntime::worldStartTime() const
{
	return m_worldStartTime;
}

void WorldRuntime::setClientStartTime(const QDateTime &when)
{
	m_clientStartTime = when;
}

void WorldRuntime::setWorldFilePath(const QString &path)
{
	m_worldFilePath = path;
}

QString WorldRuntime::worldFilePath() const
{
	return m_worldFilePath;
}

WorldRuntime::SaveSnapshot WorldRuntime::buildSaveSnapshot(const QString &fileName) const
{
	SaveSnapshot snapshot;
	snapshot.targetFilePath           = fileName.trimmed();
	snapshot.startupDirectory         = m_startupDirectory;
	snapshot.worldFilePath            = m_worldFilePath;
	snapshot.pluginsDirectory         = m_pluginsDirectory;
	snapshot.worldAttributes          = m_worldAttributes;
	snapshot.worldMultilineAttributes = m_worldMultilineAttributes;
	snapshot.includes                 = m_includes;
	snapshot.triggers                 = m_triggers;
	snapshot.aliases                  = m_aliases;
	snapshot.timers                   = m_timers;
	snapshot.macros                   = m_macros;
	snapshot.variables                = m_variables;
	snapshot.colours                  = m_colours;
	snapshot.keypadEntries            = m_keypadEntries;
	snapshot.printingStyles           = m_printingStyles;
	snapshot.plugins.reserve(m_plugins.size());
	for (const Plugin &plugin : m_plugins)
	{
		SavePluginSnapshot pluginSnapshot;
		pluginSnapshot.attributes = plugin.attributes;
		pluginSnapshot.source     = plugin.source;
		pluginSnapshot.global     = plugin.global;
		snapshot.plugins.push_back(pluginSnapshot);
	}
	return snapshot;
}

bool WorldRuntime::saveStateMatchesSnapshot(const SaveSnapshot &snapshot) const
{
	if (m_worldAttributes != snapshot.worldAttributes ||
	    m_worldMultilineAttributes != snapshot.worldMultilineAttributes ||
	    m_plugins.size() != snapshot.plugins.size())
	{
		return false;
	}

	auto includesEqual = [&](const QList<Include> &current, const QList<Include> &saved) -> bool
	{
		if (current.size() != saved.size())
			return false;
		for (int i = 0; i < current.size(); ++i)
		{
			if (current.at(i).attributes != saved.at(i).attributes)
				return false;
		}
		return true;
	};
	if (!includesEqual(m_includes, snapshot.includes))
		return false;

	auto triggersEqual = [&](const QList<Trigger> &current, const QList<Trigger> &saved) -> bool
	{
		if (current.size() != saved.size())
			return false;
		for (int i = 0; i < current.size(); ++i)
		{
			const Trigger &a = current.at(i);
			const Trigger &b = saved.at(i);
			if (a.attributes != b.attributes || a.children != b.children || a.included != b.included)
				return false;
		}
		return true;
	};
	if (!triggersEqual(m_triggers, snapshot.triggers))
		return false;

	auto aliasesEqual = [&](const QList<Alias> &current, const QList<Alias> &saved) -> bool
	{
		if (current.size() != saved.size())
			return false;
		for (int i = 0; i < current.size(); ++i)
		{
			const Alias &a = current.at(i);
			const Alias &b = saved.at(i);
			if (a.attributes != b.attributes || a.children != b.children || a.included != b.included)
				return false;
		}
		return true;
	};
	if (!aliasesEqual(m_aliases, snapshot.aliases))
		return false;

	auto timersEqual = [&](const QList<Timer> &current, const QList<Timer> &saved) -> bool
	{
		if (current.size() != saved.size())
			return false;
		for (int i = 0; i < current.size(); ++i)
		{
			const Timer &a = current.at(i);
			const Timer &b = saved.at(i);
			if (a.attributes != b.attributes || a.children != b.children || a.included != b.included)
				return false;
		}
		return true;
	};
	if (!timersEqual(m_timers, snapshot.timers))
		return false;

	auto macrosEqual = [&](const QList<Macro> &current, const QList<Macro> &saved) -> bool
	{
		if (current.size() != saved.size())
			return false;
		for (int i = 0; i < current.size(); ++i)
		{
			if (current.at(i).attributes != saved.at(i).attributes ||
			    current.at(i).children != saved.at(i).children)
				return false;
		}
		return true;
	};
	if (!macrosEqual(m_macros, snapshot.macros))
		return false;

	auto variablesEqual = [&](const QList<Variable> &current, const QList<Variable> &saved) -> bool
	{
		if (current.size() != saved.size())
			return false;
		for (int i = 0; i < current.size(); ++i)
		{
			if (current.at(i).attributes != saved.at(i).attributes ||
			    current.at(i).content != saved.at(i).content)
				return false;
		}
		return true;
	};
	if (!variablesEqual(m_variables, snapshot.variables))
		return false;

	auto coloursEqual = [&](const QList<Colour> &current, const QList<Colour> &saved) -> bool
	{
		if (current.size() != saved.size())
			return false;
		for (int i = 0; i < current.size(); ++i)
		{
			if (current.at(i).group != saved.at(i).group ||
			    current.at(i).attributes != saved.at(i).attributes)
				return false;
		}
		return true;
	};
	if (!coloursEqual(m_colours, snapshot.colours))
		return false;

	auto keypadEqual = [&](const QList<Keypad> &current, const QList<Keypad> &saved) -> bool
	{
		if (current.size() != saved.size())
			return false;
		for (int i = 0; i < current.size(); ++i)
		{
			if (current.at(i).attributes != saved.at(i).attributes ||
			    current.at(i).content != saved.at(i).content)
				return false;
		}
		return true;
	};
	if (!keypadEqual(m_keypadEntries, snapshot.keypadEntries))
		return false;

	auto printingEqual = [&](const QList<PrintingStyle> &current, const QList<PrintingStyle> &saved) -> bool
	{
		if (current.size() != saved.size())
			return false;
		for (int i = 0; i < current.size(); ++i)
		{
			if (current.at(i).group != saved.at(i).group ||
			    current.at(i).attributes != saved.at(i).attributes)
				return false;
		}
		return true;
	};
	if (!printingEqual(m_printingStyles, snapshot.printingStyles))
		return false;

	for (int i = 0; i < m_plugins.size(); ++i)
	{
		const Plugin             &plugin = m_plugins.at(i);
		const SavePluginSnapshot &saved  = snapshot.plugins.at(i);
		if (plugin.attributes != saved.attributes || plugin.source != saved.source ||
		    plugin.global != saved.global)
		{
			return false;
		}
	}

	return true;
}

bool WorldRuntime::saveWorldFile(const QString &fileName, QString *error)
{
	const QString trimmed = fileName.trimmed();
	if (trimmed.isEmpty())
	{
		if (error)
			*error = QStringLiteral("No filename specified");
		return false;
	}

	if (m_worldAttributes.value(QStringLiteral("id")).trimmed().isEmpty())
		m_worldAttributes.insert(QStringLiteral("id"), QMudWorldOptionDefaults::generateWorldUniqueId());

	// Preserve ordering: run world/plugin save callbacks before persisting.
	fireWorldSaveHandlers();

	SaveSnapshot const snapshot = buildSaveSnapshot(trimmed);
	if (!writeSaveSnapshot(snapshot, error))
		return false;

	return true;
}

void WorldRuntime::saveWorldFileAsync(const QString                             &fileName,
                                      std::function<void(bool, const QString &)> completion)
{
	const QString trimmed = fileName.trimmed();
	if (trimmed.isEmpty())
	{
		if (completion)
			completion(false, QStringLiteral("No filename specified"));
		return;
	}

	if (m_worldAttributes.value(QStringLiteral("id")).trimmed().isEmpty())
		m_worldAttributes.insert(QStringLiteral("id"), QMudWorldOptionDefaults::generateWorldUniqueId());

	// Preserve ordering: run world/plugin save callbacks before persisting.
	fireWorldSaveHandlers();

	auto           snapshot = QSharedPointer<SaveSnapshot>::create(buildSaveSnapshot(trimmed));
	QPointer const guard(this);
	auto           completionFn =
	    QSharedPointer<std::function<void(bool, const QString &)>>::create(std::move(completion));

	QThreadPool::globalInstance()->start(
	    [guard, snapshot, completionFn]
	    {
		    QString    saveError;
		    const bool ok = writeSaveSnapshot(*snapshot, &saveError);
		    QMetaObject::invokeMethod(
		        qApp,
		        [guard, snapshot, completionFn, ok, saveError]
		        {
			        if (!guard)
				        return;

			        if (ok)
			        {
				        guard->m_worldFilePath = snapshot->targetFilePath;
				        if (guard->saveStateMatchesSnapshot(*snapshot))
				        {
					        guard->setWorldFileModified(false);
					        guard->setVariablesChanged(false);
				        }
			        }

			        if (completionFn && *completionFn)
				        (*completionFn)(ok, saveError);
		        },
		        Qt::QueuedConnection);
	    });
}

bool WorldRuntime::writeSaveSnapshot(const SaveSnapshot &snapshot, QString *error)
{
	SaveSnapshot  normalizedSnapshot = snapshot;
	const QString workingDir         = resolveWorkingDir(normalizedSnapshot.startupDirectory);
	const QString worldDir           = normalizedSnapshot.worldFilePath.trimmed().isEmpty()
	                                       ? QString()
	                                       : QFileInfo(normalizedSnapshot.worldFilePath.trimmed()).absolutePath();
	const QString pluginsDir         = normalizedSnapshot.pluginsDirectory.trimmed();

	const auto    normalizePathAttributes = [&](QMap<QString, QString> &attributes)
	{ normalizePathAttributeMapForStorage(attributes, workingDir); };
	const auto normalizeEntryPathAttributes = [&](auto &entries)
	{
		for (auto &entry : entries)
			normalizePathAttributes(entry.attributes);
	};

	normalizePathAttributes(normalizedSnapshot.worldAttributes);
	normalizeEntryPathAttributes(normalizedSnapshot.triggers);
	normalizeEntryPathAttributes(normalizedSnapshot.aliases);
	normalizeEntryPathAttributes(normalizedSnapshot.timers);
	normalizeEntryPathAttributes(normalizedSnapshot.macros);
	normalizeEntryPathAttributes(normalizedSnapshot.variables);
	normalizeEntryPathAttributes(normalizedSnapshot.colours);
	normalizeEntryPathAttributes(normalizedSnapshot.keypadEntries);
	normalizeEntryPathAttributes(normalizedSnapshot.printingStyles);

	for (auto &includeEntry : normalizedSnapshot.includes)
	{
		QString includeName = includeEntry.attributes.value(QStringLiteral("name")).trimmed();
		if (includeName.isEmpty())
			continue;
		const QString normalizedInclude = normalizePathForStorage(includeName);
		const bool    hasPortableRoot   = !extractPortableRootRelativePath(normalizedInclude).isEmpty();
		if (!isAbsolutePathLike(normalizedInclude) && !hasPortableRoot && !worldDir.isEmpty())
			includeName = QDir(worldDir).filePath(normalizedInclude);
		includeEntry.attributes.insert(QStringLiteral("name"),
		                               canonicalizePathForStorage(includeName, workingDir));
	}

	for (auto &plugin : normalizedSnapshot.plugins)
	{
		normalizePathAttributes(plugin.attributes);
		QString source = plugin.source.trimmed();
		if (!source.isEmpty())
		{
			const QString normalizedSource = normalizePathForStorage(source);
			const bool    hasPortableRoot  = !extractPortableRootRelativePath(normalizedSource).isEmpty();
			if (!isAbsolutePathLike(normalizedSource) && !hasPortableRoot && !pluginsDir.isEmpty())
				source = QDir(pluginsDir).filePath(normalizedSource);
			plugin.source = canonicalizePathForStorage(source, workingDir);
		}

		if (const QString sourceAttr = plugin.attributes.value(QStringLiteral("source")).trimmed();
		    !sourceAttr.isEmpty())
		{
			QString rewrittenSource = sourceAttr;
			if (const QString normalizedSource = normalizePathForStorage(sourceAttr);
			    !isAbsolutePathLike(normalizedSource) &&
			    extractPortableRootRelativePath(normalizedSource).isEmpty() && !pluginsDir.isEmpty())
			{
				rewrittenSource = QDir(pluginsDir).filePath(normalizedSource);
			}
			plugin.attributes.insert(QStringLiteral("source"),
			                         canonicalizePathForStorage(rewrittenSource, workingDir));
		}
	}

	const QString trimmed = normalizedSnapshot.targetFilePath.trimmed();
	if (trimmed.isEmpty())
	{
		if (error)
			*error = QStringLiteral("No filename specified");
		return false;
	}

	const auto &m_worldAttributes          = normalizedSnapshot.worldAttributes;
	const auto &m_worldMultilineAttributes = normalizedSnapshot.worldMultilineAttributes;
	const auto &m_triggers                 = normalizedSnapshot.triggers;
	const auto &m_aliases                  = normalizedSnapshot.aliases;
	const auto &m_timers                   = normalizedSnapshot.timers;
	const auto &m_macros                   = normalizedSnapshot.macros;
	const auto &m_variables                = normalizedSnapshot.variables;
	const auto &m_colours                  = normalizedSnapshot.colours;
	const auto &m_keypadEntries            = normalizedSnapshot.keypadEntries;
	const auto &m_printingStyles           = normalizedSnapshot.printingStyles;
	const auto &m_plugins                  = normalizedSnapshot.plugins;

	QSaveFile   file(trimmed);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
	{
		if (error)
			*error = QStringLiteral("Unable to create the requested file.");
		return false;
	}

	QTextStream out(&file);
	out.setEncoding(QStringConverter::Utf8);
	const auto      nl      = QStringLiteral("\r\n");
	const QDateTime now     = QDateTime::currentDateTime();
	const QString   savedOn = QLocale::system().toString(now, QStringLiteral("dddd, MMMM dd, yyyy, h:mm AP"));
	const bool omitDate = isEnabledFlag(m_worldAttributes.value(QStringLiteral("omit_date_from_save_files")));

	out << R"(<?xml version="1.0" encoding="utf-8"?>)" << nl;
	out << "<!DOCTYPE qmud>" << nl;
	if (!omitDate)
		out << "<!-- Saved on " << fixHtmlString(savedOn) << " -->" << nl;
	out << "<!-- QMud version " << kVersionString << " -->" << nl;
	out << "<!-- Written by Panagiotis Kalogiratos -->" << nl;
	out << "<!-- Home Page: https://github.com/Nodens-/QMud -->" << nl;
	out << "<qmud>" << nl;

	out << "<world" << nl;
	saveXmlString(out, nl, "qmud_version", QString::fromLatin1(kVersionString));
	saveXmlNumber(out, nl, "world_file_version", kThisVersion);
	if (!omitDate)
		saveXmlDate(out, nl, "date_saved", now);
	out << nl;

	for (const auto &opt : kAlphaSaveOptions)
	{
		if (!opt.name)
			break;
		if (opt.flags & OPT_MULTLINE)
			continue;
		QString value = m_worldAttributes.value(QString::fromLatin1(opt.name));
		if (value.isEmpty())
			continue;
		if ((opt.flags & OPT_PASSWORD) != 0)
		{
			const QByteArray utf8       = value.toUtf8();
			const QString    base64Name = QString::fromLatin1(opt.name) + QStringLiteral("_base64");
			saveXmlBoolean(out, nl, base64Name.toLatin1().constData(), true);
			value = qmudEncodeBase64Text(utf8, false);
		}
		saveXmlString(out, nl, opt.name, value);
	}

	out << nl;

	for (const auto &opt : kNumericSaveOptions)
	{
		if (!opt.name)
			break;
		const QString key   = QString::fromLatin1(opt.name);
		const QString value = m_worldAttributes.value(key);
		if (const bool isBool = (opt.min == 0 && opt.max == 0); isBool)
		{
			const bool currentValue = isEnabledFlag(value);
			const bool defaultValue = opt.defaultValue != 0;
			if (currentValue != defaultValue)
				saveXmlString(out, nl, opt.name, qmudBoolToYn(currentValue));
			continue;
		}
		if (opt.flags & OPT_RGB_COLOUR)
		{
			const long colour = parseColorRef(value);
			saveXmlColour(out, nl, opt.name, colour);
			continue;
		}
		bool            ok     = false;
		const long long number = value.toLongLong(&ok);
		if (ok)
			saveXmlNumber(out, nl, opt.name, number);
	}

	out << "   > <!-- end of general world attributes -->" << nl;

	for (const auto &opt : kAlphaSaveOptions)
	{
		if (!opt.name)
			break;
		if (!(opt.flags & OPT_MULTLINE))
			continue;
		const QString value = m_worldMultilineAttributes.value(QString::fromLatin1(opt.name));
		if (value.isEmpty())
			continue;
		saveXmlMulti(out, nl, opt.name, value);
	}

	out << nl << "</world>" << nl;

	auto saveHeader = [&](const char *name)
	{
		out << nl << "<!-- " << name << " -->" << nl;
		out << nl << "<" << name << nl;
		saveXmlString(out, nl, "qmud_version", QString::fromLatin1(kVersionString));
		saveXmlNumber(out, nl, "world_file_version", kThisVersion);
		if (!omitDate)
			saveXmlDate(out, nl, "date_saved", now);
		out << "  >" << nl;
	};

	auto                   saveFooter = [&](const char *name) { out << "</" << name << ">" << nl; };

	QList<const Trigger *> triggers;
	for (const auto &tr : m_triggers)
	{
		if (tr.included || isEnabledFlag(tr.attributes.value(QStringLiteral("temporary"))))
			continue;
		triggers.push_back(&tr);
	}
	saveHeader("triggers");
	std::ranges::stable_sort(triggers,
	                         [](const Trigger *a, const Trigger *b)
	                         {
		                         bool      okA  = false;
		                         bool      okB  = false;
		                         const int seqA = a->attributes.value(QStringLiteral("sequence")).toInt(&okA);
		                         const int seqB = b->attributes.value(QStringLiteral("sequence")).toInt(&okB);
		                         const int seqValA = okA ? seqA : 0;
		                         const int seqValB = okB ? seqB : 0;
		                         if (seqValA != seqValB)
			                         return seqValA < seqValB;
		                         return a->attributes.value(QStringLiteral("match")) <
		                                b->attributes.value(QStringLiteral("match"));
	                         });
	for (const auto *tr : triggers)
	{
		out << "  <trigger" << nl;
		auto num = [&](const char *key)
		{
			bool            ok    = false;
			const long long value = tr->attributes.value(QString::fromLatin1(key)).toLongLong(&ok);
			if (ok)
				saveXmlNumber(out, nl, key, value);
		};
		auto boolean = [&](const char *key)
		{ saveXmlBoolean(out, nl, key, isEnabledFlag(tr->attributes.value(QString::fromLatin1(key)))); };
		auto text = [&](const char *key)
		{ saveXmlString(out, nl, key, tr->attributes.value(QString::fromLatin1(key))); };
		num("back_colour");
		boolean("bold");
		num("clipboard_arg");
		{
			bool            ok    = false;
			const long long value = tr->attributes.value(QStringLiteral("custom_colour")).toLongLong(&ok);
			if (ok && value != 0)
				saveXmlNumber(out, nl, "custom_colour", value);
		}
		num("colour_change_type");
		if (const bool triggerEnabled = isEnabledFlag(tr->attributes.value(QStringLiteral("enabled")));
		    triggerEnabled)
			saveXmlBoolean(out, nl, "enabled", true);
		else
			out << "   enabled=\"n\"" << nl;
		boolean("expand_variables");
		text("group");
		boolean("ignore_case");
		boolean("inverse");
		boolean("italic");
		num("lines_to_match");
		boolean("keep_evaluating");
		boolean("make_bold");
		boolean("make_italic");
		boolean("make_underline");
		text("match");
		boolean("match_back_colour");
		boolean("match_bold");
		boolean("match_inverse");
		boolean("match_italic");
		boolean("match_text_colour");
		boolean("match_underline");
		boolean("multi_line");
		text("name");
		boolean("one_shot");
		boolean("omit_from_log");
		boolean("omit_from_output");
		boolean("regexp");
		boolean("repeat");
		text("script");
		num("send_to");
		num("sequence");
		text("sound");
		boolean("sound_if_inactive");
		boolean("lowercase_wildcard");
		boolean("temporary");
		num("text_colour");
		num("user");
		text("variable");
		{
			if (const long otherText =
			        parseColorRef(tr->attributes.value(QStringLiteral("other_text_colour")));
			    otherText)
				saveXmlColour(out, nl, "other_text_colour", otherText);
			if (const long otherBack =
			        parseColorRef(tr->attributes.value(QStringLiteral("other_back_colour")));
			    otherBack)
				saveXmlColour(out, nl, "other_back_colour", otherBack);
		}
		out << "  >" << nl;
		saveXmlMulti(out, nl, "send", tr->children.value(QStringLiteral("send")));
		out << "  </trigger>" << nl;
	}
	saveFooter("triggers");

	QList<const Alias *> aliases;
	for (const auto &al : m_aliases)
	{
		if (al.included || isEnabledFlag(al.attributes.value(QStringLiteral("temporary"))))
			continue;
		aliases.push_back(&al);
	}
	saveHeader("aliases");
	std::ranges::stable_sort(aliases,
	                         [](const Alias *a, const Alias *b)
	                         {
		                         return a->attributes.value(QStringLiteral("match")) <
		                                b->attributes.value(QStringLiteral("match"));
	                         });
	for (const auto *al : aliases)
	{
		out << "  <alias" << nl;
		saveXmlString(out, nl, "name", al->attributes.value(QStringLiteral("name")));
		saveXmlString(out, nl, "script", al->attributes.value(QStringLiteral("script")));
		saveXmlString(out, nl, "match", al->attributes.value(QStringLiteral("match")));
		if (const bool aliasEnabled = isEnabledFlag(al->attributes.value(QStringLiteral("enabled")));
		    aliasEnabled)
			saveXmlBoolean(out, nl, "enabled", true);
		else
			out << "   enabled=\"n\"" << nl;
		saveXmlBoolean(out, nl, "echo_alias",
		               isEnabledFlag(al->attributes.value(QStringLiteral("echo_alias"))));
		saveXmlBoolean(out, nl, "expand_variables",
		               isEnabledFlag(al->attributes.value(QStringLiteral("expand_variables"))));
		saveXmlString(out, nl, "group", al->attributes.value(QStringLiteral("group")));
		saveXmlString(out, nl, "variable", al->attributes.value(QStringLiteral("variable")));
		saveXmlBoolean(out, nl, "omit_from_command_history",
		               isEnabledFlag(al->attributes.value(QStringLiteral("omit_from_command_history"))));
		saveXmlBoolean(out, nl, "omit_from_log",
		               isEnabledFlag(al->attributes.value(QStringLiteral("omit_from_log"))));
		saveXmlBoolean(out, nl, "regexp", isEnabledFlag(al->attributes.value(QStringLiteral("regexp"))));
		{
			bool            ok    = false;
			const long long value = al->attributes.value(QStringLiteral("send_to")).toLongLong(&ok);
			if (ok)
				saveXmlNumber(out, nl, "send_to", value);
		}
		saveXmlBoolean(out, nl, "omit_from_output",
		               isEnabledFlag(al->attributes.value(QStringLiteral("omit_from_output"))));
		saveXmlBoolean(out, nl, "one_shot", isEnabledFlag(al->attributes.value(QStringLiteral("one_shot"))));
		saveXmlBoolean(out, nl, "menu", isEnabledFlag(al->attributes.value(QStringLiteral("menu"))));
		saveXmlBoolean(out, nl, "ignore_case",
		               isEnabledFlag(al->attributes.value(QStringLiteral("ignore_case"))));
		saveXmlBoolean(out, nl, "keep_evaluating",
		               isEnabledFlag(al->attributes.value(QStringLiteral("keep_evaluating"))));
		{
			bool            ok    = false;
			const long long value = al->attributes.value(QStringLiteral("sequence")).toLongLong(&ok);
			if (ok)
				saveXmlNumber(out, nl, "sequence", value);
		}
		saveXmlBoolean(out, nl, "temporary",
		               isEnabledFlag(al->attributes.value(QStringLiteral("temporary"))));
		{
			bool            ok    = false;
			const long long value = al->attributes.value(QStringLiteral("user")).toLongLong(&ok);
			if (ok)
				saveXmlNumber(out, nl, "user", value);
		}
		out << "  >" << nl;
		saveXmlMulti(out, nl, "send", al->children.value(QStringLiteral("send")));
		out << "  </alias>" << nl;
	}
	saveFooter("aliases");

	QList<const Timer *> timers;
	for (const auto &tm : m_timers)
	{
		if (tm.included || isEnabledFlag(tm.attributes.value(QStringLiteral("temporary"))))
			continue;
		timers.push_back(&tm);
	}
	saveHeader("timers");
	for (const auto *tm : timers)
	{
		out << "  <timer ";
		saveXmlString(out, nl, "name", tm->attributes.value(QStringLiteral("name")), true);
		saveXmlString(out, nl, "script", tm->attributes.value(QStringLiteral("script")), true);
		if (const bool timerEnabled = isEnabledFlag(tm->attributes.value(QStringLiteral("enabled")));
		    timerEnabled)
			saveXmlBoolean(out, nl, "enabled", true, true);
		else
			out << "enabled=\"n\" ";

		bool            ok   = false;
		const long long hour = tm->attributes.value(QStringLiteral("hour")).toLongLong(&ok);
		if (ok)
			saveXmlNumber(out, nl, "hour", hour, true);
		const long long minute = tm->attributes.value(QStringLiteral("minute")).toLongLong(&ok);
		if (ok)
			saveXmlNumber(out, nl, "minute", minute, true);
		const double second = tm->attributes.value(QStringLiteral("second")).toDouble(&ok);
		if (ok)
			saveXmlDouble(out, nl, "second", second);

		const long long offsetHour = tm->attributes.value(QStringLiteral("offset_hour")).toLongLong(&ok);
		if (ok)
			saveXmlNumber(out, nl, "offset_hour", offsetHour, true);
		const long long offsetMinute = tm->attributes.value(QStringLiteral("offset_minute")).toLongLong(&ok);
		if (ok)
			saveXmlNumber(out, nl, "offset_minute", offsetMinute, true);
		const double offsetSecond = tm->attributes.value(QStringLiteral("offset_second")).toDouble(&ok);
		if (ok)
			saveXmlDouble(out, nl, "offset_second", offsetSecond);
		const long long sendTo = tm->attributes.value(QStringLiteral("send_to")).toLongLong(&ok);
		if (ok)
			saveXmlNumber(out, nl, "send_to", sendTo);
		saveXmlBoolean(out, nl, "temporary",
		               isEnabledFlag(tm->attributes.value(QStringLiteral("temporary"))));
		const long long user = tm->attributes.value(QStringLiteral("user")).toLongLong(&ok);
		if (ok)
			saveXmlNumber(out, nl, "user", user);

		saveXmlBoolean(out, nl, "at_time", isEnabledFlag(tm->attributes.value(QStringLiteral("at_time"))),
		               true);
		saveXmlString(out, nl, "group", tm->attributes.value(QStringLiteral("group")), true);
		saveXmlString(out, nl, "variable", tm->attributes.value(QStringLiteral("variable")));
		saveXmlBoolean(out, nl, "one_shot", isEnabledFlag(tm->attributes.value(QStringLiteral("one_shot"))),
		               true);
		saveXmlBoolean(out, nl, "omit_from_output",
		               isEnabledFlag(tm->attributes.value(QStringLiteral("omit_from_output"))), true);
		saveXmlBoolean(out, nl, "omit_from_log",
		               isEnabledFlag(tm->attributes.value(QStringLiteral("omit_from_log"))), true);
		saveXmlBoolean(out, nl, "active_closed",
		               isEnabledFlag(tm->attributes.value(QStringLiteral("active_closed"))), true);

		out << ">" << nl;
		saveXmlMulti(out, nl, "send", tm->children.value(QStringLiteral("send")));
		out << nl << "  </timer>" << nl;
	}
	saveFooter("timers");

	saveHeader("macros");
	const QStringList            macroNames = macroDescriptionList();
	QMap<QString, const Macro *> macroMap;
	for (const auto &macro : m_macros)
	{
		const QString name = macro.attributes.value(QStringLiteral("name")).trimmed();
		if (!name.isEmpty())
			macroMap.insert(name, &macro);
	}
	for (const QString &name : macroNames)
	{
		const Macro *macro = macroMap.value(name, nullptr);
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
	saveFooter("macros");

	saveHeader("variables");
	QList<Variable> variables = m_variables;
	std::ranges::stable_sort(
	    variables, [](const Variable &a, const Variable &b)
	    { return a.attributes.value(QStringLiteral("name")) < b.attributes.value(QStringLiteral("name")); });
	for (const auto &var : variables)
	{
		const QString name = var.attributes.value(QStringLiteral("name"));
		out << "  <variable name=\"" << fixHtmlString(name) << "\">" << fixHtmlMultilineString(var.content)
		    << "</variable>" << nl;
	}
	out << "</variables>" << nl;

	saveHeader("colours");
	auto parseColourValue = [](const QString &value) -> QColor
	{
		if (value.isEmpty())
			return {};
		QColor color(value);
		if (color.isValid())
			return color;
		bool      ok      = false;
		const int numeric = value.toInt(&ok);
		if (!ok)
			return {};
		const int r = numeric & 0xFF;
		const int g = (numeric >> 8) & 0xFF;
		const int b = (numeric >> 16) & 0xFF;
		return {r, g, b};
	};

	QVector<QColor> normalAnsi(8);
	QVector<QColor> boldAnsi(8);
	QVector<QColor> customText(16);
	QVector<QColor> customBack(16);
	QStringList     customNames;
	customNames.reserve(16);
	for (int i = 0; i < 16; ++i)
		customNames.push_back(QStringLiteral("Custom%1").arg(i + 1));

	normalAnsi[0] = QColor(0, 0, 0);
	normalAnsi[1] = QColor(128, 0, 0);
	normalAnsi[2] = QColor(0, 128, 0);
	normalAnsi[3] = QColor(128, 128, 0);
	normalAnsi[4] = QColor(0, 0, 128);
	normalAnsi[5] = QColor(128, 0, 128);
	normalAnsi[6] = QColor(0, 128, 128);
	normalAnsi[7] = QColor(192, 192, 192);
	boldAnsi[0]   = QColor(128, 128, 128);
	boldAnsi[1]   = QColor(255, 0, 0);
	boldAnsi[2]   = QColor(0, 255, 0);
	boldAnsi[3]   = QColor(255, 255, 0);
	boldAnsi[4]   = QColor(0, 0, 255);
	boldAnsi[5]   = QColor(255, 0, 255);
	boldAnsi[6]   = QColor(0, 255, 255);
	boldAnsi[7]   = QColor(255, 255, 255);
	for (int i = 0; i < customText.size(); ++i)
	{
		customText[i] = QColor(255, 255, 255);
		customBack[i] = QColor(0, 0, 0);
	}
	customText[0]  = QColor(255, 128, 128);
	customText[1]  = QColor(255, 255, 128);
	customText[2]  = QColor(128, 255, 128);
	customText[3]  = QColor(128, 255, 255);
	customText[4]  = QColor(0, 128, 255);
	customText[5]  = QColor(255, 128, 192);
	customText[6]  = QColor(255, 0, 0);
	customText[7]  = QColor(0, 128, 192);
	customText[8]  = QColor(255, 0, 255);
	customText[9]  = QColor(128, 64, 64);
	customText[10] = QColor(255, 128, 64);
	customText[11] = QColor(0, 128, 128);
	customText[12] = QColor(0, 64, 128);
	customText[13] = QColor(255, 0, 128);
	customText[14] = QColor(0, 128, 0);
	customText[15] = QColor(0, 0, 255);

	for (const auto &colour : m_colours)
	{
		const QString group = colour.group.trimmed().toLower();
		bool          ok    = false;
		const int     seq   = colour.attributes.value(QStringLiteral("seq")).toInt(&ok);
		const int     index = ok ? seq - 1 : -1;
		if (index < 0)
			continue;
		if (group == QStringLiteral("ansi/normal") && index < normalAnsi.size())
		{
			const QColor rgb = parseColourValue(colour.attributes.value(QStringLiteral("rgb")));
			if (rgb.isValid())
				normalAnsi[index] = rgb;
		}
		else if (group == QStringLiteral("ansi/bold") && index < boldAnsi.size())
		{
			const QColor rgb = parseColourValue(colour.attributes.value(QStringLiteral("rgb")));
			if (rgb.isValid())
				boldAnsi[index] = rgb;
		}
		else if ((group == QStringLiteral("custom/custom") || group == QStringLiteral("custom")) &&
		         index < customText.size())
		{
			const QColor text = parseColourValue(colour.attributes.value(QStringLiteral("text")));
			const QColor back = parseColourValue(colour.attributes.value(QStringLiteral("back")));
			if (text.isValid())
				customText[index] = text;
			if (back.isValid())
				customBack[index] = back;
			const QString name = colour.attributes.value(QStringLiteral("name")).trimmed();
			if (!name.isEmpty())
				customNames[index] = name;
		}
	}

	out << nl << "<ansi>" << nl;
	out << nl << " <normal>" << nl;
	for (int i = 0; i < normalAnsi.size(); ++i)
	{
		out << "   <colour ";
		saveXmlNumber(out, nl, "seq", i + 1, true);
		saveXmlColour(out, nl, "rgb", colorToLong(normalAnsi[i]), true);
		out << "/>" << nl;
	}
	out << nl << " </normal>" << nl;

	out << nl << " <bold>" << nl;
	for (int i = 0; i < boldAnsi.size(); ++i)
	{
		out << "   <colour ";
		saveXmlNumber(out, nl, "seq", i + 1, true);
		saveXmlColour(out, nl, "rgb", colorToLong(boldAnsi[i]), true);
		out << "/>" << nl;
	}
	out << nl << " </bold>" << nl;
	out << nl << "</ansi>" << nl;

	out << nl << "<custom>" << nl;
	for (int i = 0; i < customText.size(); ++i)
	{
		out << "  <colour ";
		saveXmlNumber(out, nl, "seq", i + 1, true);
		saveXmlString(out, nl, "name", customNames.value(i), true);
		saveXmlColour(out, nl, "text", colorToLong(customText[i]), true);
		saveXmlColour(out, nl, "back", colorToLong(customBack[i]), true);
		out << "/>" << nl;
	}
	out << nl << "</custom>" << nl;
	saveFooter("colours");

	const QStringList      keypadNames = keypadNameList();
	QMap<QString, QString> keypadMap;
	for (const auto &key : m_keypadEntries)
		keypadMap.insert(key.attributes.value(QStringLiteral("name")), key.content);
	saveHeader("keypad");
	for (const QString &keyName : keypadNames)
	{
		out << nl << "  <key ";
		saveXmlString(out, nl, "name", keyName, true);
		out << ">" << nl;
		saveXmlMulti(out, nl, "send", keypadMap.value(keyName));
		out << "  </key>" << nl;
	}
	saveFooter("keypad");

	saveHeader("printing");
	struct StyleState
	{
			bool bold{false};
			bool italic{false};
			bool underline{false};
	};
	QVector<StyleState> normalStyles(8);
	QVector<StyleState> boldStyles(8);

	for (const auto &style : m_printingStyles)
	{
		const QString group = style.group.trimmed().toLower();
		bool          ok    = false;
		const int     seq   = style.attributes.value(QStringLiteral("seq")).toInt(&ok);
		const int     index = ok ? seq - 1 : -1;
		if (index < 0 || index >= normalStyles.size())
			continue;
		StyleState state;
		state.bold      = isEnabledFlag(style.attributes.value(QStringLiteral("bold")));
		state.italic    = isEnabledFlag(style.attributes.value(QStringLiteral("italic")));
		state.underline = isEnabledFlag(style.attributes.value(QStringLiteral("underline")));
		if (group == QStringLiteral("ansi/bold"))
			boldStyles[index] = state;
		else if (group == QStringLiteral("ansi/normal"))
			normalStyles[index] = state;
	}

	out << nl << "<ansi>" << nl;
	out << nl << " <normal>" << nl;
	for (int i = 0; i < normalStyles.size(); ++i)
	{
		const StyleState &state = normalStyles[i];
		if (!state.bold && !state.italic && !state.underline)
			continue;
		out << "   <style ";
		saveXmlNumber(out, nl, "seq", i + 1, true);
		saveXmlBoolean(out, nl, "bold", state.bold, true);
		saveXmlBoolean(out, nl, "italic", state.italic, true);
		saveXmlBoolean(out, nl, "underline", state.underline, true);
		out << "/>" << nl;
	}
	out << nl << " </normal>" << nl;

	out << nl << " <bold>" << nl;
	for (int i = 0; i < boldStyles.size(); ++i)
	{
		const StyleState &state = boldStyles[i];
		if (!state.bold && !state.italic && !state.underline)
			continue;
		out << "   <style ";
		saveXmlNumber(out, nl, "seq", i + 1, true);
		saveXmlBoolean(out, nl, "bold", state.bold, true);
		saveXmlBoolean(out, nl, "italic", state.italic, true);
		saveXmlBoolean(out, nl, "underline", state.underline, true);
		out << "/>" << nl;
	}
	out << nl << " </bold>" << nl;
	out << nl << "</ansi>" << nl;
	saveFooter("printing");

	bool wrotePlugins = false;
	for (const auto &plugin : m_plugins)
	{
		if (plugin.global)
			continue;
		QString source = plugin.attributes.value(QStringLiteral("source")).trimmed();
		if (source.isEmpty())
			source = plugin.source.trimmed();
		if (source.isEmpty())
			continue;
		if (!wrotePlugins)
		{
			out << nl << "<!-- plugins -->" << nl;
			wrotePlugins = true;
		}
		source = normalizePathForStorage(source);
		out << "<include ";
		saveXmlString(out, nl, "name", source, true);
		saveXmlBoolean(out, nl, "plugin", true, true);
		if (!isEnabledFlag(plugin.attributes.value(QStringLiteral("enabled"))) &&
		    plugin.attributes.contains(QStringLiteral("enabled")))
			out << "enabled=\"n\" ";
		out << "/>" << nl;
	}

	out << "</qmud>" << nl;

	if (!file.commit())
	{
		if (error)
			*error = QStringLiteral("Unable to create the requested file.");
		return false;
	}

	return true;
}

void WorldRuntime::setPluginsDirectory(const QString &path)
{
	m_pluginsDirectory = path;
}

QString WorldRuntime::pluginsDirectory() const
{
	return m_pluginsDirectory;
}

void WorldRuntime::setStateFilesDirectory(const QString &path)
{
	m_stateFilesDirectory = path;
}

QString WorldRuntime::stateFilesDirectory() const
{
	return m_stateFilesDirectory;
}

void WorldRuntime::setFileBrowsingDirectory(const QString &path)
{
	m_fileBrowsingDirectory = path;
}

QString WorldRuntime::fileBrowsingDirectory() const
{
	return m_fileBrowsingDirectory;
}

void WorldRuntime::setPreferencesDatabaseName(const QString &path)
{
	m_preferencesDatabaseName = path;
}

QString WorldRuntime::preferencesDatabaseName() const
{
	return m_preferencesDatabaseName;
}

void WorldRuntime::setTranslatorFile(const QString &path)
{
	m_translatorFile = path;
}

QString WorldRuntime::translatorFile() const
{
	return m_translatorFile;
}

void WorldRuntime::setLocale(const QString &value)
{
	m_locale = value;
}

QString WorldRuntime::locale() const
{
	return m_locale;
}

void WorldRuntime::setFixedPitchFont(const QString &value)
{
	m_fixedPitchFont = value;
}

void WorldRuntime::applyDefaultWorldOptions()
{
	QMudWorldOptionDefaults::applyWorldOptionDefaults(m_worldAttributes, m_worldMultilineAttributes);
}

QString WorldRuntime::fixedPitchFont() const
{
	return m_fixedPitchFont;
}

void WorldRuntime::setStatusMessage(const QString &value)
{
	m_statusMessage = value;
}

QString WorldRuntime::statusMessage() const
{
	return m_statusMessage;
}

void WorldRuntime::setWordUnderMenu(const QString &value)
{
	m_wordUnderMenu = value;
}

QString WorldRuntime::wordUnderMenu() const
{
	return m_wordUnderMenu;
}

void WorldRuntime::setDebugIncomingPackets(bool enabled)
{
	m_debugIncomingPackets = enabled;
}

bool WorldRuntime::debugIncomingPackets() const
{
	return m_debugIncomingPackets;
}

void WorldRuntime::setLastImmediateExpression(const QString &value)
{
	m_lastImmediateExpression = value;
}

QString WorldRuntime::lastImmediateExpression() const
{
	return m_lastImmediateExpression;
}

void WorldRuntime::setVariablesChanged(bool changed)
{
	m_variablesChanged = changed;
}

bool WorldRuntime::variablesChanged() const
{
	return m_variablesChanged;
}

void WorldRuntime::setLineOmittedFromOutput(bool omitted)
{
	m_lineOmittedFromOutput = omitted;
}

bool WorldRuntime::lineOmittedFromOutput() const
{
	return m_lineOmittedFromOutput;
}

void WorldRuntime::addRecentLine(const QString &line)
{
	constexpr int kMaxRecentLines = 200;
	m_recentLines.push_back(line);
	if (m_recentLines.size() > kMaxRecentLines)
		m_recentLines.pop_front();
}

QStringList WorldRuntime::recentLines(int maxCount) const
{
	if (maxCount <= 0 || maxCount >= m_recentLines.size())
		return m_recentLines;
	return m_recentLines.mid(m_recentLines.size() - maxCount);
}

void WorldRuntime::clearRecentLines()
{
	m_recentLines.clear();
}

void WorldRuntime::bookmarkLine(int lineNumber, bool set)
{
	if (lineNumber <= 0 || lineNumber > m_lines.size())
		return;

	LineEntry &entry = m_lines[lineNumber - 1];
	if (set)
		entry.flags |= LineBookmark;
	else
		entry.flags &= ~LineBookmark;
}

void WorldRuntime::setStopTriggerEvaluation(StopTriggerEvaluation mode)
{
	m_stopTriggerEvaluation = mode;
}

WorldRuntime::StopTriggerEvaluation WorldRuntime::stopTriggerEvaluation() const
{
	return m_stopTriggerEvaluation;
}

LuaCallbackEngine *WorldRuntime::luaCallbacks() const
{
	return m_luaCallbacks;
}

void WorldRuntime::applyPackageRestrictions(bool enablePackage)
{
	if (m_luaCallbacks)
		m_luaCallbacks->applyPackageRestrictions(enablePackage);

	for (auto &plugin : m_plugins)
	{
		if (plugin.lua)
			plugin.lua->applyPackageRestrictions(enablePackage);
	}
}

void WorldRuntime::setTraceEnabled(bool enabled)
{
	m_traceEnabled = enabled;
}

bool WorldRuntime::traceEnabled() const
{
	return m_traceEnabled;
}

void WorldRuntime::setWorldFileModified(bool modified)
{
	m_worldFileModified = modified;
}

bool WorldRuntime::worldFileModified() const
{
	return m_worldFileModified;
}

qint64 WorldRuntime::newlinesReceived() const
{
	return m_newlinesReceived;
}

void WorldRuntime::setScriptFileChanged(bool changed)
{
	m_scriptFileChanged = changed;
}

bool WorldRuntime::scriptFileChanged() const
{
	return m_scriptFileChanged;
}

void WorldRuntime::setLastCommandSent(const QString &value)
{
	m_lastCommandSent = value;
}

QString WorldRuntime::lastCommandSent() const
{
	return m_lastCommandSent;
}

bool WorldRuntime::isMxpActive() const
{
	return m_mxpActive;
}

int WorldRuntime::mxpErrorCount() const
{
	return m_mxpErrors;
}

void WorldRuntime::setOutputFrozen(bool frozen)
{
	m_outputFrozen = frozen;
}

bool WorldRuntime::outputFrozen() const
{
	return m_outputFrozen;
}

const WorldRuntime::TextRectangleSettings &WorldRuntime::textRectangle() const
{
	return m_textRectangle;
}

void WorldRuntime::setTextRectangle(const TextRectangleSettings &settings)
{
	if (textRectangleSettingsEqual(m_textRectangle, settings))
		return;

	m_textRectangle = settings;
	++m_suppressWorldOutputResizedCallbacks;
	if (WorldView *view = this->view())
	{
		view->updateWrapMargin();
		view->update();
	}
	updateTelnetWindowSizeForNaws();
	if (m_suppressWorldOutputResizedCallbacks > 0)
		--m_suppressWorldOutputResizedCallbacks;
}

QList<int> WorldRuntime::udpPortList() const
{
	QList<int> ports;
	ports.reserve(m_udpListeners.size());
	for (auto it = m_udpListeners.cbegin(); it != m_udpListeners.cend(); ++it)
		ports.append(it.key());
	return ports;
}

int WorldRuntime::udpListen(const QString &pluginId, const QString &ip, int port, const QString &script)
{
	if (port < 1 || port > 65535)
		return eBadParameter;

	if (auto existing = m_udpListeners.find(port); existing != m_udpListeners.end())
	{
		if (existing->pluginId.compare(pluginId, Qt::CaseInsensitive) != 0)
			return eBadParameter;
		if (existing->socket)
		{
			existing->socket->close();
			delete existing->socket;
		}
		m_udpListeners.erase(existing);
	}

	if (script.isEmpty())
		return eNoNameSpecified;

	QHostAddress bindAddress;
	if (ip.isEmpty())
		bindAddress = QHostAddress::Any;
	else
		bindAddress = QHostAddress(ip);
	if (bindAddress.isNull())
		return eBadParameter;

	auto       socket = std::make_unique<QUdpSocket>(this);
	const bool bound  = socket->bind(bindAddress, static_cast<quint16>(port),
	                                 QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
	if (!bound)
		return eBadParameter;

	UdpListener listener;
	listener.socket      = socket.get();
	listener.pluginId    = pluginId;
	listener.script      = script;
	listener.bindAddress = ip;
	m_udpListeners.insert(port, listener);
	QUdpSocket const *udpSocket = socket.release();

	connect(udpSocket, &QUdpSocket::readyRead, this,
	        [this, port]
	        {
		        const auto it = m_udpListeners.find(port);
		        if (it == m_udpListeners.end() || !it->socket)
			        return;
		        QUdpSocket *udp = it->socket;
		        while (udp->hasPendingDatagrams())
		        {
			        QByteArray datagram;
			        datagram.resize(static_cast<int>(udp->pendingDatagramSize()));
			        QHostAddress sender;
			        quint16      senderPort = 0;
			        const qint64 bytes =
			            udp->readDatagram(datagram.data(), datagram.size(), &sender, &senderPort);
			        if (bytes <= 0)
				        continue;
			        datagram.resize(static_cast<int>(bytes));
			        const QString payload = QString::fromLatin1(datagram.constData(), datagram.size());
			        callPlugin(it->pluginId, it->script, payload, it->pluginId);
		        }
	        });

	return eOK;
}

int WorldRuntime::udpSend(const QString &ip, int port, const QByteArray &payload)
{
	if (port < 1 || port > 65535)
		return eBadParameter;
	QHostAddress const address(ip);
	if (address.isNull())
		return -1;

	QUdpSocket socket;
	if (const qint64 bytes = socket.writeDatagram(payload, address, static_cast<quint16>(port)); bytes < 0)
		return socket.error();
	return 0;
}

QString WorldRuntime::lastTelnetSubnegotiation() const
{
	return m_lastTelnetSubnegotiation;
}

#if QMUD_ENABLE_SOUND
static double normalizeSoundVolume(double volume)
{
	if (volume > 0.0 || volume < -100.0)
		volume = 0.0;
	const double scaled = 1.0 + (volume / 100.0);
	return qBound(0.0, scaled, 1.0);
}

static bool isAbsolutePathPortable(const QString &path)
{
	if (path.isEmpty())
		return false;
	const QChar first   = path.at(0);
	const bool  isDrive = path.size() > 1 && path.at(1) == QLatin1Char(':') && first.isLetter();
	return isDrive || first == QLatin1Char('/') || first == QLatin1Char('\\');
}

int WorldRuntime::playSound(int buffer, const QString &fileName, bool loop, double volume, double pan)
{
	Q_UNUSED(pan);
	if (fileName.isEmpty() && buffer == 0)
		return eBadParameter;
	if (!fileName.isEmpty() && !m_inPlaySoundPluginCallback)
	{
		m_inPlaySoundPluginCallback = true;
		if (firePluginPlaySound(fileName))
		{
			m_inPlaySoundPluginCallback = false;
			return eOK;
		}
		m_inPlaySoundPluginCallback = false;
	}
	if (buffer >= 1 && buffer <= kMaxSoundBuffers && fileName.isEmpty())
	{
		SoundBuffer &entry = m_soundBuffers[buffer - 1];
		if (!entry.effect)
			return eBadParameter;
		if (!entry.effect->isPlaying())
			return eCannotPlaySound;
		entry.looping = loop;
		entry.volume  = normalizeSoundVolume(volume);
		entry.pan     = pan;
		entry.effect->setLoopCount(loop ? QSoundEffect::Infinite : 1);
		entry.effect->setVolume(static_cast<float>(entry.volume));
		return eOK;
	}

	if (buffer < 0 || buffer > kMaxSoundBuffers)
		return eBadParameter;

	int targetBuffer = buffer;
	if (targetBuffer == 0)
	{
		for (int i = 0; i < (kMaxSoundBuffers / 2); ++i)
		{
			if (!m_soundBuffers[i].effect)
			{
				targetBuffer = i + 1;
				break;
			}
		}
	}
	if (targetBuffer == 0)
	{
		for (int i = 0; i < kMaxSoundBuffers; ++i)
		{
			if (!m_soundBuffers[i].effect || !m_soundBuffers[i].effect->isPlaying())
			{
				targetBuffer = i + 1;
				break;
			}
		}
	}
	if (targetBuffer == 0)
		targetBuffer = 1;

	if (targetBuffer < 1 || targetBuffer > kMaxSoundBuffers)
		return eBadParameter;

	SoundBuffer &entry = m_soundBuffers[targetBuffer - 1];
	if (entry.effect)
	{
		entry.effect->stop();
		delete entry.effect;
		entry.effect = nullptr;
	}
	if (entry.tempFile)
	{
		delete entry.tempFile;
		entry.tempFile = nullptr;
	}

	const QString workingDir         = resolveWorkingDir(m_startupDirectory);
	const QString normalizedFileName = normalizeSeparators(fileName.trimmed());
	QString       resolved;
	QStringList   candidates;
	if (isAbsolutePathPortable(normalizedFileName))
	{
		candidates.push_back(normalizedFileName);
	}
	else
	{
		QString relative = normalizedFileName;
		if (relative.startsWith(QStringLiteral("./")))
			relative = relative.mid(2);
		QDir const base(workingDir);
		if (!relative.startsWith(QStringLiteral("sounds/"), Qt::CaseInsensitive))
			candidates.push_back(base.filePath(QStringLiteral("sounds/%1").arg(relative)));
		candidates.push_back(base.filePath(relative));
	}

	for (const QString &candidate : candidates)
	{
		const QString normalizedCandidate = QDir::cleanPath(normalizeSeparators(candidate));
		if (QFileInfo::exists(normalizedCandidate))
		{
			resolved = normalizedCandidate;
			break;
		}
	}

	if (resolved.isEmpty())
		return eFileNotFound;

	auto *effect = new QSoundEffect(this);
	effect->setSource(QUrl::fromLocalFile(resolved));
	entry.looping = loop;
	entry.volume  = normalizeSoundVolume(volume);
	entry.pan     = pan;
	effect->setLoopCount(loop ? QSoundEffect::Infinite : 1);
	effect->setVolume(static_cast<float>(entry.volume));
	effect->play();
	entry.effect = effect;
	return eOK;
}

int WorldRuntime::playSoundMemory(int buffer, const QByteArray &data, bool loop, double volume, double pan)
{
	if (data.isEmpty())
		return eBadParameter;
	if (buffer < 0 || buffer > kMaxSoundBuffers)
		return eBadParameter;
	int targetBuffer = buffer;
	if (targetBuffer == 0)
	{
		for (int i = 0; i < (kMaxSoundBuffers / 2); ++i)
		{
			if (!m_soundBuffers[i].effect)
			{
				targetBuffer = i + 1;
				break;
			}
		}
	}
	if (targetBuffer == 0)
	{
		for (int i = 0; i < kMaxSoundBuffers; ++i)
		{
			if (!m_soundBuffers[i].effect || !m_soundBuffers[i].effect->isPlaying())
			{
				targetBuffer = i + 1;
				break;
			}
		}
	}
	if (targetBuffer == 0)
		targetBuffer = 1;

	if (targetBuffer < 1 || targetBuffer > kMaxSoundBuffers)
		return eBadParameter;

	SoundBuffer &entry = m_soundBuffers[targetBuffer - 1];
	if (entry.effect)
	{
		entry.effect->stop();
		delete entry.effect;
		entry.effect = nullptr;
	}
	if (entry.tempFile)
	{
		delete entry.tempFile;
		entry.tempFile = nullptr;
	}

	auto *temp = new QTemporaryFile(this);
	temp->setAutoRemove(true);
	if (!temp->open())
	{
		delete temp;
		return eCannotPlaySound;
	}
	if (temp->write(data) != data.size())
	{
		delete temp;
		return eCannotPlaySound;
	}
	temp->flush();
	entry.tempFile = temp;

	auto *effect = new QSoundEffect(this);
	effect->setSource(QUrl::fromLocalFile(temp->fileName()));
	entry.looping = loop;
	entry.volume  = normalizeSoundVolume(volume);
	entry.pan     = pan;
	effect->setLoopCount(loop ? QSoundEffect::Infinite : 1);
	effect->setVolume(static_cast<float>(entry.volume));
	effect->play();
	entry.effect = effect;
	return eOK;
}

int WorldRuntime::stopSound(int buffer)
{
	if (buffer == 0 && !m_inCancelSoundPluginCallback)
	{
		m_inCancelSoundPluginCallback = true;
		if (firePluginPlaySound(QString()))
		{
			m_inCancelSoundPluginCallback = false;
			return eOK;
		}
		m_inCancelSoundPluginCallback = false;
	}

	if (buffer == 0)
	{
		for (SoundBuffer &entry : m_soundBuffers)
		{
			if (entry.effect)
			{
				entry.effect->stop();
				delete entry.effect;
				entry.effect = nullptr;
			}
			if (entry.tempFile)
			{
				delete entry.tempFile;
				entry.tempFile = nullptr;
			}
		}
		return eOK;
	}

	if (buffer < 1 || buffer > kMaxSoundBuffers)
		return eBadParameter;

	SoundBuffer &entry = m_soundBuffers[buffer - 1];
	if (entry.effect)
	{
		entry.effect->stop();
		delete entry.effect;
		entry.effect = nullptr;
	}
	if (entry.tempFile)
	{
		delete entry.tempFile;
		entry.tempFile = nullptr;
	}
	return eOK;
}

int WorldRuntime::soundStatus(int buffer) const
{
	if (buffer < 1 || buffer > kMaxSoundBuffers)
		return -1;
	const SoundBuffer &entry = m_soundBuffers[buffer - 1];
	if (!entry.effect)
		return -2;
	if (!entry.effect->isPlaying())
		return 0;
	return entry.looping ? 2 : 1;
}
#else
int WorldRuntime::playSound(int, const QString &, bool, double, double)
{
	return eCannotPlaySound;
}

int WorldRuntime::playSoundMemory(int, const QByteArray &, bool, double, double)
{
	return eCannotPlaySound;
}

int WorldRuntime::stopSound(int)
{
	return eCannotPlaySound;
}

int WorldRuntime::soundStatus(int) const
{
	return -3;
}
#endif

long WorldRuntime::adjustColour(long colour, short method)
{
	const auto   base = static_cast<QMudColorRef>(colour & 0x00FFFFFF);
	const QColor baseColor(qmudRed(base), qmudGreen(base), qmudBlue(base));

	auto         adjustedHsl = [&](double saturationDelta, double luminanceDelta) -> long
	{
		QColor hsl        = baseColor.toHsl();
		float  hue        = 0.0f;
		float  saturation = 0.0f;
		float  luminance  = 0.0f;
		float  alpha      = 1.0f;
		hsl.getHslF(&hue, &saturation, &luminance, &alpha);

		// For achromatic colors Qt reports hue as -1; behavior treated this as 0.
		if (hue < 0.0f)
			hue = 0.0f;

		saturation = qBound(0.0f, saturation + static_cast<float>(saturationDelta), 1.0f);
		luminance  = qBound(0.0f, luminance + static_cast<float>(luminanceDelta), 1.0f);
		hsl.setHslF(hue, saturation, luminance, alpha);

		const QColor rgb = hsl.toRgb();
		return qmudRgb(rgb.red(), rgb.green(), rgb.blue());
	};

	switch (method)
	{
	case ADJUST_COLOUR_INVERT:
		return qmudRgb(255 - qmudRed(base), 255 - qmudGreen(base), 255 - qmudBlue(base));
	case ADJUST_COLOUR_LIGHTER:
		return adjustedHsl(0.0, 0.02);
	case ADJUST_COLOUR_DARKER:
		return adjustedHsl(0.0, -0.02);
	case ADJUST_COLOUR_LESS_COLOUR:
		return adjustedHsl(-0.05, 0.0);
	case ADJUST_COLOUR_MORE_COLOUR:
		return adjustedHsl(0.05, 0.0);
	default:
		return base;
	}
}

long WorldRuntime::blendPixel(long blend, long base, short mode, double opacity)
{
	long out = 0;
	if (!blendPixelInternal(blend, base, mode, opacity, out))
		return -2;
	return out;
}

long WorldRuntime::filterPixel(long pixel, short operation, double options)
{
	long r = qmudRed(static_cast<QMudColorRef>(pixel));
	long g = qmudGreen(static_cast<QMudColorRef>(pixel));
	long b = qmudBlue(static_cast<QMudColorRef>(pixel));

	switch (operation)
	{
	case 1:
	{
		const double threshold = options / 100.0;
		r += roundedToLong((128.0 - runtimeRandomUnit() * 256.0) * threshold);
		g += roundedToLong((128.0 - runtimeRandomUnit() * 256.0) * threshold);
		b += roundedToLong((128.0 - runtimeRandomUnit() * 256.0) * threshold);
		break;
	}
	case 2:
	{
		const double threshold = options / 100.0;
		const long   j         = roundedToLong((128.0 - runtimeRandomUnit() * 256.0) * threshold);
		r += j;
		g += j;
		b += j;
		break;
	}
	case 7:
		r += roundedToLong(options);
		g += roundedToLong(options);
		b += roundedToLong(options);
		break;
	case 8:
	{
		double c = (static_cast<double>(r) - 128.0) * options;
		r        = roundedToLong(c + 128.0);
		c        = (static_cast<double>(g) - 128.0) * options;
		g        = roundedToLong(c + 128.0);
		c        = (static_cast<double>(b) - 128.0) * options;
		b        = roundedToLong(c + 128.0);
		break;
	}
	case 9:
	{
		if (options < 0.0)
			options = 0.0;
		double c = static_cast<double>(r) / 255.0;
		c        = pow(c, options);
		r        = roundedToLong(c * 255.0);
		c        = static_cast<double>(g) / 255.0;
		c        = pow(c, options);
		g        = roundedToLong(c * 255.0);
		c        = static_cast<double>(b) / 255.0;
		c        = pow(c, options);
		b        = roundedToLong(c * 255.0);
		break;
	}
	case 10:
		r += roundedToLong(options);
		break;
	case 11:
	{
		const double c = (static_cast<double>(r) - 128.0) * options;
		r              = roundedToLong(c + 128.0);
		break;
	}
	case 12:
	{
		if (options < 0.0)
			options = 0.0;
		double c = static_cast<double>(r) / 255.0;
		c        = pow(c, options);
		r        = roundedToLong(c * 255.0);
		break;
	}
	case 13:
		g += roundedToLong(options);
		break;
	case 14:
	{
		const double c = (static_cast<double>(g) - 128.0) * options;
		g              = roundedToLong(c + 128.0);
		break;
	}
	case 15:
	{
		if (options < 0.0)
			options = 0.0;
		double c = static_cast<double>(g) / 255.0;
		c        = pow(c, options);
		g        = roundedToLong(c * 255.0);
		break;
	}
	case 16:
		b += roundedToLong(options);
		break;
	case 17:
	{
		const double c = (static_cast<double>(b) - 128.0) * options;
		b              = roundedToLong(c + 128.0);
		break;
	}
	case 18:
	{
		if (options < 0.0)
			options = 0.0;
		double c = static_cast<double>(b) / 255.0;
		c        = pow(c, options);
		b        = roundedToLong(c * 255.0);
		break;
	}
	case 19:
	{
		const double c = (static_cast<double>(r) + static_cast<double>(g) + static_cast<double>(b)) / 3.0;
		r              = roundedToLong(c);
		g              = roundedToLong(c);
		b              = roundedToLong(c);
		break;
	}
	case 20:
	{
		double c =
		    static_cast<double>(b) * 0.11 + static_cast<double>(g) * 0.59 + static_cast<double>(r) * 0.30;
		c /= 3.0;
		r = roundedToLong(c);
		g = roundedToLong(c);
		b = roundedToLong(c);
		break;
	}
	case 21:
		r = roundedToLong(static_cast<double>(r) * options);
		g = roundedToLong(static_cast<double>(g) * options);
		b = roundedToLong(static_cast<double>(b) * options);
		break;
	case 22:
		r = roundedToLong(static_cast<double>(r) * options);
		break;
	case 23:
		g = roundedToLong(static_cast<double>(g) * options);
		break;
	case 24:
		b = roundedToLong(static_cast<double>(b) * options);
		break;
	case 27:
		break;
	default:
		return -1;
	}

	return qmudRgb(QMudBlend::clampToByteRange(static_cast<int>(r)),
	               QMudBlend::clampToByteRange(static_cast<int>(g)),
	               QMudBlend::clampToByteRange(static_cast<int>(b)));
}

QString WorldRuntime::base64Encode(const QString &text, bool multiLine)
{
	const QByteArray utf8 = text.toUtf8();
	return qmudEncodeBase64Text(utf8, multiLine);
}

QString WorldRuntime::base64Decode(const QString &text)
{
	QByteArray input = text.toUtf8();
	input.replace('\r', "");
	input.replace('\n', "");
	const QByteArray decoded = QByteArray::fromBase64(input, QByteArray::Base64Encoding);
	return QString::fromUtf8(decoded);
}

QString WorldRuntime::rgbColourToName(long colour)
{
	return qmudColourToName(colour);
}

QString WorldRuntime::clientVersionString()
{
	return QString::fromLatin1(kVersionString);
}

QString WorldRuntime::errorDesc(int code)
{
	for (const IntFlagsPair *p = kErrorDescriptionMappingTable; p->value != nullptr; ++p)
	{
		if (p->key == code)
			return QString::fromLatin1(p->value);
	}
	return {};
}

QString WorldRuntime::generateName()
{
	return qmudGenerateName();
}

QStringList WorldRuntime::internalCommandsList()
{
	QStringList commands;
	for (int i = 0; kCommandIdMappingTable[i].commandId; ++i)
		commands.append(QString::fromLatin1(kCommandIdMappingTable[i].commandName));
	return commands;
}

void WorldRuntime::setOutputFontMetrics(int height, int width)
{
	m_outputFontHeight = height;
	m_outputFontWidth  = width;
}

void WorldRuntime::setInputFontMetrics(int height, int width)
{
	m_inputFontHeight = height;
	m_inputFontWidth  = width;
}

int WorldRuntime::outputFontHeight() const
{
	return m_outputFontHeight;
}

int WorldRuntime::outputFontWidth() const
{
	return m_outputFontWidth;
}

int WorldRuntime::inputFontHeight() const
{
	return m_inputFontHeight;
}

int WorldRuntime::inputFontWidth() const
{
	return m_inputFontWidth;
}

long WorldRuntime::colourNameToRGB(const QString &name)
{
	QString trimmed = name.trimmed();
	if (trimmed.size() >= 2)
	{
		const QChar first = trimmed.front();
		const QChar last  = trimmed.back();
		if ((first == QLatin1Char('"') && last == QLatin1Char('"')) ||
		    (first == QLatin1Char('\'') && last == QLatin1Char('\'')))
			trimmed = trimmed.mid(1, trimmed.size() - 2).trimmed();
	}
	if (trimmed.isEmpty())
		return -1;
	bool       ok      = false;
	const long numeric = trimmed.toLong(&ok);
	if (ok)
		return (numeric < 0 || numeric > 0xFFFFFF) ? -1 : numeric;
	const QString lowered = trimmed.toLower();
	QColor const  color(lowered);
	if (color.isValid())
		return colorToLong(color);

	static const QHash<QString, long> kMxpColors = []
	{
		auto toRef = [](unsigned int rgb) -> long
		{
			const int r = static_cast<int>((rgb >> 16) & 0xFF);
			const int g = static_cast<int>((rgb >> 8) & 0xFF);
			const int b = static_cast<int>(rgb & 0xFF);
			return r | (g << 8) | (b << 16);
		};
		QHash<QString, long> map;
		map.insert(QStringLiteral("aliceblue"), toRef(0xF0F8FF));
		map.insert(QStringLiteral("antiquewhite"), toRef(0xFAEBD7));
		map.insert(QStringLiteral("aqua"), toRef(0x00FFFF));
		map.insert(QStringLiteral("aquamarine"), toRef(0x7FFFD4));
		map.insert(QStringLiteral("azure"), toRef(0xF0FFFF));
		map.insert(QStringLiteral("beige"), toRef(0xF5F5DC));
		map.insert(QStringLiteral("bisque"), toRef(0xFFE4C4));
		map.insert(QStringLiteral("black"), toRef(0x000000));
		map.insert(QStringLiteral("blanchedalmond"), toRef(0xFFEBCD));
		map.insert(QStringLiteral("blue"), toRef(0x0000FF));
		map.insert(QStringLiteral("blueviolet"), toRef(0x8A2BE2));
		map.insert(QStringLiteral("brown"), toRef(0xA52A2A));
		map.insert(QStringLiteral("burlywood"), toRef(0xDEB887));
		map.insert(QStringLiteral("cadetblue"), toRef(0x5F9EA0));
		map.insert(QStringLiteral("chartreuse"), toRef(0x7FFF00));
		map.insert(QStringLiteral("chocolate"), toRef(0xD2691E));
		map.insert(QStringLiteral("coral"), toRef(0xFF7F50));
		map.insert(QStringLiteral("cornflowerblue"), toRef(0x6495ED));
		map.insert(QStringLiteral("cornsilk"), toRef(0xFFF8DC));
		map.insert(QStringLiteral("crimson"), toRef(0xDC143C));
		map.insert(QStringLiteral("cyan"), toRef(0x00FFFF));
		map.insert(QStringLiteral("darkblue"), toRef(0x00008B));
		map.insert(QStringLiteral("darkcyan"), toRef(0x008B8B));
		map.insert(QStringLiteral("darkgoldenrod"), toRef(0xB8860B));
		map.insert(QStringLiteral("darkgray"), toRef(0xA9A9A9));
		map.insert(QStringLiteral("darkgrey"), toRef(0xA9A9A9));
		map.insert(QStringLiteral("darkgreen"), toRef(0x006400));
		map.insert(QStringLiteral("darkkhaki"), toRef(0xBDB76B));
		map.insert(QStringLiteral("darkmagenta"), toRef(0x8B008B));
		map.insert(QStringLiteral("darkolivegreen"), toRef(0x556B2F));
		map.insert(QStringLiteral("darkorange"), toRef(0xFF8C00));
		map.insert(QStringLiteral("darkorchid"), toRef(0x9932CC));
		map.insert(QStringLiteral("darkred"), toRef(0x8B0000));
		map.insert(QStringLiteral("darksalmon"), toRef(0xE9967A));
		map.insert(QStringLiteral("darkseagreen"), toRef(0x8FBC8F));
		map.insert(QStringLiteral("darkslateblue"), toRef(0x483D8B));
		map.insert(QStringLiteral("darkslategray"), toRef(0x2F4F4F));
		map.insert(QStringLiteral("darkslategrey"), toRef(0x2F4F4F));
		map.insert(QStringLiteral("darkturquoise"), toRef(0x00CED1));
		map.insert(QStringLiteral("darkviolet"), toRef(0x9400D3));
		map.insert(QStringLiteral("deeppink"), toRef(0xFF1493));
		map.insert(QStringLiteral("deepskyblue"), toRef(0x00BFFF));
		map.insert(QStringLiteral("dimgray"), toRef(0x696969));
		map.insert(QStringLiteral("dimgrey"), toRef(0x696969));
		map.insert(QStringLiteral("dodgerblue"), toRef(0x1E90FF));
		map.insert(QStringLiteral("firebrick"), toRef(0xB22222));
		map.insert(QStringLiteral("floralwhite"), toRef(0xFFFAF0));
		map.insert(QStringLiteral("forestgreen"), toRef(0x228B22));
		map.insert(QStringLiteral("fuchsia"), toRef(0xFF00FF));
		map.insert(QStringLiteral("gainsboro"), toRef(0xDCDCDC));
		map.insert(QStringLiteral("ghostwhite"), toRef(0xF8F8FF));
		map.insert(QStringLiteral("gold"), toRef(0xFFD700));
		map.insert(QStringLiteral("goldenrod"), toRef(0xDAA520));
		map.insert(QStringLiteral("gray"), toRef(0x808080));
		map.insert(QStringLiteral("grey"), toRef(0x808080));
		map.insert(QStringLiteral("green"), toRef(0x008000));
		map.insert(QStringLiteral("greenyellow"), toRef(0xADFF2F));
		map.insert(QStringLiteral("honeydew"), toRef(0xF0FFF0));
		map.insert(QStringLiteral("hotpink"), toRef(0xFF69B4));
		map.insert(QStringLiteral("indianred"), toRef(0xCD5C5C));
		map.insert(QStringLiteral("indigo"), toRef(0x4B0082));
		map.insert(QStringLiteral("ivory"), toRef(0xFFFFF0));
		map.insert(QStringLiteral("khaki"), toRef(0xF0E68C));
		map.insert(QStringLiteral("lavender"), toRef(0xE6E6FA));
		map.insert(QStringLiteral("lavenderblush"), toRef(0xFFF0F5));
		map.insert(QStringLiteral("lawngreen"), toRef(0x7CFC00));
		map.insert(QStringLiteral("lemonchiffon"), toRef(0xFFFACD));
		map.insert(QStringLiteral("lightblue"), toRef(0xADD8E6));
		map.insert(QStringLiteral("lightcoral"), toRef(0xF08080));
		map.insert(QStringLiteral("lightcyan"), toRef(0xE0FFFF));
		map.insert(QStringLiteral("lightgoldenrodyellow"), toRef(0xFAFAD2));
		map.insert(QStringLiteral("lightgreen"), toRef(0x90EE90));
		map.insert(QStringLiteral("lightgrey"), toRef(0xD3D3D3));
		map.insert(QStringLiteral("lightgray"), toRef(0xD3D3D3));
		map.insert(QStringLiteral("lightpink"), toRef(0xFFB6C1));
		map.insert(QStringLiteral("lightsalmon"), toRef(0xFFA07A));
		map.insert(QStringLiteral("lightseagreen"), toRef(0x20B2AA));
		map.insert(QStringLiteral("lightskyblue"), toRef(0x87CEFA));
		map.insert(QStringLiteral("lightslategray"), toRef(0x778899));
		map.insert(QStringLiteral("lightslategrey"), toRef(0x778899));
		map.insert(QStringLiteral("lightsteelblue"), toRef(0xB0C4DE));
		map.insert(QStringLiteral("lightyellow"), toRef(0xFFFFE0));
		map.insert(QStringLiteral("lime"), toRef(0x00FF00));
		map.insert(QStringLiteral("limegreen"), toRef(0x32CD32));
		map.insert(QStringLiteral("linen"), toRef(0xFAF0E6));
		map.insert(QStringLiteral("magenta"), toRef(0xFF00FF));
		map.insert(QStringLiteral("maroon"), toRef(0x800000));
		map.insert(QStringLiteral("mediumaquamarine"), toRef(0x66CDAA));
		map.insert(QStringLiteral("mediumblue"), toRef(0x0000CD));
		map.insert(QStringLiteral("mediumorchid"), toRef(0xBA55D3));
		map.insert(QStringLiteral("mediumpurple"), toRef(0x9370DB));
		map.insert(QStringLiteral("mediumseagreen"), toRef(0x3CB371));
		map.insert(QStringLiteral("mediumslateblue"), toRef(0x7B68EE));
		map.insert(QStringLiteral("mediumspringgreen"), toRef(0x00FA9A));
		map.insert(QStringLiteral("mediumturquoise"), toRef(0x48D1CC));
		map.insert(QStringLiteral("mediumvioletred"), toRef(0xC71585));
		map.insert(QStringLiteral("midnightblue"), toRef(0x191970));
		map.insert(QStringLiteral("mintcream"), toRef(0xF5FFFA));
		map.insert(QStringLiteral("mistyrose"), toRef(0xFFE4E1));
		map.insert(QStringLiteral("moccasin"), toRef(0xFFE4B5));
		map.insert(QStringLiteral("navajowhite"), toRef(0xFFDEAD));
		map.insert(QStringLiteral("navy"), toRef(0x000080));
		map.insert(QStringLiteral("oldlace"), toRef(0xFDF5E6));
		map.insert(QStringLiteral("olive"), toRef(0x808000));
		map.insert(QStringLiteral("olivedrab"), toRef(0x6B8E23));
		map.insert(QStringLiteral("orange"), toRef(0xFFA500));
		map.insert(QStringLiteral("orangered"), toRef(0xFF4500));
		map.insert(QStringLiteral("orchid"), toRef(0xDA70D6));
		map.insert(QStringLiteral("palegoldenrod"), toRef(0xEEE8AA));
		map.insert(QStringLiteral("palegreen"), toRef(0x98FB98));
		map.insert(QStringLiteral("paleturquoise"), toRef(0xAFEEEE));
		map.insert(QStringLiteral("palevioletred"), toRef(0xDB7093));
		map.insert(QStringLiteral("papayawhip"), toRef(0xFFEFD5));
		map.insert(QStringLiteral("peachpuff"), toRef(0xFFDAB9));
		map.insert(QStringLiteral("peru"), toRef(0xCD853F));
		map.insert(QStringLiteral("pink"), toRef(0xFFC0CB));
		map.insert(QStringLiteral("plum"), toRef(0xDDA0DD));
		map.insert(QStringLiteral("powderblue"), toRef(0xB0E0E6));
		map.insert(QStringLiteral("purple"), toRef(0x800080));
		map.insert(QStringLiteral("rebeccapurple"), toRef(0x663399));
		map.insert(QStringLiteral("red"), toRef(0xFF0000));
		map.insert(QStringLiteral("rosybrown"), toRef(0xBC8F8F));
		map.insert(QStringLiteral("royalblue"), toRef(0x4169E1));
		map.insert(QStringLiteral("saddlebrown"), toRef(0x8B4513));
		map.insert(QStringLiteral("salmon"), toRef(0xFA8072));
		map.insert(QStringLiteral("sandybrown"), toRef(0xF4A460));
		map.insert(QStringLiteral("seagreen"), toRef(0x2E8B57));
		map.insert(QStringLiteral("seashell"), toRef(0xFFF5EE));
		map.insert(QStringLiteral("sienna"), toRef(0xA0522D));
		map.insert(QStringLiteral("silver"), toRef(0xC0C0C0));
		map.insert(QStringLiteral("skyblue"), toRef(0x87CEEB));
		map.insert(QStringLiteral("slateblue"), toRef(0x6A5ACD));
		map.insert(QStringLiteral("slategray"), toRef(0x708090));
		map.insert(QStringLiteral("slategrey"), toRef(0x708090));
		map.insert(QStringLiteral("snow"), toRef(0xFFFAFA));
		map.insert(QStringLiteral("springgreen"), toRef(0x00FF7F));
		map.insert(QStringLiteral("steelblue"), toRef(0x4682B4));
		map.insert(QStringLiteral("tan"), toRef(0xD2B48C));
		map.insert(QStringLiteral("teal"), toRef(0x008080));
		map.insert(QStringLiteral("thistle"), toRef(0xD8BFD8));
		map.insert(QStringLiteral("tomato"), toRef(0xFF6347));
		map.insert(QStringLiteral("turquoise"), toRef(0x40E0D0));
		map.insert(QStringLiteral("violet"), toRef(0xEE82EE));
		map.insert(QStringLiteral("wheat"), toRef(0xF5DEB3));
		map.insert(QStringLiteral("white"), toRef(0xFFFFFF));
		map.insert(QStringLiteral("whitesmoke"), toRef(0xF5F5F5));
		map.insert(QStringLiteral("yellow"), toRef(0xFFFF00));
		map.insert(QStringLiteral("yellowgreen"), toRef(0x9ACD32));
		return map;
	}();

	const auto it = kMxpColors.constFind(lowered);
	if (it != kMxpColors.constEnd())
		return it.value();
	return -1;
}

static int colourSeqFromAttributes(const QMap<QString, QString> &attributes)
{
	bool ok  = false;
	int  seq = attributes.value(QStringLiteral("seq")).toInt(&ok);
	if (ok)
		return seq;

	seq = attributes.value(QStringLiteral("seq_index")).toInt(&ok);
	if (ok)
		return seq + 1;

	return -1;
}

int WorldRuntime::setCustomColourText(int index, const QColor &color)
{
	if (index < 1 || index > MAX_CUSTOM || !color.isValid())
		return eBadParameter;

	const auto    group    = QStringLiteral("custom/custom");
	const QString seq      = QString::number(index);
	const QString seqIndex = QString::number(index - 1);
	const QString value    = QString::number(colorToLong(color));

	for (auto &colour : m_colours)
	{
		const QString groupKey = colour.group.trimmed().toLower();
		if (groupKey != QStringLiteral("custom/custom") && groupKey != QStringLiteral("custom"))
			continue;
		const int itemSeq = colourSeqFromAttributes(colour.attributes);
		if (itemSeq != index)
			continue;
		colour.attributes.insert(QStringLiteral("seq"), seq);
		colour.attributes.insert(QStringLiteral("seq_index"), seqIndex);
		colour.attributes.insert(QStringLiteral("text"), value);
		m_worldFileModified = true;
		if (m_view)
		{
			m_view->applyRuntimeSettings();
			m_view->update();
		}
		return eOK;
	}

	Colour entry;
	entry.group = group;
	entry.attributes.insert(QStringLiteral("seq"), seq);
	entry.attributes.insert(QStringLiteral("seq_index"), seqIndex);
	entry.attributes.insert(QStringLiteral("text"), value);
	m_colours.push_back(entry);
	m_colourCount       = safeQSizeToInt(m_colours.size());
	m_worldFileModified = true;
	if (m_view)
	{
		m_view->applyRuntimeSettings();
		m_view->update();
	}
	return eOK;
}

int WorldRuntime::setCustomColourBackground(int index, const QColor &color)
{
	if (index < 1 || index > MAX_CUSTOM || !color.isValid())
		return eBadParameter;

	const auto    group    = QStringLiteral("custom/custom");
	const QString seq      = QString::number(index);
	const QString seqIndex = QString::number(index - 1);
	const QString value    = QString::number(colorToLong(color));

	for (auto &colour : m_colours)
	{
		const QString groupKey = colour.group.trimmed().toLower();
		if (groupKey != QStringLiteral("custom/custom") && groupKey != QStringLiteral("custom"))
			continue;
		const int itemSeq = colourSeqFromAttributes(colour.attributes);
		if (itemSeq != index)
			continue;
		colour.attributes.insert(QStringLiteral("seq"), seq);
		colour.attributes.insert(QStringLiteral("seq_index"), seqIndex);
		colour.attributes.insert(QStringLiteral("back"), value);
		m_worldFileModified = true;
		if (m_view)
		{
			m_view->applyRuntimeSettings();
			m_view->update();
		}
		return eOK;
	}

	Colour entry;
	entry.group = group;
	entry.attributes.insert(QStringLiteral("seq"), seq);
	entry.attributes.insert(QStringLiteral("seq_index"), seqIndex);
	entry.attributes.insert(QStringLiteral("back"), value);
	m_colours.push_back(entry);
	m_colourCount       = safeQSizeToInt(m_colours.size());
	m_worldFileModified = true;
	if (m_view)
	{
		m_view->applyRuntimeSettings();
		m_view->update();
	}
	return eOK;
}

int WorldRuntime::setCustomColourName(int index, const QString &name)
{
	if (index < 1 || index > MAX_CUSTOM)
		return eOptionOutOfRange;
	const QString trimmed = name.trimmed();
	if (trimmed.isEmpty())
		return eNoNameSpecified;
	if (trimmed.size() > 30)
		return eInvalidObjectLabel;

	const auto    group    = QStringLiteral("custom/custom");
	const QString seq      = QString::number(index);
	const QString seqIndex = QString::number(index - 1);

	for (auto &colour : m_colours)
	{
		const QString groupKey = colour.group.trimmed().toLower();
		if (groupKey != QStringLiteral("custom/custom") && groupKey != QStringLiteral("custom"))
			continue;
		const int itemSeq = colourSeqFromAttributes(colour.attributes);
		if (itemSeq != index)
			continue;
		colour.attributes.insert(QStringLiteral("seq"), seq);
		colour.attributes.insert(QStringLiteral("seq_index"), seqIndex);
		colour.attributes.insert(QStringLiteral("name"), trimmed);
		m_worldFileModified = true;
		return eOK;
	}

	Colour entry;
	entry.group = group;
	entry.attributes.insert(QStringLiteral("seq"), seq);
	entry.attributes.insert(QStringLiteral("seq_index"), seqIndex);
	entry.attributes.insert(QStringLiteral("name"), trimmed);
	m_colours.push_back(entry);
	m_colourCount       = safeQSizeToInt(m_colours.size());
	m_worldFileModified = true;
	return eOK;
}

long WorldRuntime::customColourText(int index) const
{
	if (index < 1 || index > MAX_CUSTOM)
		return 0;
	QVector<QColor> normalAnsi;
	QVector<QColor> customText;
	QVector<QColor> customBack;
	buildCustomColours(m_colours, normalAnsi, customText, customBack);
	return colorToLong(customText.value(index - 1));
}

long WorldRuntime::customColourBackground(int index) const
{
	if (index < 1 || index > MAX_CUSTOM)
		return 0;
	QVector<QColor> normalAnsi;
	QVector<QColor> customText;
	QVector<QColor> customBack;
	buildCustomColours(m_colours, normalAnsi, customText, customBack);
	return colorToLong(customBack.value(index - 1));
}

namespace
{
	int loadWorldImage(const QString &rawName, QImage &target, QString &storedName)
	{
		const QString fileName = rawName.trimmed();
		if (fileName.isEmpty())
		{
			target = QImage();
			storedName.clear();
			return eOK;
		}

		if (fileName.size() < 5)
			return eBadParameter;

		const QString lower = fileName.toLower();
		if (!lower.endsWith(QStringLiteral(".png")) && !lower.endsWith(QStringLiteral(".bmp")))
			return eBadParameter;

		if (!QFileInfo::exists(fileName))
			return eFileNotFound;

		QImageReader reader(fileName);
		reader.setAutoTransform(true);
		const QImage image = reader.read();
		if (image.isNull())
			return eCouldNotOpenFile;

		target     = image;
		storedName = fileName;
		return eOK;
	}
} // namespace

int WorldRuntime::setBackgroundImage(const QString &fileName, int mode)
{
	if (mode < 0 || mode > 13)
		return eBadParameter;
	const int result = loadWorldImage(fileName, m_backgroundImage, m_backgroundImageName);
	if (result == eOK)
	{
		m_backgroundImageMode = mode;
		m_worldFileModified   = true;
		if (m_view)
		{
			m_view->applyRuntimeSettings();
			m_view->update();
		}
	}
	return result;
}

int WorldRuntime::setForegroundImage(const QString &fileName, int mode)
{
	if (mode < 0 || mode > 13)
		return eBadParameter;
	const int result = loadWorldImage(fileName, m_foregroundImage, m_foregroundImageName);
	if (result == eOK)
	{
		m_foregroundImageMode = mode;
		m_worldFileModified   = true;
		if (m_view)
		{
			m_view->applyRuntimeSettings();
			m_view->update();
		}
	}
	return result;
}

QImage WorldRuntime::backgroundImage() const
{
	return m_backgroundImage;
}

QImage WorldRuntime::foregroundImage() const
{
	return m_foregroundImage;
}

int WorldRuntime::backgroundImageMode() const
{
	return m_backgroundImageMode;
}

int WorldRuntime::foregroundImageMode() const
{
	return m_foregroundImageMode;
}

QString WorldRuntime::backgroundImageName() const
{
	return m_backgroundImageName;
}

QString WorldRuntime::foregroundImageName() const
{
	return m_foregroundImageName;
}

bool WorldRuntime::closeNotepad(const QString &title, bool querySave)
{
	const QString trimmedTitle = title.trimmed();
	if (trimmedTitle.isEmpty())
		return false;
	const auto           ownerToken = reinterpret_cast<qulonglong>(this);
	const QString        worldId    = m_worldAttributes.value(QStringLiteral("id")).trimmed();

	AppController const *app  = AppController::instance();
	MainWindow const    *main = app ? app->mainWindow() : nullptr;
	if (!main)
		return false;

	TextChildWindow               *target          = nullptr;
	TextChildWindow               *unnamedFallback = nullptr;
	const QList<TextChildWindow *> notepads        = main->findChildren<TextChildWindow *>();
	for (TextChildWindow *text : notepads)
	{
		if (text->windowTitle().compare(trimmedTitle, Qt::CaseInsensitive) != 0)
			continue;

		const qulonglong relatedToken = text->property("worldRuntimeToken").toULongLong();
		if (relatedToken == ownerToken)
		{
			target = text;
			break;
		}
		if (relatedToken != 0)
			continue;

		if (worldId.isEmpty())
		{
			target = text;
			break;
		}
		const QString related = text->property("worldId").toString().trimmed();
		if (related.compare(worldId, Qt::CaseInsensitive) == 0)
		{
			target = text;
			break;
		}
		if (related.isEmpty() && !unnamedFallback)
			unnamedFallback = text;
	}
	if (!target)
		target = unnamedFallback;

	if (!target)
		return false;

	QPointer const guard(target);
	target->setQuerySaveOnClose(querySave);
	target->close();
	return guard.isNull() || !guard->isVisible();
}

QVariant WorldRuntime::debugCommand(const QString &command)
{
	auto          note = [this](const QString &line) { outputText(line, true, true); };

	const QString cmd = command.trimmed().toLower();
	if (cmd == QStringLiteral("summary"))
	{
		note(QStringLiteral("World: %1")
		         .arg(worldName().isEmpty() ? QStringLiteral("(unnamed)") : worldName()));
		note(QStringLiteral("Connected: %1")
		         .arg(isConnected() ? QStringLiteral("yes") : QStringLiteral("no")));
		note(QStringLiteral("Bytes in/out: %1 / %2").arg(m_bytesIn).arg(m_bytesOut));
		note(QStringLiteral("Packets in/out: %1 / %2").arg(m_inputPacketCount).arg(m_outputPacketCount));
		note(QStringLiteral("Lines in/out: %1 / %2").arg(m_linesReceived).arg(m_totalLinesSent));
		note(QStringLiteral("New lines: %1").arg(m_newLines));
		note(QStringLiteral("Triggers/Aliases/Timers fired: %1 / %2 / %3")
		         .arg(m_triggersMatchedThisSession)
		         .arg(m_aliasesMatchedThisSession)
		         .arg(m_timersFiredThisSession));
		note(QStringLiteral("Counts - triggers:%1 aliases:%2 timers:%3 variables:%4 arrays:%5 plugins:%6")
		         .arg(m_triggers.size())
		         .arg(m_aliases.size())
		         .arg(m_timers.size())
		         .arg(m_variables.size())
		         .arg(m_arrays.size())
		         .arg(m_plugins.size()));
		return {};
	}

	if (cmd == QStringLiteral("variables"))
	{
		int count = 0;
		for (const Variable &var : m_variables)
		{
			const QString name = var.attributes.value(QStringLiteral("name"));
			note(QStringLiteral("%1 = %2").arg(name, var.content));
			++count;
		}
		note(QStringLiteral("%1 variable%2.").arg(count).arg(count == 1 ? QString() : QStringLiteral("s")));
		return {};
	}

	if (cmd == QStringLiteral("arrays"))
	{
		int count = 0;
		for (auto it = m_arrays.cbegin(); it != m_arrays.cend(); ++it)
		{
			note(QStringLiteral("[%1] (%2 items)").arg(it.key()).arg(it->values.size()));
			for (auto kv = it->values.cbegin(); kv != it->values.cend(); ++kv)
				note(QStringLiteral("  %1 = %2").arg(kv.key(), kv.value()));
			++count;
		}
		note(QStringLiteral("%1 array%2.").arg(count).arg(count == 1 ? QString() : QStringLiteral("s")));
		return {};
	}

	if (cmd == QStringLiteral("plugins"))
	{
		int count = 0;
		for (const Plugin &plugin : m_plugins)
		{
			const QString id      = plugin.attributes.value(QStringLiteral("id")).isEmpty()
			                            ? QStringLiteral("(no id)")
			                            : plugin.attributes.value(QStringLiteral("id"));
			const QString name    = plugin.attributes.value(QStringLiteral("name")).isEmpty()
			                            ? QStringLiteral("(unnamed)")
			                            : plugin.attributes.value(QStringLiteral("name"));
			const QString enabled = plugin.enabled ? QStringLiteral("enabled") : QStringLiteral("disabled");
			note(QStringLiteral("%1 | %2 | %3").arg(id, name, enabled));
			++count;
		}
		note(QStringLiteral("%1 plugin%2.").arg(count).arg(count == 1 ? QString() : QStringLiteral("s")));
		return {};
	}

	if (cmd == QStringLiteral("triggers"))
	{
		int count = 0;
		for (const Trigger &trigger : m_triggers)
		{
			note(QStringLiteral("[%1] %2").arg(trigger.attributes.value(QStringLiteral("name")),
			                                   trigger.attributes.value(QStringLiteral("match"))));
			++count;
		}
		note(QStringLiteral("%1 trigger%2.").arg(count).arg(count == 1 ? QString() : QStringLiteral("s")));
		return {};
	}

	if (cmd == QStringLiteral("aliases"))
	{
		int count = 0;
		for (const Alias &alias : m_aliases)
		{
			note(QStringLiteral("[%1] %2").arg(alias.attributes.value(QStringLiteral("name")),
			                                   alias.attributes.value(QStringLiteral("match"))));
			++count;
		}
		note(QStringLiteral("%1 alias%2.").arg(count).arg(count == 1 ? QString() : QStringLiteral("es")));
		return {};
	}

	if (cmd == QStringLiteral("internal_commands"))
	{
		const QStringList commands = internalCommandsList();
		for (const QString &item : commands)
			note(item);
		note(QStringLiteral("%1 internal command%2.")
		         .arg(commands.size())
		         .arg(commands.size() == 1 ? QString() : QStringLiteral("s")));
		return {};
	}

	if (cmd == QStringLiteral("commands"))
	{
		const QString last = m_lastCommandSent.trimmed();
		if (!last.isEmpty())
			note(last);
		const int count = last.isEmpty() ? 0 : 1;
		note(QStringLiteral("%1 command%2.").arg(count).arg(count == 1 ? QString() : QStringLiteral("s")));
		return {};
	}

	if (cmd == QStringLiteral("options") || cmd == QStringLiteral("alpha_options"))
	{
		QStringList keys = m_worldAttributes.keys();
		std::ranges::sort(keys, [](const QString &a, const QString &b)
		                  { return a.compare(b, Qt::CaseInsensitive) < 0; });
		for (const QString &key : keys)
			note(QStringLiteral("%1 = %2").arg(key, m_worldAttributes.value(key)));
		note(QStringLiteral("%1 option%2.")
		         .arg(keys.size())
		         .arg(keys.size() == 1 ? QString() : QStringLiteral("s")));
		return {};
	}

	note(QStringLiteral("----- Debug commands available -----"));
	note(QStringLiteral("aliases"));
	note(QStringLiteral("alpha_options"));
	note(QStringLiteral("arrays"));
	note(QStringLiteral("commands"));
	note(QStringLiteral("internal_commands"));
	note(QStringLiteral("options"));
	note(QStringLiteral("plugins"));
	note(QStringLiteral("summary"));
	note(QStringLiteral("triggers"));
	note(QStringLiteral("variables"));
	return {};
}

void WorldRuntime::setQueuedCommandCount(int count)
{
	m_queuedCommandCount = count;
}

int WorldRuntime::queuedCommandCount() const
{
	return m_queuedCommandCount;
}

qint64 WorldRuntime::bytesIn() const
{
	return m_bytesIn;
}

qint64 WorldRuntime::bytesOut() const
{
	return m_bytesOut;
}

int WorldRuntime::inputPacketCount() const
{
	return m_inputPacketCount;
}

int WorldRuntime::outputPacketCount() const
{
	return m_outputPacketCount;
}

qint64 WorldRuntime::totalCompressedBytes() const
{
	return m_telnet.totalCompressedBytes();
}

qint64 WorldRuntime::totalUncompressedBytes() const
{
	return m_telnet.totalUncompressedBytes();
}

int WorldRuntime::mccpType() const
{
	return m_telnet.mccpType();
}

qint64 WorldRuntime::mxpTagCount() const
{
	return m_telnet.mxpTagCount();
}

qint64 WorldRuntime::mxpEntityCount() const
{
	return m_telnet.mxpEntityCount();
}

int WorldRuntime::customElementCount() const
{
	return m_telnet.customElementCount();
}

int WorldRuntime::customEntityCount() const
{
	return m_telnet.customEntityCount();
}

QList<TelnetProcessor::CustomElementInfo> WorldRuntime::customMxpElements() const
{
	return m_telnet.customElementInfos();
}

void WorldRuntime::setCustomMxpElements(const QList<TelnetProcessor::CustomElementInfo> &elements)
{
	m_telnet.setCustomElementInfos(elements);
}

TelnetProcessor::MxpSessionState WorldRuntime::mxpSessionState() const
{
	return m_telnet.mxpSessionState();
}

void WorldRuntime::setMxpSessionState(const TelnetProcessor::MxpSessionState &state)
{
	m_telnet.setMxpSessionState(state);
	m_mxpActive = state.enabled;
	if (!state.enabled)
	{
		m_mxpTagStack.clear();
		m_mxpOpenTags.clear();
		m_mxpTextBuffer.clear();
		resetMxpRenderState();
		clearAnsiActionContext();
	}
}

QString WorldRuntime::peerAddressString() const
{
	if (!m_socket)
		return {};
	return m_socket->peerAddressString();
}

quint32 WorldRuntime::peerAddressV4() const
{
	if (!m_socket)
		return 0;
	return m_socket->peerAddressV4();
}

QString WorldRuntime::proxyAddressString() const
{
	return m_proxyAddressString;
}

quint32 WorldRuntime::proxyAddressV4() const
{
	return m_proxyAddressV4;
}

bool WorldRuntime::isCompressing() const
{
	return m_telnet.isCompressing();
}

bool WorldRuntime::isPuebloActive() const
{
	return m_telnet.isPuebloActive();
}

bool WorldRuntime::isConnecting() const
{
	switch (m_connectPhase)
	{
	case eConnectNotConnected:
	case eConnectConnectedToMud:
	case eConnectDisconnecting:
		return false;
	default:
		return true;
	}
}

bool WorldRuntime::isMapping() const
{
	return m_isMapping;
}

int WorldRuntime::mappingCount() const
{
	return safeQSizeToInt(m_mappingList.size());
}

QString WorldRuntime::mappingItem(int index) const
{
	if (index < 0 || index >= m_mappingList.size())
		return {};
	return m_mappingList.at(index);
}

QString WorldRuntime::mappingString(bool omitComments) const
{
	QString lastDir;
	QString output;
	int     count = 0;

	auto    outputPrevious = [&](const QString &current)
	{
		if (!lastDir.isEmpty())
		{
			QString formatted = lastDir;
			if (formatted.size() > 1)
				formatted = QStringLiteral("(%1)").arg(formatted);
			if (count == 1)
				output += formatted + QLatin1Char(' ');
			else if (count > 1)
				output += QString::number(count) + formatted + QLatin1Char(' ');
		}
		lastDir = current;
		count   = 1;
	};

	for (const QString &entry : m_mappingList)
	{
		if (entry.size() > 2 && entry.startsWith(QLatin1Char('{')) && entry.endsWith(QLatin1Char('}')))
		{
			outputPrevious(QString());
			if (!omitComments)
			{
				if (!output.isEmpty())
					output += QLatin1Char('\n');
				output += entry;
				output += QLatin1Char('\n');
			}
			lastDir.clear();
			count = 0;
			continue;
		}

		if (entry == lastDir && count <= 98)
		{
			count++;
			continue;
		}
		outputPrevious(entry);
	}

	outputPrevious(QString());
	return output;
}

unsigned short WorldRuntime::noteStyle() const
{
	return m_noteStyle;
}

void WorldRuntime::setNoteStyle(unsigned short style)
{
	m_noteStyle = style;
}

static QColor parseColourValue(const QString &value)
{
	if (value.isEmpty())
		return {};
	QColor color(value);
	if (color.isValid())
		return color;
	bool      ok      = false;
	const int numeric = value.toInt(&ok);
	if (!ok)
		return {};
	const int r = numeric & 0xFF;
	const int g = (numeric >> 8) & 0xFF;
	const int b = (numeric >> 16) & 0xFF;
	return {r, g, b};
}

static void buildCustomColours(const QList<WorldRuntime::Colour> &colours, QVector<QColor> &normalAnsi,
                               QVector<QColor> &customText, QVector<QColor> &customBack)
{
	normalAnsi = QVector<QColor>(8);
	customText = QVector<QColor>(16);
	customBack = QVector<QColor>(16);

	normalAnsi[0] = QColor(0, 0, 0);
	normalAnsi[1] = QColor(128, 0, 0);
	normalAnsi[2] = QColor(0, 128, 0);
	normalAnsi[3] = QColor(128, 128, 0);
	normalAnsi[4] = QColor(0, 0, 128);
	normalAnsi[5] = QColor(128, 0, 128);
	normalAnsi[6] = QColor(0, 128, 128);
	normalAnsi[7] = QColor(192, 192, 192);

	for (int i = 0; i < customText.size(); ++i)
	{
		customText[i] = QColor(255, 255, 255);
		customBack[i] = QColor(0, 0, 0);
	}

	customText[0]  = QColor(255, 128, 128);
	customText[1]  = QColor(255, 255, 128);
	customText[2]  = QColor(128, 255, 128);
	customText[3]  = QColor(128, 255, 255);
	customText[4]  = QColor(0, 128, 255);
	customText[5]  = QColor(255, 128, 192);
	customText[6]  = QColor(255, 0, 0);
	customText[7]  = QColor(0, 128, 192);
	customText[8]  = QColor(255, 0, 255);
	customText[9]  = QColor(128, 64, 64);
	customText[10] = QColor(255, 128, 64);
	customText[11] = QColor(0, 128, 128);
	customText[12] = QColor(0, 64, 128);
	customText[13] = QColor(255, 0, 128);
	customText[14] = QColor(0, 128, 0);
	customText[15] = QColor(0, 0, 255);

	for (const auto &colour : colours)
	{
		const QString group = colour.group.trimmed().toLower();
		bool          ok    = false;
		const int     seq   = colour.attributes.value(QStringLiteral("seq")).toInt(&ok);
		const int     index = ok ? seq - 1 : -1;
		if (index < 0)
			continue;
		if (group == QStringLiteral("ansi/normal") && index < normalAnsi.size())
		{
			const QColor rgb = parseColourValue(colour.attributes.value(QStringLiteral("rgb")));
			if (rgb.isValid())
				normalAnsi[index] = rgb;
		}
		else if ((group == QStringLiteral("custom/custom") || group == QStringLiteral("custom")) &&
		         index < customText.size())
		{
			const QColor text = parseColourValue(colour.attributes.value(QStringLiteral("text")));
			const QColor back = parseColourValue(colour.attributes.value(QStringLiteral("back")));
			if (text.isValid())
				customText[index] = text;
			if (back.isValid())
				customBack[index] = back;
		}
	}
}

static long colorToLong(const QColor &color)
{
	if (!color.isValid())
		return 0;
	return (color.red() | (color.green() << 8) | (color.blue() << 16));
}

static QString convertToRegularExpression(const QString &text)
{
	const QByteArray input = text.toLatin1();
	QByteArray       out;
	out.append('^');

	for (unsigned char const c : input)
	{
		if (c == '\n')
		{
			out.append("\\n");
		}
		else if (c < ' ')
		{
			out.append("\\x");
			appendHexByte(out, c);
		}
		else if (isAsciiAlnumByte(c) || c == ' ' || c >= 0x80)
		{
			out.append(static_cast<char>(c));
		}
		else if (c == '*')
		{
			out.append("(.*?)");
		}
		else
		{
			out.append('\\');
			out.append(static_cast<char>(c));
		}
	}

	out.append('$');

	return QString::fromLatin1(out);
}

constexpr int kAnsiBlack = 0;
constexpr int kAnsiWhite = 7;

bool          WorldRuntime::notesInRgb() const
{
	return m_notesInRgb;
}

int WorldRuntime::noteTextColour() const
{
	return m_noteTextColour;
}

void WorldRuntime::setNoteTextColour(int value)
{
	if (value < 0 || value > MAX_CUSTOM)
		return;
	m_noteTextColour = value - 1;
	m_notesInRgb     = false;
}

long WorldRuntime::noteColourFore() const
{
	if (m_notesInRgb)
		return m_noteColourFore;

	QVector<QColor> normalAnsi;
	QVector<QColor> customText;
	QVector<QColor> customBack;
	buildCustomColours(m_colours, normalAnsi, customText, customBack);
	const bool custom16Default =
	    isEnabledFlag(m_worldAttributes.value(QStringLiteral("custom_16_is_default_colour")));
	if (const bool sameColour = (m_noteTextColour == kSameColour || m_noteTextColour < 0); sameColour)
		return custom16Default ? colorToLong(customText.value(15))
		                       : colorToLong(normalAnsi.value(kAnsiWhite));
	if (m_noteTextColour >= 0 && m_noteTextColour < customText.size())
		return colorToLong(customText.value(m_noteTextColour));
	return 0;
}

long WorldRuntime::noteColourBack() const
{
	if (m_notesInRgb)
		return m_noteColourBack;

	QVector<QColor> normalAnsi;
	QVector<QColor> customText;
	QVector<QColor> customBack;
	buildCustomColours(m_colours, normalAnsi, customText, customBack);
	const bool custom16Default =
	    isEnabledFlag(m_worldAttributes.value(QStringLiteral("custom_16_is_default_colour")));
	if (const bool sameColour = (m_noteTextColour == kSameColour || m_noteTextColour < 0); sameColour)
		return custom16Default ? colorToLong(customBack.value(15))
		                       : colorToLong(normalAnsi.value(kAnsiBlack));
	if (m_noteTextColour >= 0 && m_noteTextColour < customBack.size())
		return colorToLong(customBack.value(m_noteTextColour));
	return 0;
}

void WorldRuntime::setNoteColourFore(long value)
{
	if (!m_notesInRgb)
	{
		QVector<QColor> normalAnsi;
		QVector<QColor> customText;
		QVector<QColor> customBack;
		buildCustomColours(m_colours, normalAnsi, customText, customBack);
		const bool custom16Default =
		    isEnabledFlag(m_worldAttributes.value(QStringLiteral("custom_16_is_default_colour")));
		if (const bool sameColour = (m_noteTextColour == kSameColour || m_noteTextColour < 0); sameColour)
			m_noteColourBack = custom16Default ? colorToLong(customBack.value(15))
			                                   : colorToLong(normalAnsi.value(kAnsiBlack));
		else if (m_noteTextColour >= 0 && m_noteTextColour < customBack.size())
			m_noteColourBack = colorToLong(customBack.value(m_noteTextColour));
	}

	m_notesInRgb     = true;
	m_noteColourFore = value & 0x00FFFFFF;
}

void WorldRuntime::setNoteColourBack(long value)
{
	if (!m_notesInRgb)
	{
		QVector<QColor> normalAnsi;
		QVector<QColor> customText;
		QVector<QColor> customBack;
		buildCustomColours(m_colours, normalAnsi, customText, customBack);
		const bool custom16Default =
		    isEnabledFlag(m_worldAttributes.value(QStringLiteral("custom_16_is_default_colour")));
		if (const bool sameColour = (m_noteTextColour == kSameColour || m_noteTextColour < 0); sameColour)
			m_noteColourFore = custom16Default ? colorToLong(customText.value(15))
			                                   : colorToLong(normalAnsi.value(kAnsiWhite));
		else if (m_noteTextColour >= 0 && m_noteTextColour < customText.size())
			m_noteColourFore = colorToLong(customText.value(m_noteTextColour));
	}

	m_notesInRgb     = true;
	m_noteColourBack = value & 0x00FFFFFF;
}

long WorldRuntime::mapColourValue(long original) const
{
	const auto it = m_colourTranslationMap.find(original);
	if (it == m_colourTranslationMap.end())
		return original;
	return it.value();
}

long WorldRuntime::getMapColour(long value) const
{
	return mapColourValue(value);
}

long WorldRuntime::getSysColor(int index)
{
	const QPalette palette = QGuiApplication::palette();
	QColor         color;
	switch (index)
	{
	case 0: // Scrollbar
		color = palette.color(QPalette::Mid);
		break;
	case 1: // Background (desktop)
		color = palette.color(QPalette::Window);
		break;
	case 2: // Active caption
		color = palette.color(QPalette::Highlight);
		break;
	case 3: // Inactive caption
		color = palette.color(QPalette::Dark);
		break;
	case 4: // Menu
		color = palette.color(QPalette::Button);
		break;
	case 5: // Window
		color = palette.color(QPalette::Window);
		break;
	case 6: // Window frame
		color = palette.color(QPalette::Shadow);
		break;
	case 7: // Menu text
		color = palette.color(QPalette::ButtonText);
		break;
	case 8: // Window text
		color = palette.color(QPalette::WindowText);
		break;
	case 9: // Caption text
		color = palette.color(QPalette::HighlightedText);
		break;
	case 10: // Active border
		color = palette.color(QPalette::Dark);
		break;
	case 11: // Inactive border
		color = palette.color(QPalette::Mid);
		break;
	case 12: // Application workspace
		color = palette.color(QPalette::Base);
		break;
	case 13: // Highlight
		color = palette.color(QPalette::Highlight);
		break;
	case 14: // Highlight text
		color = palette.color(QPalette::HighlightedText);
		break;
	case 15: // Button face
		color = palette.color(QPalette::Button);
		break;
	case 16: // Button shadow
		color = palette.color(QPalette::Shadow);
		break;
	case 17: // Gray text
		color = palette.color(QPalette::Mid);
		break;
	case 18: // Button text
		color = palette.color(QPalette::ButtonText);
		break;
	case 19: // Inactive caption text
		color = palette.color(QPalette::WindowText);
		break;
	case 20: // Button highlight
		color = palette.color(QPalette::Light);
		break;
	case 21: // 3D dark shadow
		color = palette.color(QPalette::Dark);
		break;
	case 22: // 3D light
		color = palette.color(QPalette::Light);
		break;
	case 23: // Info text
		color = palette.color(QPalette::ToolTipText);
		break;
	case 24: // Info background
		color = palette.color(QPalette::ToolTipBase);
		break;
	default:
		return 0;
	}

	return colorToLong(color);
}

long WorldRuntime::getUniqueNumber()
{
	AppController *app = AppController::instance();
	if (!app)
		return 0;
	const qint64 value = app->nextUniqueNumber();
	return value & 0x7fffffff;
}

QString WorldRuntime::makeRegularExpression(const QString &text)
{
	return convertToRegularExpression(text);
}

QString WorldRuntime::metaphone(const QString &text, int length)
{
	const QPair<QString, QString> encoded = qmudDoubleMetaphone(text, length);
	if (encoded.second.isEmpty())
		return encoded.first;
	return encoded.first + QStringLiteral(",") + encoded.second;
}

double WorldRuntime::mtRand()
{
	return runtimeRandomUnit();
}

int WorldRuntime::readNamesFile(const QString &fileName)
{
	if (fileName.isEmpty())
		return eNoNameSpecified;

	try
	{
		qmudReadNames(fileName, true);
	}
	catch (const std::exception &)
	{
		return eCouldNotOpenFile;
	}

	return eOK;
}

void WorldRuntime::mapColour(long original, long replacement)
{
	m_colourTranslationMap.insert(original, replacement);
}

QMap<long, long> WorldRuntime::mapColourList() const
{
	return m_colourTranslationMap;
}

long WorldRuntime::normalColour(int index) const
{
	if (index < 1 || index > 8)
		return 0;
	const int seq = index;
	for (const auto &colour : m_colours)
	{
		if (colour.group.trimmed().compare(QStringLiteral("ansi/normal"), Qt::CaseInsensitive) != 0)
			continue;
		bool      ok      = false;
		const int itemSeq = colour.attributes.value(QStringLiteral("seq")).toInt(&ok);
		if (!ok || itemSeq != seq)
			continue;
		const QString rgb       = colour.attributes.value(QStringLiteral("rgb"));
		bool          numericOk = false;
		const int     numeric   = rgb.toInt(&numericOk);
		if (numericOk)
			return numeric;
		QColor const parsed(rgb);
		if (parsed.isValid())
			return (parsed.red() | (parsed.green() << 8) | (parsed.blue() << 16));
	}
	return 0;
}

void WorldRuntime::setNormalColour(int index, long value)
{
	if (index < 1 || index > 8)
		return;
	const int seq = index;
	for (auto &colour : m_colours)
	{
		if (colour.group.trimmed().compare(QStringLiteral("ansi/normal"), Qt::CaseInsensitive) != 0)
			continue;
		bool      ok      = false;
		const int itemSeq = colour.attributes.value(QStringLiteral("seq")).toInt(&ok);
		if (!ok || itemSeq != seq)
			continue;
		colour.attributes.insert(QStringLiteral("rgb"), QString::number(value & 0x00FFFFFF));
		return;
	}
	Colour entry;
	entry.group = QStringLiteral("ansi/normal");
	entry.attributes.insert(QStringLiteral("seq"), QString::number(seq));
	entry.attributes.insert(QStringLiteral("rgb"), QString::number(value & 0x00FFFFFF));
	m_colours.push_back(entry);
}

int WorldRuntime::addToMapper(const QString &direction, const QString &reverse)
{
	const auto badChars = QStringLiteral("{}()/\\");
	if (direction.contains(badChars) || reverse.contains(badChars))
		return eBadMapItem;

	if (direction.isEmpty() && reverse.isEmpty())
		return eBadMapItem;

	const QString entry = QStringLiteral("%1/%2").arg(direction, reverse);
	m_mappingList.append(entry);
	return eOK;
}

int WorldRuntime::addMapperComment(const QString &comment)
{
	const auto badChars = QStringLiteral("{}()/\\");
	if (comment.contains(badChars))
		return eBadMapItem;

	const QString entry = QStringLiteral("{%1}").arg(comment);
	m_mappingList.append(entry);
	return eOK;
}

int WorldRuntime::shiftTabCompleteItem(const QString &item)
{
	if (item.isEmpty() || item.size() > 30)
		return eBadParameter;
	if (item == QStringLiteral("<clear>"))
	{
		m_shiftTabCompleteItems.clear();
		return eOK;
	}
	if (item == QStringLiteral("<functions>"))
	{
		m_tabCompleteFunctions = true;
		return eOK;
	}
	if (item == QStringLiteral("<nofunctions>"))
	{
		m_tabCompleteFunctions = false;
		return eOK;
	}

	const QChar  first     = item.at(0);
	const ushort firstCode = first.unicode();
	const bool   firstIsLetter =
	    (firstCode >= 'A' && firstCode <= 'Z') || (firstCode >= 'a' && firstCode <= 'z');
	if (!firstIsLetter)
		return eBadParameter;

	for (const QChar ch : item)
	{
		const ushort code = ch.unicode();
		const bool   isAlnum =
		    (code >= 'A' && code <= 'Z') || (code >= 'a' && code <= 'z') || (code >= '0' && code <= '9');
		if (isAlnum || code == '_' || code == '-' || code == '.')
			continue;
		return eBadParameter;
	}

	m_shiftTabCompleteItems.insert(item);
	return eOK;
}

int WorldRuntime::deleteLastMapItem()
{
	if (m_mappingList.isEmpty())
		return eNoMapItems;
	m_mappingList.removeLast();
	return eOK;
}

int WorldRuntime::deleteAllMapItems()
{
	if (m_mappingList.isEmpty())
		return eNoMapItems;
	m_mappingList.clear();
	return eOK;
}

void WorldRuntime::deleteLines(int count)
{
	if (count <= 0 || m_lines.isEmpty())
		return;
	if (count >= m_lines.size())
		m_lines.clear();
	else
		m_lines.erase(m_lines.end() - count, m_lines.end());
	if (m_view)
		m_view->rebuildOutputFromLines(m_lines);
}

void WorldRuntime::deleteOutput()
{
	m_lines.clear();
	if (m_view)
		m_view->clearOutputBuffer();
}

int WorldRuntime::deleteVariable(const QString &name)
{
	for (int i = 0; i < m_variables.size(); ++i)
	{
		const QString varName = m_variables.at(i).attributes.value(QStringLiteral("name"));
		if (varName.compare(name, Qt::CaseInsensitive) == 0)
		{
			m_variables.removeAt(i);
			m_variableCount    = safeQSizeToInt(m_variables.size());
			m_variablesChanged = true;
			return eOK;
		}
	}
	return eVariableNotFound;
}

int WorldRuntime::discardQueuedCommands() const
{
	if (!m_commandProcessor)
		return 0;
	return m_commandProcessor->discardQueuedCommands();
}

void WorldRuntime::setMappingEnabled(bool enabled)
{
	m_isMapping = enabled;
}

QString WorldRuntime::evaluateSpeedwalk(const QString &speedWalkString) const
{
	if (!m_commandProcessor)
		return {};
	return m_commandProcessor->evaluateSpeedwalk(speedWalkString);
}

int WorldRuntime::executeCommand(const QString &text) const
{
	if (!m_commandProcessor)
		return eWorldClosed;
	return m_commandProcessor->executeCommand(text);
}

QString WorldRuntime::getEntityValue(const QString &name) const
{
	QByteArray value;
	if (m_telnet.getCustomEntityValue(name.toLocal8Bit(), value))
		return QString::fromUtf8(value);
	return {};
}

void WorldRuntime::setEntityValue(const QString &name, const QString &value)
{
	const QByteArray key      = name.toUtf8();
	const QByteArray contents = value.toUtf8();
	m_telnet.setCustomEntity(key, contents);
}

void WorldRuntime::addTriggerTimeNs(qint64 ns)
{
	if (ns > 0)
		m_triggerTimeNs += ns;
}

void WorldRuntime::addAliasTimeNs(qint64 ns)
{
	if (ns > 0)
		m_aliasTimeNs += ns;
}

double WorldRuntime::triggerTimeSeconds() const
{
	return static_cast<double>(m_triggerTimeNs) / 1000000000.0;
}

double WorldRuntime::aliasTimeSeconds() const
{
	return static_cast<double>(m_aliasTimeNs) / 1000000000.0;
}

bool WorldRuntime::removeMapReverses() const
{
	return m_removeMapReverses;
}

void WorldRuntime::setRemoveMapReverses(bool enabled)
{
	m_removeMapReverses = enabled;
}

int WorldRuntime::lastGoToLine() const
{
	return m_lastGoTo;
}

void WorldRuntime::setLastGoToLine(int line)
{
	m_lastGoTo = line;
}

void WorldRuntime::fireWorldConnectHandlers()
{
	const unsigned short previousActionSource = m_currentActionSource;
	m_currentActionSource                     = eWorldAction;
	if (isLuaScriptingEnabled(m_worldAttributes) && m_luaCallbacks)
	{
		if (const QString callbackName =
		        m_worldAttributes.value(QStringLiteral("on_world_connect")).trimmed();
		    !callbackName.isEmpty())
			m_luaCallbacks->callFunctionNoArgs(callbackName);
	}
	callPluginCallbacksNoArgs(QStringLiteral("OnPluginConnect"));
	m_currentActionSource = previousActionSource;
}

void WorldRuntime::fireWorldDisconnectHandlers()
{
	const unsigned short previousActionSource = m_currentActionSource;
	m_currentActionSource                     = eWorldAction;
	if (isLuaScriptingEnabled(m_worldAttributes) && m_luaCallbacks)
	{
		if (const QString callbackName =
		        m_worldAttributes.value(QStringLiteral("on_world_disconnect")).trimmed();
		    !callbackName.isEmpty())
			m_luaCallbacks->callFunctionNoArgs(callbackName);
	}
	callPluginCallbacksNoArgs(QStringLiteral("OnPluginDisconnect"));
	m_currentActionSource = previousActionSource;
}

void WorldRuntime::fireWorldOpenHandlers()
{
	const unsigned short previousActionSource = m_currentActionSource;
	m_currentActionSource                     = eWorldAction;
	if (isLuaScriptingEnabled(m_worldAttributes) && m_luaCallbacks)
	{
		if (const QString callbackName = m_worldAttributes.value(QStringLiteral("on_world_open")).trimmed();
		    !callbackName.isEmpty())
			m_luaCallbacks->callFunctionNoArgs(callbackName);
	}
	m_currentActionSource = previousActionSource;
}

void WorldRuntime::fireWorldCloseHandlers()
{
	const unsigned short previousActionSource = m_currentActionSource;
	m_currentActionSource                     = eWorldAction;
	if (isLuaScriptingEnabled(m_worldAttributes) && m_luaCallbacks)
	{
		if (const QString callbackName = m_worldAttributes.value(QStringLiteral("on_world_close")).trimmed();
		    !callbackName.isEmpty())
			m_luaCallbacks->callFunctionNoArgs(callbackName);
	}
	m_currentActionSource = previousActionSource;
}

void WorldRuntime::fireWorldSaveHandlers()
{
	const unsigned short previousActionSource = m_currentActionSource;
	m_currentActionSource                     = eWorldAction;
	if (isLuaScriptingEnabled(m_worldAttributes) && m_luaCallbacks)
	{
		if (const QString callbackName = m_worldAttributes.value(QStringLiteral("on_world_save")).trimmed();
		    !callbackName.isEmpty())
			m_luaCallbacks->callFunctionNoArgs(callbackName);
	}
	callPluginCallbacksNoArgs(QStringLiteral("OnPluginWorldSave"));
	m_currentActionSource = previousActionSource;
}

void WorldRuntime::fireWorldGetFocusHandlers()
{
	const unsigned short previousActionSource = m_currentActionSource;
	m_currentActionSource                     = eWorldAction;
	if (isLuaScriptingEnabled(m_worldAttributes) && m_luaCallbacks)
	{
		if (const QString callbackName =
		        m_worldAttributes.value(QStringLiteral("on_world_get_focus")).trimmed();
		    !callbackName.isEmpty())
			m_luaCallbacks->callFunctionNoArgs(callbackName);
	}
	callPluginCallbacksNoArgs(QStringLiteral("OnPluginGetFocus"));
	m_currentActionSource = previousActionSource;
}

void WorldRuntime::fireWorldLoseFocusHandlers()
{
	const unsigned short previousActionSource = m_currentActionSource;
	m_currentActionSource                     = eWorldAction;
	if (isLuaScriptingEnabled(m_worldAttributes) && m_luaCallbacks)
	{
		if (const QString callbackName =
		        m_worldAttributes.value(QStringLiteral("on_world_lose_focus")).trimmed();
		    !callbackName.isEmpty())
			m_luaCallbacks->callFunctionNoArgs(callbackName);
	}
	callPluginCallbacksNoArgs(QStringLiteral("OnPluginLoseFocus"));
	m_currentActionSource = previousActionSource;
}

void WorldRuntime::mxpError(int level, long messageNumber, const QString &message)
{
	if (const QString callbackName = m_worldAttributes.value(QStringLiteral("on_mxp_error")).trimmed();
	    !callbackName.isEmpty() && m_luaCallbacks)
	{
		const bool suppress =
		    m_luaCallbacks->callMxpError(callbackName, level, messageNumber, m_linesReceived, message);
		if (suppress)
			return;
	}

	constexpr const char *sLevel[] = {" ", "E", "W", "I", "A"};
	auto                 *p        = "?";
	if (level >= 0 && level < static_cast<int>(std::size(sLevel)))
		p = sLevel[level];

	if (level == DBG_ERROR)
		m_mxpErrors++;

	int           debugLevel = DBG_NONE;
	bool          ok         = false;
	const QString rawDebug   = m_worldAttributes.value(QStringLiteral("mxp_debug_level"));
	debugLevel               = rawDebug.toInt(&ok);
	if (!ok)
	{
		const WorldNumericOption *opt =
		    QMudWorldOptions::findWorldNumericOption(QStringLiteral("mxp_debug_level"));
		if (opt)
			debugLevel = static_cast<int>(opt->defaultValue);
	}

	if (level > debugLevel)
		return;

	auto title = QStringLiteral("MXP Messages");
	if (const QString worldName = m_worldAttributes.value(QStringLiteral("name")); !worldName.isEmpty())
		title += QStringLiteral(" - %1").arg(worldName);

	const QString pluginPayload = QStringLiteral("%1,%2,%3,%4")
	                                  .arg(QString::fromLatin1(p))
	                                  .arg(messageNumber)
	                                  .arg(m_linesReceived)
	                                  .arg(message);
	callPluginCallbacks(QStringLiteral("OnPluginMXPerror"), pluginPayload);

	const QString formatted = QStringLiteral("%1 %2: (%3) %4\r\n")
	                              .arg(QString::fromLatin1(p))
	                              .arg(messageNumber, 5)
	                              .arg(m_linesReceived, 5)
	                              .arg(message);

	emit mxpDebugMessage(title, formatted);
}

void WorldRuntime::mxpStartUp()
{
	if (m_mxpActive)
		return;
	m_mxpActive = true;

	if (isLuaScriptingEnabled(m_worldAttributes) && m_luaCallbacks)
	{
		const QString callbackName = m_worldAttributes.value(QStringLiteral("on_mxp_start")).trimmed();
		if (!callbackName.isEmpty())
			m_luaCallbacks->callMxpStartUp(callbackName);
	}

	callPluginCallbacksNoArgs(QStringLiteral("OnPluginMXPstart"));
}

void WorldRuntime::mxpShutDown()
{
	if (!m_mxpActive)
		return;
	m_mxpActive = false;
	m_mxpTagStack.clear();
	m_mxpOpenTags.clear();
	m_mxpTextBuffer.clear();
	resetMxpRenderState();
	clearAnsiActionContext();

	if (isLuaScriptingEnabled(m_worldAttributes) && m_luaCallbacks)
	{
		const QString callbackName = m_worldAttributes.value(QStringLiteral("on_mxp_stop")).trimmed();
		if (!callbackName.isEmpty())
			m_luaCallbacks->callMxpShutDown(callbackName);
	}

	callPluginCallbacksNoArgs(QStringLiteral("OnPluginMXPstop"));
}

void WorldRuntime::resetMxp()
{
	m_telnet.resetMxp();
	mxpShutDown();
}

void WorldRuntime::resetMxpTags()
{
	m_telnet.resetMxp();
	m_mxpTagStack.clear();
	m_mxpOpenTags.clear();
	m_mxpTextBuffer.clear();
	resetMxpRenderState();
	clearAnsiActionContext();
}

void WorldRuntime::resetIpCache()
{
	m_hasCachedIp = false;
}

void WorldRuntime::resetAllTimers()
{
	for (auto &timer : m_timers)
		resetTimerFields(timer);
}

bool WorldRuntime::hasCachedIp() const
{
	return m_hasCachedIp;
}

int WorldRuntime::mxpBuiltinElementCount()
{
	return mxpBuiltinTagCount();
}

int WorldRuntime::mxpBuiltinEntityCount()
{
	return TelnetProcessor::builtinEntityCount();
}

int WorldRuntime::mxpOpenTagCount() const
{
	return safeQSizeToInt(m_mxpOpenTags.size());
}

int WorldRuntime::openLog(const QString &logFileName, bool append)
{
	if (m_logFile.isOpen())
		return eLogFileAlreadyOpen;

	m_logFileName = logFileName;

	if (m_logFileName.isEmpty())
	{
		if (const QString autoName = m_worldAttributes.value(QStringLiteral("auto_log_file_name"));
		    !autoName.isEmpty())
		{
			if (const QString playerName = m_worldAttributes.value(QStringLiteral("player")).trimmed();
			    patternUsesPlayerDirective(autoName) && playerName.isEmpty())
				return eCouldNotOpenFile;
			m_logFileName = formatTime(QDateTime::currentDateTime(), autoName, false);
		}
	}

	if (m_logFileName.isEmpty())
		return eCouldNotOpenFile;

	// Resolve relative log paths under the configured default log directory.
	// If no default is configured, fall back to <startup>/logs.
	const QString workingDir = resolveWorkingDir(m_startupDirectory);
	QString       logBaseDir = m_defaultLogDirectory.trimmed();
	if (logBaseDir.isEmpty())
		logBaseDir = QDir::cleanPath(QDir(workingDir).filePath(QStringLiteral("logs")));
	else
		logBaseDir = makeAbsolutePath(logBaseDir, workingDir);
	m_logFileName = makeAbsolutePath(m_logFileName, logBaseDir);

	// Ensure missing parent folders (for patterns like logs/foo/bar.txt) exist.
	const QFileInfo logInfo(m_logFileName);
	if (const QString parentDir = logInfo.path(); !parentDir.isEmpty() && parentDir != QStringLiteral("."))
	{
		if (const QDir parent(parentDir); !parent.exists() && !QDir().mkpath(parentDir))
			return eCouldNotOpenFile;
	}

	m_logRotationBaseFileName = m_logFileName;
	m_logFile.setFileName(m_logFileName);
	QIODevice::OpenMode mode = QIODevice::WriteOnly;
	if (append)
		mode |= QIODevice::Append;
	else
		mode |= QIODevice::Truncate;

	if (!m_logFile.open(mode))
		return eCouldNotOpenFile;

	m_lastFlushTime = QDateTime::currentDateTime();
	emit logStateChanged(true);
	return eOK;
}

int WorldRuntime::closeLog()
{
	if (!m_logFile.isOpen())
		return eLogFileNotOpen;
	const QString closedLogFileName = m_logFileName;

	const bool    logRaw = isEnabledFlag(m_worldAttributes.value(QStringLiteral("log_raw")));
	if (!logRaw)
	{
		const QMap<QString, QString> &multi     = worldMultilineAttributes();
		QString                       postamble = multi.value(QStringLiteral("log_file_postamble"));
		if (postamble.isEmpty())
			postamble = m_worldAttributes.value(QStringLiteral("log_file_postamble"));
		if (!postamble.isEmpty())
		{
			const bool      logHtml = isEnabledFlag(m_worldAttributes.value(QStringLiteral("log_html")));
			QDateTime const now     = QDateTime::currentDateTime();
			postamble.replace(QStringLiteral("%n"), QStringLiteral("\n"));
			postamble = formatTime(now, postamble, logHtml);
			postamble.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
			writeLog(postamble);
			writeLog(QStringLiteral("\n"));
		}
	}

	m_logFile.close();
	if (logRotateGzipEnabled() && !closedLogFileName.isEmpty() && QFileInfo::exists(closedLogFileName))
	{
		QString gzipError;
		if (!gzipFileInPlace(closedLogFileName, &gzipError) && !gzipError.isEmpty())
			QMessageBox::warning(nullptr, QStringLiteral("QMud"), gzipError);
	}
	emit logStateChanged(false);
	return eOK;
}

qint64 WorldRuntime::logRotateLimitBytes() const
{
	bool   ok       = false;
	qint64 rotateMb = m_worldAttributes.value(QStringLiteral("log_rotate_mb")).toLongLong(&ok);
	if (!ok)
		rotateMb = 100;
	if (rotateMb <= 0)
		return 0;
	return rotateMb * 1024 * 1024;
}

bool WorldRuntime::logRotateGzipEnabled() const
{
	return isEnabledFlag(m_worldAttributes.value(QStringLiteral("log_rotate_gzip"), QStringLiteral("1")));
}

QString WorldRuntime::buildRotatedLogFileName(const QString &previousFileName) const
{
	const QString baseName  = m_logRotationBaseFileName.isEmpty() ? m_logFileName : m_logRotationBaseFileName;
	QString       candidate = baseName;
	int           index     = 2;
	while (candidate == previousFileName || QFileInfo::exists(candidate) ||
	       QFileInfo::exists(candidate + QStringLiteral(".gz")))
	{
		candidate = appendNumberBeforeExtension(baseName, index);
		++index;
	}
	return candidate;
}

bool WorldRuntime::gzipFileInPlace(const QString &sourceFileName, QString *errorMessage)
{
	return qmudGzipFileInPlace(sourceFileName, errorMessage);
}

int WorldRuntime::rotateLogFile()
{
	if (!m_logFile.isOpen())
		return eLogFileNotOpen;
	if (m_logRotateInProgress)
		return eOK;

	m_logRotateInProgress          = true;
	const QString previousFileName = m_logFileName;
	const bool    gzipRotated      = logRotateGzipEnabled();

	m_logFile.flush();
	m_logFile.close();

	if (gzipRotated && QFileInfo::exists(previousFileName))
	{
		QString gzipError;
		if (!gzipFileInPlace(previousFileName, &gzipError) && !gzipError.isEmpty())
			QMessageBox::warning(nullptr, QStringLiteral("QMud"), gzipError);
	}

	const QString nextFileName = buildRotatedLogFileName(previousFileName);
	m_logFileName              = nextFileName;
	m_logFile.setFileName(nextFileName);
	if (!m_logFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
	{
		const QString openError = QStringLiteral("Unable to open rotated log file \"%1\"").arg(nextFileName);
		QMessageBox::warning(nullptr, QStringLiteral("QMud"), openError);
		m_logFileName = previousFileName;
		m_logFile.setFileName(previousFileName);
		if (!m_logFile.open(QIODevice::WriteOnly | QIODevice::Append))
			QMessageBox::warning(
			    nullptr, QStringLiteral("QMud"),
			    QStringLiteral("Unable to reopen previous log file \"%1\"").arg(previousFileName));
		m_logRotateInProgress = false;
		return eCouldNotOpenFile;
	}

	m_lastFlushTime       = QDateTime::currentDateTime();
	m_logRotateInProgress = false;
	return eOK;
}

int WorldRuntime::writeLog(const QByteArray &bytes)
{
	if (!m_logFile.isOpen())
		return eLogFileNotOpen;
	if (bytes.isEmpty())
		return eOK;

	const qint64 written = m_logFile.write(bytes);
	if (written != bytes.size())
	{
		QString const str = QStringLiteral("An error occurred writing to log file \"%1\"").arg(m_logFileName);
		m_logFile.close();
		QMessageBox::warning(nullptr, QStringLiteral("QMud"), str);
		return eLogFileBadWrite;
	}

	m_lastFlushTime          = QDateTime::currentDateTime();
	const qint64 rotateLimit = logRotateLimitBytes();
	if (rotateLimit > 0 && m_logFile.size() >= rotateLimit)
		return rotateLogFile();
	return eOK;
}

int WorldRuntime::writeLog(const QString &text)
{
	return writeLog(text.toLocal8Bit());
}

int WorldRuntime::flushLog()
{
	if (!m_logFile.isOpen())
		return eLogFileNotOpen;
	return m_logFile.flush() ? eOK : eLogFileBadWrite;
}

bool WorldRuntime::isLogOpen() const
{
	return m_logFile.isOpen();
}

QString WorldRuntime::logFileName() const
{
	return m_logFileName;
}

qint64 WorldRuntime::logFilePosition() const
{
	if (!m_logFile.isOpen())
		return 0;
	return m_logFile.pos();
}

QString WorldRuntime::formatTime(const QDateTime &time, const QString &format, bool fixHtml) const
{
	const auto workingDir = TimeFormatUtils::resolveWorkingDir(m_startupDirectory);
	const auto worldDir   = TimeFormatUtils::makeAbsolutePath(m_defaultWorldDirectory, workingDir);
	const auto logDir     = TimeFormatUtils::makeAbsolutePath(m_defaultLogDirectory, workingDir);

	TimeFormatUtils::WorldTimeFormatContext context;
	context.workingDir = workingDir;
	context.worldName  = m_worldAttributes.value(QStringLiteral("name"));
	context.playerName = m_worldAttributes.value(QStringLiteral("player"));
	context.worldDir   = worldDir;
	context.logDir     = logDir;

	return TimeFormatUtils::formatWorldTime(time, format, context, fixHtml, &fixHtmlString);
}

void WorldRuntime::setDefaultWorldDirectory(const QString &path)
{
	m_defaultWorldDirectory = path;
}

void WorldRuntime::setDefaultLogDirectory(const QString &path)
{
	m_defaultLogDirectory = path;
}

void WorldRuntime::setStartupDirectory(const QString &path)
{
	m_startupDirectory = path;
}

QString WorldRuntime::startupDirectory() const
{
	return m_startupDirectory;
}

QString WorldRuntime::defaultWorldDirectory() const
{
	return m_defaultWorldDirectory;
}

QString WorldRuntime::defaultLogDirectory() const
{
	return m_defaultLogDirectory;
}

void WorldRuntime::setWindowTitleOverride(const QString &title)
{
	if (m_windowTitleOverride == title)
		return;
	m_windowTitleOverride = title;
	emit windowTitleChanged();
}

QString WorldRuntime::windowTitleOverride() const
{
	return m_windowTitleOverride;
}

void WorldRuntime::setMainTitleOverride(const QString &title)
{
	if (m_mainTitleOverride == title)
		return;
	m_mainTitleOverride = title;
	emit mainTitleChanged();
}

QString WorldRuntime::mainTitleOverride() const
{
	return m_mainTitleOverride;
}

bool WorldRuntime::hasAnyPluginCallback(const QString &functionName)
{
	if (functionName.isEmpty())
		return false;

	noteObservedPluginCallbackQuery(m_observedPluginCallbackQueryGeneration, functionName,
	                                m_observedPluginCallbackGeneration);

	if (ensureObservedPluginCallback(m_observedPluginCallbacks, functionName))
	{
		for (auto &plugin : m_plugins)
		{
			if (plugin.lua)
				plugin.lua->setObservedPluginCallbacks(m_observedPluginCallbacks);
		}
		m_pluginCallbackPresenceDirty = true;
	}

	if (m_plugins.isEmpty())
		return false;

	const int pluginCount = safeQSizeToInt(m_plugins.size());
	if (m_pluginCallbackPresencePluginCount != pluginCount)
	{
		m_pluginCallbackPresencePluginCount = pluginCount;
		m_pluginCallbackPresenceDirty       = true;
	}

	if (m_pluginCallbackPresenceDirty)
		rebuildPluginCallbackPresenceCache();
	return m_pluginCallbackPresenceCounts.value(functionName) > 0;
}

void WorldRuntime::invalidatePluginCallbackPresenceCache()
{
	m_pluginCallbackPresenceDirty = true;
}

void WorldRuntime::rebuildPluginCallbackPresenceCache()
{
	m_pluginCallbackPresenceCounts.clear();
	m_pluginCallbackRecipientIndices.clear();
	pruneStaleObservedPluginCallbacks(m_observedPluginCallbacks, m_observedPluginCallbackQueryGeneration,
	                                  m_observedPluginCallbackGeneration,
	                                  observedPluginCallbackRetentionGenerations());

	if (m_observedPluginCallbacks.isEmpty())
	{
		m_pluginCallbackPresencePluginCount = safeQSizeToInt(m_plugins.size());
		m_pluginCallbackPresenceDirty       = false;
		advanceObservedPluginCallbackGeneration(m_observedPluginCallbackGeneration);
		return;
	}

	for (int pluginIndex = 0; pluginIndex < m_plugins.size(); ++pluginIndex)
	{
		auto &plugin = m_plugins[pluginIndex];
		if (!canExecutePlugin(plugin) || !plugin.lua)
			continue;
		plugin.lua->setObservedPluginCallbacks(m_observedPluginCallbacks);
		if (!plugin.lua->loadScript())
			continue;
		for (const QString &name : m_observedPluginCallbacks)
		{
			if (name.isEmpty())
				continue;
			if (plugin.lua->hasObservedPluginCallback(name))
			{
				++m_pluginCallbackPresenceCounts[name];
				m_pluginCallbackRecipientIndices[name].push_back(pluginIndex);
			}
		}
	}
	m_pluginCallbackPresencePluginCount = safeQSizeToInt(m_plugins.size());
	m_pluginCallbackPresenceDirty       = false;
	advanceObservedPluginCallbackGeneration(m_observedPluginCallbackGeneration);
}

void WorldRuntime::bindPluginCallbackObserver(const Plugin &plugin)
{
	if (!plugin.lua)
		return;
	plugin.lua->setObservedPluginCallbacks(m_observedPluginCallbacks);
	plugin.lua->setCallbackCatalogObserver([this] { invalidatePluginCallbackPresenceCache(); });
	invalidatePluginCallbackPresenceCache();
}

bool WorldRuntime::callPluginCallbacksStopOnFalse(const QString &functionName, const QString &payload)
{
	if (!hasAnyPluginCallback(functionName))
		return true;

	bool result = true;
	for (auto &plugin : m_plugins)
	{
		if (!canExecutePlugin(plugin))
			continue;
		bool       hasFunction = false;
		const bool ok = plugin.lua->callFunctionWithString(functionName, payload, &hasFunction, true);
		if (hasFunction && !ok)
		{
			result = false;
			break;
		}
	}
	return result;
}

void WorldRuntime::callPluginCallbacks(const QString &functionName, const QString &payload)
{
	if (!hasAnyPluginCallback(functionName))
		return;

	for (auto &plugin : m_plugins)
	{
		if (!canExecutePlugin(plugin))
			continue;
		plugin.lua->callFunctionWithString(functionName, payload, nullptr, true);
	}
}

void WorldRuntime::callPluginCallbacksNoArgs(const QString &functionName)
{
	if (!hasAnyPluginCallback(functionName))
		return;

	for (auto &plugin : m_plugins)
	{
		if (!canExecutePlugin(plugin))
			continue;
		plugin.lua->callFunctionNoArgs(functionName, nullptr, true);
	}
}

bool WorldRuntime::callPluginCallbacksStopOnTrue(const QString &functionName, long arg1, const QString &arg2)
{
	if (!hasAnyPluginCallback(functionName))
		return false;

	for (auto &plugin : m_plugins)
	{
		if (!canExecutePlugin(plugin))
			continue;
		bool       hasFunction = false;
		const bool ok =
		    plugin.lua->callFunctionWithNumberAndString(functionName, arg1, arg2, &hasFunction, false);
		if (hasFunction && ok)
			return true;
	}
	return false;
}

bool WorldRuntime::callPluginCallbacksStopOnTrueWithString(const QString &functionName,
                                                           const QString &payload)
{
	if (!hasAnyPluginCallback(functionName))
		return false;

	for (auto &plugin : m_plugins)
	{
		if (!canExecutePlugin(plugin))
			continue;
		bool hasFunction = false;
		plugin.lua->callFunctionWithString(functionName, payload, &hasFunction, false);
		// Legacy SendToFirstPluginCallbacks semantics: handled if callback exists,
		// regardless of the callback's boolean return value.
		if (hasFunction)
			return true;
	}
	return false;
}

void WorldRuntime::callPluginCallbacksTransformBytes(const QString &functionName, QByteArray &payload)
{
	if (!hasAnyPluginCallback(functionName))
		return;

	for (auto &plugin : m_plugins)
	{
		if (!canExecutePlugin(plugin))
			continue;
		bool hasFunction = false;
		plugin.lua->callFunctionWithBytesInOut(functionName, payload, &hasFunction);
	}
}

void WorldRuntime::callPluginCallbacksTransformString(const QString &functionName, QString &payload)
{
	if (!hasAnyPluginCallback(functionName))
		return;

	for (auto &plugin : m_plugins)
	{
		if (!canExecutePlugin(plugin))
			continue;
		bool hasFunction = false;
		plugin.lua->callFunctionWithStringInOut(functionName, payload, &hasFunction);
	}
}

bool WorldRuntime::callPluginCallbacksStopOnFalseWithNumberAndString(const QString &functionName, long arg1,
                                                                     const QString &arg2)
{
	if (!hasAnyPluginCallback(functionName))
		return true;

	for (auto &plugin : m_plugins)
	{
		if (!canExecutePlugin(plugin))
			continue;
		bool       hasFunction = false;
		const bool ok =
		    plugin.lua->callFunctionWithNumberAndString(functionName, arg1, arg2, &hasFunction, true);
		if (hasFunction && !ok)
			return false;
	}
	return true;
}

bool WorldRuntime::callPluginCallbacksStopOnFalseWithTwoNumbersAndString(const QString &functionName,
                                                                         long arg1, long arg2,
                                                                         const QString &arg3)
{
	if (!hasAnyPluginCallback(functionName))
		return true;

	for (auto &plugin : m_plugins)
	{
		if (!canExecutePlugin(plugin))
			continue;
		bool       hasFunction = false;
		const bool ok = plugin.lua->callFunctionWithTwoNumbersAndString(functionName, arg1, arg2, arg3,
		                                                                &hasFunction, true);
		if (hasFunction && !ok)
			return false;
	}
	return true;
}

void WorldRuntime::callPluginCallbacksWithNumberAndString(const QString &functionName, long arg1,
                                                          const QString &arg2)
{
	if (!hasAnyPluginCallback(functionName))
		return;

	for (auto &plugin : m_plugins)
	{
		if (!canExecutePlugin(plugin))
			continue;
		plugin.lua->callFunctionWithNumberAndString(functionName, arg1, arg2, nullptr, true);
	}
}

void WorldRuntime::callPluginCallbacksWithBytes(const QString &functionName, const QByteArray &payload)
{
	if (!hasAnyPluginCallback(functionName))
		return;

	for (auto &plugin : m_plugins)
	{
		if (!canExecutePlugin(plugin))
			continue;
		plugin.lua->callFunctionWithBytes(functionName, payload, nullptr, true);
	}
}

bool WorldRuntime::callPluginCallbacksStopOnTrueBytes(const QString &functionName, long arg1,
                                                      const QByteArray &payload)
{
	if (!hasAnyPluginCallback(functionName))
		return false;

	for (auto &plugin : m_plugins)
	{
		if (!canExecutePlugin(plugin))
			continue;
		bool       hasFunction = false;
		const bool ok =
		    plugin.lua->callFunctionWithNumberAndBytes(functionName, arg1, payload, &hasFunction, false);
		if (hasFunction && ok)
			return true;
	}
	return false;
}

void WorldRuntime::callPluginCallbacksWithNumberAndBytes(const QString &functionName, long arg1,
                                                         const QByteArray &payload)
{
	if (!hasAnyPluginCallback(functionName))
		return;

	for (auto &plugin : m_plugins)
	{
		if (!canExecutePlugin(plugin))
			continue;
		plugin.lua->callFunctionWithNumberAndBytes(functionName, arg1, payload, nullptr, true);
	}
}

bool WorldRuntime::callPluginHotspotFunction(const QString &pluginId, const QString &functionName, long flags,
                                             const QString &hotspotId)
{
	if (pluginId.isEmpty() || functionName.isEmpty())
		return false;
	const int index = findPluginIndex(m_plugins, pluginId);
	if (index < 0)
		return false;
	Plugin const &plugin = m_plugins[index];
	if (!canExecutePlugin(plugin))
		return false;
	const unsigned short previousActionSource = m_currentActionSource;
	m_currentActionSource                     = eHotspotCallback;
	bool       hasFunction                    = false;
	const bool ok =
	    plugin.lua->callFunctionWithNumberAndString(functionName, flags, hotspotId, &hasFunction, false);
	m_currentActionSource = previousActionSource;
	return hasFunction && ok;
}

bool WorldRuntime::callWorldHotspotFunction(const QString &functionName, long flags, const QString &hotspotId)
{
	if (functionName.isEmpty() || !m_luaCallbacks)
		return false;
	const unsigned short previousActionSource = m_currentActionSource;
	m_currentActionSource                     = eHotspotCallback;
	bool       hasFunction                    = false;
	const bool ok =
	    m_luaCallbacks->callFunctionWithNumberAndString(functionName, flags, hotspotId, &hasFunction, false);
	m_currentActionSource = previousActionSource;
	return hasFunction && ok;
}

void WorldRuntime::setLuaScriptText(const QString &script)
{
	m_luaScriptText = script;
	if (m_luaCallbacks)
		m_luaCallbacks->setScriptText(script);
}

void WorldRuntime::sendText(const QString &text, bool addNewline)
{
	if (!addNewline)
	{
		sendToWorld(text.toUtf8());
		return;
	}

	const bool echo = isEnabledFlag(m_worldAttributes.value(QStringLiteral("display_my_input")));
	static_cast<void>(sendCommand(text, echo, false, false, false, false));
}

void WorldRuntime::outputText(const QString &text, bool note, bool newLine)
{
	if (text.isEmpty() && !newLine)
		return;
	const int type = note ? 1 : 0;
	const int log  = note ? (isEnabledFlag(m_worldAttributes.value(QStringLiteral("log_notes"))) ? 1 : 0)
	                      : (isEnabledFlag(m_worldAttributes.value(QStringLiteral("log_output"))) ? 1 : 0);
	firePluginScreendraw(type, log, text);
	QString                     displayText          = text;
	const bool                  serverSideWrapActive = isConnected() && m_telnet.isNawsNegotiated();
	const FixedColumnWrapConfig wrapConfig =
	    (!note && serverSideWrapActive)
	        ? FixedColumnWrapConfig{}
	        : localOutputWrapConfig(m_worldAttributes, serverSideWrapActive, m_telnet.windowColumns());
	if (wrapConfig.enabled && !displayText.isEmpty())
		wrapPlainLineForColumn(displayText, wrapConfig.wrapColumn, wrapConfig.indentParas);
	emit outputRequested(displayText, newLine, note);
}

void WorldRuntime::outputStyledText(const QString &text, const QVector<StyleSpan> &spans, bool note,
                                    bool newLine)
{
	if (text.isEmpty() && spans.isEmpty() && !newLine)
		return;
	const int type = note ? 1 : 0;
	const int log  = note ? (isEnabledFlag(m_worldAttributes.value(QStringLiteral("log_notes"))) ? 1 : 0)
	                      : (isEnabledFlag(m_worldAttributes.value(QStringLiteral("log_output"))) ? 1 : 0);
	firePluginScreendraw(type, log, text);
	QString                     displayText          = text;
	QVector<StyleSpan>          displaySpans         = spans;
	const bool                  serverSideWrapActive = isConnected() && m_telnet.isNawsNegotiated();
	const FixedColumnWrapConfig wrapConfig =
	    (!note && serverSideWrapActive)
	        ? FixedColumnWrapConfig{}
	        : localOutputWrapConfig(m_worldAttributes, serverSideWrapActive, m_telnet.windowColumns());
	if (wrapConfig.enabled && !displayText.isEmpty())
	{
		if (displaySpans.isEmpty())
			wrapPlainLineForColumn(displayText, wrapConfig.wrapColumn, wrapConfig.indentParas);
		else
			wrapStyledLineForColumn(displayText, displaySpans, wrapConfig.wrapColumn, wrapConfig.indentParas);
	}
	emit outputStyledRequested(displayText, displaySpans, newLine, note);
}

void WorldRuntime::prepareInputEchoForDisplay(QString &text, QVector<StyleSpan> &spans,
                                              const bool appendToCurrentLine) const
{
	if (text.isEmpty())
		return;

	const bool wrapEnabled = isEnabledFlag(m_worldAttributes.value(QStringLiteral("wrap")));
	if (!wrapEnabled)
		return;

	const bool                  nawsNegotiated = isConnected() && m_telnet.isNawsNegotiated();
	const FixedColumnWrapConfig wrapConfig =
	    localOutputWrapConfig(m_worldAttributes, nawsNegotiated, m_telnet.windowColumns());
	if (!wrapConfig.enabled || wrapConfig.wrapColumn <= 0)
		return;

	int firstLinePrefixColumns = 0;
	if (appendToCurrentLine && !m_lines.isEmpty())
	{
		firstLinePrefixColumns = trailingLineColumnWidthForWrap(m_lines.constLast().text);
		if (firstLinePrefixColumns >= wrapConfig.wrapColumn)
		{
			text.prepend(QLatin1Char('\n'));
			if (!spans.isEmpty())
			{
				StyleSpan prefixSpan = spans.constFirst();
				prefixSpan.length    = 1;
				spans.prepend(prefixSpan);
			}
			firstLinePrefixColumns = 0;
		}
	}
	if (spans.isEmpty())
		wrapPlainLineForColumn(text, wrapConfig.wrapColumn, wrapConfig.indentParas, firstLinePrefixColumns);
	else
		wrapStyledLineForColumn(text, spans, wrapConfig.wrapColumn, wrapConfig.indentParas,
		                        firstLinePrefixColumns);
}

void WorldRuntime::outputAnsiText(const QString &text, bool note)
{
	if (text.isEmpty())
		return;
	const bool                  serverSideWrapActive = isConnected() && m_telnet.isNawsNegotiated();
	const FixedColumnWrapConfig wrapConfig =
	    (!note && serverSideWrapActive)
	        ? FixedColumnWrapConfig{}
	        : localOutputWrapConfig(m_worldAttributes, serverSideWrapActive, m_telnet.windowColumns());

	auto parseColorValue = [](const QString &value) -> QColor
	{
		if (value.isEmpty())
			return {};
		QColor color(value);
		if (color.isValid())
			return color;
		bool      ok      = false;
		const int numeric = value.toInt(&ok);
		if (!ok)
			return {};
		const int r = numeric & 0xFF;
		const int g = (numeric >> 8) & 0xFF;
		const int b = (numeric >> 16) & 0xFF;
		return {r, g, b};
	};

	QVector<QColor> normalAnsi(8);
	QVector<QColor> boldAnsi(8);
	QVector<QColor> customText(16);
	QVector<QColor> customBack(16);
	normalAnsi[0] = QColor(0, 0, 0);
	normalAnsi[1] = QColor(128, 0, 0);
	normalAnsi[2] = QColor(0, 128, 0);
	normalAnsi[3] = QColor(128, 128, 0);
	normalAnsi[4] = QColor(0, 0, 128);
	normalAnsi[5] = QColor(128, 0, 128);
	normalAnsi[6] = QColor(0, 128, 128);
	normalAnsi[7] = QColor(192, 192, 192);
	boldAnsi[0]   = QColor(128, 128, 128);
	boldAnsi[1]   = QColor(255, 0, 0);
	boldAnsi[2]   = QColor(0, 255, 0);
	boldAnsi[3]   = QColor(255, 255, 0);
	boldAnsi[4]   = QColor(0, 0, 255);
	boldAnsi[5]   = QColor(255, 0, 255);
	boldAnsi[6]   = QColor(0, 255, 255);
	boldAnsi[7]   = QColor(255, 255, 255);
	for (int i = 0; i < customText.size(); ++i)
	{
		customText[i] = QColor(255, 255, 255);
		customBack[i] = QColor(0, 0, 0);
	}

	for (const auto &colour : m_colours)
	{
		const QString group = colour.group.trimmed().toLower();
		bool          ok    = false;
		const int     seq   = colour.attributes.value(QStringLiteral("seq")).toInt(&ok);
		const int     index = ok ? seq - 1 : -1;
		if (index < 0)
			continue;
		if (group == QStringLiteral("ansi/normal") && index < normalAnsi.size())
		{
			const QColor rgb = parseColorValue(colour.attributes.value(QStringLiteral("rgb")));
			if (rgb.isValid())
				normalAnsi[index] = rgb;
		}
		else if (group == QStringLiteral("ansi/bold") && index < boldAnsi.size())
		{
			const QColor rgb = parseColorValue(colour.attributes.value(QStringLiteral("rgb")));
			if (rgb.isValid())
				boldAnsi[index] = rgb;
		}
		else if ((group == QStringLiteral("custom/custom") || group == QStringLiteral("custom")) &&
		         index < customText.size())
		{
			const QColor textColor = parseColorValue(colour.attributes.value(QStringLiteral("text")));
			const QColor backColor = parseColorValue(colour.attributes.value(QStringLiteral("back")));
			if (textColor.isValid())
				customText[index] = textColor;
			if (backColor.isValid())
				customBack[index] = backColor;
		}
	}

	QColor defaultFore = parseColorValue(m_worldAttributes.value(QStringLiteral("output_text_colour")));
	QColor defaultBack = parseColorValue(m_worldAttributes.value(QStringLiteral("output_background_colour")));
	if (note)
	{
		const QColor noteFore = parseColorValue(m_worldAttributes.value(QStringLiteral("note_text_colour")));
		if (noteFore.isValid())
			defaultFore = noteFore;
	}
	if (!defaultFore.isValid())
		defaultFore = normalAnsi.value(7);
	if (!defaultBack.isValid())
		defaultBack = normalAnsi.value(0);

	struct StyleState
	{
			bool    bold{false};
			bool    underline{false};
			bool    italic{false};
			bool    blink{false};
			bool    inverse{false};
			bool    strike{false};
			QString fore;
			QString back;
	};

	StyleState current;
	current.fore = defaultFore.name();
	current.back = defaultBack.name();

	auto colorFromIndex = [](int idx) -> QString
	{
		if (idx < 0 || idx >= 256)
			return {};
		const AppController *app = AppController::instance();
		const QMudColorRef   ref = app ? app->xtermColorAt(idx) : qmudRgb(0, 0, 0);
		return QColor(qmudRed(ref), qmudGreen(ref), qmudBlue(ref)).name();
	};

	auto setFgIndex = [&](int idx)
	{
		if (idx < 0 || idx >= normalAnsi.size())
		{
			current.fore.clear();
			return;
		}
		const QColor chosen = current.bold ? boldAnsi.at(idx) : normalAnsi.at(idx);
		current.fore        = chosen.isValid() ? chosen.name() : QString();
	};
	auto setBgIndex = [&](int idx)
	{
		if (idx < 0 || idx >= normalAnsi.size())
		{
			current.back.clear();
			return;
		}
		const QColor chosen = normalAnsi.at(idx);
		current.back        = chosen.isValid() ? chosen.name() : QString();
	};

	QString            lineText;
	QVector<StyleSpan> lineSpans;

	auto               emitLine = [&](bool newLine)
	{
		QString            displayText  = lineText;
		QVector<StyleSpan> displaySpans = lineSpans;
		if (wrapConfig.enabled && !displayText.isEmpty())
			wrapStyledLineForColumn(displayText, displaySpans, wrapConfig.wrapColumn, wrapConfig.indentParas);
		emit outputStyledRequested(displayText, displaySpans, newLine, note);
		lineText.clear();
		lineSpans.clear();
	};

	auto appendPlain = [&](const QString &raw)
	{
		if (raw.isEmpty())
			return;
		QString cooked = raw;
		cooked.remove(QLatin1Char('\r'));
		if (cooked.isEmpty())
			return;
		const int segmentLength = safeQSizeToInt(cooked.size());
		int       start         = 0;
		while (start <= segmentLength)
		{
			const qsizetype newline = cooked.indexOf(QLatin1Char('\n'), start);
			const int pieceLen = (newline >= 0) ? safeQSizeToInt(newline - start) : (segmentLength - start);
			if (pieceLen > 0)
			{
				lineText += cooked.mid(start, pieceLen);
				StyleSpan span;
				span.length    = pieceLen;
				span.fore      = current.fore.isEmpty() ? QColor() : QColor(current.fore);
				span.back      = current.back.isEmpty() ? QColor() : QColor(current.back);
				span.bold      = current.bold;
				span.italic    = current.italic;
				span.blink     = current.blink;
				span.underline = current.underline;
				span.inverse   = current.inverse;
				span.strike    = current.strike;
				if (!lineSpans.isEmpty())
				{
					StyleSpan &lastSpan = lineSpans.last();
					if (lastSpan.fore == span.fore && lastSpan.back == span.back &&
					    lastSpan.bold == span.bold && lastSpan.italic == span.italic &&
					    lastSpan.blink == span.blink && lastSpan.underline == span.underline &&
					    lastSpan.inverse == span.inverse && lastSpan.strike == span.strike &&
					    lastSpan.changed == span.changed)
						lastSpan.length += span.length;
					else
						lineSpans.push_back(span);
				}
				else
					lineSpans.push_back(span);
			}
			if (newline < 0)
				break;
			emitLine(true);
			start = safeQSizeToInt(newline + 1);
		}
	};

	auto applySgr = [&](const QVector<int> &codes)
	{
		QVector<int> params = codes;
		if (params.isEmpty())
			params.push_back(0);
		for (int i = 0; i < params.size(); ++i)
		{
			const int code = params.at(i);
			if (code == 0)
			{
				current      = StyleState();
				current.fore = defaultFore.name();
				current.back = defaultBack.name();
			}
			else if (code == 1)
				current.bold = true;
			else if (code == 3)
				current.italic = true;
			else if (code == 5)
				current.blink = true;
			else if (code == 4)
				current.underline = true;
			else if (code == 7)
			{
				current.inverse = true;
				if (!current.fore.isEmpty() || !current.back.isEmpty())
					qSwap(current.fore, current.back);
			}
			else if (code == 9)
				current.strike = true;
			else if (code == 22)
				current.bold = false;
			else if (code == 23)
				current.italic = false;
			else if (code == 25)
				current.blink = false;
			else if (code == 24)
				current.underline = false;
			else if (code == 27)
				current.inverse = false;
			else if (code == 29)
				current.strike = false;
			else if (code == 39)
				current.fore = defaultFore.name();
			else if (code == 49)
				current.back = defaultBack.name();
			else if (code >= 30 && code <= 37)
				setFgIndex(code - 30);
			else if (code >= 40 && code <= 47)
				setBgIndex(code - 40);
			else if (code >= 90 && code <= 97)
			{
				const int idx = code - 90;
				if (idx >= 0 && idx < boldAnsi.size())
					current.fore = boldAnsi.at(idx).name();
			}
			else if (code >= 100 && code <= 107)
			{
				const int idx = code - 100;
				if (idx >= 0 && idx < boldAnsi.size())
					current.back = boldAnsi.at(idx).name();
			}
			else if (code == 38 || code == 48)
			{
				const bool isFore = (code == 38);
				if (i + 1 >= params.size())
					continue;
				const int mode = params.at(i + 1);
				if (mode == 5 && i + 2 < params.size())
				{
					const int idx = params.at(i + 2);
					if (isFore)
						current.fore = colorFromIndex(idx);
					else
						current.back = colorFromIndex(idx);
					i += 2;
				}
				else if (mode == 2 && i + 4 < params.size())
				{
					const int     r   = params.at(i + 2);
					const int     g   = params.at(i + 3);
					const int     b   = params.at(i + 4);
					const QString rgb = QColor(r, g, b).name();
					if (isFore)
						current.fore = rgb;
					else
						current.back = rgb;
					i += 4;
				}
			}
		}
	};

	QByteArray const bytes = text.toUtf8();
	QString          plainText;
	for (int i = 0; i < bytes.size(); ++i)
	{
		const char ch = bytes.at(i);
		if (ch == '\r')
			continue;
		if (ch == '\x1b')
		{
			if (i + 1 >= bytes.size())
				break;
			if (bytes.at(i + 1) != '[')
				continue;
			appendPlain(plainText);
			plainText.clear();
			i += 2;
			QByteArray paramBytes;
			bool       aborted = false;
			while (i < bytes.size())
			{
				const auto byte = static_cast<unsigned char>(bytes.at(i));
				if (byte >= 0x40 && byte <= 0x7E)
					break;
				if (byte >= 0x20 && byte <= 0x3F)
				{
					paramBytes.append(static_cast<char>(byte));
					++i;
					continue;
				}
				aborted = true;
				break;
			}
			if (aborted)
			{
				i -= 1;
				continue;
			}
			if (i >= bytes.size())
				break;
			const char finalByte = bytes.at(i);
			if (finalByte == 'm')
			{
				paramBytes.replace(':', ';');
				const QList<QByteArray> parts = paramBytes.split(';');
				QVector<int>            codes;
				for (const QByteArray &part : parts)
				{
					if (part.isEmpty())
					{
						codes.push_back(0);
						continue;
					}
					bool      ok    = false;
					const int value = part.toInt(&ok);
					if (ok)
						codes.push_back(value);
				}
				applySgr(codes);
			}
			continue;
		}
		plainText.append(ch);
	}

	appendPlain(plainText);
	if (!lineText.isEmpty() || !lineSpans.isEmpty())
		emitLine(false);
}

int WorldRuntime::sendCommand(const QString &text, bool echo, bool queue, bool log, bool history,
                              bool immediate) const
{
	if (m_connectPhase != eConnectConnectedToMud)
		return eWorldClosed;
	if (!m_commandProcessor)
		return eWorldClosed;
	if (m_commandProcessor->pluginProcessingSent())
		return eItemInUse;

	if (immediate)
		m_commandProcessor->sendImmediateText(text, echo, log, history);
	else
		m_commandProcessor->sendRawText(text, echo, queue, log, history);
	return eOK;
}

void WorldRuntime::logInputCommand(const QString &text) const
{
	if (!m_commandProcessor)
		return;
	m_commandProcessor->logInputCommand(text);
}

bool WorldRuntime::executeAcceleratorCommand(int commandId, const QString &keyLabel)
{
	if (!m_commandProcessor)
		return false;
	const AcceleratorEntry *entry = acceleratorEntryForCommand(commandId);
	if (!entry || entry->text.isEmpty())
		return false;

	if (entry->sendTo == eSendToExecute)
		return m_commandProcessor->executeCommand(entry->text) == eOK;

	Plugin const *plugin = nullptr;
	if (!entry->pluginId.isEmpty())
	{
		const int index = findPluginIndex(m_plugins, entry->pluginId);
		if (index < 0)
			return false;
		plugin = &m_plugins[index];
		if (!hasValidPluginId(*plugin) || !plugin->enabled || plugin->installPending)
			return false;
	}

	const QString description =
	    keyLabel.isEmpty() ? QStringLiteral("Accelerator") : QStringLiteral("Accelerator: %1").arg(keyLabel);
	m_commandProcessor->sendToFromAccelerator(entry->sendTo, entry->text, description, plugin);
	return true;
}

void WorldRuntime::setNoCommandEcho(bool enabled) const
{
	if (m_commandProcessor)
		m_commandProcessor->setNoEcho(enabled);
}

bool WorldRuntime::noCommandEcho() const
{
	return m_commandProcessor ? m_commandProcessor->noEcho() : false;
}

void WorldRuntime::syncChatAcceptCallsWithPreferences()
{
	const bool shouldAccept =
	    isEnabledFlag(m_worldAttributes.value(QStringLiteral("accept_chat_connections")));
	if (!shouldAccept)
	{
		if (m_chatServer)
			chatStopAcceptingCalls();
		return;
	}

	const int desiredPort = chatIncomingPort();

	if (m_chatServer)
	{
		const int currentPort = m_chatServer->serverPort();
		if (currentPort == desiredPort)
			return;

		chatStopAcceptingCalls();
		setWorldAttribute(QStringLiteral("accept_chat_connections"), QStringLiteral("1"));
	}

	chatAcceptCalls(static_cast<short>(desiredPort));
}

void WorldRuntime::setCommandProcessor(WorldCommandProcessor *processor)
{
	m_commandProcessor = processor;
}

void WorldRuntime::refreshCommandProcessorOptions()
{
	if (!m_commandProcessor)
		return;
	m_commandProcessor->setRuntime(this);
}

QStringList WorldRuntime::queuedCommands() const
{
	if (!m_commandProcessor)
		return {};
	return m_commandProcessor->queuedCommands();
}

int WorldRuntime::allocateAcceleratorCommand()
{
	if (m_nextAcceleratorCommand >= (kAcceleratorFirstCommand + kAcceleratorCount))
		return -1;
	return m_nextAcceleratorCommand++;
}

void WorldRuntime::registerAccelerator(qint64 key, int commandId, const AcceleratorEntry &entry)
{
	m_acceleratorKeyToCommand[key]         = commandId;
	m_commandToAcceleratorEntry[commandId] = entry;
}

void WorldRuntime::removeAccelerator(qint64 key)
{
	const auto it = m_acceleratorKeyToCommand.find(key);
	if (it == m_acceleratorKeyToCommand.end())
		return;
	m_commandToAcceleratorEntry.remove(it.value());
	m_acceleratorKeyToCommand.erase(it);
}

int WorldRuntime::acceleratorCommandForKey(qint64 key) const
{
	const auto it = m_acceleratorKeyToCommand.constFind(key);
	return it == m_acceleratorKeyToCommand.constEnd() ? -1 : it.value();
}

bool WorldRuntime::hasAccelerator(int commandId) const
{
	return m_commandToAcceleratorEntry.contains(commandId);
}

const WorldRuntime::AcceleratorEntry *WorldRuntime::acceleratorEntryForCommand(int commandId) const
{
	const auto it = m_commandToAcceleratorEntry.constFind(commandId);
	if (it == m_commandToAcceleratorEntry.constEnd())
		return nullptr;
	return &it.value();
}

QVector<qint64> WorldRuntime::acceleratorKeys() const
{
	QVector<qint64> keys;
	keys.reserve(m_acceleratorKeyToCommand.size());
	for (auto it = m_acceleratorKeyToCommand.constBegin(); it != m_acceleratorKeyToCommand.constEnd(); ++it)
		keys.append(it.key());
	return keys;
}

QString WorldRuntime::acceleratorCommandText(int commandId) const
{
	if (const AcceleratorEntry *entry = acceleratorEntryForCommand(commandId))
		return entry->text;
	return {};
}

int WorldRuntime::acceleratorSendTarget(int commandId) const
{
	if (const AcceleratorEntry *entry = acceleratorEntryForCommand(commandId))
		return entry->sendTo;
	return 0;
}

QString WorldRuntime::acceleratorPluginId(int commandId) const
{
	if (const AcceleratorEntry *entry = acceleratorEntryForCommand(commandId))
		return entry->pluginId;
	return {};
}

bool WorldRuntime::firePluginCommand(const QString &text)
{
	return callPluginCallbacksStopOnFalse(QStringLiteral("OnPluginCommand"), text);
}

void WorldRuntime::firePluginCommandChanged()
{
	callPluginCallbacksNoArgs(QStringLiteral("OnPluginCommandChanged"));
}

bool WorldRuntime::firePluginSend(const QString &text)
{
	return callPluginCallbacksStopOnFalse(QStringLiteral("OnPluginSend"), text);
}

void WorldRuntime::firePluginCommandEntered(QString &text)
{
	callPluginCallbacksTransformString(QStringLiteral("OnPluginCommandEntered"), text);
}

void WorldRuntime::firePluginTabComplete(QString &text)
{
	callPluginCallbacksTransformString(QStringLiteral("OnPluginTabComplete"), text);
}

bool WorldRuntime::firePluginLineReceived(const QString &text)
{
	return callPluginCallbacksStopOnFalse(QStringLiteral("OnPluginLineReceived"), text);
}

bool WorldRuntime::firePluginPlaySound(const QString &sound)
{
	return callPluginCallbacksStopOnTrueWithString(QStringLiteral("OnPluginPlaySound"), sound);
}

bool WorldRuntime::firePluginTrace(const QString &message)
{
	return callPluginCallbacksStopOnTrueWithString(QStringLiteral("OnPluginTrace"), message);
}

void WorldRuntime::firePluginScreendraw(int type, int log, const QString &text)
{
	if (m_inScreendrawCallback)
		return;
	m_inScreendrawCallback = true;
	callPluginCallbacksWithTwoNumbersAndString(QStringLiteral("OnPluginScreendraw"), type, log, text);
	m_inScreendrawCallback = false;
}

void WorldRuntime::firePluginTick()
{
	callPluginCallbacksNoArgs(QStringLiteral("OnPluginTick"));
}

void WorldRuntime::firePluginSent(const QString &text)
{
	callPluginCallbacks(QStringLiteral("OnPluginSent"), text);
}

void WorldRuntime::firePluginPartialLine(const QString &text)
{
	callPluginCallbacks(QStringLiteral("OnPluginPartialLine"), text);
}

void WorldRuntime::notifyMiniWindowMouseMoved(int x, int y, const QString &windowName)
{
	callPluginCallbacksWithTwoNumbersAndString(QStringLiteral("OnPluginMouseMoved"), x, y, windowName);
}

void WorldRuntime::notifyWorldOutputResized()
{
	updateTelnetWindowSizeForNaws();
	if (m_suppressWorldOutputResizedCallbacks > 0)
		return;
	callPluginCallbacksNoArgs(QStringLiteral("OnPluginWorldOutputResized"));
}

void WorldRuntime::refreshNawsWindowSize()
{
	updateTelnetWindowSizeForNaws();
}

void WorldRuntime::updateTelnetWindowSizeForNaws()
{
	const bool textRectangleCompatActive = m_textRectangle.left != 0 || m_textRectangle.top != 0 ||
	                                       m_textRectangle.right != 0 || m_textRectangle.bottom != 0;
	const bool nawsEnabled =
	    isEnabledFlag(m_worldAttributes.value(QStringLiteral("naws"))) && !textRectangleCompatActive;
	m_telnet.setNawsEnabled(nawsEnabled);
	const bool wrapEnabled     = isEnabledFlag(m_worldAttributes.value(QStringLiteral("wrap")));
	const int  worldWrapColumn = m_worldAttributes.value(QStringLiteral("wrap_column")).toInt();
	const int  minimumColumns  = wrapEnabled ? worldWrapColumn : 0;

	int        columns = 0;
	int        rows    = 0;

	if (m_view)
	{
		const QRect textRect     = textRectangleCompatActive ? m_view->outputTextRectangleUnreserved()
		                                                     : m_view->outputTextRectangle();
		int         widthPixels  = textRect.width();
		const int   heightPixels = textRect.height();
		if (widthPixels > 0 && heightPixels > 0)
		{
			const QFont        font = m_view->outputFont();
			const QFontMetrics metrics(font);
			const int          charWidth  = qMax(1, metrics.horizontalAdvance(QLatin1Char('x')));
			const int          charHeight = qMax(1, metrics.lineSpacing());
			columns                       = widthPixels / charWidth;
			rows                          = heightPixels / charHeight;
		}
	}

	if (columns <= 0 || rows <= 0)
	{
		// When geometry is not measurable yet (e.g. hidden/inactive view), keep
		// NAWS values protocol-valid using configured wrap or last known telnet size.
		columns = m_telnet.windowColumns();
		if (minimumColumns > 0)
			columns = qMax(columns, minimumColumns);
		rows = m_telnet.windowRows() > 0 ? m_telnet.windowRows() : 24;
	}
	else if (minimumColumns > 0)
		columns = qMax(columns, minimumColumns);

	if (columns > 0 && rows > 0)
		m_telnet.setWindowSize(columns, rows);

	if (m_socket && m_socket->isConnected())
	{
		const QByteArray outbound = m_telnet.takeOutboundData();
		if (!outbound.isEmpty())
			sendToWorld(outbound);
	}
}

void WorldRuntime::notifyDrawOutputWindow(int firstLine, int offset)
{
	if (m_inDrawOutputWindowCallback)
		return;
	++m_outputWindowRedrawCount;
	m_inDrawOutputWindowCallback = true;
	callPluginCallbacksWithTwoNumbersAndString(QStringLiteral("OnPluginDrawOutputWindow"), firstLine, offset,
	                                           QString());
	m_inDrawOutputWindowCallback = false;
}

int WorldRuntime::outputWindowRedrawCount() const
{
	return m_outputWindowRedrawCount;
}

bool WorldRuntime::suppressScriptErrorOutputToWorld() const
{
	return m_inDrawOutputWindowCallback || m_inScreendrawCallback;
}

bool WorldRuntime::forceScriptErrorOutputToWorld() const
{
	return m_forceScriptErrorOutputDepth > 0;
}

void WorldRuntime::notifyOutputSelectionChanged()
{
	callPluginCallbacksNoArgs(QStringLiteral("OnPluginSelectionChanged"));
}

static void setDefault(QMap<QString, QString> &attributes, const QString &key, const QString &value)
{
	if (!attributes.contains(key))
		attributes.insert(key, value);
}

static void applyTriggerDefaults(WorldRuntime::Trigger &trigger)
{
	QMap<QString, QString> &a = trigger.attributes;
	setDefault(a, QStringLiteral("enabled"), QStringLiteral("1"));
	setDefault(a, QStringLiteral("ignore_case"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("omit_from_log"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("omit_from_output"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("keep_evaluating"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("expand_variables"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("send_to"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("regexp"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("repeat"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("sequence"), QStringLiteral("100"));
	setDefault(a, QStringLiteral("sound_if_inactive"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("lowercase_wildcard"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("temporary"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("user"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("one_shot"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("lines_to_match"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("colour_change_type"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("match_back_colour"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("match_text_colour"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("match_bold"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("match_italic"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("match_underline"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("match_inverse"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("bold"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("italic"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("underline"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("inverse"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("make_bold"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("make_italic"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("make_underline"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("custom_colour"), QStringLiteral("0"));
}

static void applyAliasDefaults(WorldRuntime::Alias &alias)
{
	QMap<QString, QString> &a = alias.attributes;
	setDefault(a, QStringLiteral("enabled"), QStringLiteral("1"));
	setDefault(a, QStringLiteral("ignore_case"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("expand_variables"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("omit_from_log"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("regexp"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("omit_from_output"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("sequence"), QStringLiteral("100"));
	setDefault(a, QStringLiteral("keep_evaluating"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("menu"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("send_to"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("echo_alias"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("omit_from_command_history"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("user"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("one_shot"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("temporary"), QStringLiteral("0"));
}

static void applyTimerDefaults(WorldRuntime::Timer &timer)
{
	QMap<QString, QString> &a = timer.attributes;
	setDefault(a, QStringLiteral("enabled"), QStringLiteral("1"));
	setDefault(a, QStringLiteral("one_shot"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("active_closed"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("send_to"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("omit_from_output"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("omit_from_log"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("at_time"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("hour"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("minute"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("second"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("offset_hour"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("offset_minute"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("offset_second"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("user"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("temporary"), QStringLiteral("0"));
}

static void applyVariableDefaults(WorldRuntime::Variable &variable)
{
	QMap<QString, QString> &a = variable.attributes;
	setDefault(a, QStringLiteral("trim"), QStringLiteral("0"));
}

static void applyPrintingDefaults(WorldRuntime::PrintingStyle &style)
{
	QMap<QString, QString> &a = style.attributes;
	setDefault(a, QStringLiteral("bold"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("italic"), QStringLiteral("0"));
	setDefault(a, QStringLiteral("underline"), QStringLiteral("0"));
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

static QList<WorldRuntime::Macro> normalizeMacroSlots(const QList<WorldRuntime::Macro> &macros)
{
	QMap<QString, WorldRuntime::Macro> macroByName;
	for (const auto &macro : macros)
	{
		const QString name = macro.attributes.value(QStringLiteral("name")).trimmed();
		if (name.isEmpty())
			continue;
		const QString key = name.toLower();
		if (macroByName.contains(key))
			continue;
		WorldRuntime::Macro normalized = macro;
		normalized.attributes.insert(QStringLiteral("name"), name);
		macroByName.insert(key, normalized);
	}

	const QStringList          macroNames = macroDescriptionList();
	QList<WorldRuntime::Macro> normalizedMacros;
	normalizedMacros.reserve(macroNames.size());
	for (int i = 0; i < macroNames.size(); ++i)
	{
		const QString      &canonicalName = macroNames.at(i);
		const QString       key           = canonicalName.toLower();
		const auto          it            = macroByName.constFind(key);

		WorldRuntime::Macro macro;
		if (it != macroByName.cend())
			macro = it.value();
		else
			macro.attributes.insert(QStringLiteral("type"), QStringLiteral("send_now"));

		macro.attributes.insert(QStringLiteral("name"), canonicalName);
		macro.attributes.insert(QStringLiteral("index"), QString::number(i));
		normalizedMacros.push_back(macro);
	}

	return normalizedMacros;
}

static QStringList keypadNameList()
{
	return {QStringLiteral("0"),      QStringLiteral("1"),      QStringLiteral("2"),
	        QStringLiteral("3"),      QStringLiteral("4"),      QStringLiteral("5"),
	        QStringLiteral("6"),      QStringLiteral("7"),      QStringLiteral("8"),
	        QStringLiteral("9"),      QStringLiteral("."),      QStringLiteral("/"),
	        QStringLiteral("*"),      QStringLiteral("-"),      QStringLiteral("+"),
	        QStringLiteral("Ctrl+0"), QStringLiteral("Ctrl+1"), QStringLiteral("Ctrl+2"),
	        QStringLiteral("Ctrl+3"), QStringLiteral("Ctrl+4"), QStringLiteral("Ctrl+5"),
	        QStringLiteral("Ctrl+6"), QStringLiteral("Ctrl+7"), QStringLiteral("Ctrl+8"),
	        QStringLiteral("Ctrl+9"), QStringLiteral("Ctrl+."), QStringLiteral("Ctrl+/"),
	        QStringLiteral("Ctrl+*"), QStringLiteral("Ctrl+-"), QStringLiteral("Ctrl++")};
}

void WorldRuntime::applyFromDocument(const WorldDocument &doc)
{
	m_loadingDocument        = true;
	const QString workingDir = resolveWorkingDir(m_startupDirectory);
	m_worldAttributes        = doc.worldAttributes();
	for (auto it = m_worldAttributes.begin(); it != m_worldAttributes.end(); ++it)
	{
		if (isCanonicalizablePathAttributeName(it.key()) &&
		    it.value().trimmed() != QString::fromLatin1(kNoSoundLiteral))
			it.value() = canonicalizePathForRuntime(it.value(), workingDir);
		else if (isLikelyPathAttributeName(it.key()))
			it.value() = normalizePathForRuntime(it.value());
		if (it.key() == QStringLiteral("auto_log_file_name"))
			it.value() = normalizeAutoLogFileNameValue(it.value());
	}
	m_worldMultilineAttributes = doc.worldMultilineAttributes();

	// Legacy behavior: world files store password base64-encoded when password_base64 is enabled.
	// Decode on load so runtime attributes always hold clear text for connect/send paths.
	if (isEnabledFlag(m_worldAttributes.value(QStringLiteral("password_base64"))))
	{
		const QString encodedPassword = m_worldAttributes.value(QStringLiteral("password"));
		if (!encodedPassword.isEmpty())
		{
			const QByteArray decoded =
			    QByteArray::fromBase64(encodedPassword.toLatin1(), QByteArray::Base64Encoding);
			if (!decoded.isEmpty())
				m_worldAttributes.insert(QStringLiteral("password"), QString::fromUtf8(decoded));
		}
	}

	applyDefaultWorldOptions();
	m_traceEnabled      = isEnabledFlag(m_worldAttributes.value(QStringLiteral("trace")));
	m_worldFileModified = false;
	m_variablesChanged  = false;
	m_scriptFileChanged = false;
	m_worldFileVersion  = doc.worldFileVersion();
	m_qmudVersion       = doc.qmudVersion();
	m_dateSaved         = doc.dateSaved();
	m_comments          = doc.comments();

	if (m_worldMultilineAttributes.contains(QStringLiteral("script")))
		setLuaScriptText(m_worldMultilineAttributes.value(QStringLiteral("script")));

	m_triggerCount       = safeQSizeToInt(doc.triggers().size());
	m_aliasCount         = safeQSizeToInt(doc.aliases().size());
	m_timerCount         = safeQSizeToInt(doc.timers().size());
	m_variableCount      = safeQSizeToInt(doc.variables().size());
	m_colourCount        = safeQSizeToInt(doc.colours().size());
	m_keypadCount        = safeQSizeToInt(doc.keypadEntries().size());
	m_printingStyleCount = safeQSizeToInt(doc.printingStyles().size());
	m_pluginCount        = safeQSizeToInt(doc.plugins().size());
	m_includeCount       = safeQSizeToInt(doc.includes().size());
	m_scriptCount        = safeQSizeToInt(doc.scripts().size());
	m_connectPhase       = eConnectNotConnected;
	m_connectViaProxy    = false;
	m_proxyAddressString.clear();
	m_proxyAddressV4 = 0;
	m_connectTime    = QDateTime();
	m_recentLines.clear();
	m_stopTriggerEvaluation = KeepEvaluating;

	m_triggers.clear();
	for (const auto &t : doc.triggers())
	{
		Trigger rt;
		rt.attributes = t.attributes;
		rt.children   = t.children;
		rt.included   = t.included;
		applyTriggerDefaults(rt);
		m_triggers.push_back(rt);
	}
	m_aliases.clear();
	for (const auto &a : doc.aliases())
	{
		Alias ra;
		ra.attributes = a.attributes;
		ra.children   = a.children;
		ra.included   = a.included;
		applyAliasDefaults(ra);
		m_aliases.push_back(ra);
	}
	m_timers.clear();
	for (const auto &t : doc.timers())
	{
		Timer rt;
		rt.attributes = t.attributes;
		rt.children   = t.children;
		rt.included   = t.included;
		applyTimerDefaults(rt);
		resetTimerFields(rt);
		m_timers.push_back(rt);
	}
	QList<Macro> parsedMacros;
	parsedMacros.reserve(doc.macros().size());
	for (const auto &m : doc.macros())
	{
		Macro rm;
		rm.attributes = m.attributes;
		rm.children   = m.children;
		parsedMacros.push_back(rm);
	}
	m_macros     = normalizeMacroSlots(parsedMacros);
	m_macroCount = safeQSizeToInt(m_macros.size());
	m_variables.clear();
	for (const auto &v : doc.variables())
	{
		Variable rv;
		rv.attributes = v.attributes;
		rv.content    = v.content;
		applyVariableDefaults(rv);
		m_variables.push_back(rv);
	}
	m_colours.clear();
	for (const auto &c : doc.colours())
	{
		Colour rc;
		rc.group      = c.group;
		rc.attributes = c.attributes;
		bool      ok  = false;
		const int seq = rc.attributes.value(QStringLiteral("seq")).toInt(&ok);
		if (ok)
			rc.attributes.insert(QStringLiteral("seq_index"), QString::number(seq - 1));
		m_colours.push_back(rc);
	}
	m_keypadEntries.clear();
	const QStringList keypadNames = keypadNameList();
	for (const auto &k : doc.keypadEntries())
	{
		Keypad rk;
		rk.attributes         = k.attributes;
		rk.content            = k.content;
		const QString keyName = rk.attributes.value(QStringLiteral("name")).trimmed();
		const int     index   = boundedQSizeToInt(keypadNames.indexOf(keyName));
		if (index >= 0)
			rk.attributes.insert(QStringLiteral("index"), QString::number(index));
		m_keypadEntries.push_back(rk);
	}
	m_printingStyles.clear();
	for (const auto &p : doc.printingStyles())
	{
		PrintingStyle rp;
		rp.group      = p.group;
		rp.attributes = p.attributes;
		bool      ok  = false;
		const int seq = rp.attributes.value(QStringLiteral("seq")).toInt(&ok);
		if (ok)
			rp.attributes.insert(QStringLiteral("seq_index"), QString::number(seq - 1));
		applyPrintingDefaults(rp);
		m_printingStyles.push_back(rp);
	}
	m_plugins.clear();
	resetObservedPluginCallbackTracking(m_observedPluginCallbacks, m_observedPluginCallbackQueryGeneration,
	                                    m_observedPluginCallbackGeneration);
	m_pluginCallbackPresenceCounts.clear();
	m_pluginCallbackRecipientIndices.clear();
	m_pluginCallbackPresencePluginCount = -1;
	invalidatePluginCallbackPresenceCache();
	for (const auto &p : doc.plugins())
	{
		Plugin rp;
		rp.attributes             = p.attributes;
		rp.description            = p.description;
		rp.script                 = p.script;
		rp.source                 = rp.attributes.value(QStringLiteral("source"));
		rp.directory              = rp.attributes.value(QStringLiteral("directory"));
		const QString pluginId    = rp.attributes.value(QStringLiteral("id")).trimmed().toLower();
		rp.sequence               = pluginSequenceFromAttributes(rp.attributes);
		rp.version                = rp.attributes.value(QStringLiteral("version")).toDouble();
		rp.requiredVersion        = rp.attributes.value(QStringLiteral("requires")).toDouble();
		rp.dateWritten            = parsePluginDate(rp.attributes.value(QStringLiteral("date_written")));
		rp.dateModified           = parsePluginDate(rp.attributes.value(QStringLiteral("date_modified")));
		rp.dateInstalled          = QDateTime::currentDateTime();
		rp.global                 = isEnabledFlag(rp.attributes.value(QStringLiteral("global")));
		rp.saveState              = isEnabledFlag(rp.attributes.value(QStringLiteral("save_state")));
		const QString enabledFlag = rp.attributes.value(QStringLiteral("enabled"));
		const bool    requestedEnabled = enabledFlag.isEmpty() ? true : isEnabledFlag(enabledFlag);
		rp.enabled                     = requestedEnabled;
		for (const auto &t : p.triggers)
		{
			Trigger rt;
			rt.attributes                 = t.attributes;
			rt.children                   = t.children;
			rt.included                   = t.included;
			const bool hasExplicitEnabled = rt.attributes.contains(QStringLiteral("enabled"));
			applyTriggerDefaults(rt);
			if (!hasExplicitEnabled)
				rt.attributes.insert(QStringLiteral("enabled"), QStringLiteral("0"));
			rp.triggers.push_back(rt);
		}
		for (const auto &a : p.aliases)
		{
			Alias ra;
			ra.attributes                 = a.attributes;
			ra.children                   = a.children;
			ra.included                   = a.included;
			const bool hasExplicitEnabled = ra.attributes.contains(QStringLiteral("enabled"));
			applyAliasDefaults(ra);
			if (!hasExplicitEnabled)
				ra.attributes.insert(QStringLiteral("enabled"), QStringLiteral("0"));
			rp.aliases.push_back(ra);
		}
		for (const auto &t : p.timers)
		{
			Timer rt;
			rt.attributes                 = t.attributes;
			rt.children                   = t.children;
			rt.included                   = t.included;
			const bool hasExplicitEnabled = rt.attributes.contains(QStringLiteral("enabled"));
			applyTimerDefaults(rt);
			if (!hasExplicitEnabled)
				rt.attributes.insert(QStringLiteral("enabled"), QStringLiteral("0"));
			resetTimerFields(rt);
			rp.timers.push_back(rt);
		}
		for (const auto &v : p.variables)
		{
			const QString name = v.attributes.value(QStringLiteral("name"));
			if (!name.isEmpty())
				rp.variables.insert(name, v.content);
		}
		// Preserve load-path behavior: world plugins pick up persisted state variables on world load.
		if (!m_stateFilesDirectory.isEmpty())
		{
			const QString worldId = m_worldAttributes.value(QStringLiteral("id")).trimmed();
			if (!worldId.isEmpty() && !pluginId.isEmpty())
			{
				QString base = m_stateFilesDirectory;
				if (!base.endsWith('/') && !base.endsWith('\\'))
					base += '/';
				const QString   stateFile = base + worldId + "-" + pluginId + "-state.xml";
				QFileInfo const stateInfo(stateFile);
				if (stateInfo.exists() && stateInfo.size() > 0)
				{
					WorldDocument stateDoc;
					stateDoc.setLoadMask(WorldDocument::XML_VARIABLES | WorldDocument::XML_NO_PLUGINS);
					if (stateDoc.loadFromFile(stateInfo.absoluteFilePath()))
					{
						for (const auto &v : stateDoc.variables())
						{
							const QString name = v.attributes.value(QStringLiteral("name"));
							if (!name.isEmpty())
								rp.variables.insert(name, v.content);
						}
					}
					else
					{
						qWarning() << "Plugin state load failed:" << stateDoc.errorString();
					}
				}
			}
		}
		const QString language = rp.attributes.value(QStringLiteral("language"));
		if (language.compare(QStringLiteral("lua"), Qt::CaseInsensitive) == 0)
		{
			rp.lua = QSharedPointer<LuaCallbackEngine>::create();
			rp.lua->setWorldRuntime(this);
			rp.lua->setScriptText(rp.script);
			rp.lua->setPluginInfo(rp.attributes.value(QStringLiteral("id")),
			                      rp.attributes.value(QStringLiteral("name")));
			bindPluginCallbackObserver(rp);
		}
		if (!requestedEnabled && rp.lua && hasValidPluginId(rp))
		{
			rp.enabled             = true;
			rp.disableAfterInstall = true;
		}
		m_plugins.push_back(rp);
	}
	sortPluginsBySequence();
	m_pluginCount = safeQSizeToInt(m_plugins.size());
	invalidatePluginCallbackPresenceCache();
	for (auto &plugin : m_plugins)
	{
		queuePluginInstall(plugin);
	}
	m_includes.clear();
	QHash<QString, bool> uniqueNonPluginIncludes;
	for (const auto &i : doc.includes())
	{
		Include ri;
		ri.attributes = i.attributes;
		if (!isEnabledFlag(ri.attributes.value(QStringLiteral("plugin"))))
		{
			const QString includeName =
			    normalizePathForStorage(ri.attributes.value(QStringLiteral("name")).trimmed());
			if (includeName.isEmpty())
				continue;
			const QString includeKey = includeName.toLower();
			if (uniqueNonPluginIncludes.contains(includeKey))
				continue;
			uniqueNonPluginIncludes.insert(includeKey, true);
			ri.attributes.insert(QStringLiteral("name"), includeName);
		}
		m_includes.push_back(ri);
	}
	m_includeCount = safeQSizeToInt(m_includes.size());
	m_scripts.clear();
	for (const auto &s : doc.scripts())
	{
		Script rs;
		rs.content = s.content;
		m_scripts.push_back(rs);
	}
	const QString scriptFile = m_worldAttributes.value(QStringLiteral("script_filename"));
	if (m_scriptWatcher)
	{
		const QStringList watched = m_scriptWatcher->files();
		if (!watched.isEmpty())
			m_scriptWatcher->removePaths(watched);
		if (!scriptFile.isEmpty())
		{
			const QString scriptWorkingDir = resolveWorkingDir(m_startupDirectory);
			const QString path             = makeAbsolutePath(scriptFile, scriptWorkingDir);
			if (!path.isEmpty())
				m_scriptWatcher->addPath(path);
		}
	}
	m_loadingDocument = false;
	refreshCommandProcessorOptions();
	installPendingPlugins();
}

const QMap<QString, QString> &WorldRuntime::worldAttributes() const
{
	return m_worldAttributes;
}

void WorldRuntime::setWorldAttribute(const QString &key, const QString &value)
{
	QString normalizedValue = value;
	if (isLikelyPathAttributeName(key))
		normalizedValue = normalizePathForRuntime(normalizedValue);
	if (key == QStringLiteral("auto_log_file_name"))
		normalizedValue = normalizeAutoLogFileNameValue(normalizedValue);
	const auto existing = m_worldAttributes.constFind(key);
	if (existing != m_worldAttributes.constEnd() && existing.value() == normalizedValue)
	{
		return;
	}
	m_worldAttributes.insert(key, normalizedValue);
	if (key == QStringLiteral("max_output_lines"))
		enforceOutputLineLimit();
	if (key == QStringLiteral("naws"))
	{
		updateTelnetWindowSizeForNaws();
		if (m_view)
			m_view->applyRuntimeSettingsWithoutOutputRebuild();
	}
	if (!m_loadingDocument && worldAttributeAffectsCommandProcessor(key))
		refreshCommandProcessorOptions();
	if (!m_loadingDocument)
		m_worldFileModified = true;
	emit worldAttributeChanged(key);
	if (key == QStringLiteral("script_filename") && m_scriptWatcher)
	{
		const QStringList watched = m_scriptWatcher->files();
		if (!watched.isEmpty())
			m_scriptWatcher->removePaths(watched);
		if (!normalizedValue.isEmpty())
		{
			const QString workingDir = resolveWorkingDir(m_startupDirectory);
			const QString path       = makeAbsolutePath(normalizedValue, workingDir);
			if (!path.isEmpty())
				m_scriptWatcher->addPath(path);
		}
	}
}

const QMap<QString, QString> &WorldRuntime::worldMultilineAttributes() const
{
	return m_worldMultilineAttributes;
}

void WorldRuntime::setWorldMultilineAttribute(const QString &key, const QString &value)
{
	if (!upsertWorldMultilineAttributeIfChanged(m_worldMultilineAttributes, key, value))
		return;
	if (!m_loadingDocument)
		m_worldFileModified = true;
	if (key == QStringLiteral("script"))
		setLuaScriptText(value);
}

int WorldRuntime::worldFileVersion() const
{
	return m_worldFileVersion;
}

QString WorldRuntime::qmudVersion() const
{
	return m_qmudVersion;
}

QDateTime WorldRuntime::dateSaved() const
{
	return m_dateSaved;
}

int WorldRuntime::triggerCount() const
{
	return m_triggerCount;
}

int WorldRuntime::aliasCount() const
{
	return m_aliasCount;
}

int WorldRuntime::timerCount() const
{
	return m_timerCount;
}

int WorldRuntime::macroCount() const
{
	return m_macroCount;
}

int WorldRuntime::variableCount() const
{
	return m_variableCount;
}

int WorldRuntime::colourCount() const
{
	return m_colourCount;
}

int WorldRuntime::keypadCount() const
{
	return m_keypadCount;
}

int WorldRuntime::printingStyleCount() const
{
	return m_printingStyleCount;
}

int WorldRuntime::pluginCount() const
{
	return m_pluginCount;
}

int WorldRuntime::includeCount() const
{
	return m_includeCount;
}

int WorldRuntime::scriptCount() const
{
	return m_scriptCount;
}

const QList<WorldRuntime::Trigger> &WorldRuntime::triggers() const
{
	return m_triggers;
}

QList<WorldRuntime::Trigger> &WorldRuntime::triggersMutable()
{
	return m_triggers;
}

void WorldRuntime::setTriggers(const QList<Trigger> &triggers)
{
	m_triggers.clear();
	for (const auto &t : triggers)
	{
		Trigger rt = t;
		applyTriggerDefaults(rt);
		m_triggers.push_back(rt);
	}
	m_triggerCount      = safeQSizeToInt(m_triggers.size());
	m_worldFileModified = true;
}

void WorldRuntime::markTriggersChanged()
{
	m_triggerCount      = safeQSizeToInt(m_triggers.size());
	m_worldFileModified = true;
}

const QList<WorldRuntime::Alias> &WorldRuntime::aliases() const
{
	return m_aliases;
}

QList<WorldRuntime::Alias> &WorldRuntime::aliasesMutable()
{
	return m_aliases;
}

void WorldRuntime::setAliases(const QList<Alias> &aliases)
{
	m_aliases.clear();
	for (const auto &a : aliases)
	{
		Alias ra = a;
		applyAliasDefaults(ra);
		m_aliases.push_back(ra);
	}
	m_aliasCount        = safeQSizeToInt(m_aliases.size());
	m_worldFileModified = true;
}

void WorldRuntime::markAliasesChanged()
{
	m_aliasCount        = safeQSizeToInt(m_aliases.size());
	m_worldFileModified = true;
}

const QList<WorldRuntime::Timer> &WorldRuntime::timers() const
{
	return m_timers;
}

QList<WorldRuntime::Timer> &WorldRuntime::timersMutable()
{
	return m_timers;
}

void WorldRuntime::setTimers(const QList<Timer> &timers)
{
	m_timers.clear();
	for (const auto &t : timers)
	{
		Timer rt = t;
		applyTimerDefaults(rt);
		m_timers.push_back(rt);
	}
	m_timerCount = safeQSizeToInt(m_timers.size());
	noteTimerStructureMutation();
	m_worldFileModified = true;
}

void WorldRuntime::markTimersChanged()
{
	m_timerCount        = safeQSizeToInt(m_timers.size());
	m_worldFileModified = true;
}

quint64 WorldRuntime::timerStructureMutationSerial() const
{
	return m_timerStructureMutationSerial;
}

void WorldRuntime::noteTimerStructureMutation()
{
	++m_timerStructureMutationSerial;
}

const QList<WorldRuntime::Macro> &WorldRuntime::macros() const
{
	return m_macros;
}

void WorldRuntime::setMacros(const QList<Macro> &macros)
{
	m_macros            = normalizeMacroSlots(macros);
	m_macroCount        = safeQSizeToInt(m_macros.size());
	m_worldFileModified = true;
}

const QList<WorldRuntime::Variable> &WorldRuntime::variables() const
{
	return m_variables;
}

bool WorldRuntime::findVariable(const QString &name, QString &value) const
{
	for (const auto &var : m_variables)
	{
		const QString varName = var.attributes.value(QStringLiteral("name"));
		if (varName.compare(name, Qt::CaseInsensitive) == 0)
		{
			value = var.content;
			return true;
		}
	}
	return false;
}

void WorldRuntime::setVariable(const QString &name, const QString &value)
{
	for (auto &var : m_variables)
	{
		const QString varName = var.attributes.value(QStringLiteral("name"));
		if (varName.compare(name, Qt::CaseInsensitive) == 0)
		{
			var.content        = value;
			m_variablesChanged = true;
			return;
		}
	}

	Variable variable;
	variable.attributes.insert(QStringLiteral("name"), name);
	variable.content = value;
	m_variables.push_back(variable);
	m_variablesChanged = true;
}

void WorldRuntime::setVariables(const QList<Variable> &variables)
{
	m_variables = variables;
	for (auto &var : m_variables)
		applyVariableDefaults(var);
	m_variableCount    = safeQSizeToInt(m_variables.size());
	m_variablesChanged = true;
}

int WorldRuntime::arrayCreate(const QString &name)
{
	if (m_arrays.contains(name))
		return eArrayAlreadyExists;
	ArrayEntry const entry;
	m_arrays.insert(name, entry);
	return eOK;
}

int WorldRuntime::arrayDelete(const QString &name)
{
	const auto it = m_arrays.find(name);
	if (it == m_arrays.end())
		return eArrayDoesNotExist;
	m_arrays.erase(it);
	return eOK;
}

int WorldRuntime::arrayClear(const QString &name)
{
	const auto it = m_arrays.find(name);
	if (it == m_arrays.end())
		return eArrayDoesNotExist;
	it->values.clear();
	return eOK;
}

bool WorldRuntime::arrayExists(const QString &name) const
{
	return m_arrays.contains(name);
}

int WorldRuntime::arrayCount() const
{
	return safeQSizeToInt(m_arrays.size());
}

int WorldRuntime::arraySize(const QString &name) const
{
	const auto it = m_arrays.find(name);
	if (it == m_arrays.end())
		return 0;
	return safeQSizeToInt(it->values.size());
}

bool WorldRuntime::arrayKeyExists(const QString &name, const QString &key) const
{
	const auto it = m_arrays.find(name);
	if (it == m_arrays.end())
		return false;
	return it->values.contains(key);
}

bool WorldRuntime::arrayGet(const QString &name, const QString &key, QString &value) const
{
	const auto it = m_arrays.find(name);
	if (it == m_arrays.end())
		return false;
	const auto kv = it->values.find(key);
	if (kv == it->values.end())
		return false;
	value = kv.value();
	return true;
}

int WorldRuntime::arraySet(const QString &name, const QString &key, const QString &value)
{
	const auto it = m_arrays.find(name);
	if (it == m_arrays.end())
		return eArrayDoesNotExist;
	if (it->values.contains(key))
	{
		it->values.insert(key, value);
		return eSetReplacingExistingValue;
	}
	it->values.insert(key, value);
	return eOK;
}

int WorldRuntime::arrayDeleteKey(const QString &name, const QString &key)
{
	const auto it = m_arrays.find(name);
	if (it == m_arrays.end())
		return eArrayDoesNotExist;
	const auto kv = it->values.find(key);
	if (kv == it->values.end())
		return eKeyDoesNotExist;
	it->values.erase(kv);
	return eOK;
}

bool WorldRuntime::arrayFirstKey(const QString &name, QString &key) const
{
	const auto it = m_arrays.find(name);
	if (it == m_arrays.end() || it->values.isEmpty())
		return false;
	key = it->values.firstKey();
	return true;
}

bool WorldRuntime::arrayLastKey(const QString &name, QString &key) const
{
	const auto it = m_arrays.find(name);
	if (it == m_arrays.end() || it->values.isEmpty())
		return false;
	key = it->values.lastKey();
	return true;
}

QStringList WorldRuntime::arrayListAll() const
{
	return m_arrays.keys();
}

QStringList WorldRuntime::arrayListKeys(const QString &name) const
{
	const auto it = m_arrays.find(name);
	if (it == m_arrays.end())
		return {};
	return it->values.keys();
}

QStringList WorldRuntime::arrayListValues(const QString &name) const
{
	const auto it = m_arrays.find(name);
	if (it == m_arrays.end())
		return {};
	return it->values.values();
}

namespace
{
	int mapSqlErrorToSqlite(const QSqlError &err)
	{
		if (!err.isValid())
			return SQLITE_OK;
		const QString text = err.text().toLower();
		if (text.contains(QStringLiteral("locked")) || text.contains(QStringLiteral("busy")))
			return SQLITE_BUSY;
		if (text.contains(QStringLiteral("readonly")))
			return SQLITE_READONLY;
		return SQLITE_ERROR;
	}

	int applySqliteWalAndNormalSynchronous(const QSqlDatabase &db, QString &errorMessage)
	{
		if (!db.isValid() || !db.isOpen())
		{
			errorMessage = QStringLiteral("Database connection is not open.");
			return SQLITE_ERROR;
		}

		QSqlQuery query(db);
		if (!query.exec(QStringLiteral("PRAGMA journal_mode=WAL")))
		{
			errorMessage = query.lastError().text();
			return mapSqlErrorToSqlite(query.lastError());
		}

		const QString mode = query.next() ? query.value(0).toString().trimmed() : QString();
		if (mode.compare(QStringLiteral("wal"), Qt::CaseInsensitive) != 0)
		{
			errorMessage = QStringLiteral("PRAGMA journal_mode returned '%1' instead of 'wal'.").arg(mode);
			return SQLITE_ERROR;
		}

		if (!query.exec(QStringLiteral("PRAGMA synchronous=NORMAL")))
		{
			errorMessage = query.lastError().text();
			return mapSqlErrorToSqlite(query.lastError());
		}

		return SQLITE_OK;
	}

	int mapVariantTypeToSqlite(const QVariant &value)
	{
		if (value.isNull())
			return SQLITE_NULL;
		switch (value.typeId())
		{
		case QMetaType::Int:
		case QMetaType::UInt:
		case QMetaType::LongLong:
		case QMetaType::ULongLong:
		case QMetaType::Short:
		case QMetaType::UShort:
		case QMetaType::Char:
		case QMetaType::UChar:
			return SQLITE_INTEGER;
		case QMetaType::Double:
		case QMetaType::Float:
			return SQLITE_FLOAT;
		case QMetaType::QByteArray:
			return SQLITE_BLOB;
		default:
			return SQLITE_TEXT;
		}
	}
} // namespace

int WorldRuntime::databaseOpen(const QString &name, const QString &filename, int flags)
{
	const auto it = m_databases.find(name);
	if (it != m_databases.end())
	{
		if (it->diskName == filename)
			return SQLITE_OK;
		return kDbErrorDatabaseAlreadyExists;
	}

	const bool isMemory = (flags & SQLITE_OPEN_MEMORY) != 0 || filename == QStringLiteral(":memory:");
	if (!isMemory && (flags & SQLITE_OPEN_CREATE) == 0 && !QFileInfo::exists(filename))
		return SQLITE_CANTOPEN;

	static int    connectionCounter = 0;
	DatabaseEntry entry;
	entry.diskName       = filename;
	entry.connectionName = QStringLiteral("world_db_%1").arg(++connectionCounter);
	entry.db             = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), entry.connectionName);
	entry.db.setDatabaseName(isMemory ? QStringLiteral(":memory:") : filename);
	if (flags & SQLITE_OPEN_READONLY)
		entry.db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
	if (!entry.db.open())
	{
		entry.lastError        = mapSqlErrorToSqlite(entry.db.lastError());
		entry.lastErrorMessage = entry.db.lastError().text();
		entry.db               = QSqlDatabase();
		QSqlDatabase::removeDatabase(entry.connectionName);
		return entry.lastError;
	}

	if (!isMemory && (flags & SQLITE_OPEN_READONLY) == 0)
	{
		entry.lastError = applySqliteWalAndNormalSynchronous(entry.db, entry.lastErrorMessage);
		if (entry.lastError != SQLITE_OK)
		{
			entry.db.close();
			entry.db = QSqlDatabase();
			QSqlDatabase::removeDatabase(entry.connectionName);
			return entry.lastError;
		}
	}

	m_databases.insert(name, entry);
	return SQLITE_OK;
}

int WorldRuntime::databaseClose(const QString &name)
{
	const auto it = m_databases.find(name);
	if (it == m_databases.end())
		return kDbErrorIdNotFound;
	if (!it->db.isValid() || !it->db.isOpen())
		return kDbErrorNotOpen;
	if (it->stmtPrepared)
	{
		it->stmt->finish();
		it->stmtPrepared = false;
		it->stmt.clear();
	}
	it->db.close();
	const QString connectionName = it->connectionName;
	m_databases.erase(it);
	QSqlDatabase::removeDatabase(connectionName);
	return SQLITE_OK;
}

int WorldRuntime::databasePrepare(const QString &name, const QString &sql)
{
	const auto it = m_databases.find(name);
	if (it == m_databases.end())
		return kDbErrorIdNotFound;
	if (!it->db.isValid() || !it->db.isOpen())
		return kDbErrorNotOpen;
	if (it->stmtPrepared)
		return kDbErrorHavePreparedStatement;

	it->validRow = false;
	it->columns  = 0;
	it->stmt     = QSharedPointer<QSqlQuery>::create(it->db);
	if (!it->stmt->prepare(sql))
	{
		it->lastError        = mapSqlErrorToSqlite(it->stmt->lastError());
		it->lastErrorMessage = it->stmt->lastError().text();
		return it->lastError;
	}
	it->stmtPrepared = true;
	it->stmtExecuted = false;
	it->lastError    = SQLITE_OK;
	it->lastErrorMessage.clear();
	it->columns = it->stmt->record().count();
	return SQLITE_OK;
}

int WorldRuntime::databaseFinalize(const QString &name)
{
	const auto it = m_databases.find(name);
	if (it == m_databases.end())
		return kDbErrorIdNotFound;
	if (!it->db.isValid() || !it->db.isOpen())
		return kDbErrorNotOpen;
	if (!it->stmtPrepared)
		return kDbErrorNoPreparedStatement;

	it->stmt->finish();
	it->stmt.clear();
	it->stmtPrepared = false;
	it->stmtExecuted = false;
	it->validRow     = false;
	it->columns      = 0;
	return SQLITE_OK;
}

int WorldRuntime::databaseReset(const QString &name)
{
	const auto it = m_databases.find(name);
	if (it == m_databases.end())
		return kDbErrorIdNotFound;
	if (!it->db.isValid() || !it->db.isOpen())
		return kDbErrorNotOpen;
	if (!it->stmtPrepared)
		return kDbErrorNoPreparedStatement;
	it->stmt->finish();
	it->stmtExecuted = false;
	it->validRow     = false;
	it->columns      = 0;
	return SQLITE_OK;
}

int WorldRuntime::databaseColumns(const QString &name) const
{
	const auto it = m_databases.find(name);
	if (it == m_databases.end())
		return kDbErrorIdNotFound;
	if (!it->db.isValid() || !it->db.isOpen())
		return kDbErrorNotOpen;
	if (!it->stmtPrepared)
		return kDbErrorNoPreparedStatement;
	return it->columns;
}

int WorldRuntime::databaseStep(const QString &name)
{
	const auto it = m_databases.find(name);
	if (it == m_databases.end())
		return kDbErrorIdNotFound;
	if (!it->db.isValid() || !it->db.isOpen())
		return kDbErrorNotOpen;
	if (!it->stmtPrepared)
		return kDbErrorNoPreparedStatement;

	if (!it->stmtExecuted)
	{
		if (!it->stmt->exec())
		{
			it->lastError        = mapSqlErrorToSqlite(it->stmt->lastError());
			it->lastErrorMessage = it->stmt->lastError().text();
			it->validRow         = false;
			return it->lastError;
		}
		it->stmtExecuted = true;
		it->columns      = it->stmt->record().count();
	}

	if (it->stmt->next())
	{
		it->validRow  = true;
		it->lastError = SQLITE_ROW;
		return SQLITE_ROW;
	}

	it->validRow  = false;
	it->lastError = SQLITE_DONE;
	return SQLITE_DONE;
}

QString WorldRuntime::databaseError(const QString &name) const
{
	const auto it = m_databases.find(name);
	if (it == m_databases.end())
		return QStringLiteral("database id not found");
	if (!it->db.isValid() || !it->db.isOpen())
		return QStringLiteral("database not open");

	switch (it->lastError)
	{
	case SQLITE_ROW:
		return QStringLiteral("row ready");
	case SQLITE_DONE:
		return QStringLiteral("finished");
	case kDbErrorIdNotFound:
		return QStringLiteral("database id not found");
	case kDbErrorNotOpen:
		return QStringLiteral("database not open");
	case kDbErrorHavePreparedStatement:
		return QStringLiteral("already have prepared statement");
	case kDbErrorNoPreparedStatement:
		return QStringLiteral("do not have prepared statement");
	case kDbErrorNoValidRow:
		return QStringLiteral("do not have a valid row");
	case kDbErrorDatabaseAlreadyExists:
		return QStringLiteral("database already exists under a different disk name");
	case kDbErrorColumnOutOfRange:
		return QStringLiteral("column count out of valid range");
	default:
		return it->lastErrorMessage.isEmpty() ? QStringLiteral("database error") : it->lastErrorMessage;
	}
}

QString WorldRuntime::databaseColumnName(const QString &name, int column) const
{
	const auto it = m_databases.find(name);
	if (it == m_databases.end() || !it->db.isValid() || !it->db.isOpen() || !it->stmtPrepared || column < 1 ||
	    column > it->columns)
		return {};
	return it->stmt->record().fieldName(column - 1);
}

QString WorldRuntime::databaseColumnText(const QString &name, int column, bool *ok) const
{
	if (ok)
		*ok = false;
	const auto it = m_databases.find(name);
	if (it == m_databases.end() || !it->db.isValid() || !it->db.isOpen() || !it->stmtPrepared ||
	    !it->validRow || column < 1 || column > it->columns)
		return {};
	const QVariant value = it->stmt->value(column - 1);
	if (value.isNull())
		return {};
	if (ok)
		*ok = true;
	return value.toString();
}

bool WorldRuntime::databaseColumnValue(const QString &name, int column, QVariant &value) const
{
	const auto it = m_databases.find(name);
	if (it == m_databases.end() || !it->db.isValid() || !it->db.isOpen() || !it->stmtPrepared ||
	    !it->validRow || column < 1 || column > it->columns)
		return false;

	value = it->stmt->value(column - 1);
	return true;
}

int WorldRuntime::databaseColumnType(const QString &name, int column) const
{
	const auto it = m_databases.find(name);
	if (it == m_databases.end())
		return kDbErrorIdNotFound;
	if (!it->db.isValid() || !it->db.isOpen())
		return kDbErrorNotOpen;
	if (!it->stmtPrepared)
		return kDbErrorNoPreparedStatement;
	if (!it->validRow)
		return kDbErrorNoValidRow;
	if (column < 1 || column > it->columns)
		return kDbErrorColumnOutOfRange;
	return mapVariantTypeToSqlite(it->stmt->value(column - 1));
}

int WorldRuntime::databaseTotalChanges(const QString &name) const
{
	const auto it = m_databases.find(name);
	if (it == m_databases.end())
		return kDbErrorIdNotFound;
	if (!it->db.isValid() || !it->db.isOpen())
		return kDbErrorNotOpen;
	QSqlQuery query(it->db);
	if (!query.exec(QStringLiteral("SELECT total_changes()")) || !query.next())
		return 0;
	return query.value(0).toInt();
}

int WorldRuntime::databaseChanges(const QString &name) const
{
	const auto it = m_databases.find(name);
	if (it == m_databases.end())
		return kDbErrorIdNotFound;
	if (!it->db.isValid() || !it->db.isOpen())
		return kDbErrorNotOpen;
	QSqlQuery query(it->db);
	if (!query.exec(QStringLiteral("SELECT changes()")) || !query.next())
		return 0;
	return query.value(0).toInt();
}

QString WorldRuntime::databaseLastInsertRowid(const QString &name) const
{
	const auto it = m_databases.find(name);
	if (it == m_databases.end() || !it->db.isValid() || !it->db.isOpen())
		return {};
	QSqlQuery query(it->db);
	if (!query.exec(QStringLiteral("SELECT last_insert_rowid()")) || !query.next())
		return {};
	return query.value(0).toString();
}

QStringList WorldRuntime::databaseList() const
{
	return m_databases.keys();
}

QVariant WorldRuntime::databaseInfo(const QString &name, int infoType) const
{
	const auto it = m_databases.find(name);
	if (it == m_databases.end())
		return {};

	switch (infoType)
	{
	case 1:
		return it->diskName;
	case 2:
		return it->stmtPrepared;
	case 3:
		return it->validRow;
	case 4:
		return it->columns;
	default:
		return {};
	}
}

int WorldRuntime::databaseExec(const QString &name, const QString &sql)
{
	const auto it = m_databases.find(name);
	if (it == m_databases.end())
		return kDbErrorIdNotFound;
	if (!it->db.isValid() || !it->db.isOpen())
		return kDbErrorNotOpen;
	if (it->stmtPrepared)
		return kDbErrorHavePreparedStatement;

	it->validRow = false;
	it->columns  = 0;
	QSqlQuery         query(it->db);
	const QStringList statements = sql.split(QLatin1Char(';'), Qt::SkipEmptyParts);
	for (const QString &stmt : statements)
	{
		const QString trimmed = stmt.trimmed();
		if (trimmed.isEmpty())
			continue;
		if (!query.exec(trimmed))
		{
			it->lastError        = mapSqlErrorToSqlite(query.lastError());
			it->lastErrorMessage = query.lastError().text();
			return it->lastError;
		}
	}
	it->lastError = SQLITE_OK;
	it->lastErrorMessage.clear();
	return SQLITE_OK;
}

QStringList WorldRuntime::databaseColumnNames(const QString &name) const
{
	const auto it = m_databases.find(name);
	if (it == m_databases.end() || !it->db.isValid() || !it->db.isOpen() || !it->stmtPrepared)
		return {};
	QStringList      names;
	const QSqlRecord record = it->stmt->record();
	for (int i = 0; i < it->columns; ++i)
		names.append(record.fieldName(i));
	return names;
}

bool WorldRuntime::databaseColumnValues(const QString &name, QVector<QVariant> &values) const
{
	values.clear();
	const auto it = m_databases.find(name);
	if (it == m_databases.end() || !it->db.isValid() || !it->db.isOpen() || !it->stmtPrepared ||
	    !it->validRow)
		return false;
	values.reserve(it->columns);
	for (int i = 0; i < it->columns; ++i)
		values.push_back(it->stmt->value(i));
	return true;
}

const QList<WorldRuntime::Colour> &WorldRuntime::colours() const
{
	return m_colours;
}

void WorldRuntime::setColours(const QList<Colour> &colours)
{
	m_colours           = colours;
	m_colourCount       = safeQSizeToInt(m_colours.size());
	m_worldFileModified = true;
}

namespace
{
	QColor parseColorValueRuntime(const QString &value)
	{
		if (value.isEmpty())
			return {};

		QColor color(value);
		if (color.isValid())
			return color;

		bool      ok      = false;
		const int numeric = value.toInt(&ok);
		if (!ok)
			return {};

		const int r = (numeric >> 16) & 0xFF;
		const int g = (numeric >> 8) & 0xFF;
		const int b = numeric & 0xFF;
		return {r, g, b};
	}

	QVector<QColor> defaultAnsiColours(bool bold)
	{
		QVector<QColor> colours(8);
		if (bold)
		{
			colours[0] = QColor(128, 128, 128);
			colours[1] = QColor(255, 0, 0);
			colours[2] = QColor(0, 255, 0);
			colours[3] = QColor(255, 255, 0);
			colours[4] = QColor(0, 0, 255);
			colours[5] = QColor(255, 0, 255);
			colours[6] = QColor(0, 255, 255);
			colours[7] = QColor(255, 255, 255);
		}
		else
		{
			colours[0] = QColor(0, 0, 0);
			colours[1] = QColor(128, 0, 0);
			colours[2] = QColor(0, 128, 0);
			colours[3] = QColor(128, 128, 0);
			colours[4] = QColor(0, 0, 128);
			colours[5] = QColor(128, 0, 128);
			colours[6] = QColor(0, 128, 128);
			colours[7] = QColor(192, 192, 192);
		}
		return colours;
	}

} // namespace

QColor WorldRuntime::ansiColour(bool bold, int index) const
{
	if (index < 1 || index > 8)
		return {};

	const QString targetGroup = bold ? QStringLiteral("ansi/bold") : QStringLiteral("ansi/normal");
	for (const auto &colour : m_colours)
	{
		if (colour.group.trimmed().compare(targetGroup, Qt::CaseInsensitive) != 0)
			continue;
		const int seq = colourSeqFromAttributes(colour.attributes);
		if (seq != index)
			continue;
		const QColor rgb = parseColorValueRuntime(colour.attributes.value(QStringLiteral("rgb")));
		if (rgb.isValid())
			return rgb;
	}

	return defaultAnsiColours(bold).value(index - 1);
}

void WorldRuntime::setAnsiColour(bool bold, int index, const QColor &color)
{
	if (index < 1 || index > 8 || !color.isValid())
		return;

	const QString targetGroup = bold ? QStringLiteral("ansi/bold") : QStringLiteral("ansi/normal");
	const QString rgb         = QString::number((color.red() << 16) | (color.green() << 8) | color.blue());

	for (auto &colour : m_colours)
	{
		if (colour.group.trimmed().compare(targetGroup, Qt::CaseInsensitive) != 0)
			continue;
		const int seq = colourSeqFromAttributes(colour.attributes);
		if (seq != index)
			continue;
		colour.attributes.insert(QStringLiteral("seq"), QString::number(index));
		colour.attributes.insert(QStringLiteral("seq_index"), QString::number(index - 1));
		colour.attributes.insert(QStringLiteral("rgb"), rgb);
		m_worldFileModified = true;
		return;
	}

	Colour entry;
	entry.group = targetGroup;
	entry.attributes.insert(QStringLiteral("seq"), QString::number(index));
	entry.attributes.insert(QStringLiteral("seq_index"), QString::number(index - 1));
	entry.attributes.insert(QStringLiteral("rgb"), rgb);
	m_colours.push_back(entry);
	m_colourCount       = safeQSizeToInt(m_colours.size());
	m_worldFileModified = true;
}

const QList<WorldRuntime::Keypad> &WorldRuntime::keypadEntries() const
{
	return m_keypadEntries;
}

void WorldRuntime::setKeypadEntries(const QList<Keypad> &entries)
{
	m_keypadEntries     = entries;
	m_keypadCount       = safeQSizeToInt(m_keypadEntries.size());
	m_worldFileModified = true;
}

const QList<WorldRuntime::PrintingStyle> &WorldRuntime::printingStyles() const
{
	return m_printingStyles;
}

void WorldRuntime::setPrintingStyles(const QList<PrintingStyle> &styles)
{
	m_printingStyles     = styles;
	m_printingStyleCount = safeQSizeToInt(m_printingStyles.size());
	m_worldFileModified  = true;
}

const QList<WorldRuntime::Plugin> &WorldRuntime::plugins() const
{
	return m_plugins;
}

QList<WorldRuntime::Plugin> &WorldRuntime::pluginsMutable()
{
	return m_plugins;
}

bool WorldRuntime::loadPluginFile(const QString &fileName, QString *error, bool markGlobal)
{
	const QString raw = fileName.trimmed();
	if (raw.isEmpty())
	{
		if (error)
			*error = QStringLiteral("No plugin file specified");
		return false;
	}

	QString const workingDir = resolveWorkingDir(m_startupDirectory);
	QString       pluginsDir = m_pluginsDirectory;
	if (!pluginsDir.isEmpty())
		pluginsDir = makeAbsolutePath(pluginsDir, workingDir);
	QString resolved = normalizeSeparators(raw);
	if (resolved.startsWith(QStringLiteral("./")) || resolved.startsWith(QStringLiteral(".\\")))
		resolved = resolved.mid(2);

	if (resolved.startsWith(QLatin1Char('/')))
	{
		QString candidate = resolved;
		while (candidate.startsWith(QLatin1Char('/')))
			candidate.remove(0, 1);
		const QString lower = candidate.toLower();
		const bool    looksPortable =
		    lower == QStringLiteral("worlds") || lower.startsWith(QStringLiteral("worlds/")) ||
		    lower == QStringLiteral("logs") || lower.startsWith(QStringLiteral("logs/"));
		if (looksPortable)
			resolved = candidate;
	}
	QFileInfo info(resolved);
	if (!info.isAbsolute())
	{
		const QString normalizedResolved  = normalizeSeparators(resolved);
		const QString normalizedPlugins   = normalizeSeparators(pluginsDir);
		const QString normalizedWorking   = normalizeSeparators(workingDir);
		const bool    looksLikeWorldsPath = normalizedResolved.startsWith(QStringLiteral("worlds/"));
		const bool    pluginsIsWorlds     = normalizedPlugins.endsWith(QStringLiteral("worlds/plugins/")) ||
		                             normalizedPlugins.endsWith(QStringLiteral("worlds/plugins"));
		if (!normalizedPlugins.isEmpty() && !looksLikeWorldsPath && !pluginsIsWorlds)
			resolved = QDir::cleanPath(QDir(normalizedPlugins).filePath(normalizedResolved));
		else
			resolved = QDir::cleanPath(QDir(normalizedWorking).filePath(normalizedResolved));
		info = QFileInfo(resolved);
	}

	if (!info.exists())
	{
		if (error)
			*error = QStringLiteral("Plugin file not found: %1").arg(resolved);
		return false;
	}

	WorldDocument           doc;
	constexpr unsigned long mask = WorldDocument::XML_TRIGGERS | WorldDocument::XML_ALIASES |
	                               WorldDocument::XML_TIMERS | WorldDocument::XML_VARIABLES |
	                               WorldDocument::XML_PLUGINS | WorldDocument::XML_INCLUDES;
	doc.setLoadMask(mask);
	if (!doc.loadFromPluginFile(resolved))
	{
		if (error)
			*error = doc.errorString();
		return false;
	}

	if (!doc.expandIncludes(resolved, pluginsDir, resolveWorkingDir(m_startupDirectory),
	                        m_stateFilesDirectory))
	{
		if (error)
			*error = doc.errorString();
		return false;
	}

	if (doc.plugins().isEmpty())
	{
		if (error)
			*error = QStringLiteral("No plugin found");
		return false;
	}

	const WorldDocument::Plugin &p             = doc.plugins().front();
	const QString                pluginId      = p.attributes.value(QStringLiteral("id")).trimmed().toLower();
	const int                    existingIndex = findPluginIndex(m_plugins, pluginId);
	if (existingIndex >= 0)
	{
		Plugin &existing = m_plugins[existingIndex];
		if (markGlobal)
		{
			existing.global = true;
			if (existing.source.isEmpty())
			{
				existing.source    = resolved;
				existing.directory = info.absolutePath();
			}
			return true;
		}
		if (error)
		{
			const QString pluginName = existing.attributes.value(QStringLiteral("name")).trimmed();
			if (!pluginName.isEmpty())
				*error = QStringLiteral("The plugin \"%1\" is already loaded.").arg(pluginName);
			else
				*error = QStringLiteral("The plugin is already loaded.");
		}
		return false;
	}

	Plugin rp;
	rp.attributes                  = p.attributes;
	rp.description                 = p.description;
	rp.script                      = p.script;
	rp.source                      = resolved;
	rp.directory                   = info.absolutePath();
	rp.global                      = markGlobal;
	rp.sequence                    = pluginSequenceFromAttributes(rp.attributes);
	rp.version                     = rp.attributes.value(QStringLiteral("version")).toDouble();
	rp.requiredVersion             = rp.attributes.value(QStringLiteral("requires")).toDouble();
	rp.dateWritten                 = parsePluginDate(rp.attributes.value(QStringLiteral("date_written")));
	rp.dateModified                = parsePluginDate(rp.attributes.value(QStringLiteral("date_modified")));
	rp.dateInstalled               = QDateTime::currentDateTime();
	rp.saveState                   = isEnabledFlag(rp.attributes.value(QStringLiteral("save_state")));
	const QString enabledFlag      = rp.attributes.value(QStringLiteral("enabled"));
	const bool    requestedEnabled = enabledFlag.isEmpty() ? true : isEnabledFlag(enabledFlag);
	rp.enabled                     = requestedEnabled;

	for (const auto &t : p.triggers)
	{
		Trigger rt;
		rt.attributes                 = t.attributes;
		rt.children                   = t.children;
		rt.included                   = t.included;
		const bool hasExplicitEnabled = rt.attributes.contains(QStringLiteral("enabled"));
		applyTriggerDefaults(rt);
		if (!hasExplicitEnabled)
			rt.attributes.insert(QStringLiteral("enabled"), QStringLiteral("0"));
		rp.triggers.push_back(rt);
	}
	for (const auto &a : p.aliases)
	{
		Alias ra;
		ra.attributes                 = a.attributes;
		ra.children                   = a.children;
		ra.included                   = a.included;
		const bool hasExplicitEnabled = ra.attributes.contains(QStringLiteral("enabled"));
		applyAliasDefaults(ra);
		if (!hasExplicitEnabled)
			ra.attributes.insert(QStringLiteral("enabled"), QStringLiteral("0"));
		rp.aliases.push_back(ra);
	}
	for (const auto &t : p.timers)
	{
		Timer rt;
		rt.attributes                 = t.attributes;
		rt.children                   = t.children;
		rt.included                   = t.included;
		const bool hasExplicitEnabled = rt.attributes.contains(QStringLiteral("enabled"));
		applyTimerDefaults(rt);
		if (!hasExplicitEnabled)
			rt.attributes.insert(QStringLiteral("enabled"), QStringLiteral("0"));
		resetTimerFields(rt);
		rp.timers.push_back(rt);
	}
	for (const auto &v : p.variables)
	{
		const QString name = v.attributes.value(QStringLiteral("name"));
		if (!name.isEmpty())
			rp.variables.insert(name, v.content);
	}
	if (!m_stateFilesDirectory.isEmpty())
	{
		const QString worldId = m_worldAttributes.value(QStringLiteral("id"));
		if (!worldId.isEmpty() && !pluginId.isEmpty())
		{
			QString base = m_stateFilesDirectory;
			if (!base.endsWith('/') && !base.endsWith('\\'))
				base += '/';
			const QString   stateFile = base + worldId + "-" + pluginId + "-state.xml";
			QFileInfo const stateInfo(stateFile);
			if (stateInfo.exists())
			{
				if (stateInfo.size() > 0)
				{
					WorldDocument stateDoc;
					stateDoc.setLoadMask(WorldDocument::XML_VARIABLES | WorldDocument::XML_NO_PLUGINS);
					if (!stateDoc.loadFromFile(stateInfo.absoluteFilePath()))
					{
						if (error)
							*error = stateDoc.errorString();
						return false;
					}
					for (const auto &v : stateDoc.variables())
					{
						const QString name = v.attributes.value(QStringLiteral("name"));
						if (!name.isEmpty())
							rp.variables.insert(name, v.content);
					}
				}
			}
		}
	}

	const QString language = rp.attributes.value(QStringLiteral("language"));
	if (language.compare(QStringLiteral("lua"), Qt::CaseInsensitive) == 0)
	{
		rp.lua = QSharedPointer<LuaCallbackEngine>::create();
		rp.lua->setWorldRuntime(this);
		rp.lua->setScriptText(rp.script);
		rp.lua->setPluginInfo(rp.attributes.value(QStringLiteral("id")),
		                      rp.attributes.value(QStringLiteral("name")));
		bindPluginCallbackObserver(rp);
	}
	if (!requestedEnabled && rp.lua && hasValidPluginId(rp))
	{
		rp.enabled             = true;
		rp.disableAfterInstall = true;
	}

	m_plugins.push_back(rp);
	sortPluginsBySequence();
	m_pluginCount = safeQSizeToInt(m_plugins.size());
	noteTimerStructureMutation();
	invalidatePluginCallbackPresenceCache();

	// Queue install for the plugin we just loaded (not "last after sort"),
	// otherwise lower-sequence plugins can remain permanently install-pending.
	const int installedIndex = findPluginIndex(m_plugins, pluginId);
	if (installedIndex >= 0)
		queuePluginInstall(m_plugins[installedIndex]);
	callPluginCallbacksNoArgs(QStringLiteral("OnPluginListChanged"));
	return true;
}

bool WorldRuntime::unloadPlugin(const QString &pluginId, QString *error)
{
	const int index = findPluginIndex(m_plugins, pluginId);
	if (index < 0)
	{
		if (error)
			*error = QStringLiteral("Plugin not found");
		return false;
	}

	Plugin &plugin = m_plugins[index];
	if (plugin.lua)
		plugin.lua->callFunctionNoArgs(QStringLiteral("OnPluginClose"));
	savePluginStateForPlugin(plugin, false, nullptr);
	m_plugins.removeAt(index);
	m_pluginCount = safeQSizeToInt(m_plugins.size());
	noteTimerStructureMutation();
	invalidatePluginCallbackPresenceCache();
	callPluginCallbacksNoArgs(QStringLiteral("OnPluginListChanged"));
	return true;
}

bool WorldRuntime::enablePlugin(const QString &pluginId, bool enable)
{
	const int index = findPluginIndex(m_plugins, pluginId);
	if (index < 0)
		return false;
	Plugin &plugin = m_plugins[index];
	if (plugin.enabled == enable)
	{
		if (enable && plugin.disableAfterInstall)
		{
			plugin.disableAfterInstall = false;
			plugin.attributes.insert(QStringLiteral("enabled"), QStringLiteral("1"));
			invalidatePluginCallbackPresenceCache();
			if (!m_loadingDocument)
				m_worldFileModified = true;
		}
		return true;
	}
	plugin.enabled = enable;
	if (!enable)
		plugin.disableAfterInstall = false;
	plugin.attributes.insert(QStringLiteral("enabled"), enable ? QStringLiteral("1") : QStringLiteral("0"));
	if (enable)
	{
		if (plugin.lua)
			plugin.lua->callFunctionNoArgs(QStringLiteral("OnPluginEnable"));
		// Callback may toggle plugin state (e.g. script calls EnablePlugin on itself).
		if (plugin.enabled && plugin.installPending)
			queuePluginInstall(plugin);
	}
	else
	{
		if (plugin.lua)
			plugin.lua->callFunctionNoArgs(QStringLiteral("OnPluginDisable"));
	}
	if (!m_loadingDocument)
		m_worldFileModified = true;
	invalidatePluginCallbackPresenceCache();
	return true;
}

int WorldRuntime::reloadPlugin(const QString &pluginId, QString *error)
{
	const int index = findPluginIndex(m_plugins, pluginId);
	if (index < 0)
		return eNoSuchPlugin;

	const Plugin  existing = m_plugins.at(index);
	const QString source   = existing.source;
	if (source.isEmpty())
		return ePluginFileNotFound;

	const bool wasGlobal  = existing.global;
	const bool wasEnabled = existing.enabled;

	QString    unloadError;
	if (!unloadPlugin(pluginId, &unloadError))
	{
		if (error)
			*error = unloadError;
		return eNoSuchPlugin;
	}

	QString loadError;
	if (!loadPluginFile(source, &loadError, wasGlobal))
	{
		if (error)
			*error = loadError;
		if (loadError.contains(QStringLiteral("not found"), Qt::CaseInsensitive))
			return ePluginFileNotFound;
		return eProblemsLoadingPlugin;
	}

	if (!wasEnabled)
		enablePlugin(pluginId, false);

	return eOK;
}

bool WorldRuntime::isPluginInstalled(const QString &pluginId) const
{
	return findPluginIndex(m_plugins, pluginId) >= 0;
}

int WorldRuntime::pluginSupports(const QString &pluginId, const QString &routine) const
{
	const int index = findPluginIndex(m_plugins, pluginId);
	if (index < 0)
		return eNoSuchPlugin;
	if (routine.trimmed().isEmpty())
		return eNoSuchRoutine;
	const Plugin &plugin = m_plugins.at(index);
	if (!plugin.lua)
		return eNoSuchRoutine;
	return plugin.lua->hasFunction(routine) ? eOK : eNoSuchRoutine;
}

int WorldRuntime::callPlugin(const QString &pluginId, const QString &routine, const QString &argument,
                             const QString &callingPluginId)
{
	const int index = findPluginIndex(m_plugins, pluginId);
	if (index < 0)
		return eNoSuchPlugin;
	Plugin &plugin = m_plugins[index];
	if (!plugin.lua)
		return eNoSuchRoutine;
	if (!plugin.enabled)
		return ePluginDisabled;
	if (routine.trimmed().isEmpty())
		return eNoSuchRoutine;

	const QString savedCalling = plugin.callingPluginId;
	plugin.callingPluginId     = callingPluginId;
	bool       hasFunction     = false;
	const bool ok              = plugin.lua->callFunctionWithString(routine, argument, &hasFunction, false);
	plugin.callingPluginId     = savedCalling;
	if (!hasFunction)
		return eNoSuchRoutine;
	return ok ? eOK : eErrorCallingPluginRoutine;
}

#ifdef QMUD_ENABLE_LUA_SCRIPTING
int WorldRuntime::callPluginLua(const QString &pluginId, const QString &routine, lua_State *callerState,
                                int firstArg, const QString &callingPluginId)
{
	if (!callerState)
		return 0;

	if (routine.trimmed().isEmpty())
	{
		lua_pushnumber(callerState, eNoSuchRoutine);
		lua_pushstring(callerState, "No function name supplied");
		return 2;
	}

	const int index = findPluginIndex(m_plugins, pluginId);
	if (index < 0)
	{
		lua_pushnumber(callerState, eNoSuchPlugin);
		const QString error = QStringLiteral("Plugin ID (%1) is not installed").arg(pluginId);
		lua_pushstring(callerState, error.toLocal8Bit().constData());
		return 2;
	}

	Plugin &plugin = m_plugins[index];
	if (!plugin.enabled)
	{
		lua_pushnumber(callerState, ePluginDisabled);
		const QString error = QStringLiteral("Plugin '%1' (%2) disabled")
		                          .arg(plugin.attributes.value(QStringLiteral("name")), pluginId);
		lua_pushstring(callerState, error.toLocal8Bit().constData());
		return 2;
	}

	if (!plugin.lua || !plugin.lua->loadScript())
	{
		lua_pushnumber(callerState, eNoSuchRoutine);
		const QString error = QStringLiteral("Scripting not enabled in plugin '%1' (%2)")
		                          .arg(plugin.attributes.value(QStringLiteral("name")), pluginId);
		lua_pushstring(callerState, error.toLocal8Bit().constData());
		return 2;
	}

	lua_State *targetState = plugin.lua->luaState();
	if (!targetState)
	{
		lua_pushnumber(callerState, eNoSuchRoutine);
		const QString error = QStringLiteral("Scripting not enabled in plugin '%1' (%2)")
		                          .arg(plugin.attributes.value(QStringLiteral("name")), pluginId);
		lua_pushstring(callerState, error.toLocal8Bit().constData());
		return 2;
	}

	[[maybe_unused]] const auto refreshCallbackCatalog = qScopeGuard(
	    [&plugin]
	    {
		    if (plugin.lua)
			    plugin.lua->refreshLuaCallbackCatalogNow();
	    });

	auto pushNestedFunction = [](lua_State *L, const QString &dottedName) -> bool
	{
		const QStringList parts = dottedName.split(QLatin1Char('.'), Qt::SkipEmptyParts);
		if (parts.isEmpty())
			return false;
		const QByteArray first = parts.first().toLocal8Bit();
		lua_getglobal(L, first.constData());
		if (parts.size() == 1)
			return lua_isfunction(L, -1);
		if (!lua_istable(L, -1))
		{
			lua_pop(L, 1);
			return false;
		}
		for (int i = 1; i < parts.size(); ++i)
		{
			const QByteArray field = parts.at(i).toLocal8Bit();
			lua_getfield(L, -1, field.constData());
			lua_remove(L, -2);
			if (i < parts.size() - 1)
			{
				if (!lua_istable(L, -1))
				{
					lua_pop(L, 1);
					return false;
				}
			}
		}
		if (!lua_isfunction(L, -1))
		{
			lua_pop(L, 1);
			return false;
		}
		return true;
	};

	const int     top          = lua_gettop(callerState);
	const int     argCount     = qMax(0, top - firstArg + 1);
	const QString savedCalling = plugin.callingPluginId;
	plugin.callingPluginId     = callingPluginId;

	if (targetState == callerState)
	{
		if (!pushNestedFunction(targetState, routine))
		{
			plugin.callingPluginId = savedCalling;
			lua_pushnumber(callerState, eNoSuchRoutine);
			const QString error =
			    QStringLiteral("No function '%1' in plugin '%2' (%3)")
			        .arg(routine, plugin.attributes.value(QStringLiteral("name")), pluginId);
			lua_pushstring(callerState, error.toLocal8Bit().constData());
			return 2;
		}

		lua_insert(targetState, firstArg);
		if (lua_pcall(targetState, argCount, LUA_MULTRET, 0) != 0)
		{
			const char   *err      = lua_tostring(targetState, -1);
			const QString luaError = QString::fromUtf8(err ? err : "unknown");
			lua_pop(targetState, 1);
			plugin.callingPluginId = savedCalling;

			lua_pushnumber(callerState, eErrorCallingPluginRoutine);
			const QString error =
			    QStringLiteral("Runtime error in function '%1', plugin '%2' (%3)")
			        .arg(routine, plugin.attributes.value(QStringLiteral("name")), pluginId);
			lua_pushstring(callerState, error.toLocal8Bit().constData());
			lua_pushstring(callerState, luaError.toLocal8Bit().constData());
			return 3;
		}

		plugin.callingPluginId = savedCalling;
		const int retCount     = lua_gettop(targetState);
		lua_pushnumber(targetState, eOK);
		lua_insert(targetState, firstArg);
		return retCount + 1;
	}

	lua_settop(targetState, 0);
	if (!pushNestedFunction(targetState, routine))
	{
		plugin.callingPluginId = savedCalling;
		lua_pushnumber(callerState, eNoSuchRoutine);
		const QString error = QStringLiteral("No function '%1' in plugin '%2' (%3)")
		                          .arg(routine, plugin.attributes.value(QStringLiteral("name")), pluginId);
		lua_pushstring(callerState, error.toLocal8Bit().constData());
		return 2;
	}

	lua_checkstack(targetState, argCount + 2);
	for (int i = firstArg; i <= top; ++i)
	{
		switch (const int type = lua_type(callerState, i); type)
		{
		case LUA_TNIL:
			lua_pushnil(targetState);
			break;
		case LUA_TBOOLEAN:
			lua_pushboolean(targetState, lua_toboolean(callerState, i));
			break;
		case LUA_TNUMBER:
			lua_pushnumber(targetState, lua_tonumber(callerState, i));
			break;
		case LUA_TSTRING:
		{
			size_t      len = 0;
			const char *s   = lua_tolstring(callerState, i, &len);
			lua_pushlstring(targetState, s, len);
			break;
		}
		default:
		{
			lua_settop(targetState, 0);
			plugin.callingPluginId = savedCalling;
			lua_pushnumber(callerState, eBadParameter);
			const int     displayIndex = i - firstArg + 3; // plugin ID + routine removed
			const QString error        = QStringLiteral("Cannot pass argument #%1 (%2 type) to CallPlugin")
			                          .arg(displayIndex)
			                          .arg(QString::fromLatin1(lua_typename(callerState, type)));
			lua_pushstring(callerState, error.toLocal8Bit().constData());
			return 2;
		}
		}
	}

	if (lua_pcall(targetState, argCount, LUA_MULTRET, 0) != 0)
	{
		const char   *err      = lua_tostring(targetState, -1);
		const QString luaError = QString::fromUtf8(err ? err : "unknown");
		lua_settop(targetState, 0);
		plugin.callingPluginId = savedCalling;

		lua_pushnumber(callerState, eErrorCallingPluginRoutine);
		const QString error = QStringLiteral("Runtime error in function '%1', plugin '%2' (%3)")
		                          .arg(routine, plugin.attributes.value(QStringLiteral("name")), pluginId);
		lua_pushstring(callerState, error.toLocal8Bit().constData());
		lua_pushstring(callerState, luaError.toLocal8Bit().constData());
		return 3;
	}

	const int retCount = lua_gettop(targetState);
	lua_pushnumber(callerState, eOK);
	for (int i = 1; i <= retCount; ++i)
	{
		switch (const int type = lua_type(targetState, i); type)
		{
		case LUA_TNIL:
			lua_pushnil(callerState);
			break;
		case LUA_TBOOLEAN:
			lua_pushboolean(callerState, lua_toboolean(targetState, i));
			break;
		case LUA_TNUMBER:
			lua_pushnumber(callerState, lua_tonumber(targetState, i));
			break;
		case LUA_TSTRING:
		{
			size_t      len = 0;
			const char *s   = lua_tolstring(targetState, i, &len);
			lua_pushlstring(callerState, s, len);
			break;
		}
		default:
		{
			lua_settop(targetState, 0);
			plugin.callingPluginId = savedCalling;
			lua_pushnumber(callerState, eErrorCallingPluginRoutine);
			const QString error =
			    QStringLiteral(
			        "Cannot handle return value #%1 (%2 type) from function '%3' in plugin '%4' (%5)")
			        .arg(i)
			        .arg(QString::fromLatin1(lua_typename(targetState, type)))
			        .arg(routine, plugin.attributes.value(QStringLiteral("name")), pluginId);
			lua_pushstring(callerState, error.toLocal8Bit().constData());
			return 2;
		}
		}
	}

	lua_settop(targetState, 0);
	plugin.callingPluginId = savedCalling;
	return retCount + 1;
}
#endif

int WorldRuntime::broadcastPlugin(long message, const QString &text, const QString &callingPluginId,
                                  const QString &callingPluginName)
{
	const QString callbackName = QStringLiteral("OnPluginBroadcast");
	if (!hasAnyPluginCallback(callbackName))
		return 0;
	const auto recipientsIt = m_pluginCallbackRecipientIndices.constFind(callbackName);
	if (recipientsIt == m_pluginCallbackRecipientIndices.constEnd())
		return 0;
	const int          pluginCount = safeQSizeToInt(m_plugins.size());
	const QVector<int> recipientIndices =
	    qmudFilterValidPluginRecipientIndices(recipientsIt.value(), pluginCount);
	if (recipientIndices.isEmpty())
		return 0;
	if (qmudShouldSkipSelfOnlyPluginBroadcast(
	        recipientIndices, pluginCount, callingPluginId,
	        [this](const int index) { return m_plugins.at(index).attributes.value(QStringLiteral("id")); }))
		return 0;
	const QByteArray callingPluginIdUtf8   = callingPluginId.toUtf8();
	const QByteArray callingPluginNameUtf8 = callingPluginName.toUtf8();
	const QByteArray textUtf8              = text.toUtf8();

	int              count = 0;
	for (const int pluginIndex : recipientIndices)
	{
		if (pluginIndex < 0 || pluginIndex >= m_plugins.size())
			continue;
		auto &plugin = m_plugins[pluginIndex];
		if (!canExecutePlugin(plugin))
			continue;
		if (!plugin.lua)
			continue;

		const QString pluginId = plugin.attributes.value(QStringLiteral("id"));
		if (!callingPluginId.isEmpty() && pluginId.compare(callingPluginId, Qt::CaseInsensitive) == 0)
			continue;

		bool hasFunction = false;
		plugin.lua->callFunctionWithNumberAndUtf8Strings(callbackName, message, callingPluginIdUtf8,
		                                                 callingPluginNameUtf8, textUtf8, &hasFunction);
		if (hasFunction)
			++count;
	}
	return count;
}

QString WorldRuntime::worldName() const
{
	return m_worldAttributes.value(QStringLiteral("name"));
}

QString WorldRuntime::chatOurName()
{
	QString chatName = m_worldAttributes.value(QStringLiteral("chat_name"));
	if (chatName.trimmed().isEmpty())
	{
		const QString fallback = worldName();
		if (!fallback.trimmed().isEmpty())
		{
			chatName = fallback;
			setWorldAttribute(QStringLiteral("chat_name"), chatName);
		}
	}
	if (chatName.trimmed().isEmpty())
		chatName = QStringLiteral("Name-not-set");
	return chatName;
}

int WorldRuntime::chatIncomingPort() const
{
	bool ok   = false;
	int  port = m_worldAttributes.value(QStringLiteral("chat_port")).toInt(&ok);
	if (!ok || port == 0)
		port = kDefaultChatPort;
	return port;
}

QString WorldRuntime::chatLocalAddress()
{
	const auto addresses = QNetworkInterface::allAddresses();
	for (const QHostAddress &addr : addresses)
	{
		if (addr.protocol() == QAbstractSocket::IPv4Protocol && !addr.isLoopback())
			return addr.toString();
	}
	return QStringLiteral("127.0.0.1");
}

bool WorldRuntime::validateIncomingChatCalls() const
{
	return isEnabledFlag(m_worldAttributes.value(QStringLiteral("validate_incoming_chat_calls")));
}

bool WorldRuntime::autoAllowFiles() const
{
	return isEnabledFlag(m_worldAttributes.value(QStringLiteral("auto_allow_files")));
}

bool WorldRuntime::autoAllowSnooping() const
{
	return isEnabledFlag(m_worldAttributes.value(QStringLiteral("auto_allow_snooping")));
}

QString WorldRuntime::chatSaveDirectory() const
{
	return m_worldAttributes.value(QStringLiteral("chat_file_save_directory"));
}

bool WorldRuntime::ignoreChatColours() const
{
	return isEnabledFlag(m_worldAttributes.value(QStringLiteral("ignore_chat_colours")));
}

int WorldRuntime::chatMaxLines() const
{
	return m_worldAttributes.value(QStringLiteral("chat_max_lines_per_message")).toInt();
}

int WorldRuntime::chatMaxBytes() const
{
	return m_worldAttributes.value(QStringLiteral("chat_max_bytes_per_message")).toInt();
}

long WorldRuntime::chatForegroundColour() const
{
	bool            ok    = false;
	const qlonglong value = m_worldAttributes.value(QStringLiteral("chat_foreground_colour")).toLongLong(&ok);
	return ok ? static_cast<long>(value) : colorToLong(QColor(255, 0, 0));
}

long WorldRuntime::chatBackgroundColour() const
{
	bool            ok    = false;
	const qlonglong value = m_worldAttributes.value(QStringLiteral("chat_background_colour")).toLongLong(&ok);
	return ok ? static_cast<long>(value) : colorToLong(QColor(0, 0, 0));
}

bool WorldRuntime::isChatAlreadyConnected(const QString &address, int port,
                                          const ChatConnection *exclude) const
{
	return std::ranges::any_of(chatConnections(),
	                           [&](const ChatConnection *connection)
	                           {
		                           return connection && connection != exclude && connection->isConnected() &&
		                                  connection->allegedAddress() == address &&
		                                  connection->allegedPort() == port;
	                           });
}

QList<WorldRuntime::ChatConnection *> WorldRuntime::chatConnections() const
{
	return m_chatConnections.values();
}

WorldRuntime::ChatConnection *WorldRuntime::chatConnectionById(long id) const
{
	const auto it = m_chatConnections.constFind(id);
	return it == m_chatConnections.constEnd() ? nullptr : it.value();
}

void WorldRuntime::removeChatConnection(ChatConnection *connection)
{
	if (!connection)
		return;
	const auto it = m_chatConnections.find(connection->id());
	if (it != m_chatConnections.end() && it.value() == connection)
		m_chatConnections.erase(it);
	connection->deleteLater();
}

int WorldRuntime::sendChatMessageToAll(int message, const QString &text, bool unlessIgnoring,
                                       bool incomingOnly, bool outgoingOnly, long exceptId,
                                       const QString &group, long stamp)
{
	int count = 0;
	for (ChatConnection *connection : chatConnections())
	{
		if (!connection || !connection->isConnected())
			continue;
		if (incomingOnly && !connection->isIncoming())
			continue;
		if (outgoingOnly && connection->isIncoming())
			continue;
		if (unlessIgnoring && connection->m_ignore)
			continue;
		if (exceptId != 0 && connection->id() == exceptId)
			continue;
		if (!group.isEmpty() && connection->m_group.compare(group, Qt::CaseInsensitive) != 0)
			continue;

		if (group.isEmpty())
			connection->m_countOutgoingAll++;
		else
			connection->m_countOutgoingGroup++;

		connection->sendChatMessage(message, text, stamp);
		++count;
	}

	if (count > 0)
	{
		if (message == kChatTextEverybody)
		{
			m_lastChatMessageSent = text;
			m_lastChatMessageTime = QDateTime::currentDateTime();
		}
		else if (message == kChatTextGroup)
		{
			m_lastChatGroupMessageSent = text;
			m_lastChatGroupMessageTime = QDateTime::currentDateTime();
		}
	}

	return count;
}

int WorldRuntime::chatAcceptCalls(short port)
{
	if (m_chatServer)
		return eChatAlreadyListening;

	if (port != 0)
		setWorldAttribute(QStringLiteral("chat_port"), QString::number(port));

	const int incomingPort      = chatIncomingPort();
	bool      hasConfiguredPort = false;
	const int configuredPort =
	    m_worldAttributes.value(QStringLiteral("chat_port")).trimmed().toInt(&hasConfiguredPort);
	if (!hasConfiguredPort || configuredPort == 0)
		setWorldAttribute(QStringLiteral("chat_port"), QString::number(incomingPort));

	if (!isEnabledFlag(m_worldAttributes.value(QStringLiteral("accept_chat_connections"))))
		setWorldAttribute(QStringLiteral("accept_chat_connections"), QStringLiteral("1"));

	auto chatServer = std::make_unique<QTcpServer>(this);
	if (!chatServer->listen(QHostAddress::Any, static_cast<quint16>(incomingPort)))
	{
		const QString errorText = QStringLiteral("Cannot accept calls on port %1, code = %2 (%3)")
		                              .arg(incomingPort)
		                              .arg(static_cast<int>(chatServer->serverError()))
		                              .arg(chatServer->errorString());
		chatNote(0, errorText);
		return eCannotCreateChatSocket;
	}

	m_chatServer = chatServer.release();
	connect(m_chatServer, &QTcpServer::newConnection, this,
	        [this]
	        {
		        chatNote(0, QStringLiteral("Incoming chat call"));
		        while (m_chatServer && m_chatServer->hasPendingConnections())
		        {
			        auto *socket = m_chatServer->nextPendingConnection();
			        if (!socket)
				        continue;
			        auto connection = std::make_unique<ChatConnection>(this, socket);
			        connection->startIncoming();
			        auto *accepted = connection.release();
			        m_chatConnections.insert(accepted->id(), accepted);
			        chatNote(0, QStringLiteral("Accepted call from %1 port %2")
			                        .arg(socket->peerAddress().toString())
			                        .arg(socket->peerPort()));
		        }
	        });

	chatNote(0, QStringLiteral("Listening for chat connections on port %1").arg(incomingPort));
	return eOK;
}

int WorldRuntime::chatCall(const QString &server, long port, bool zChat)
{
	const QString trimmed = server.trimmed();
	if (trimmed.isEmpty())
		return eBadParameter;

	auto socket     = std::make_unique<QTcpSocket>(this);
	auto connection = std::make_unique<ChatConnection>(this, socket.get());
	connection->startOutgoing(trimmed, static_cast<int>(port), zChat);
	const auto *releasedSocket = socket.release();
	auto       *released       = connection.release();
	m_chatConnections.insert(released->id(), released);
	Q_UNUSED(releasedSocket);
	return eOK;
}

int WorldRuntime::chatDisconnect(long id)
{
	ChatConnection *connection = chatConnectionById(id);
	if (!connection)
		return eChatIDNotFound;

	chatNote(0, QStringLiteral("Connection to %1 dropped.").arg(connection->m_remoteUserName));
	connection->close();
	return eOK;
}

int WorldRuntime::chatDisconnectAll()
{
	int count = 0;
	for (ChatConnection *connection : chatConnections())
	{
		if (!connection || connection->m_deleteMe)
			continue;
		count++;
		connection->close();
	}
	chatNote(0, QStringLiteral("%1 connection%2 closed.")
	                .arg(count)
	                .arg(count == 1 ? QString() : QStringLiteral("s")));
	return count;
}

int WorldRuntime::chatEverybody(const QString &message, bool emote)
{
	const QString bold    = QString::fromLatin1(ansiCode(kAnsiBold));
	const QString cyan    = QString::fromLatin1(ansiCode(kAnsiTextCyan));
	const QString red     = QString::fromLatin1(ansiCode(kAnsiTextRed));
	const QString ourName = chatOurName();

	QString       payload;
	if (emote)
		payload = QStringLiteral("\n%1%2%3 %4%5\n").arg(bold, cyan, ourName, message, red);
	else
		payload =
		    QStringLiteral("\n%1 chats to everybody, '%2%3%4%5'\n").arg(ourName, bold, cyan, message, red);

	const int count = sendChatMessageToAll(kChatTextEverybody, payload, true, false, false, 0, QString(), 0);
	if (count > 0)
	{
		if (emote)
			chatNote(
			    8,
			    QStringLiteral("You emote to everybody: %1%2%3 %4%5").arg(bold, cyan, ourName, message, red));
		else
			chatNote(8, QStringLiteral("You chat to everybody, '%1%2%3%4'").arg(bold, cyan, message, red));
		return eOK;
	}

	chatNote(8, QStringLiteral("No (relevant) chat connections."));
	return eNoChatConnections;
}

long WorldRuntime::chatGetId(const QString &who) const
{
	QString const trimmed = who.trimmed();
	if (trimmed.isEmpty())
		return 0;

	bool       ok = false;
	const long id = trimmed.toLong(&ok);
	if (ok)
	{
		if (chatConnectionById(id))
			return id;
		const_cast<WorldRuntime *>(this)->chatNote(13,
		                                           QStringLiteral("Chat ID %1 is not connected.").arg(id));
		return 0;
	}

	for (ChatConnection const *connection : chatConnections())
	{
		if (!connection || !connection->isConnected())
			continue;
		if (connection->m_remoteUserName.compare(trimmed, Qt::CaseInsensitive) == 0)
			return connection->id();
	}

	const_cast<WorldRuntime *>(this)->chatNote(13,
	                                           QStringLiteral("Cannot find connection \"%1\".").arg(trimmed));
	return 0;
}

int WorldRuntime::chatGroup(const QString &group, const QString &message, bool emote)
{
	if (group.trimmed().isEmpty())
		return eBadParameter;

	const QString bold       = QString::fromLatin1(ansiCode(kAnsiBold));
	const QString cyan       = QString::fromLatin1(ansiCode(kAnsiTextCyan));
	const QString red        = QString::fromLatin1(ansiCode(kAnsiTextRed));
	const QString ourName    = chatOurName();
	const QString groupLabel = group.leftJustified(15, QLatin1Char(' '));

	QString       payload;
	if (emote)
		payload = QStringLiteral("%1\nTo the group, %2%3%4 %5%6\n")
		              .arg(groupLabel, bold, cyan, ourName, message, red);
	else
		payload = QStringLiteral("%1\n%2 chats to the group, '%3%4%5%6'\n")
		              .arg(groupLabel, ourName, bold, cyan, message, red);

	const int count = sendChatMessageToAll(kChatTextGroup, payload, true, false, false, 0, group, 0);
	if (count > 0)
	{
		if (emote)
			chatNote(9, QStringLiteral("You emote to the group %1: %2%3%4 %5%6")
			                .arg(group, bold, cyan, ourName, message, red));
		else
			chatNote(
			    9,
			    QStringLiteral("You chat to the group %1, '%2%3%4%5'").arg(group, bold, cyan, message, red));
		return eOK;
	}

	chatNote(9, QStringLiteral("No chat connections in the group %1.").arg(group));
	return eNoChatConnections;
}

int WorldRuntime::chatId(long id, const QString &message, bool emote)
{
	if (id == 0)
		return eChatIDNotFound;

	ChatConnection *connection = chatConnectionById(id);
	if (!connection)
	{
		chatNote(7, QStringLiteral("Chat ID %1 is not connected.").arg(id));
		return eChatIDNotFound;
	}

	const QString bold    = QString::fromLatin1(ansiCode(kAnsiBold));
	const QString cyan    = QString::fromLatin1(ansiCode(kAnsiTextCyan));
	const QString red     = QString::fromLatin1(ansiCode(kAnsiTextRed));
	const QString ourName = chatOurName();

	QString       payload;
	if (emote)
		payload = QStringLiteral("\nTo you, %1%2%3 %4%5\n").arg(bold, cyan, ourName, message, red);
	else
		payload = QStringLiteral("\n%1 chats to you, '%2%3%4%5'\n").arg(ourName, bold, cyan, message, red);

	connection->m_countOutgoingPersonal++;
	connection->sendChatMessage(kChatTextPersonal, payload);

	if (emote)
		chatNote(7, QStringLiteral("You emote to %1: %2%3%4 %5%6")
		                .arg(connection->m_remoteUserName, bold, cyan, ourName, message, red));
	else
		chatNote(7, QStringLiteral("You chat to %1, '%2%3%4%5'")
		                .arg(connection->m_remoteUserName, bold, cyan, message, red));
	return eOK;
}

int WorldRuntime::chatMessage(long id, short message, const QString &text) const
{
	ChatConnection *connection = chatConnectionById(id);
	if (!connection)
		return eChatIDNotFound;
	connection->sendChatMessage(message, text);
	return eOK;
}

int WorldRuntime::chatNameChange(const QString &newName)
{
	if (newName.trimmed().isEmpty())
		return eBadParameter;
	QString oldName = m_worldAttributes.value(QStringLiteral("chat_name"));
	if (oldName.trimmed().isEmpty())
		oldName = QStringLiteral("<no name>");

	if (oldName != newName)
		setWorldAttribute(QStringLiteral("chat_name"), newName);

	chatNote(2, QStringLiteral("Your chat name changed from %1 to %2").arg(oldName, newName));

	sendChatMessageToAll(kChatNameChange, newName, false, false, false, 0, QString(), 0);
	return eOK;
}

void WorldRuntime::chatNote(short noteType, const QString &message)
{
	QString text = message;
	if (text.startsWith(QLatin1Char('\n')))
		text.remove(0, 1);
	if (text.endsWith(QLatin1Char('\n')))
		text.chop(1);

	const int maxBytes = chatMaxBytes();
	if (maxBytes > 0)
	{
		QByteArray bytes = text.toLatin1();
		if (bytes.size() > maxBytes)
		{
			bytes = bytes.left(maxBytes);
			text  = QString::fromLatin1(bytes);
			text += QStringLiteral("\n[Chat message truncated, exceeds %1 bytes]").arg(maxBytes);
		}
	}

	const int maxLines = chatMaxLines();
	if (maxLines > 0)
	{
		int lines = 0;
		for (int i = 0; i < text.size(); ++i)
		{
			if (text.at(i) != QLatin1Char('\n'))
				continue;
			if (++lines >= maxLines)
			{
				const bool truncated = (i + 1 < text.size());
				text                 = text.left(i);
				if (truncated)
					text += QStringLiteral("\n[Chat message truncated, exceeds %1 lines]").arg(maxLines);
				break;
			}
		}
	}

	if (!callPluginCallbacksStopOnFalseWithNumberAndString(QStringLiteral("OnPluginChatDisplay"), noteType,
	                                                       text))
		return;

	QString output = text;
	if (ignoreChatColours())
		output = qmudStripAnsiEscapeCodes(output);

	const QString prefix = m_worldAttributes.value(QStringLiteral("chat_message_prefix"));
	output               = prefix + output;

	long foreValue = chatForegroundColour();
	long backValue = chatBackgroundColour();
	if (foreValue == backValue)
	{
		foreValue = colorToLong(QColor(255, 0, 0));
		backValue = colorToLong(QColor(0, 0, 0));
	}
	const QColor  foreColor(lowByteToInt(foreValue), lowByteToInt(foreValue >> 8),
	                        lowByteToInt(foreValue >> 16));
	const QColor  backColor(lowByteToInt(backValue), lowByteToInt(backValue >> 8),
	                        lowByteToInt(backValue >> 16));
	const QString colourPrefix = QStringLiteral("\x1b[38;2;%1;%2;%3m\x1b[48;2;%4;%5;%6m")
	                                 .arg(foreColor.red())
	                                 .arg(foreColor.green())
	                                 .arg(foreColor.blue())
	                                 .arg(backColor.red())
	                                 .arg(backColor.green())
	                                 .arg(backColor.blue());
	const auto reset = QStringLiteral("\x1b[0m");
	outputAnsiText(colourPrefix + output + reset + QLatin1Char('\n'), true);
}

int WorldRuntime::chatPasteEverybody()
{
	const QString contents = QGuiApplication::clipboard()->text();
	if (contents.isEmpty())
		return eClipboardEmpty;

	const QString bold = QString::fromLatin1(ansiCode(kAnsiBold));
	const QString cyan = QString::fromLatin1(ansiCode(kAnsiTextCyan));
	const QString red  = QString::fromLatin1(ansiCode(kAnsiTextRed));

	const QString payload = QStringLiteral("\n%1 pastes to everybody: \n\n%2%3%4%5\n")
	                            .arg(chatOurName(), bold, cyan, contents, red);

	const int count = sendChatMessageToAll(kChatTextEverybody, payload, true, false, false, 0, QString(), 0);
	if (count > 0)
	{
		chatNote(8, QStringLiteral("You paste to everybody: \n\n%1%2%3%4").arg(bold, cyan, contents, red));
		return eOK;
	}

	chatNote(8, QStringLiteral("No (relevant) chat connections."));
	return eNoChatConnections;
}

int WorldRuntime::chatPasteText(long id)
{
	if (id == 0)
		return eChatIDNotFound;

	ChatConnection *connection = chatConnectionById(id);
	if (!connection)
	{
		chatNote(7, QStringLiteral("Chat ID %1 is not connected.").arg(id));
		return eChatIDNotFound;
	}

	const QString contents = QGuiApplication::clipboard()->text();
	if (contents.isEmpty())
		return eClipboardEmpty;

	const QString bold = QString::fromLatin1(ansiCode(kAnsiBold));
	const QString cyan = QString::fromLatin1(ansiCode(kAnsiTextCyan));
	const QString red  = QString::fromLatin1(ansiCode(kAnsiTextRed));

	const QString payload =
	    QStringLiteral("\n%1 pastes to you: \n\n%2%3%4%5\n").arg(chatOurName(), bold, cyan, contents, red);
	connection->sendChatMessage(kChatTextPersonal, payload);
	chatNote(7, QStringLiteral("You paste to %1: \n\n%2%3%4%5")
	                .arg(connection->m_remoteUserName, bold, cyan, contents, red));
	return eOK;
}

int WorldRuntime::chatPeekConnections(long id) const
{
	ChatConnection *connection = chatConnectionById(id);
	if (!connection)
		return eChatIDNotFound;
	connection->sendChatMessage(kChatPeekConnections, QString());
	return eOK;
}

int WorldRuntime::chatPersonal(const QString &who, const QString &message, bool emote)
{
	const QString trimmed = who.trimmed();
	if (trimmed.isEmpty())
		return eBadParameter;

	int count = 0;
	for (ChatConnection const *connection : chatConnections())
	{
		if (!connection || !connection->isConnected())
			continue;
		if (connection->m_remoteUserName.compare(trimmed, Qt::CaseInsensitive) != 0)
			continue;
		if (chatId(connection->id(), message, emote) == eOK)
			count++;
	}

	if (count == 0)
	{
		chatNote(7, QStringLiteral("%1 is not connected.").arg(trimmed));
		return eChatPersonNotFound;
	}
	if (count > 1)
		chatNote(7, QStringLiteral("%1 matches.").arg(count));
	return eOK;
}

int WorldRuntime::chatPing(long id) const
{
	ChatConnection *connection = chatConnectionById(id);
	if (!connection)
		return eChatIDNotFound;

	connection->m_pingTimer.start();
	const QString payload =
	    QDateTime::currentDateTime().toString(QStringLiteral("dddd, MMMM dd, yyyy, h:mm AP"));
	connection->sendChatMessage(kChatPingRequest, payload);
	return eOK;
}

int WorldRuntime::chatRequestConnections(long id) const
{
	ChatConnection *connection = chatConnectionById(id);
	if (!connection)
		return eChatIDNotFound;
	connection->sendChatMessage(kChatRequestConnections, QString());
	return eOK;
}

int WorldRuntime::chatSendFile(long id, const QString &path)
{
	if (id == 0)
		return eChatIDNotFound;

	ChatConnection *connection = chatConnectionById(id);
	if (!connection)
	{
		chatNote(14, QStringLiteral("Chat ID %1 is not connected.").arg(id));
		return eChatIDNotFound;
	}

	if (connection->m_doingFileTransfer)
	{
		if (connection->m_sendFile)
			chatNote(14, QStringLiteral("Already sending file %1").arg(connection->m_ourFileName));
		else
			chatNote(14, QStringLiteral("Already receiving file %1").arg(connection->m_ourFileName));
		return eAlreadyTransferringFile;
	}

	QString fileName = path;
	if (fileName.trimmed().isEmpty())
	{
		const QString initialDir = fileBrowsingDirectory();
		fileName = QFileDialog::getOpenFileName(nullptr, QStringLiteral("Select file to send"), initialDir,
		                                        QStringLiteral("All files (*.*)"));
		if (fileName.isEmpty())
			return eFileNotFound;
	}

	connection->m_ourFileName = fileName;
	QFileInfo const info(fileName);
	connection->m_senderFileName = info.fileName();

	connection->m_file = std::make_unique<QFile>(fileName);
	if (!connection->m_file->open(QIODevice::ReadOnly))
	{
		chatNote(14, QStringLiteral("File %1 cannot be opened.").arg(fileName));
		connection->m_file.reset();
		connection->m_ourFileName.clear();
		connection->m_fileSize = 0;
		return eFileNotFound;
	}

	connection->m_fileSize = static_cast<long>(connection->m_file->size());
	connection->m_fileBuffer.resize(connection->m_fileBlockSize);

	connection->sendChatMessage(
	    kChatFileStart,
	    QStringLiteral("%1,%2").arg(connection->m_senderFileName).arg(connection->m_fileSize));

	connection->m_startedFileTransfer = QDateTime::currentDateTime();
	connection->m_sendFile            = true;
	connection->m_doingFileTransfer   = true;
	connection->m_blocksTransferred   = 0;
	connection->m_fileBlocks =
	    (connection->m_fileSize + connection->m_fileBlockSize - 1L) / connection->m_fileBlockSize;

	connection->m_fileSha1.reset();
	const double kb = static_cast<double>(connection->m_fileSize) / 1024.0;
	chatNote(14, QStringLiteral("Initiated transfer of file %1, %2 bytes (%3 Kb).")
	                 .arg(fileName)
	                 .arg(connection->m_fileSize)
	                 .arg(kb, 0, 'f', 1));
	return eOK;
}

int WorldRuntime::chatStopAcceptingCalls()
{
	if (m_chatServer)
	{
		m_chatServer->close();
		m_chatServer->deleteLater();
		m_chatServer = nullptr;
		chatNote(0, QStringLiteral("Stopped accepting chat connections."));
		setWorldAttribute(QStringLiteral("accept_chat_connections"), QStringLiteral("0"));
	}
	return eOK;
}

int WorldRuntime::chatStopFileTransfer(long id)
{
	if (id == 0)
		return eChatIDNotFound;

	ChatConnection *connection = chatConnectionById(id);
	if (!connection)
	{
		chatNote(7, QStringLiteral("Chat ID %1 is not connected.").arg(id));
		return eChatIDNotFound;
	}

	if (!connection->m_doingFileTransfer)
		return eNotTransferringFile;

	connection->stopFileTransfer(true);
	return eOK;
}

QVariant WorldRuntime::chatInfo(long id, int infoType) const
{
	const ChatConnection *connection = chatConnectionById(id);
	if (!connection)
		return {};

	switch (infoType)
	{
	case 1:
		return connection->m_serverName;
	case 2:
		return connection->m_remoteUserName;
	case 3:
		return connection->m_group;
	case 4:
		return connection->m_remoteVersion;
	case 5:
		return connection->m_allegedAddress;
	case 6:
		return connection->m_serverAddress.toString();
	case 7:
		return connection->m_serverPort;
	case 8:
		return connection->m_allegedPort;
	case 9:
		return connection->m_chatStatus;
	case 10:
		return connection->m_chatConnectionType;
	case 11:
		return QVariant::fromValue(static_cast<qlonglong>(connection->m_chatId));
	case 12:
		return connection->m_incoming;
	case 13:
		return connection->m_canSnoop;
	case 14:
		return connection->m_youAreSnooping;
	case 15:
		return connection->m_heIsSnooping;
	case 16:
		return connection->m_canSendCommands;
	case 17:
		return connection->m_private;
	case 18:
		return connection->m_canSendFiles;
	case 19:
		return connection->m_ignore;
	case 20:
		return connection->m_lastPingTime;
	case 21:
		return connection->m_whenStarted.isValid() ? QVariant(toOleDate(connection->m_whenStarted))
		                                           : QVariant();
	case 22:
		return connection->m_lastIncoming.isValid() ? QVariant(toOleDate(connection->m_lastIncoming))
		                                            : QVariant();
	case 23:
		return connection->m_lastOutgoing.isValid() ? QVariant(toOleDate(connection->m_lastOutgoing))
		                                            : QVariant();
	case 24:
		return connection->m_startedFileTransfer.isValid()
		           ? QVariant(toOleDate(connection->m_startedFileTransfer))
		           : QVariant();
	case 25:
		return connection->m_doingFileTransfer;
	case 26:
		return connection->m_sendFile;
	case 27:
		return connection->m_senderFileName;
	case 28:
		return connection->m_ourFileName;
	case 29:
		return QVariant::fromValue(static_cast<qlonglong>(connection->m_fileSize));
	case 30:
		return QVariant::fromValue(static_cast<qlonglong>(connection->m_fileBlocks));
	case 31:
		return QVariant::fromValue(static_cast<qlonglong>(connection->m_blocksTransferred));
	case 32:
		return QVariant::fromValue(static_cast<qlonglong>(connection->m_fileBlockSize));
	case 33:
		return QVariant::fromValue(static_cast<qlonglong>(connection->m_countIncomingPersonal));
	case 34:
		return QVariant::fromValue(static_cast<qlonglong>(connection->m_countIncomingAll));
	case 35:
		return QVariant::fromValue(static_cast<qlonglong>(connection->m_countIncomingGroup));
	case 36:
		return QVariant::fromValue(static_cast<qlonglong>(connection->m_countOutgoingPersonal));
	case 37:
		return QVariant::fromValue(static_cast<qlonglong>(connection->m_countOutgoingAll));
	case 38:
		return QVariant::fromValue(static_cast<qlonglong>(connection->m_countOutgoingGroup));
	case 39:
		return QVariant::fromValue(static_cast<qlonglong>(connection->m_countMessages));
	case 40:
		return QVariant::fromValue(static_cast<qlonglong>(connection->m_countFileBytesIn));
	case 41:
		return QVariant::fromValue(static_cast<qlonglong>(connection->m_countFileBytesOut));
	case 42:
		return QVariant::fromValue(static_cast<qlonglong>(connection->m_zChatStamp));
	case 43:
		return connection->m_emailAddress;
	case 44:
		return connection->m_pgpKey;
	case 45:
		return connection->m_zChatStatus;
	case 46:
		return QVariant::fromValue(static_cast<qlonglong>(connection->m_userOption));
	default:
		break;
	}
	return {};
}

QList<long> WorldRuntime::chatList() const
{
	QList<long> result;
	for (ChatConnection const *connection : chatConnections())
	{
		if (connection && connection->isConnected())
			result.push_back(connection->id());
	}
	return result;
}

QVariant WorldRuntime::chatOption(long id, const QString &optionName) const
{
	const ChatConnection *connection = chatConnectionById(id);
	if (!connection)
		return {};

	const QString name = optionName.trimmed().toLower();
	if (name == QStringLiteral("can_send_commands"))
		return connection->m_canSendCommands;
	if (name == QStringLiteral("can_send_files"))
		return connection->m_canSendFiles;
	if (name == QStringLiteral("can_snoop"))
		return connection->m_canSnoop;
	if (name == QStringLiteral("ignore"))
		return connection->m_ignore;
	if (name == QStringLiteral("served"))
		return connection->m_incoming;
	if (name == QStringLiteral("private"))
		return connection->m_private;
	if (name == QStringLiteral("user"))
		return QVariant::fromValue(static_cast<qlonglong>(connection->m_userOption));
	if (name == QStringLiteral("server"))
		return connection->m_serverName;
	if (name == QStringLiteral("username"))
		return connection->m_remoteUserName;
	if (name == QStringLiteral("group"))
		return connection->m_group;
	if (name == QStringLiteral("version"))
		return connection->m_remoteVersion;
	if (name == QStringLiteral("address"))
		return connection->m_allegedAddress;
	return {};
}

int WorldRuntime::chatSetOption(long id, const QString &optionName, const QString &value)
{
	ChatConnection *connection = chatConnectionById(id);
	if (!connection)
		return eChatIDNotFound;

	const QString name    = optionName.trimmed().toLower();
	const QString trimmed = value.trimmed();

	auto          parseBool = [&](bool &ok) -> long
	{
		if (trimmed.compare(QStringLiteral("y"), Qt::CaseInsensitive) == 0)
		{
			ok = true;
			return 1;
		}
		if (trimmed.compare(QStringLiteral("n"), Qt::CaseInsensitive) == 0)
		{
			ok = true;
			return 0;
		}
		bool       numericOk = false;
		const long num       = trimmed.toLong(&numericOk);
		ok                   = numericOk;
		return num;
	};

	if (name == QStringLiteral("can_send_commands") || name == QStringLiteral("can_send_files") ||
	    name == QStringLiteral("can_snoop") || name == QStringLiteral("ignore") ||
	    name == QStringLiteral("served") || name == QStringLiteral("private") ||
	    name == QStringLiteral("user"))
	{
		bool       ok      = false;
		const long numeric = parseBool(ok);
		if (!ok)
			return eOptionOutOfRange;

		bool changed = false;
		if (name == QStringLiteral("can_send_commands"))
		{
			changed                       = connection->m_canSendCommands != (numeric != 0);
			connection->m_canSendCommands = (numeric != 0);
			if (changed)
				connection->sendChatMessage(kChatMessage, QStringLiteral("You can %1send %2 commands")
				                                              .arg(connection->m_canSendCommands
				                                                       ? QStringLiteral("now ")
				                                                       : QStringLiteral("no longer "))
				                                              .arg(chatOurName()));
		}
		else if (name == QStringLiteral("can_send_files"))
		{
			changed                    = connection->m_canSendFiles != (numeric != 0);
			connection->m_canSendFiles = (numeric != 0);
			if (changed)
				connection->sendChatMessage(kChatMessage, QStringLiteral("You can %1send %2 files")
				                                              .arg(connection->m_canSendFiles
				                                                       ? QStringLiteral("now ")
				                                                       : QStringLiteral("no longer "))
				                                              .arg(chatOurName()));
		}
		else if (name == QStringLiteral("can_snoop"))
		{
			changed                = connection->m_canSnoop != (numeric != 0);
			connection->m_canSnoop = (numeric != 0);
			if (changed)
				connection->sendChatMessage(
				    kChatMessage,
				    QStringLiteral("You can %1snoop %2")
				        .arg(connection->m_canSnoop ? QStringLiteral("now ") : QStringLiteral("no longer "))
				        .arg(chatOurName()));
		}
		else if (name == QStringLiteral("ignore"))
		{
			changed              = connection->m_ignore != (numeric != 0);
			connection->m_ignore = (numeric != 0);
			if (changed)
				connection->sendChatMessage(
				    kChatMessage,
				    QStringLiteral("%1 is %2ignoring you")
				        .arg(chatOurName(), connection->m_ignore ? QString() : QStringLiteral("no longer ")));
		}
		else if (name == QStringLiteral("served"))
		{
			changed                = connection->m_incoming != (numeric != 0);
			connection->m_incoming = (numeric != 0);
		}
		else if (name == QStringLiteral("private"))
		{
			changed               = connection->m_private != (numeric != 0);
			connection->m_private = (numeric != 0);
			if (changed)
				connection->sendChatMessage(
				    kChatMessage, QStringLiteral("%1 has marked your connection as %2")
				                      .arg(chatOurName(), connection->m_private ? QStringLiteral("private")
				                                                                : QStringLiteral("public")));
		}
		else if (name == QStringLiteral("user"))
		{
			changed                  = connection->m_userOption != numeric;
			connection->m_userOption = numeric;
		}

		Q_UNUSED(changed);
		return eOK;
	}

	if (name == QStringLiteral("group"))
	{
		const bool changed  = connection->m_group != trimmed;
		connection->m_group = trimmed;
		if (changed)
		{
			if (!trimmed.isEmpty())
				connection->sendChatMessage(
				    kChatMessage,
				    QStringLiteral("%1 has added you to the group %2").arg(chatOurName(), trimmed));
			else
				connection->sendChatMessage(
				    kChatMessage,
				    QStringLiteral("%1 has removed you from the chat group").arg(chatOurName()));
		}
		return eOK;
	}

	if (name == QStringLiteral("server") || name == QStringLiteral("username") ||
	    name == QStringLiteral("version") || name == QStringLiteral("address"))
		return ePluginCannotSetOption;

	return eUnknownOption;
}

QVariant WorldRuntime::pluginInfo(const QString &pluginId, int infoType) const
{
	const int index = findPluginIndex(m_plugins, pluginId);
	if (index < 0)
		return {};

	const Plugin &plugin = m_plugins.at(index);
	switch (infoType)
	{
	case 1:
		return plugin.attributes.value(QStringLiteral("name"));
	case 2:
		return plugin.attributes.value(QStringLiteral("author"));
	case 3:
		return plugin.description;
	case 4:
		return plugin.script;
	case 5:
		return plugin.attributes.value(QStringLiteral("language"));
	case 6:
		return plugin.source;
	case 7:
		return plugin.attributes.value(QStringLiteral("id"));
	case 8:
		return plugin.attributes.value(QStringLiteral("purpose"));
	case 9:
		return plugin.triggers.size();
	case 10:
		return plugin.aliases.size();
	case 11:
		return plugin.timers.size();
	case 12:
		return plugin.variables.size();
	case 13:
		return plugin.dateWritten.isValid() ? QVariant(plugin.dateWritten) : QVariant();
	case 14:
		return plugin.dateModified.isValid() ? QVariant(plugin.dateModified) : QVariant();
	case 15:
		return plugin.saveState;
	case 16:
		return plugin.lua != nullptr;
	case 17:
		return plugin.enabled;
	case 18:
		return plugin.requiredVersion;
	case 19:
		return plugin.version;
	case 20:
	{
		QString dir = plugin.directory;
		if (!dir.isEmpty() && !dir.endsWith('/') && !dir.endsWith('\\'))
			dir += '/';
#ifdef Q_OS_WIN
		return QDir::toNativeSeparators(dir);
#else
		dir.replace(QLatin1Char('\\'), QLatin1Char('/'));
		return dir;
#endif
	}
	case 21:
		return index + 1;
	case 22:
		return plugin.dateInstalled.isValid() ? QVariant(plugin.dateInstalled) : QVariant();
	case 23:
		return plugin.callingPluginId;
	case 24:
		return 0.0;
	case 25:
		return plugin.sequence;
	default:
		break;
	}
	return {};
}

QStringList WorldRuntime::pluginIdList() const
{
	QStringList ids;
	for (const Plugin &plugin : m_plugins)
	{
		const QString id = plugin.attributes.value(QStringLiteral("id"));
		if (!id.isEmpty())
			ids.push_back(id);
	}
	return ids;
}

QString WorldRuntime::pluginVariableValue(const QString &pluginId, const QString &name) const
{
	QString value;
	if (findPluginVariable(pluginId, name, value))
		return value;
	return {};
}

bool WorldRuntime::findPluginVariable(const QString &pluginId, const QString &name, QString &value) const
{
	const int index = findPluginIndex(m_plugins, pluginId);
	if (index < 0)
		return false;
	const Plugin &plugin = m_plugins.at(index);
	for (auto it = plugin.variables.constBegin(); it != plugin.variables.constEnd(); ++it)
	{
		if (it.key().compare(name, Qt::CaseInsensitive) == 0)
		{
			value = it.value();
			return true;
		}
	}
	return false;
}

void WorldRuntime::setPluginVariableValue(const QString &pluginId, const QString &name, const QString &value)
{
	const int index = findPluginIndex(m_plugins, pluginId);
	if (index < 0 || name.isEmpty())
		return;
	Plugin &plugin = m_plugins[index];
	for (auto it = plugin.variables.begin(); it != plugin.variables.end(); ++it)
	{
		if (it.key().compare(name, Qt::CaseInsensitive) == 0)
		{
			it.value() = value;
			return;
		}
	}
	plugin.variables.insert(name, value);
}

QStringList WorldRuntime::pluginVariableList(const QString &pluginId) const
{
	const int index = findPluginIndex(m_plugins, pluginId);
	if (index < 0)
		return {};
	const Plugin &plugin = m_plugins.at(index);
	return plugin.variables.keys();
}

QStringList WorldRuntime::pluginTriggerList(const QString &pluginId) const
{
	const int index = findPluginIndex(m_plugins, pluginId);
	if (index < 0)
		return {};
	QStringList names;
	for (const auto &trigger : m_plugins.at(index).triggers)
	{
		const QString name = trigger.attributes.value(QStringLiteral("name"));
		if (!name.isEmpty())
			names.push_back(name);
	}
	return names;
}

QStringList WorldRuntime::pluginAliasList(const QString &pluginId) const
{
	const int index = findPluginIndex(m_plugins, pluginId);
	if (index < 0)
		return {};
	QStringList names;
	for (const auto &alias : m_plugins.at(index).aliases)
	{
		const QString name = alias.attributes.value(QStringLiteral("name"));
		if (!name.isEmpty())
			names.push_back(name);
	}
	return names;
}

QStringList WorldRuntime::pluginTimerList(const QString &pluginId) const
{
	const int index = findPluginIndex(m_plugins, pluginId);
	if (index < 0)
		return {};
	QStringList names;
	for (const auto &timer : m_plugins.at(index).timers)
	{
		const QString name = timer.attributes.value(QStringLiteral("name"));
		if (!name.isEmpty())
			names.push_back(name);
	}
	return names;
}

namespace
{
	QString normalizeLabel(const QString &name)
	{
		return name.trimmed().toLower();
	}

	bool resolveWildcardValue(const QStringList &wildcards, const QMap<QString, QString> &namedWildcards,
	                          const QString &wildcardName, QString &value)
	{
		if (wildcardName.isEmpty())
			return false;
		bool      okNumber = false;
		const int index    = wildcardName.toInt(&okNumber);
		if (okNumber)
		{
			if (index >= 0 && index < wildcards.size())
			{
				value = wildcards.at(index);
				return true;
			}
			return false;
		}
		if (namedWildcards.contains(wildcardName))
		{
			value = namedWildcards.value(wildcardName);
			return true;
		}
		return false;
	}
} // namespace

void WorldRuntime::callPluginCallbacksWithTwoNumbersAndString(const QString &functionName, long arg1,
                                                              long arg2, const QString &arg3)
{
	if (!hasAnyPluginCallback(functionName))
		return;

	for (auto &plugin : m_plugins)
	{
		if (!canExecutePlugin(plugin))
			continue;
		plugin.lua->callFunctionWithTwoNumbersAndString(functionName, arg1, arg2, arg3, nullptr, true);
	}
}

void WorldRuntime::setTriggerWildcards(const QString &triggerName, const QStringList &wildcards,
                                       const QMap<QString, QString> &namedWildcards)
{
	const QString key = normalizeLabel(triggerName);
	if (key.isEmpty())
		return;
	m_triggerWildcards.insert(key, wildcards);
	m_triggerNamedWildcards.insert(key, namedWildcards);
}

void WorldRuntime::setAliasWildcards(const QString &aliasName, const QStringList &wildcards,
                                     const QMap<QString, QString> &namedWildcards)
{
	const QString key = normalizeLabel(aliasName);
	if (key.isEmpty())
		return;
	m_aliasWildcards.insert(key, wildcards);
	m_aliasNamedWildcards.insert(key, namedWildcards);
}

bool WorldRuntime::triggerWildcard(const QString &triggerName, const QString &wildcardName,
                                   QString &value) const
{
	const QString key = normalizeLabel(triggerName);
	if (key.isEmpty() || !m_triggerWildcards.contains(key))
		return false;
	const QStringList            wildcards = m_triggerWildcards.value(key);
	const QMap<QString, QString> named     = m_triggerNamedWildcards.value(key);
	return resolveWildcardValue(wildcards, named, wildcardName, value);
}

bool WorldRuntime::aliasWildcard(const QString &aliasName, const QString &wildcardName, QString &value) const
{
	const QString key = normalizeLabel(aliasName);
	if (key.isEmpty() || !m_aliasWildcards.contains(key))
		return false;
	const QStringList            wildcards = m_aliasWildcards.value(key);
	const QMap<QString, QString> named     = m_aliasNamedWildcards.value(key);
	return resolveWildcardValue(wildcards, named, wildcardName, value);
}

void WorldRuntime::setPluginTriggerWildcards(const QString &pluginId, const QString &triggerName,
                                             const QStringList            &wildcards,
                                             const QMap<QString, QString> &namedWildcards)
{
	const int index = findPluginIndex(m_plugins, pluginId);
	if (index < 0)
		return;
	Plugin       &plugin = m_plugins[index];
	const QString key    = normalizeLabel(triggerName);
	if (key.isEmpty())
		return;
	plugin.triggerWildcards.insert(key, wildcards);
	plugin.triggerNamedWildcards.insert(key, namedWildcards);
}

void WorldRuntime::setPluginAliasWildcards(const QString &pluginId, const QString &aliasName,
                                           const QStringList            &wildcards,
                                           const QMap<QString, QString> &namedWildcards)
{
	const int index = findPluginIndex(m_plugins, pluginId);
	if (index < 0)
		return;
	Plugin       &plugin = m_plugins[index];
	const QString key    = normalizeLabel(aliasName);
	if (key.isEmpty())
		return;
	plugin.aliasWildcards.insert(key, wildcards);
	plugin.aliasNamedWildcards.insert(key, namedWildcards);
}

bool WorldRuntime::pluginTriggerWildcard(const QString &pluginId, const QString &triggerName,
                                         const QString &wildcardName, QString &value) const
{
	const int index = findPluginIndex(m_plugins, pluginId);
	if (index < 0)
		return false;
	const Plugin &plugin = m_plugins.at(index);
	const QString key    = normalizeLabel(triggerName);
	if (key.isEmpty() || !plugin.triggerWildcards.contains(key))
		return false;
	const QStringList            wildcards = plugin.triggerWildcards.value(key);
	const QMap<QString, QString> named     = plugin.triggerNamedWildcards.value(key);
	return resolveWildcardValue(wildcards, named, wildcardName, value);
}

bool WorldRuntime::pluginAliasWildcard(const QString &pluginId, const QString &aliasName,
                                       const QString &wildcardName, QString &value) const
{
	const int index = findPluginIndex(m_plugins, pluginId);
	if (index < 0)
		return false;
	const Plugin &plugin = m_plugins.at(index);
	const QString key    = normalizeLabel(aliasName);
	if (key.isEmpty() || !plugin.aliasWildcards.contains(key))
		return false;
	const QStringList            wildcards = plugin.aliasWildcards.value(key);
	const QMap<QString, QString> named     = plugin.aliasNamedWildcards.value(key);
	return resolveWildcardValue(wildcards, named, wildcardName, value);
}

WorldRuntime::Plugin *WorldRuntime::pluginForId(const QString &pluginId)
{
	const int index = findPluginIndex(m_plugins, pluginId);
	if (index < 0)
		return nullptr;
	return &m_plugins[index];
}

const WorldRuntime::Plugin *WorldRuntime::pluginForId(const QString &pluginId) const
{
	const int index = findPluginIndex(m_plugins, pluginId);
	if (index < 0)
		return nullptr;
	return &m_plugins[index];
}

int WorldRuntime::savePluginState(const QString &pluginId, bool scripted, QString *error)
{
	const QString trimmed = pluginId.trimmed();
	if (trimmed.isEmpty())
		return eNotAPlugin;
	const int index = findPluginIndex(m_plugins, trimmed);
	if (index < 0)
		return eNoSuchPlugin;
	return savePluginStateForPlugin(m_plugins[index], scripted, error);
}

int WorldRuntime::savePluginStateForPlugin(Plugin &plugin, bool scripted, QString *error) const
{
	if (!scripted && !plugin.saveState)
		return eOK;
	if (plugin.savingStateNow)
		return ePluginCouldNotSaveState;
	if (m_stateFilesDirectory.isEmpty())
		return scripted ? ePluginCouldNotSaveState : eOK;
	const QString worldId = m_worldAttributes.value(QStringLiteral("id")).trimmed();
	if (worldId.isEmpty())
		return scripted ? ePluginCouldNotSaveState : eOK;
	const QString pluginId = plugin.attributes.value(QStringLiteral("id")).trimmed();
	if (pluginId.isEmpty())
		return scripted ? ePluginCouldNotSaveState : eOK;

	QString base = m_stateFilesDirectory;
	if (!base.endsWith('/') && !base.endsWith('\\'))
		base += '/';
	const QDir baseDir(base);
	if (!baseDir.exists())
		return scripted ? ePluginCouldNotSaveState : eOK;

	plugin.savingStateNow = true;
	if (plugin.lua)
		plugin.lua->callFunctionNoArgs(QStringLiteral("OnPluginSaveState"));
	plugin.savingStateNow = false;

	const QString stateFile = base + worldId + "-" + pluginId + "-state.xml";
	QSaveFile     file(stateFile);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
	{
		if (error)
			*error = QStringLiteral("Unable to create plugin state file: %1").arg(stateFile);
		return scripted ? ePluginCouldNotSaveState : eOK;
	}

	QTextStream out(&file);
	out.setEncoding(QStringConverter::Utf8);
	const auto      nl = QStringLiteral("\r\n");

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

	for (auto it = plugin.variables.constBegin(); it != plugin.variables.constEnd(); ++it)
	{
		out << "  <variable name=\"" << fixHtmlString(it.key()) << "\">" << fixHtmlMultilineString(it.value())
		    << "</variable>" << nl;
	}

	out << "</variables>" << nl;
	out << "</qmud>" << nl;

	if (!file.commit())
	{
		if (error)
			*error = QStringLiteral("Unable to write plugin state file: %1").arg(stateFile);
		return scripted ? ePluginCouldNotSaveState : eOK;
	}

	return eOK;
}

void WorldRuntime::sortPluginsBySequence()
{
	std::ranges::stable_sort(m_plugins,
	                         [](const Plugin &a, const Plugin &b) { return a.sequence < b.sequence; });
}

const QList<WorldRuntime::Include> &WorldRuntime::includes() const
{
	return m_includes;
}

const QList<WorldRuntime::Script> &WorldRuntime::scripts() const
{
	return m_scripts;
}

QString WorldRuntime::comments() const
{
	return m_comments;
}

void WorldRuntime::addLine(const QString &text, int flags, bool hardReturn, const QDateTime &time)
{
	LineEntry entry;
	entry.text       = text;
	entry.flags      = flags;
	entry.hardReturn = hardReturn;
	entry.spans.clear();
	entry.time       = time;
	entry.lineNumber = m_nextLineNumber++;
	if (m_lineTimer.isValid())
	{
		entry.ticks   = static_cast<double>(m_lineTimer.nsecsElapsed()) / 1000000000.0;
		entry.elapsed = entry.ticks;
	}
	else
	{
		const double fallback = (m_worldStartTime.isValid() && time.isValid())
		                            ? static_cast<double>(m_worldStartTime.msecsTo(time)) / 1000.0
		                            : 0.0;
		entry.ticks           = fallback;
		entry.elapsed         = fallback;
	}
	m_lines.push_back(entry);
	enforceOutputLineLimit();
}

void WorldRuntime::addLine(const QString &text, int flags, const QVector<StyleSpan> &spans, bool hardReturn,
                           const QDateTime &time)
{
	LineEntry entry;
	entry.text       = text;
	entry.flags      = flags;
	entry.hardReturn = hardReturn;
	entry.spans      = spans;
	entry.time       = time;
	entry.lineNumber = m_nextLineNumber++;
	if (m_lineTimer.isValid())
	{
		entry.ticks   = static_cast<double>(m_lineTimer.nsecsElapsed()) / 1000000000.0;
		entry.elapsed = entry.ticks;
	}
	else
	{
		const double fallback = (m_worldStartTime.isValid() && time.isValid())
		                            ? static_cast<double>(m_worldStartTime.msecsTo(time)) / 1000.0
		                            : 0.0;
		entry.ticks           = fallback;
		entry.elapsed         = fallback;
	}
	m_lines.push_back(entry);
	enforceOutputLineLimit();
}

int WorldRuntime::maxOutputLinesLimit() const
{
	bool      ok         = false;
	const int configured = m_worldAttributes.value(QStringLiteral("max_output_lines")).toInt(&ok);
	if (!ok || configured <= 0)
		return 0;
	return qBound(0, configured, 500000);
}

void WorldRuntime::enforceOutputLineLimit()
{
	const int maxLines = maxOutputLinesLimit();
	if (maxLines <= 0 || m_lines.size() <= maxLines)
		return;

	const int removed = safeQSizeToInt(m_lines.size() - maxLines);
	m_lines.erase(m_lines.begin(), m_lines.begin() + removed);

	if (m_lastGoTo > m_lines.size())
		m_lastGoTo = qMax(1, safeQSizeToInt(m_lines.size()));

	if (!m_luaContextLineActive)
		return;

	if (!m_luaContextLineBuffered)
	{
		m_luaContextLineBufferIndex = safeQSizeToInt(m_lines.size() + 1);
		return;
	}

	if (m_luaContextLineBufferIndex > removed)
	{
		m_luaContextLineBufferIndex -= removed;
		return;
	}

	if (m_lines.isEmpty())
	{
		m_luaContextLineBuffered    = false;
		m_luaContextLineBufferIndex = 0;
		return;
	}

	m_luaContextLineBufferIndex = 1;
	m_luaContextLineEntry       = m_lines.first();
}

const QVector<WorldRuntime::LineEntry> &WorldRuntime::lines() const
{
	return m_lines;
}

void WorldRuntime::replaceOutputLines(const QVector<LineEntry> &lines)
{
	m_lines = lines;

	qint64 maxLineNumber = 0;
	for (const LineEntry &line : m_lines)
		maxLineNumber = qMax(maxLineNumber, line.lineNumber);
	m_nextLineNumber = qMax<qint64>(1, maxLineNumber + 1);

	enforceOutputLineLimit();
	if (m_view)
		m_view->restoreOutputFromPersistedLines(m_lines);
}

void WorldRuntime::finalizePendingInputLineHardReturn()
{
	if (m_lines.isEmpty())
		return;

	LineEntry &last = m_lines.last();
	if (last.hardReturn)
		return;
	if ((last.flags & LineInput) == 0)
		return;

	last.hardReturn = true;
}

void WorldRuntime::clearLastLineHardReturn()
{
	if (m_lines.isEmpty())
		return;

	LineEntry &last = m_lines.last();
	if (!last.hardReturn)
		return;

	last.hardReturn = false;
}

void WorldRuntime::beginIncomingLineLuaContext(const QString &text, int flags,
                                               const QVector<StyleSpan> &spans, bool hardReturn)
{
	m_luaContextLineEntry.text       = text;
	m_luaContextLineEntry.flags      = flags;
	m_luaContextLineEntry.hardReturn = hardReturn;
	m_luaContextLineEntry.spans      = spans;
	m_luaContextLineEntry.time       = QDateTime::currentDateTime();
	m_luaContextLineEntry.lineNumber = m_nextLineNumber;
	if (m_lineTimer.isValid())
	{
		m_luaContextLineEntry.ticks   = static_cast<double>(m_lineTimer.nsecsElapsed()) / 1000000000.0;
		m_luaContextLineEntry.elapsed = m_luaContextLineEntry.ticks;
	}
	else
	{
		const double fallback =
		    (m_worldStartTime.isValid() && m_luaContextLineEntry.time.isValid())
		        ? static_cast<double>(m_worldStartTime.msecsTo(m_luaContextLineEntry.time)) / 1000.0
		        : 0.0;
		m_luaContextLineEntry.ticks   = fallback;
		m_luaContextLineEntry.elapsed = fallback;
	}
	m_luaContextLineBuffered    = false;
	m_luaContextLineCommitted   = false;
	m_luaContextLineBufferIndex = safeQSizeToInt(m_lines.size() + 1);
	m_luaContextLineActive      = true;
}

void WorldRuntime::endIncomingLineLuaContext()
{
	m_luaContextLineBuffered    = false;
	m_luaContextLineCommitted   = false;
	m_luaContextLineBufferIndex = 0;
	m_luaContextLineActive      = false;
}

void WorldRuntime::markIncomingLineLuaContextBuffered()
{
	if (!m_luaContextLineActive)
		return;
	m_luaContextLineBuffered    = true;
	m_luaContextLineBufferIndex = safeQSizeToInt(m_lines.size());
	if (m_luaContextLineBufferIndex > 0 && m_luaContextLineBufferIndex <= m_lines.size())
		m_luaContextLineEntry = m_lines.at(m_luaContextLineBufferIndex - 1);
}

void WorldRuntime::markIncomingLineLuaContextCommitted()
{
	if (!m_luaContextLineActive)
		return;
	m_luaContextLineCommitted = true;
}

bool WorldRuntime::luaContextLinePresentInBuffer() const
{
	return m_luaContextLineActive && m_luaContextLineBuffered;
}

bool WorldRuntime::luaContextLineEntry(int lineNumber, LineEntry &entry) const
{
	if (lineNumber <= 0)
		return false;

	const bool pendingLineVisible  = m_luaContextLineActive && !m_luaContextLineCommitted;
	const bool pendingLineBuffered = pendingLineVisible && luaContextLinePresentInBuffer();
	if (pendingLineVisible && pendingLineBuffered && lineNumber == m_luaContextLineBufferIndex)
	{
		entry = m_luaContextLineEntry;
		return true;
	}

	if (pendingLineVisible && !pendingLineBuffered && lineNumber == m_lines.size() + 1)
	{
		entry            = m_luaContextLineEntry;
		entry.lineNumber = m_nextLineNumber;
		return true;
	}

	if (lineNumber <= m_lines.size())
	{
		entry = m_lines.at(lineNumber - 1);
		return true;
	}
	return false;
}

int WorldRuntime::luaContextLinesInBufferCount() const
{
	const bool pendingLineVisible = m_luaContextLineActive && !m_luaContextLineCommitted;
	if (const bool pendingLineBuffered = pendingLineVisible && luaContextLinePresentInBuffer();
	    pendingLineVisible && pendingLineBuffered)
	{
		// During trigger/deferred Lua callbacks, keep GetLinesInBufferCount()
		// pinned to the currently-processed incoming line even if callback-side
		// output appends additional lines before scripts finish.
		return m_luaContextLineBufferIndex;
	}
	return safeQSizeToInt(m_lines.size()) + (pendingLineVisible ? 1 : 0);
}

int WorldRuntime::windowCreate(const QString &name, int left, int top, int width, int height, int position,
                               int flags, const QColor &background, const QString &pluginId)
{
	if (name.isEmpty())
		return eNoNameSpecified;
	if (width < 0 || height < 0)
		return eBadParameter;

	auto it = m_miniWindows.find(name);
	if (it == m_miniWindows.end())
	{
		MiniWindow window;
		window.name = name;
		it          = m_miniWindows.insert(name, window);
	}

	MiniWindow &window = it.value();
	window.name        = name;
	window.create(left, top, width, height, position, flags, background);
	window.creatingPlugin = pluginId;
	if ((flags & kMiniWindowKeepHotspots) == 0)
	{
		window.hotspots.clear();
		window.callbackPlugin.clear();
		window.mouseOverHotspot.clear();
		window.mouseDownHotspot.clear();
		window.flagsOnMouseDown = 0;
	}

	emit miniWindowsChanged();
	return eOK;
}

int WorldRuntime::windowShow(const QString &name, bool show)
{
	const auto it = m_miniWindows.find(name);
	if (it == m_miniWindows.end())
		return eNoSuchWindow;
	it.value().show = show;
	emit miniWindowsChanged();
	return eOK;
}

int WorldRuntime::windowDelete(const QString &name)
{
	const auto it = m_miniWindows.find(name);
	if (it == m_miniWindows.end())
		return eNoSuchWindow;
	if (it.value().executingScript)
		return eItemInUse;
	m_miniWindows.erase(it);
	emit miniWindowsChanged();
	return eOK;
}

QStringList WorldRuntime::windowList() const
{
	return m_miniWindows.keys();
}

QVariant WorldRuntime::windowInfo(const QString &name, int infoType) const
{
	const MiniWindow *window = miniWindow(name);
	if (!window)
		return {};

	switch (infoType)
	{
	case 1:
		return window->location.x();
	case 2:
		return window->location.y();
	case 3:
		return window->width;
	case 4:
		return window->height;
	case 5:
		return window->show;
	case 6:
		return window->temporarilyHide;
	case 7:
		return window->position;
	case 8:
		return window->flags;
	case 9:
		if (window->background.isValid())
			return QVariant::fromValue<qlonglong>(colorToRef(window->background));
		return 0;
	case 10:
		return window->rect.left();
	case 11:
		return window->rect.top();
	case 12:
		return window->rect.right();
	case 13:
		return window->rect.bottom();
	case 14:
		return window->lastMousePosition.x();
	case 15:
		return window->lastMousePosition.y();
	case 16:
		return window->lastMouseUpdate;
	case 17:
		return window->clientMousePosition.x();
	case 18:
		return window->clientMousePosition.y();
	case 19:
		return window->mouseOverHotspot;
	case 20:
		return window->mouseDownHotspot;
	case 21:
		return window->installedAt.isValid() ? QVariant(toOleDate(window->installedAt)) : QVariant();
	case 22:
		return window->zOrder;
	case 23:
		return window->creatingPlugin;
	default:
		return {};
	}
}

MiniWindow *WorldRuntime::miniWindow(const QString &name)
{
	const auto it = m_miniWindows.find(name);
	if (it == m_miniWindows.end())
		return nullptr;
	return &it.value();
}

const MiniWindow *WorldRuntime::miniWindow(const QString &name) const
{
	const auto it = m_miniWindows.find(name);
	if (it == m_miniWindows.end())
		return nullptr;
	return &it.value();
}

QVector<MiniWindow *> WorldRuntime::sortedMiniWindows()
{
	QVector<MiniWindow *> ordered;
	ordered.reserve(m_miniWindows.size());
	for (auto it = m_miniWindows.begin(); it != m_miniWindows.end(); ++it)
		ordered.push_back(&it.value());

	std::ranges::sort(ordered,
	                  [](const MiniWindow *a, const MiniWindow *b)
	                  {
		                  if (a->zOrder == b->zOrder)
			                  return a->name < b->name;
		                  return a->zOrder < b->zOrder;
	                  });

	return ordered;
}

void WorldRuntime::layoutMiniWindows(const QSize &clientSize, const QSize &ownerSize, bool underneath,
                                     const QVector<MiniWindow *> *orderedWindows)
{
	const int             clientWidth  = clientSize.width();
	const int             clientHeight = clientSize.height();
	const int             ownerWidth   = ownerSize.width();
	const int             ownerHeight  = ownerSize.height();
	QVector<MiniWindow *> fallbackWindows;
	if (!orderedWindows)
	{
		fallbackWindows = sortedMiniWindows();
		orderedWindows  = &fallbackWindows;
	}
	const QVector<MiniWindow *> &windows = *orderedWindows;

	int                          absoluteMaxRight  = 0;
	int                          absoluteMaxBottom = 0;
	bool                         sawAbsolute       = false;
	for (MiniWindow const *window : windows)
	{
		if (!window || !window->show)
			continue;
		const bool drawUnder = (window->flags & kMiniWindowDrawUnderneath) != 0;
		if (drawUnder != underneath)
			continue;
		if ((window->flags & kMiniWindowAbsoluteLocation) == 0)
			continue;
		sawAbsolute = true;

		absoluteMaxRight  = qMax(absoluteMaxRight, window->location.x() + qMax(0, window->width));
		absoluteMaxBottom = qMax(absoluteMaxBottom, window->location.y() + qMax(0, window->height));
	}

	int       &referenceRight  = underneath ? m_absoluteReferenceRightUnder : m_absoluteReferenceRightOver;
	int       &referenceBottom = underneath ? m_absoluteReferenceBottomUnder : m_absoluteReferenceBottomOver;
	const bool captureActive   = m_view && m_view->isMiniWindowCaptureActive();
	if (!sawAbsolute)
	{
		referenceRight  = 0;
		referenceBottom = 0;
	}
	else if (captureActive)
	{
		// Keep drag scaling stable while a miniwindow is being moved/resized.
		referenceRight  = qMax(referenceRight, absoluteMaxRight);
		referenceBottom = qMax(referenceBottom, absoluteMaxBottom);
	}
	else
	{
		// Outside drag, use current extents to avoid stale startup/session anchors.
		referenceRight  = absoluteMaxRight;
		referenceBottom = absoluteMaxBottom;
	}

	double absoluteScaleX        = 1.0;
	double absoluteScaleY        = 1.0;
	bool   forceUnscaledAbsolute = false;
	if (AppController const *app = AppController::instance())
	{
		if (app->getGlobalOption(QStringLiteral("DisableWindowScaler")).toInt() != 0)
			forceUnscaledAbsolute = true;
	}
	if (m_view)
	{
		if (QWidget const *rootWindow = m_view->window(); rootWindow)
			forceUnscaledAbsolute = forceUnscaledAbsolute || rootWindow->isMaximized();
	}
	if (!forceUnscaledAbsolute)
	{
		if (clientWidth > 0 && referenceRight > clientWidth)
			absoluteScaleX = static_cast<double>(clientWidth) / static_cast<double>(referenceRight);
		if (clientHeight > 0 && referenceBottom > clientHeight)
			absoluteScaleY = static_cast<double>(clientHeight) / static_cast<double>(referenceBottom);
	}

	auto scaledAbsoluteRect = [clientWidth, clientHeight, absoluteScaleX,
	                           absoluteScaleY](const MiniWindow *window) -> QRect
	{
		if (!window || clientWidth <= 0 || clientHeight <= 0)
			return {};

		const int canonicalWidth  = qMax(0, window->width);
		const int canonicalHeight = qMax(0, window->height);

		int       scaledWidth  = qRound(static_cast<double>(canonicalWidth) * absoluteScaleX);
		int       scaledHeight = qRound(static_cast<double>(canonicalHeight) * absoluteScaleY);
		if (canonicalWidth > 0 && scaledWidth <= 0)
			scaledWidth = 1;
		if (canonicalHeight > 0 && scaledHeight <= 0)
			scaledHeight = 1;
		if (scaledWidth > clientWidth)
			scaledWidth = clientWidth;
		if (scaledHeight > clientHeight)
			scaledHeight = clientHeight;

		int scaledLeft = qRound(static_cast<double>(window->location.x()) * absoluteScaleX);
		int scaledTop  = qRound(static_cast<double>(window->location.y()) * absoluteScaleY);

		if (scaledLeft < 0)
			scaledLeft = 0;
		if (scaledTop < 0)
			scaledTop = 0;

		if (scaledLeft + scaledWidth > clientWidth)
			scaledLeft = qMax(0, clientWidth - scaledWidth);
		if (scaledTop + scaledHeight > clientHeight)
			scaledTop = qMax(0, clientHeight - scaledHeight);

		return {scaledLeft, scaledTop, scaledWidth, scaledHeight};
	};

	QPoint                topLeft(0, 0);
	QPoint                topRight(clientWidth, 0);
	QPoint                bottomLeft(0, clientHeight);
	QPoint                bottomRight(clientWidth, clientHeight);

	int                   topWidths    = 0;
	int                   rightHeights = 0;
	int                   bottomWidths = 0;
	int                   leftHeights  = 0;

	QVector<MiniWindow *> topWindows;
	QVector<MiniWindow *> rightWindows;
	QVector<MiniWindow *> bottomWindows;
	QVector<MiniWindow *> leftWindows;

	for (MiniWindow *window : windows)
	{
		window->temporarilyHide = false;

		if (!window->show)
			continue;

		const bool drawUnder = (window->flags & kMiniWindowDrawUnderneath) != 0;
		if (drawUnder != underneath)
			continue;

		const int w = window->width;
		const int h = window->height;

		if (window->flags & kMiniWindowAbsoluteLocation)
		{
			// Keep canonical plugin coordinates in window->location, but fit absolute
			// windows proportionally into the current client area when downsized.
			window->rect = scaledAbsoluteRect(window);
			continue;
		}

		switch (window->position)
		{
		case 0:
			window->rect = QRect(0, 0, clientWidth, clientHeight);
			continue;
		case 1:
			if (h > 0)
			{
				const double ratio = static_cast<double>(w) / static_cast<double>(h);
				window->rect       = QRect(0, 0, static_cast<int>(clientHeight * ratio), clientHeight);
			}
			else
				window->rect = QRect(0, 0, clientWidth, clientHeight);
			continue;
		case 2:
			window->rect = QRect(0, 0, ownerWidth, ownerHeight);
			continue;
		case 3:
			if (h > 0)
			{
				const double ratio = static_cast<double>(w) / static_cast<double>(h);
				window->rect       = QRect(0, 0, static_cast<int>(ownerHeight * ratio), ownerHeight);
			}
			else
				window->rect = QRect(0, 0, ownerWidth, ownerHeight);
			continue;
		case 4:
			window->rect = QRect(0, 0, w, h);
			topLeft.setX(qMax(topLeft.x(), window->rect.x() + window->rect.width()));
			topLeft.setY(qMax(topLeft.y(), window->rect.y() + window->rect.height()));
			break;
		case 5:
			topWidths += w;
			topWindows.push_back(window);
			break;
		case 6:
			window->rect = QRect(clientWidth - w, 0, w, h);
			topRight.setX(qMin(topRight.x(), window->rect.x()));
			topRight.setY(qMax(topRight.y(), window->rect.y() + window->rect.height()));
			break;
		case 7:
			rightHeights += h;
			rightWindows.push_back(window);
			break;
		case 8:
			window->rect = QRect(clientWidth - w, clientHeight - h, w, h);
			bottomRight.setX(qMin(bottomRight.x(), window->rect.x()));
			bottomRight.setY(qMin(bottomRight.y(), window->rect.y()));
			break;
		case 9:
			bottomWidths += w;
			bottomWindows.push_back(window);
			break;
		case 10:
			window->rect = QRect(0, clientHeight - h, w, h);
			bottomLeft.setX(qMax(bottomLeft.x(), window->rect.x() + window->rect.width()));
			bottomLeft.setY(qMin(bottomLeft.y(), window->rect.y()));
			break;
		case 11:
			leftHeights += h;
			leftWindows.push_back(window);
			break;
		case 12:
			window->rect = QRect((clientWidth - w) / 2, (clientHeight - h) / 2, w, h);
			break;
		default:
			break;
		}
	}

	const int topRoom    = topRight.x() - topLeft.x();
	const int rightRoom  = bottomRight.y() - topRight.y();
	const int bottomRoom = bottomRight.x() - bottomLeft.x();
	const int leftRoom   = bottomLeft.y() - topLeft.y();

	auto discardOverflow = [](QVector<MiniWindow *> &dockedWindows, int &totalSize, int room, bool vertical)
	{
		while (!dockedWindows.isEmpty() && totalSize > room)
		{
			MiniWindow *window = dockedWindows.back();
			dockedWindows.removeLast();
			totalSize -= vertical ? window->height : window->width;
			window->temporarilyHide = true;
		}
	};

	discardOverflow(topWindows, topWidths, topRoom, false);
	discardOverflow(rightWindows, rightHeights, rightRoom, true);
	discardOverflow(bottomWindows, bottomWidths, bottomRoom, false);
	discardOverflow(leftWindows, leftHeights, leftRoom, true);

	auto distribute = [](QVector<MiniWindow *> &dockedWindows, int room, int startPos, bool vertical,
	                     int fixedX, int fixedY, bool alignRight, bool alignBottom)
	{
		if (dockedWindows.isEmpty())
			return;
		int totalSize = 0;
		for (const MiniWindow *window : dockedWindows)
			totalSize += vertical ? window->height : window->width;
		const int gap   = (room - totalSize) / (safeQSizeToInt(dockedWindows.size()) + 1);
		int       start = startPos + gap;
		for (MiniWindow *window : dockedWindows)
		{
			if (vertical)
			{
				const int x  = alignRight ? fixedX - window->width : fixedX;
				window->rect = QRect(x, start, window->width, window->height);
				start += window->height + gap;
			}
			else
			{
				const int y  = alignBottom ? fixedY - window->height : fixedY;
				window->rect = QRect(start, y, window->width, window->height);
				start += window->width + gap;
			}
		}
	};

	distribute(topWindows, topRoom, topLeft.x(), false, 0, 0, false, false);
	distribute(rightWindows, rightRoom, topRight.y(), true, clientWidth, 0, true, false);
	distribute(bottomWindows, bottomRoom, bottomLeft.x(), false, 0, clientHeight, false, true);
	distribute(leftWindows, leftRoom, topLeft.y(), true, 0, 0, false, false);
}

int WorldRuntime::windowRectOp(const QString &name, int action, int left, int top, int right, int bottom,
                               long colour1, long colour2)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;

	QImage &surface = window->surface;
	if (surface.isNull())
		return eOK;

	const QRect rect = rectFromCoords(*window, left, top, right, bottom);
	if (rect.width() <= 0 || rect.height() <= 0)
		return eOK;

	switch (action)
	{
	case 1: // frame
	{
		QPainter painter(&surface);
		painter.setPen(QPen(colorFromRef(colour1)));
		painter.setBrush(Qt::NoBrush);
		painter.drawRect(rect.adjusted(0, 0, -1, -1));
		break;
	}
	case 2: // fill
	{
		QPainter painter(&surface);
		painter.fillRect(rect, colorFromRef(colour1));
		break;
	}
	case 3: // invert
	{
		const QRect clipped = rect.intersected(surface.rect());
		for (int y = clipped.top(); y <= clipped.bottom(); ++y)
		{
			auto *line = reinterpret_cast<QRgb *>(surface.scanLine(y));
			for (int x = clipped.left(); x <= clipped.right(); ++x)
			{
				const QColor current(line[x]);
				line[x] =
				    qRgba(255 - current.red(), 255 - current.green(), 255 - current.blue(), current.alpha());
			}
		}
		break;
	}
	case 4: // 3D rect
	{
		QPainter     painter(&surface);
		const QColor topLeftColour     = colorFromRef(colour1);
		const QColor bottomRightColour = colorFromRef(colour2);
		painter.setPen(topLeftColour);
		painter.drawLine(rect.topLeft(), rect.topRight());
		painter.drawLine(rect.topLeft(), rect.bottomLeft());
		painter.setPen(bottomRightColour);
		painter.drawLine(rect.bottomLeft(), rect.bottomRight());
		painter.drawLine(rect.topRight(), rect.bottomRight());
		break;
	}
	case 5: // draw edge
	{
		if (colour1 != kEdgeRaised && colour1 != kEdgeEtched && colour1 != kEdgeBump &&
		    colour1 != kEdgeSunken)
			return eBadParameter;
		if ((colour2 & 0xFF) > 0x1F)
			return eBadParameter;

		const QColor base  = window->background.isValid() ? window->background : QColor(64, 64, 64);
		QColor       light = base.lighter(150);
		QColor       dark  = base.darker(150);
		if (colour1 == kEdgeSunken || colour1 == kEdgeEtched)
			qSwap(light, dark);

		bool const drawTop    = (colour2 & (kBfTopLeft | kBfTopRight | kBfRect)) != 0;
		bool const drawBottom = (colour2 & (kBfBottomLeft | kBfBottomRight | kBfRect)) != 0;
		bool const drawLeft   = (colour2 & (kBfTopLeft | kBfBottomLeft | kBfRect)) != 0;
		bool const drawRight  = (colour2 & (kBfTopRight | kBfBottomRight | kBfRect)) != 0;

		QPainter   painter(&surface);
		if (colour2 & kBfMiddle)
			painter.fillRect(rect.adjusted(1, 1, -1, -1), base);

		if (drawTop)
		{
			painter.setPen(light);
			painter.drawLine(rect.topLeft(), rect.topRight());
		}
		if (drawLeft)
		{
			painter.setPen(light);
			painter.drawLine(rect.topLeft(), rect.bottomLeft());
		}
		if (drawBottom)
		{
			painter.setPen(dark);
			painter.drawLine(rect.bottomLeft(), rect.bottomRight());
		}
		if (drawRight)
		{
			painter.setPen(dark);
			painter.drawLine(rect.topRight(), rect.bottomRight());
		}

		if (colour1 == kEdgeEtched || colour1 == kEdgeBump)
		{
			QRect const inner = rect.adjusted(1, 1, -1, -1);
			if (inner.width() > 1 && inner.height() > 1)
			{
				painter.setPen(dark);
				painter.drawLine(inner.topLeft(), inner.topRight());
				painter.drawLine(inner.topLeft(), inner.bottomLeft());
				painter.setPen(light);
				painter.drawLine(inner.bottomLeft(), inner.bottomRight());
				painter.drawLine(inner.topRight(), inner.bottomRight());
			}
		}
		break;
	}
	case 6: // flood fill border
	case 7: // flood fill surface
	{
		const QColor borderColour = colorFromRef(colour1);
		const QColor fillColour   = colorFromRef(colour2);
		const QPoint start(left, top);
		if (!surface.rect().contains(start))
			return eOK;

		const QRgb borderRgb = borderColour.rgb();
		const QRgb fillRgb   = fillColour.rgb();
		const QRgb startRgb  = surface.pixel(start);
		if (action == 7 && (startRgb & 0x00FFFFFF) != (borderRgb & 0x00FFFFFF))
			return eOK;
		if (action == 6 && (startRgb & 0x00FFFFFF) == (borderRgb & 0x00FFFFFF))
			return eOK;

		QVector<QPoint> stack;
		stack.reserve(surface.width() * surface.height() / 8);
		stack.push_back(start);
		while (!stack.isEmpty())
		{
			QPoint const p = stack.back();
			stack.pop_back();
			if (!surface.rect().contains(p))
				continue;
			const QRgb current     = surface.pixel(p);
			const bool matchBorder = ((current & 0x00FFFFFF) == (borderRgb & 0x00FFFFFF));
			if (action == 6)
			{
				if (matchBorder || (current & 0x00FFFFFF) == (fillRgb & 0x00FFFFFF))
					continue;
			}
			else
			{
				if (!matchBorder || (current & 0x00FFFFFF) == (fillRgb & 0x00FFFFFF))
					continue;
			}
			surface.setPixel(p, qRgba(fillColour.red(), fillColour.green(), fillColour.blue(), 0xFF));
			stack.push_back(QPoint(p.x() + 1, p.y()));
			stack.push_back(QPoint(p.x() - 1, p.y()));
			stack.push_back(QPoint(p.x(), p.y() + 1));
			stack.push_back(QPoint(p.x(), p.y() - 1));
		}
		break;
	}
	default:
		return eUnknownOption;
	}

	emit miniWindowsChanged();
	return eOK;
}

int WorldRuntime::windowCircleOp(const QString &name, int action, int left, int top, int right, int bottom,
                                 long penColour, long penStyle, int penWidth, long brushColour,
                                 long brushStyle, int extra1, int extra2, int extra3, int extra4)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;

	QImage &surface = window->surface;
	if (surface.isNull())
		return eOK;

	const QRect rect = rectFromCoords(*window, left, top, right, bottom);
	if (rect.width() <= 0 || rect.height() <= 0)
		return eOK;

	if (validatePenStyle(penStyle, penWidth) != eOK)
		return ePenStyleNotValid;
	if (validateBrushStyle(brushStyle) != eOK)
		return eBrushStyleNotValid;

	QPainter painter(&surface);
	QPen     pen(colorFromRef(penColour));
	pen.setWidth(qMax(1, penWidth));
	pen.setStyle(mapPenStyle(penStyle));
	pen.setCapStyle(mapPenCap(penStyle));
	pen.setJoinStyle(mapPenJoin(penStyle));
	painter.setPen(penStyle == 5 ? Qt::NoPen : pen);

	bool         brushOk = true;
	QBrush const brush   = MiniWindowBrushUtils::makeBrush(brushStyle, penColour, brushColour, &brushOk);
	if (!brushOk)
		return eBrushStyleNotValid;
	painter.setBrush(brush);

	switch (action)
	{
	case 1:
		painter.drawEllipse(rect);
		break;
	case 2:
		painter.drawRect(rect.adjusted(0, 0, -1, -1));
		break;
	case 3:
		painter.drawRoundedRect(rect, extra1, extra2, Qt::AbsoluteSize);
		break;
	case 4:
	case 5:
	{
		const QPointF center     = rect.center();
		const double  startAngle = QLineF(center, QPointF(extra1, extra2)).angle();
		const double  endAngle   = QLineF(center, QPointF(extra3, extra4)).angle();
		double        span       = endAngle - startAngle;
		if (span <= 0)
			span += 360.0;
		const int start  = static_cast<int>(startAngle * 16.0);
		const int span16 = static_cast<int>(span * 16.0);
		if (action == 4)
			painter.drawChord(rect, start, span16);
		else
			painter.drawPie(rect, start, span16);
	}
	break;
	default:
		return eUnknownOption;
	}

	emit miniWindowsChanged();
	return eOK;
}

int WorldRuntime::windowLine(const QString &name, int x1, int y1, int x2, int y2, long penColour,
                             long penStyle, int penWidth)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;

	QImage &surface = window->surface;
	if (surface.isNull())
		return eOK;

	if (validatePenStyle(penStyle, penWidth) != eOK)
		return ePenStyleNotValid;

	QPainter painter(&surface);
	QPen     pen(colorFromRef(penColour));
	pen.setWidth(qMax(1, penWidth));
	pen.setStyle(mapPenStyle(penStyle));
	pen.setCapStyle(mapPenCap(penStyle));
	pen.setJoinStyle(mapPenJoin(penStyle));
	painter.setPen(penStyle == 5 ? Qt::NoPen : pen);
	painter.drawLine(x1, y1, x2, y2);
	emit miniWindowsChanged();
	return eOK;
}

int WorldRuntime::windowArc(const QString &name, int left, int top, int right, int bottom, int x1, int y1,
                            int x2, int y2, long penColour, long penStyle, int penWidth)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;

	QImage &surface = window->surface;
	if (surface.isNull())
		return eOK;

	if (validatePenStyle(penStyle, penWidth) != eOK)
		return ePenStyleNotValid;

	const QRect rect = rectFromCoords(*window, left, top, right, bottom);
	QPainter    painter(&surface);
	QPen        pen(colorFromRef(penColour));
	pen.setWidth(qMax(1, penWidth));
	pen.setStyle(mapPenStyle(penStyle));
	pen.setCapStyle(mapPenCap(penStyle));
	pen.setJoinStyle(mapPenJoin(penStyle));
	painter.setPen(penStyle == 5 ? Qt::NoPen : pen);

	const QPointF center     = rect.center();
	const int     endX       = window->fixRight(x2);
	const int     endY       = window->fixBottom(y2);
	const double  startAngle = QLineF(center, QPointF(x1, y1)).angle();
	const double  endAngle   = QLineF(center, QPointF(endX, endY)).angle();
	double        span       = endAngle - startAngle;
	if (span <= 0)
		span += 360.0;
	painter.drawArc(rect, static_cast<int>(startAngle * 16.0), static_cast<int>(span * 16.0));
	emit miniWindowsChanged();
	return eOK;
}

int WorldRuntime::windowBezier(const QString &name, const QString &points, long penColour, long penStyle,
                               int penWidth)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;

	QVector<double>   coords;
	const QStringList parts = points.split(',', Qt::SkipEmptyParts);
	coords.reserve(parts.size());
	for (const QString &part : parts)
		coords.push_back(part.trimmed().toDouble());

	if (coords.size() < 8 || (coords.size() % 6) != 2)
		return eInvalidNumberOfPoints;

	if (validatePenStyle(penStyle, penWidth) != eOK)
		return ePenStyleNotValid;

	QPainter painter(&window->surface);
	QPen     pen(colorFromRef(penColour));
	pen.setWidth(qMax(1, penWidth));
	pen.setStyle(mapPenStyle(penStyle));
	pen.setCapStyle(mapPenCap(penStyle));
	pen.setJoinStyle(mapPenJoin(penStyle));
	painter.setPen(penStyle == 5 ? Qt::NoPen : pen);
	painter.setBrush(Qt::NoBrush);

	QPainterPath path;
	path.moveTo(coords[0], coords[1]);
	for (int i = 2; i + 5 < coords.size(); i += 6)
	{
		path.cubicTo(coords[i], coords[i + 1], coords[i + 2], coords[i + 3], coords[i + 4], coords[i + 5]);
	}
	painter.drawPath(path);
	emit miniWindowsChanged();
	return eOK;
}

int WorldRuntime::windowPolygon(const QString &name, const QString &points, long penColour, long penStyle,
                                int penWidth, long brushColour, long brushStyle, bool closePolygon,
                                bool winding)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;

	QVector<QPointF>  pts;
	const QStringList parts = points.split(',', Qt::SkipEmptyParts);
	if (parts.size() < 4 || (parts.size() % 2) != 0)
		return eInvalidNumberOfPoints;

	for (int i = 0; i < parts.size(); i += 2)
	{
		const double x = parts.at(i).trimmed().toDouble();
		const double y = parts.at(i + 1).trimmed().toDouble();
		pts.push_back(QPointF(x, y));
	}

	if (validatePenStyle(penStyle, penWidth) != eOK)
		return ePenStyleNotValid;
	if (validateBrushStyle(brushStyle) != eOK)
		return eBrushStyleNotValid;

	QPainter painter(&window->surface);
	QPen     pen(colorFromRef(penColour));
	pen.setWidth(qMax(1, penWidth));
	pen.setStyle(mapPenStyle(penStyle));
	pen.setCapStyle(mapPenCap(penStyle));
	pen.setJoinStyle(mapPenJoin(penStyle));
	painter.setPen(penStyle == 5 ? Qt::NoPen : pen);
	bool         brushOk = true;
	QBrush const brush   = MiniWindowBrushUtils::makeBrush(brushStyle, penColour, brushColour, &brushOk);
	if (!brushOk)
		return eBrushStyleNotValid;
	painter.setBrush(brush);

	if (closePolygon)
		painter.drawPolygon(pts.constData(), safeQSizeToInt(pts.size()),
		                    winding ? Qt::WindingFill : Qt::OddEvenFill);
	else
		painter.drawPolyline(pts.constData(), safeQSizeToInt(pts.size()));

	emit miniWindowsChanged();
	return eOK;
}

int WorldRuntime::windowGradient(const QString &name, int left, int top, int right, int bottom,
                                 long startColour, long endColour, int mode)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;

	QImage &surface = window->surface;
	if (surface.isNull())
		return eOK;

	const QRect rect = rectFromCoords(*window, left, top, right, bottom);
	if (rect.width() <= 0 || rect.height() <= 0)
		return eOK;

	QPainter painter(&surface);
	if (mode == 3)
	{
		QImage       texture(rect.size(), QImage::Format_ARGB32);
		const QColor mult = colorFromRef(startColour);
		for (int y = 0; y < texture.height(); ++y)
		{
			auto *line = reinterpret_cast<QRgb *>(texture.scanLine(y));
			for (int x = 0; x < texture.width(); ++x)
			{
				const int c = (x ^ y) & 0xFF;
				line[x] = qRgb((c * mult.red()) & 0xFF, (c * mult.green()) & 0xFF, (c * mult.blue()) & 0xFF);
			}
		}
		painter.drawImage(rect.topLeft(), texture);
	}
	else
	{
		const QColor    start = colorFromRef(startColour);
		const QColor    end   = colorFromRef(endColour);
		QLinearGradient gradient;
		if (mode == 2)
			gradient = QLinearGradient(rect.left(), rect.top(), rect.left(), rect.bottom());
		else
			gradient = QLinearGradient(rect.left(), rect.top(), rect.right(), rect.top());
		gradient.setColorAt(0.0, start);
		gradient.setColorAt(1.0, end);
		painter.fillRect(rect, gradient);
	}

	emit miniWindowsChanged();
	return eOK;
}

int WorldRuntime::windowFont(const QString &name, const QString &fontId, const QString &fontName, double size,
                             bool bold, bool italic, bool underline, bool strikeout, int charset,
                             int pitchAndFamily)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;

	if (fontName.isEmpty() && size == 0.0)
	{
		window->fonts.remove(fontId);
		return eOK;
	}

	QFont font;
	if (!fontName.isEmpty())
		qmudApplyMonospaceFallback(font, fontName);
	const QString preferredFamily = fontName.isEmpty() ? font.family() : fontName;
	const QString charsetFamily   = qmudFamilyForCharset(preferredFamily, charset);
	if (!charsetFamily.isEmpty())
		qmudApplyMonospaceFallback(font, charsetFamily);
	const double pointSize = size > 0.0 ? size : 10.0;
	font.setPointSizeF(pointSize);
	font.setBold(bold);
	font.setItalic(italic);
	font.setUnderline(underline);
	font.setStrikeOut(strikeout);

	if ((pitchAndFamily & 0x03) == 0x01)
		font.setFixedPitch(true);

	switch (pitchAndFamily & 0xF0)
	{
	case 0x10:
		font.setStyleHint(QFont::Serif);
		break;
	case 0x20:
		font.setStyleHint(QFont::SansSerif);
		break;
	case 0x30:
		font.setStyleHint(QFont::TypeWriter);
		break;
	case 0x40:
	case 0x50:
		font.setStyleHint(QFont::Decorative);
		break;
	default:
		font.setStyleHint(QFont::AnyStyle);
		break;
	}

	MiniWindowFont entry;
	entry.font    = font;
	entry.metrics = QFontMetrics(font);
	window->fonts.insert(fontId, entry);
	return eOK;
}

QVariant WorldRuntime::windowFontInfo(const QString &name, const QString &fontId, int infoType) const
{
	const MiniWindow *window = miniWindow(name);
	if (!window)
		return {};
	const auto it = window->fonts.find(fontId);
	if (it == window->fonts.end())
		return {};

	const QFontMetrics &metrics = it.value().metrics;
	const QFont        &font    = it.value().font;

	switch (infoType)
	{
	case 1:
		return metrics.height();
	case 2:
		return metrics.ascent();
	case 3:
		return metrics.descent();
	case 4:
		return 0;
	case 5:
		return metrics.leading();
	case 6:
		return metrics.averageCharWidth();
	case 7:
		return metrics.maxWidth();
	case 8:
		return font.weight();
	case 9:
		return 0;
	case 10:
		if (const QScreen *screen = QGuiApplication::primaryScreen())
			return static_cast<int>(screen->logicalDotsPerInchX());
		return 0;
	case 11:
		if (const QScreen *screen = QGuiApplication::primaryScreen())
			return static_cast<int>(screen->logicalDotsPerInchY());
		return 0;
	case 12:
		return 32;
	case 13:
		return 255;
	case 14:
		return 63;
	case 15:
		return 32;
	case 16:
		return font.italic() ? 1 : 0;
	case 17:
		return font.underline() ? 1 : 0;
	case 18:
		return font.strikeOut() ? 1 : 0;
	case 19:
	{
		const int pitch  = font.fixedPitch() ? 0x01 : 0x02;
		int       family = 0x00;
		switch (font.styleHint())
		{
		case QFont::Serif:
			family = 0x10;
			break;
		case QFont::SansSerif:
			family = 0x20;
			break;
		case QFont::TypeWriter:
			family = 0x30;
			break;
		case QFont::Decorative:
			family = 0x50;
			break;
		default:
			family = 0x00;
			break;
		}
		return pitch | family;
	}
	case 20:
		return 0;
	case 21:
		return font.family();
	default:
		return {};
	}
}

QStringList WorldRuntime::windowFontList(const QString &name) const
{
	const MiniWindow *window = miniWindow(name);
	if (!window)
		return {};
	return window->fonts.keys();
}

int WorldRuntime::windowText(const QString &name, const QString &fontId, const QString &text, int left,
                             int top, int right, int bottom, long colour)
{
	if (name.isEmpty())
		return -1;
	MiniWindow *window = miniWindow(name);
	if (!window)
		return -1;

	const auto it = window->fonts.find(fontId);
	if (it == window->fonts.end())
		return -2;

	QImage &surface = window->surface;
	if (surface.isNull())
		return eOK;

	if (text.isEmpty())
		return 0;

	const QRect rect = rectFromCoords(*window, left, top, right, bottom);
	if (rect.width() <= 0 || rect.height() <= 0)
		return eOK;

	QPainter painter(&surface);
	painter.setFont(it.value().font);
	painter.setPen(colorFromRef(colour));
	painter.setClipRect(rect);
	painter.drawText(rect, Qt::AlignLeft | Qt::AlignTop, text);

	const int width = it.value().metrics.horizontalAdvance(text);
	emit      miniWindowsChanged();
	return qMin(width, rect.width());
}

int WorldRuntime::windowTextWidth(const QString &name, const QString &fontId, const QString &text) const
{
	if (name.isEmpty())
		return -1;
	const MiniWindow *window = miniWindow(name);
	if (!window)
		return -1;
	const auto it = window->fonts.find(fontId);
	if (it == window->fonts.end())
		return -2;
	return it.value().metrics.horizontalAdvance(text);
}

int WorldRuntime::windowSetPixel(const QString &name, int x, int y, long colour)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;
	QImage &surface = window->surface;
	if (!surface.rect().contains(x, y))
		return eOK;
	const QColor c = colorFromRef(colour);
	surface.setPixel(x, y, qRgba(c.red(), c.green(), c.blue(), 0xFF));
	emit miniWindowsChanged();
	return eOK;
}

QVariant WorldRuntime::windowGetPixel(const QString &name, int x, int y) const
{
	if (name.isEmpty())
		return {static_cast<qlonglong>(-2)};
	const MiniWindow *window = miniWindow(name);
	if (!window)
		return {static_cast<qlonglong>(-2)};
	const QImage &surface = window->surface;
	if (!surface.rect().contains(x, y))
		return {static_cast<qlonglong>(-1)};
	const QColor colour(surface.pixel(x, y));
	return {static_cast<qlonglong>(colorToRef(colour))};
}

int WorldRuntime::windowCreateImage(const QString &name, const QString &imageId, long row1, long row2,
                                    long row3, long row4, long row5, long row6, long row7, long row8)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;

	const long      rows[8] = {row1, row2, row3, row4, row5, row6, row7, row8};
	MiniWindowImage img;
	img.image = QImage(8, 8, QImage::Format_ARGB32);
	for (int y = 0; y < 8; ++y)
	{
		const long row = rows[y];
		for (int x = 0; x < 8; ++x)
		{
			const bool bit = (row >> (7 - x)) & 0x01;
			img.image.setPixel(x, y, bit ? qRgba(255, 255, 255, 255) : qRgba(0, 0, 0, 255));
		}
	}

	img.hasAlpha   = false;
	img.monochrome = true;
	window->images.insert(imageId, img);
	return eOK;
}

int WorldRuntime::windowLoadImage(const QString &name, const QString &imageId, const QString &filename)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;

	const QString trimmed = filename.trimmed();
	if (trimmed.isEmpty())
	{
		window->images.remove(imageId);
		return eOK;
	}

	const QString lower = trimmed.toLower();
	if (!lower.endsWith(QStringLiteral(".bmp")) && !lower.endsWith(QStringLiteral(".png")))
		return eBadParameter;

	if (!QFileInfo::exists(trimmed))
		return eFileNotFound;

	QImage image;
	if (!image.load(trimmed))
		return eUnableToLoadImage;
	const bool isMonochrome =
	    (image.format() == QImage::Format_Mono || image.format() == QImage::Format_MonoLSB);

	MiniWindowImage entry;
	entry.image      = image.convertToFormat(QImage::Format_ARGB32);
	entry.source     = trimmed;
	entry.hasAlpha   = entry.image.hasAlphaChannel();
	entry.monochrome = isMonochrome;
	window->images.insert(imageId, entry);
	return eOK;
}

int WorldRuntime::windowLoadImageMemory(const QString &name, const QString &imageId, const QByteArray &data,
                                        bool hasAlpha)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;

	QImage image;
	bool   decodedHasAlpha = false;
	bool   isMonochrome    = false;
	if (!qmudLookupMemoryImageDecodeCache(m_memoryImageDecodeCache, data, image, decodedHasAlpha,
	                                      isMonochrome))
	{
		const QImage decoded = QImage::fromData(data);
		if (decoded.isNull())
			return eUnableToLoadImage;
		isMonochrome =
		    (decoded.format() == QImage::Format_Mono || decoded.format() == QImage::Format_MonoLSB);
		image           = decoded.convertToFormat(QImage::Format_ARGB32);
		decodedHasAlpha = image.hasAlphaChannel();
		qmudInsertMemoryImageDecodeCache(m_memoryImageDecodeCache, m_memoryImageDecodeCacheBytes, data, image,
		                                 decodedHasAlpha, isMonochrome, kMemoryImageDecodeCacheMaxEntries,
		                                 kMemoryImageDecodeCacheMaxBytes);
	}

	MiniWindowImage entry;
	entry.image      = image;
	entry.source     = QStringLiteral("<memory>");
	entry.hasAlpha   = hasAlpha || decodedHasAlpha;
	entry.monochrome = isMonochrome;
	window->images.insert(imageId, entry);
	return eOK;
}

QStringList WorldRuntime::windowImageList(const QString &name) const
{
	const MiniWindow *window = miniWindow(name);
	if (!window)
		return {};
	return window->images.keys();
}

QVariant WorldRuntime::windowImageInfo(const QString &name, const QString &imageId, int infoType) const
{
	const MiniWindow *window = miniWindow(name);
	if (!window)
		return {};
	const auto it = window->images.find(imageId);
	if (it == window->images.end())
		return {};

	const QImage &image      = it.value().image;
	const bool    monochrome = it.value().monochrome;
	switch (infoType)
	{
	case 1:
		return 0;
	case 2:
		return image.width();
	case 3:
		return image.height();
	case 4:
		if (monochrome)
			return bytesPerLine(image.width(), 1);
		return image.bytesPerLine();
	case 5:
		return 1;
	case 6:
		if (monochrome)
			return 1;
		return image.depth();
	default:
		return {};
	}
}

int WorldRuntime::windowImageFromWindow(const QString &name, const QString &imageId,
                                        const QString &sourceWindow)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;
	const MiniWindow *src = miniWindow(sourceWindow);
	if (!src)
		return eNoSuchWindow;

	MiniWindowImage entry;
	entry.image      = src->surface.copy();
	entry.source     = sourceWindow;
	entry.hasAlpha   = entry.image.hasAlphaChannel();
	entry.monochrome = false;
	window->images.insert(imageId, entry);
	return eOK;
}

int WorldRuntime::windowDrawImage(const QString &name, const QString &imageId, int left, int top, int right,
                                  int bottom, int mode, int srcLeft, int srcTop, int srcRight, int srcBottom)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;
	const auto it = window->images.find(imageId);
	if (it == window->images.end())
		return eImageNotInstalled;

	QImage       &surface = window->surface;
	const QImage &image   = it.value().image;
	if (surface.isNull() || image.isNull())
		return eOK;

	QRect source(srcLeft, srcTop, srcRight - srcLeft, srcBottom - srcTop);
	if (srcRight <= 0)
		source.setRight(image.width() + srcRight - 1);
	if (srcBottom <= 0)
		source.setBottom(image.height() + srcBottom - 1);
	source = source.intersected(image.rect());

	if (source.width() <= 0 || source.height() <= 0)
		return eOK;

	QPainter painter(&surface);
	if (mode == 2)
	{
		QRect const target = rectFromCoords(*window, left, top, right, bottom);
		if (target.width() <= 0 || target.height() <= 0)
			return eOK;
		painter.drawImage(target, image, source);
	}
	else if (mode == 3)
	{
		QImage     copy = image.copy(source).convertToFormat(QImage::Format_ARGB32);
		const auto key  = QColor(image.pixel(0, 0));
		for (int y = 0; y < copy.height(); ++y)
		{
			auto *line = reinterpret_cast<QRgb *>(copy.scanLine(y));
			for (int x = 0; x < copy.width(); ++x)
			{
				if ((line[x] & 0x00FFFFFF) == (key.rgb() & 0x00FFFFFF))
					line[x] &= 0x00FFFFFF;
			}
		}
		painter.drawImage(QPoint(left, top), copy);
	}
	else if (mode == 1)
		painter.drawImage(QPoint(left, top), image.copy(source));
	else
		return eBadParameter;

	emit miniWindowsChanged();
	return eOK;
}

int WorldRuntime::windowDrawImageAlpha(const QString &name, const QString &imageId, int left, int top,
                                       int right, int bottom, double opacity, int srcLeft, int srcTop)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;
	const auto it = window->images.find(imageId);
	if (it == window->images.end())
		return eImageNotInstalled;
	if (opacity < 0.0 || opacity > 1.0)
		return eBadParameter;
	if (!it.value().hasAlpha)
		return eImageNotInstalled;

	QImage       &surface = window->surface;
	const QImage &image   = it.value().image;
	if (surface.isNull() || image.isNull())
		return eOK;

	int drawLeft = left;
	int drawTop  = top;

	int sourceLeft = qMax(0, srcLeft);
	int sourceTop  = qMax(0, srcTop);
	int targetWidth{};
	int targetHeight{};

	if (right == 0)
		targetWidth = image.width() - sourceLeft;
	else
		targetWidth = window->fixRight(right) - left;

	if (bottom == 0)
		targetHeight = image.height() - sourceTop;
	else
		targetHeight = window->fixBottom(bottom) - top;

	if (drawLeft < 0)
	{
		sourceLeft -= drawLeft;
		targetWidth += drawLeft;
		drawLeft = 0;
	}

	if (drawTop < 0)
	{
		sourceTop -= drawTop;
		targetHeight += drawTop;
		drawTop = 0;
	}

	if (drawLeft >= window->width || drawTop >= window->height)
		return eOK;

	targetWidth  = qMin(targetWidth, window->width - drawLeft);
	targetHeight = qMin(targetHeight, window->height - drawTop);
	if (targetWidth <= 0 || targetHeight <= 0)
		return eOK;

	QRect source(sourceLeft, sourceTop, targetWidth, targetHeight);
	source = source.intersected(image.rect());
	if (source.width() <= 0 || source.height() <= 0)
		return eOK;

	QImage base = surface.copy(drawLeft, drawTop, source.width(), source.height())
	                  .convertToFormat(QImage::Format_ARGB32);
	QImage const overlay = image.copy(source).convertToFormat(QImage::Format_ARGB32);
	const int    w       = qMin(base.width(), overlay.width());
	const int    h       = qMin(base.height(), overlay.height());
	for (int y = 0; y < h; ++y)
	{
		auto       *baseLine = reinterpret_cast<QRgb *>(base.scanLine(y));
		const auto *overLine = reinterpret_cast<const QRgb *>(overlay.constScanLine(y));
		for (int x = 0; x < w; ++x)
		{
			const QColor a        = QColor::fromRgba(overLine[x]);
			const QColor b        = QColor::fromRgba(baseLine[x]);
			const int    mask     = a.alpha();
			const int    blendedR = (a.red() * mask + b.red() * (255 - mask)) / 255;
			const int    blendedG = (a.green() * mask + b.green() * (255 - mask)) / 255;
			const int    blendedB = (a.blue() * mask + b.blue() * (255 - mask)) / 255;
			const int    outR =
                (opacity < 1.0) ? QMudBlend::simpleOpacity(b.red(), blendedR, opacity) : blendedR;
			const int outG =
			    (opacity < 1.0) ? QMudBlend::simpleOpacity(b.green(), blendedG, opacity) : blendedG;
			const int outB =
			    (opacity < 1.0) ? QMudBlend::simpleOpacity(b.blue(), blendedB, opacity) : blendedB;
			baseLine[x] = qRgba(clampByte(outR), clampByte(outG), clampByte(outB), 0xFF);
		}
	}

	QPainter painter(&surface);
	painter.drawImage(QPoint(drawLeft, drawTop), base);
	emit miniWindowsChanged();
	return eOK;
}

int WorldRuntime::windowImageOp(const QString &name, int action, int left, int top, int right, int bottom,
                                long penColour, long penStyle, int penWidth, long brushColour,
                                const QString &imageId, int ellipseWidth, int ellipseHeight)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;
	const auto it = window->images.find(imageId);
	if (it == window->images.end())
		return eImageNotInstalled;

	QImage &surface = window->surface;
	if (surface.isNull())
		return eOK;

	const QRect rect = rectFromCoords(*window, left, top, right, bottom);
	if (validatePenStyle(penStyle, penWidth) != eOK)
		return ePenStyleNotValid;

	QPainter painter(&surface);
	QPen     pen(colorFromRef(penColour));
	pen.setWidth(qMax(1, penWidth));
	pen.setStyle(mapPenStyle(penStyle));
	pen.setCapStyle(mapPenCap(penStyle));
	pen.setJoinStyle(mapPenJoin(penStyle));
	painter.setPen(penStyle == 5 ? Qt::NoPen : pen);

	QImage pattern = it.value().image;
	if (it.value().monochrome)
	{
		const QColor fore = MiniWindowBrushUtils::colorFromRefOrTransparent(brushColour);
		const QColor back = MiniWindowBrushUtils::colorFromRefOrTransparent(penColour);
		QImage       recoloured(pattern.size(), QImage::Format_ARGB32);
		for (int y = 0; y < pattern.height(); ++y)
		{
			for (int x = 0; x < pattern.width(); ++x)
			{
				const QColor sample(pattern.pixel(x, y));
				const bool   bit = qGray(sample.rgb()) > 127;
				recoloured.setPixel(x, y, (bit ? fore : back).rgba());
			}
		}
		pattern = recoloured;
	}
	QBrush const brush(QPixmap::fromImage(pattern));
	painter.setBrush(brush);

	switch (action)
	{
	case 1:
		painter.drawEllipse(rect);
		break;
	case 2:
		painter.drawRect(rect.adjusted(0, 0, -1, -1));
		break;
	case 3:
		painter.drawRoundedRect(rect, ellipseWidth, ellipseHeight, Qt::AbsoluteSize);
		break;
	default:
		return eUnknownOption;
	}

	emit miniWindowsChanged();
	return eOK;
}

int WorldRuntime::windowMergeImageAlpha(const QString &name, const QString &imageId, const QString &maskId,
                                        int left, int top, int right, int bottom, int mode, double opacity,
                                        int srcLeft, int srcTop, int srcRight, int srcBottom)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;
	const auto imageIt = window->images.find(imageId);
	const auto maskIt  = window->images.find(maskId);
	if (imageIt == window->images.end() || maskIt == window->images.end())
		return eImageNotInstalled;
	if (opacity < 0.0 || opacity > 1.0)
		return eBadParameter;

	QImage       &surface = window->surface;
	const QImage &image   = imageIt.value().image;
	const QImage &mask    = maskIt.value().image;

	if (mode != 0 && mode != 1)
		return eUnknownOption;

	const int fixedRight   = window->fixRight(right);
	const int fixedBottom  = window->fixBottom(bottom);
	const int targetLeft   = qMax(0, left);
	const int targetTop    = qMax(0, top);
	const int targetRight  = qMin(fixedRight, window->width);
	const int targetBottom = qMin(fixedBottom, window->height);
	const int targetWidth  = targetRight - targetLeft;
	const int targetHeight = targetBottom - targetTop;
	if (targetWidth <= 0 || targetHeight <= 0)
		return eOK;

	int const sourceLeft   = qMax(0, srcLeft);
	int const sourceTop    = qMax(0, srcTop);
	int       sourceRight  = srcRight;
	int       sourceBottom = srcBottom;
	if (sourceRight <= 0)
		sourceRight = image.width() + sourceRight;
	if (sourceBottom <= 0)
		sourceBottom = image.height() + sourceBottom;
	sourceRight  = qMin(sourceRight, image.width());
	sourceBottom = qMin(sourceBottom, image.height());

	const int width  = qMin(targetWidth, sourceRight - sourceLeft);
	const int height = qMin(targetHeight, sourceBottom - sourceTop);
	if (width <= 0 || height <= 0)
		return eOK;

	if (mask.width() < sourceLeft + width || mask.height() < sourceTop + height)
		return eBadParameter;

	QImage base = surface.copy(targetLeft, targetTop, width, height).convertToFormat(QImage::Format_ARGB32);
	QImage const src =
	    image.copy(sourceLeft, sourceTop, width, height).convertToFormat(QImage::Format_ARGB32);
	QImage const maskCopy =
	    mask.copy(sourceLeft, sourceTop, width, height).convertToFormat(QImage::Format_ARGB32);

	const QColor opaqueColor(image.pixel(0, 0));

	for (int y = 0; y < height; ++y)
	{
		auto       *baseLine = reinterpret_cast<QRgb *>(base.scanLine(y));
		const auto *srcLine  = reinterpret_cast<const QRgb *>(src.constScanLine(y));
		const auto *maskLine = reinterpret_cast<const QRgb *>(maskCopy.constScanLine(y));
		for (int x = 0; x < width; ++x)
		{
			QColor const a(srcLine[x]);
			QColor const b(baseLine[x]);
			QColor const m(maskLine[x]);
			int          rA = a.red();
			int          gA = a.green();
			int          bA = a.blue();
			if (mode == 1 && a.red() == opaqueColor.red() && a.green() == opaqueColor.green() &&
			    a.blue() == opaqueColor.blue())
			{
				rA = b.red();
				gA = b.green();
				bA = b.blue();
			}

			const int rM = m.red();
			const int gM = m.green();
			const int bM = m.blue();

			auto      blendMask = [](int A, int B, int M) { return (A * M + B * (255 - M)) / 255; };

			const int rawR = blendMask(rA, b.red(), rM);
			const int rawG = blendMask(gA, b.green(), gM);
			const int rawB = blendMask(bA, b.blue(), bM);

			const int outR = (opacity < 1.0) ? QMudBlend::simpleOpacity(b.red(), rawR, opacity) : rawR;
			const int outG = (opacity < 1.0) ? QMudBlend::simpleOpacity(b.green(), rawG, opacity) : rawG;
			const int outB = (opacity < 1.0) ? QMudBlend::simpleOpacity(b.blue(), rawB, opacity) : rawB;
			baseLine[x]    = qRgba(clampByte(outR), clampByte(outG), clampByte(outB), 0xFF);
		}
	}

	QPainter painter(&surface);
	painter.drawImage(QPoint(targetLeft, targetTop), base);
	emit miniWindowsChanged();
	return eOK;
}

int WorldRuntime::windowGetImageAlpha(const QString &name, const QString &imageId, int left, int top,
                                      int right, int bottom, int srcLeft, int srcTop)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;
	const auto it = window->images.find(imageId);
	if (it == window->images.end())
		return eImageNotInstalled;
	if (!it.value().hasAlpha)
		return eImageNotInstalled;

	QImage       &surface = window->surface;
	const QImage &image   = it.value().image;
	if (surface.isNull() || image.isNull())
		return eOK;

	const int fixedRight   = window->fixRight(right);
	const int fixedBottom  = window->fixBottom(bottom);
	const int targetLeft   = qMax(0, left);
	const int targetTop    = qMax(0, top);
	const int targetRight  = qMin(fixedRight, window->width);
	const int targetBottom = qMin(fixedBottom, window->height);
	const int targetWidth  = targetRight - targetLeft;
	const int targetHeight = targetBottom - targetTop;
	if (targetWidth <= 0 || targetHeight <= 0)
		return eOK;

	QRect source(qMax(0, srcLeft), qMax(0, srcTop), targetWidth, targetHeight);
	if (srcLeft < 0)
		source.moveLeft(0);
	if (srcTop < 0)
		source.moveTop(0);
	source = source.intersected(image.rect());
	if (source.width() <= 0 || source.height() <= 0)
		return eOK;

	QImage alphaImage(source.size(), QImage::Format_ARGB32);
	for (int y = 0; y < source.height(); ++y)
	{
		for (int x = 0; x < source.width(); ++x)
		{
			const int alpha = qAlpha(image.pixel(source.x() + x, source.y() + y));
			alphaImage.setPixel(x, y, qRgba(alpha, alpha, alpha, 0xFF));
		}
	}

	QPainter painter(&surface);
	painter.drawImage(QPoint(targetLeft, targetTop), alphaImage);
	emit miniWindowsChanged();
	return eOK;
}

int WorldRuntime::windowBlendImage(const QString &name, const QString &imageId, int left, int top, int right,
                                   int bottom, int mode, double opacity, int srcLeft, int srcTop,
                                   int srcRight, int srcBottom)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;
	const auto it = window->images.find(imageId);
	if (it == window->images.end())
		return eImageNotInstalled;
	if (opacity < 0.0 || opacity > 1.0)
		return eBadParameter;
	if (mode < 1 || mode > 64)
		return eUnknownOption;

	QImage       &surface = window->surface;
	const QImage &image   = it.value().image;

	const int     fixedRight   = window->fixRight(right);
	const int     fixedBottom  = window->fixBottom(bottom);
	const int     targetLeft   = qMax(0, left);
	const int     targetTop    = qMax(0, top);
	const int     targetRight  = qMin(fixedRight, window->width);
	const int     targetBottom = qMin(fixedBottom, window->height);
	const int     targetWidth  = targetRight - targetLeft;
	const int     targetHeight = targetBottom - targetTop;
	if (targetWidth <= 0 || targetHeight <= 0)
		return eOK;

	int const sourceLeft   = qMax(0, srcLeft);
	int const sourceTop    = qMax(0, srcTop);
	int       sourceRight  = srcRight;
	int       sourceBottom = srcBottom;
	if (sourceRight <= 0)
		sourceRight = image.width() + sourceRight;
	if (sourceBottom <= 0)
		sourceBottom = image.height() + sourceBottom;
	sourceRight  = qMin(sourceRight, image.width());
	sourceBottom = qMin(sourceBottom, image.height());

	const int width  = qMin(targetWidth, sourceRight - sourceLeft);
	const int height = qMin(targetHeight, sourceBottom - sourceTop);
	if (width <= 0 || height <= 0)
		return eOK;

	QImage base = surface.copy(targetLeft, targetTop, width, height).convertToFormat(QImage::Format_ARGB32);
	QImage const blend =
	    image.copy(sourceLeft, sourceTop, width, height).convertToFormat(QImage::Format_ARGB32);

	for (int y = 0; y < height; ++y)
	{
		auto       *baseLine  = reinterpret_cast<QRgb *>(base.scanLine(y));
		const auto *blendLine = reinterpret_cast<const QRgb *>(blend.constScanLine(y));
		for (int x = 0; x < width; ++x)
		{
			const long blendPixel = static_cast<long>(qRed(blendLine[x])) |
			                        (static_cast<long>(qGreen(blendLine[x])) << 8) |
			                        (static_cast<long>(qBlue(blendLine[x])) << 16);
			const long basePixel = static_cast<long>(qRed(baseLine[x])) |
			                       (static_cast<long>(qGreen(baseLine[x])) << 8) |
			                       (static_cast<long>(qBlue(baseLine[x])) << 16);
			long result = 0;
			if (!blendPixelInternal(blendPixel, basePixel, static_cast<short>(mode), opacity, result))
				return eUnknownOption;
			const int r = lowByteToInt(result);
			const int g = lowByteToInt(result >> 8);
			const int b = lowByteToInt(result >> 16);
			baseLine[x] = qRgba(r, g, b, 0xFF);
		}
	}

	QPainter painter(&surface);
	painter.drawImage(QPoint(targetLeft, targetTop), base);
	emit miniWindowsChanged();
	return eOK;
}

int WorldRuntime::windowFilter(const QString &name, int left, int top, int right, int bottom, int operation,
                               double options, int extra)
{
	Q_UNUSED(extra);
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;

	QImage &surface = window->surface;
	if (surface.isNull())
		return eOK;

	const int fixedRight   = window->fixRight(right);
	const int fixedBottom  = window->fixBottom(bottom);
	const int targetLeft   = qMax(0, left);
	const int targetTop    = qMax(0, top);
	const int targetRight  = qMin(fixedRight, window->width);
	const int targetBottom = qMin(fixedBottom, window->height);
	const int width        = targetRight - targetLeft;
	const int height       = targetBottom - targetTop;
	if (width <= 0 || height <= 0)
		return eOK;

	QImage copy = surface.copy(targetLeft, targetTop, width, height).convertToFormat(QImage::Format_ARGB32);
	const int  bpl = bytesPerLine24(width);
	QByteArray buffer;
	imageToBgrBuffer(copy, buffer, width, height, bpl);

	switch (operation)
	{
	case 1:
		noise(reinterpret_cast<unsigned char *>(buffer.data()), width, height, bpl, options);
		break;
	case 2:
		monoNoise(reinterpret_cast<unsigned char *>(buffer.data()), width, height, bpl, options);
		break;
	case 3:
	{
		const double matrix[5] = {1, 1, 1, 1, 1};
		generalFilter(reinterpret_cast<unsigned char *>(buffer.data()), width, height, bpl, options, matrix,
		              5);
	}
	break;
	case 4:
	{
		constexpr double matrix[5] = {-1, -1, 7, -1, -1};
		generalFilter(reinterpret_cast<unsigned char *>(buffer.data()), width, height, bpl, options, matrix,
		              3);
	}
	break;
	case 5:
	{
		constexpr double matrix[5] = {0, 2.5, -6, 2.5, 0};
		generalFilter(reinterpret_cast<unsigned char *>(buffer.data()), width, height, bpl, options, matrix,
		              1);
	}
	break;
	case 6:
	{
		constexpr double matrix[5] = {1, 2, 1, -1, -2};
		generalFilter(reinterpret_cast<unsigned char *>(buffer.data()), width, height, bpl, options, matrix,
		              1);
	}
	break;
	case 7:
		brightness(reinterpret_cast<unsigned char *>(buffer.data()), width, height, bpl, options);
		break;
	case 8:
		contrast(reinterpret_cast<unsigned char *>(buffer.data()), width, height, bpl, options);
		break;
	case 9:
		gammaAdjust(reinterpret_cast<unsigned char *>(buffer.data()), width, height, bpl, options);
		break;
	case 10:
		colourBrightness(reinterpret_cast<unsigned char *>(buffer.data()), width, height, bpl, options, 2);
		break;
	case 11:
		colourContrast(reinterpret_cast<unsigned char *>(buffer.data()), width, height, bpl, options, 2);
		break;
	case 12:
		colourGamma(reinterpret_cast<unsigned char *>(buffer.data()), width, height, bpl, options, 2);
		break;
	case 13:
		colourBrightness(reinterpret_cast<unsigned char *>(buffer.data()), width, height, bpl, options, 1);
		break;
	case 14:
		colourContrast(reinterpret_cast<unsigned char *>(buffer.data()), width, height, bpl, options, 1);
		break;
	case 15:
		colourGamma(reinterpret_cast<unsigned char *>(buffer.data()), width, height, bpl, options, 1);
		break;
	case 16:
		colourBrightness(reinterpret_cast<unsigned char *>(buffer.data()), width, height, bpl, options, 0);
		break;
	case 17:
		colourContrast(reinterpret_cast<unsigned char *>(buffer.data()), width, height, bpl, options, 0);
		break;
	case 18:
		colourGamma(reinterpret_cast<unsigned char *>(buffer.data()), width, height, bpl, options, 0);
		break;
	case 19:
		makeGreyscale(reinterpret_cast<unsigned char *>(buffer.data()), width, height, bpl, options, true);
		break;
	case 20:
		makeGreyscale(reinterpret_cast<unsigned char *>(buffer.data()), width, height, bpl, options, false);
		break;
	case 21:
		brightnessMultiply(reinterpret_cast<unsigned char *>(buffer.data()), width, height, bpl, options);
		break;
	case 22:
		colourBrightnessMultiply(reinterpret_cast<unsigned char *>(buffer.data()), width, height, bpl,
		                         options, 2);
		break;
	case 23:
		colourBrightnessMultiply(reinterpret_cast<unsigned char *>(buffer.data()), width, height, bpl,
		                         options, 1);
		break;
	case 24:
		colourBrightnessMultiply(reinterpret_cast<unsigned char *>(buffer.data()), width, height, bpl,
		                         options, 0);
		break;
	case 25:
	{
		const double matrix[5] = {0, 1, 1, 1, 0};
		generalFilter(reinterpret_cast<unsigned char *>(buffer.data()), width, height, bpl, options, matrix,
		              3);
	}
	break;
	case 26:
	{
		constexpr double matrix[5] = {0, 0.5, 1, 0.5, 0};
		generalFilter(reinterpret_cast<unsigned char *>(buffer.data()), width, height, bpl, options, matrix,
		              2);
	}
	break;
	case 27:
		averageBuffer(reinterpret_cast<unsigned char *>(buffer.data()), width, height, bpl);
		break;
	default:
		return eUnknownOption;
	}

	bgrBufferToImage(buffer, copy, width, height, bpl);
	QPainter painter(&surface);
	painter.drawImage(QPoint(targetLeft, targetTop), copy);
	emit miniWindowsChanged();
	return eOK;
}

int WorldRuntime::windowTransformImage(const QString &name, const QString &imageId, float left, float top,
                                       int mode, float mxx, float mxy, float myx, float myy)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;
	const auto it = window->images.find(imageId);
	if (it == window->images.end())
		return eImageNotInstalled;

	QImage       &surface = window->surface;
	const QImage &image   = it.value().image;
	if (surface.isNull() || image.isNull())
		return eOK;

	QPainter         painter(&surface);
	QTransform const transform(mxx, myx, mxy, myy, left, top);
	painter.setTransform(transform, true);
	if (mode == 3)
	{
		QImage     copy = image.convertToFormat(QImage::Format_ARGB32);
		const auto key  = QColor(copy.pixel(0, 0));
		for (int y = 0; y < copy.height(); ++y)
		{
			auto *line = reinterpret_cast<QRgb *>(copy.scanLine(y));
			for (int x = 0; x < copy.width(); ++x)
			{
				if ((line[x] & 0x00FFFFFF) == (key.rgb() & 0x00FFFFFF))
					line[x] &= 0x00FFFFFF;
			}
		}
		painter.drawImage(QPointF(0, 0), copy);
	}
	else
		painter.drawImage(QPointF(0, 0), image);
	emit miniWindowsChanged();
	return eOK;
}

int WorldRuntime::windowWrite(const QString &name, const QString &filename)
{
	const MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;
	if (filename.isEmpty())
		return eNoNameSpecified;

	const QString lower = filename.toLower();
	if (!lower.endsWith(QStringLiteral(".bmp")) && !lower.endsWith(QStringLiteral(".png")))
		return eBadParameter;

	if (!window->surface.save(filename))
		return eCouldNotOpenFile;
	return eOK;
}

int WorldRuntime::windowPosition(const QString &name, int left, int top, int position, int flags)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;

	if ((flags & kMiniWindowAbsoluteLocation) != 0 && m_view && m_view->isMiniWindowCaptureActive())
	{
		const int maxWidth  = m_view->outputClientWidth();
		const int maxHeight = m_view->outputClientHeight();

		if (maxWidth > 0)
		{
			if (window->width >= maxWidth)
				left = 0;
			else
				left = qBound(0, left, maxWidth - window->width);
		}
		if (maxHeight > 0)
		{
			if (window->height >= maxHeight)
				top = 0;
			else
				top = qBound(0, top, maxHeight - window->height);
		}
	}

	window->location = QPoint(left, top);
	window->position = position;
	window->flags    = flags;
	if ((flags & kMiniWindowTransparent) == 0 && !window->transparentSurfaceCache.isNull())
	{
		window->transparentSurfaceCache     = QImage();
		window->transparentSurfaceSourceKey = 0;
		window->transparentSurfaceKeyRgb    = 0;
	}
	emit miniWindowsChanged();
	return eOK;
}

int WorldRuntime::windowResize(const QString &name, int width, int height, long colour)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;
	if (width < 0 || height < 0)
		return eBadParameter;
	if (window->width == width && window->height == height)
		return eOK;

	QImage       newImage(qMax(1, width), qMax(1, height), QImage::Format_ARGB32);
	QColor const fill = colorFromRef(colour);
	newImage.fill(fill);
	QPainter painter(&newImage);
	painter.drawImage(0, 0, window->surface);
	window->surface = newImage;
	window->width   = width;
	window->height  = height;
	emit miniWindowsChanged();
	return eOK;
}

int WorldRuntime::windowSetZOrder(const QString &name, int zOrder)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;
	window->zOrder = zOrder;
	emit miniWindowsChanged();
	return eOK;
}

int WorldRuntime::windowAddHotspot(const QString &name, const QString &hotspotId, int left, int top,
                                   int right, int bottom, const QString &mouseOver,
                                   const QString &cancelMouseOver, const QString &mouseDown,
                                   const QString &cancelMouseDown, const QString &mouseUp,
                                   const QString &tooltip, int cursor, int flags, const QString &pluginId)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;

	static bool s_inWindowAddHotspot = false;
	if (s_inWindowAddHotspot)
		return eItemInUse;
	struct AddHotspotGuard
	{
			bool &flag;
			~AddHotspotGuard()
			{
				flag = false;
			}
	} const guard{s_inWindowAddHotspot};
	s_inWindowAddHotspot = true;

	if ((!mouseOver.isEmpty() && !isValidScriptLabel(mouseOver)) ||
	    (!cancelMouseOver.isEmpty() && !isValidScriptLabel(cancelMouseOver)) ||
	    (!mouseDown.isEmpty() && !isValidScriptLabel(mouseDown)) ||
	    (!cancelMouseDown.isEmpty() && !isValidScriptLabel(cancelMouseDown)) ||
	    (!mouseUp.isEmpty() && !isValidScriptLabel(mouseUp)))
		return eInvalidObjectLabel;

	if (!window->callbackPlugin.isEmpty() && window->callbackPlugin != pluginId)
		return eHotspotPluginChanged;
	if (window->callbackPlugin.isEmpty())
		window->callbackPlugin = pluginId;

	const auto existing = window->hotspots.find(hotspotId);
	if (existing != window->hotspots.end())
	{
		if (window->mouseOverHotspot == hotspotId)
			window->mouseOverHotspot.clear();
		if (window->mouseDownHotspot == hotspotId)
			window->mouseDownHotspot.clear();
	}

	MiniWindowHotspot hotspot;
	hotspot.rect            = rectFromCoords(*window, left, top, right, bottom);
	hotspot.mouseOver       = mouseOver;
	hotspot.cancelMouseOver = cancelMouseOver;
	hotspot.mouseDown       = mouseDown;
	hotspot.cancelMouseDown = cancelMouseDown;
	hotspot.mouseUp         = mouseUp;
	hotspot.tooltip         = tooltip;
	hotspot.cursor          = cursor;
	hotspot.flags           = flags;

	window->hotspots.insert(hotspotId, hotspot);
	if (m_view && !m_view->isMiniWindowCaptureActive() && m_view->hasLastMousePosition())
	{
		const QPoint lastGlobal = m_view->lastMousePosition();
		if (window->rect.contains(lastGlobal) && hotspot.rect.contains(window->lastMousePosition))
			m_view->recheckMiniWindowHover();
	}
	return eOK;
}

int WorldRuntime::windowDeleteHotspot(const QString &name, const QString &hotspotId)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;
	const auto it = window->hotspots.find(hotspotId);
	if (it == window->hotspots.end())
		return eHotspotNotInstalled;
	window->hotspots.erase(it);
	if (window->mouseOverHotspot == hotspotId)
		window->mouseOverHotspot.clear();
	if (window->mouseDownHotspot == hotspotId)
		window->mouseDownHotspot.clear();
	if (window->hotspots.isEmpty())
		window->callbackPlugin.clear();
	return eOK;
}

int WorldRuntime::windowDeleteAllHotspots(const QString &name)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;
	window->hotspots.clear();
	window->mouseOverHotspot.clear();
	window->mouseDownHotspot.clear();
	window->callbackPlugin.clear();
	return eOK;
}

QStringList WorldRuntime::windowHotspotList(const QString &name) const
{
	const MiniWindow *window = miniWindow(name);
	if (!window)
		return {};
	return window->hotspots.keys();
}

QVariant WorldRuntime::windowHotspotInfo(const QString &name, const QString &hotspotId, int infoType) const
{
	const MiniWindow *window = miniWindow(name);
	if (!window)
		return {};
	const auto it = window->hotspots.find(hotspotId);
	if (it == window->hotspots.end())
		return {};

	const MiniWindowHotspot &hotspot = it.value();
	switch (infoType)
	{
	case 1:
		return hotspot.rect.left();
	case 2:
		return hotspot.rect.top();
	case 3:
		return hotspot.rect.right();
	case 4:
		return hotspot.rect.bottom();
	case 5:
		return hotspot.mouseOver;
	case 6:
		return hotspot.cancelMouseOver;
	case 7:
		return hotspot.mouseDown;
	case 8:
		return hotspot.cancelMouseDown;
	case 9:
		return hotspot.mouseUp;
	case 10:
		return hotspot.tooltip;
	case 11:
		return hotspot.cursor;
	case 12:
		return hotspot.flags;
	case 13:
		return hotspot.moveCallback;
	case 14:
		return hotspot.releaseCallback;
	case 15:
		return hotspot.dragFlags;
	default:
		return {};
	}
}

int WorldRuntime::windowHotspotTooltip(const QString &name, const QString &hotspotId, const QString &tooltip)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;
	const auto it = window->hotspots.find(hotspotId);
	if (it == window->hotspots.end())
		return eHotspotNotInstalled;
	it.value().tooltip = tooltip;
	return eOK;
}

int WorldRuntime::windowMoveHotspot(const QString &name, const QString &hotspotId, int left, int top,
                                    int right, int bottom)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;
	const auto it = window->hotspots.find(hotspotId);
	if (it == window->hotspots.end())
		return eHotspotNotInstalled;
	it.value().rect = rectFromCoords(*window, left, top, right, bottom);
	return eOK;
}

int WorldRuntime::windowDragHandler(const QString &name, const QString &hotspotId,
                                    const QString &moveCallback, const QString &releaseCallback, int flags,
                                    const QString &pluginId)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;
	if ((!moveCallback.isEmpty() && !isValidScriptLabel(moveCallback)) ||
	    (!releaseCallback.isEmpty() && !isValidScriptLabel(releaseCallback)))
		return eInvalidObjectLabel;
	if (!window->callbackPlugin.isEmpty() && window->callbackPlugin != pluginId)
		return eHotspotPluginChanged;
	if (window->callbackPlugin.isEmpty())
		window->callbackPlugin = pluginId;
	const auto it = window->hotspots.find(hotspotId);
	if (it == window->hotspots.end())
		return eHotspotNotInstalled;
	it.value().moveCallback    = moveCallback;
	it.value().releaseCallback = releaseCallback;
	it.value().dragFlags       = flags;
	return eOK;
}

int WorldRuntime::windowScrollwheelHandler(const QString &name, const QString &hotspotId,
                                           const QString &moveCallback, const QString &pluginId)
{
	MiniWindow *window = miniWindow(name);
	if (!window)
		return eNoSuchWindow;
	if (!moveCallback.isEmpty() && !isValidScriptLabel(moveCallback))
		return eInvalidObjectLabel;
	if (!window->callbackPlugin.isEmpty() && window->callbackPlugin != pluginId)
		return eHotspotPluginChanged;
	if (window->callbackPlugin.isEmpty())
		window->callbackPlugin = pluginId;
	const auto it = window->hotspots.find(hotspotId);
	if (it == window->hotspots.end())
		return eHotspotNotInstalled;
	it.value().scrollwheelCallback = moveCallback;
	return eOK;
}

QString WorldRuntime::windowMenu(const QString &name, int left, int top, const QString &items,
                                 const QString &pluginId)
{
	Q_UNUSED(pluginId);
	if (!m_view)
		return {};

	MiniWindow const *window = miniWindow(name);
	if (!window)
		return {};

	if (!window->show || window->temporarilyHide)
		return {};

	if (left < 0 || left > window->width || top < 0 || top > window->height)
		return {};

	QString menuText = items;
	if (menuText.isEmpty())
		return {};

	bool returnNumber = false;
	if (!menuText.isEmpty() && menuText.at(0) == QLatin1Char('!'))
	{
		returnNumber = true;
		menuText.remove(0, 1);
	}

	enum class AlignH
	{
		Left,
		Center,
		Right
	};
	enum class AlignV
	{
		Top,
		Center,
		Bottom
	};
	auto alignH = AlignH::Left;
	auto alignV = AlignV::Top;
	if (!menuText.isEmpty() && menuText.at(0) == QLatin1Char('~') && menuText.size() > 3)
	{
		const QChar h = menuText.at(1);
		const QChar v = menuText.at(2);
		menuText.remove(0, 3);
		switch (h.toLower().unicode())
		{
		case 'c':
			alignH = AlignH::Center;
			break;
		case 'r':
			alignH = AlignH::Right;
			break;
		default:
			break;
		}
		switch (v.toLower().unicode())
		{
		case 'c':
			alignV = AlignV::Center;
			break;
		case 'b':
			alignV = AlignV::Bottom;
			break;
		default:
			break;
		}
	}

	QStringList const parts = menuText.split(QLatin1Char('|'), Qt::KeepEmptyParts);
	QMenu             menu;
	// Preserve popup interaction: if user right-clicks elsewhere while this menu
	// is open, replay that click so the new location can open its own context menu.
	menu.setAttribute(Qt::WA_NoMouseReplay, false);
	QStack<QMenu *> stack;
	stack.push(&menu);
	int           itemIndex        = 1;
	constexpr int kMxpMenuCount    = 100;
	bool          reachedMenuLimit = false;

	auto          createAction = [&](QMenu *parent, const QString &text, bool checked, bool disabled)
	{
		QAction *action = parent->addAction(text);
		action->setEnabled(!disabled);
		if (checked)
		{
			action->setCheckable(true);
			action->setChecked(true);
		}
		if (!disabled)
		{
			if (itemIndex > kMxpMenuCount)
			{
				reachedMenuLimit = true;
				return static_cast<QAction *>(nullptr);
			}
			action->setData(itemIndex++);
			if (itemIndex > kMxpMenuCount)
				reachedMenuLimit = true;
		}
		else
		{
			action->setData(0);
		}
		return action;
	};

	for (QString part : parts)
	{
		if (part.isEmpty())
		{
			stack.top()->addSeparator();
			continue;
		}
		if (part == QLatin1String("-"))
		{
			stack.top()->addSeparator();
			continue;
		}
		if (part.startsWith(QLatin1Char('>')))
		{
			stack.push(stack.top()->addMenu(part.mid(1)));
			continue;
		}
		if (part.startsWith(QLatin1Char('<')))
		{
			if (stack.size() > 1)
				stack.pop();
			continue;
		}
		bool checked  = false;
		bool disabled = false;
		while (!part.isEmpty() && (part.at(0) == QLatin1Char('+') || part.at(0) == QLatin1Char('^')))
		{
			if (part.at(0) == QLatin1Char('+'))
				checked = true;
			else if (part.at(0) == QLatin1Char('^'))
				disabled = true;
			part.remove(0, 1);
		}
		if (!createAction(stack.top(), part, checked, disabled) || reachedMenuLimit)
			break;
	}

	QPoint      globalPos = m_view->miniWindowGlobalPosition(window, left, top);
	const QSize hint      = menu.sizeHint();
	if (alignH == AlignH::Center)
		globalPos.setX(globalPos.x() - hint.width() / 2);
	else if (alignH == AlignH::Right)
		globalPos.setX(globalPos.x() - hint.width());
	if (alignV == AlignV::Center)
		globalPos.setY(globalPos.y() - hint.height() / 2);
	else if (alignV == AlignV::Bottom)
		globalPos.setY(globalPos.y() - hint.height());

	ContextMenuDismissReplayFilter replayFilter(&menu);
	qApp->installEventFilter(&replayFilter);
	QAction const *selected = menu.exec(globalPos);
	qApp->removeEventFilter(&replayFilter);

	if (!selected && replayFilter.hasReplayPoint())
	{
		const QPoint   replayPos = replayFilter.replayPoint();
		QPointer const view      = m_view;
		QTimer::singleShot(0, qApp,
		                   [view, replayPos]
		                   {
			                   if (view)
				                   view->showContextMenuAtGlobalPos(replayPos);
		                   });
	}

	if (!selected)
		return {};

	if (returnNumber)
		return QString::number(selected->data().toInt());
	return selected->text();
}

void WorldRuntime::setView(WorldView *view)
{
	if (m_viewDestroyedConnection)
	{
		QObject::disconnect(m_viewDestroyedConnection);
		m_viewDestroyedConnection = QMetaObject::Connection{};
	}
	m_view = view;
	if (m_view)
	{
		m_viewDestroyedConnection = connect(m_view, &QObject::destroyed, this,
		                                    [this]
		                                    {
			                                    m_view                    = nullptr;
			                                    m_viewDestroyedConnection = QMetaObject::Connection{};
		                                    });
	}
}

WorldView *WorldRuntime::view() const
{
	return m_view;
}

void WorldRuntime::queuePluginInstall(Plugin &plugin)
{
	if (!plugin.lua || !hasValidPluginId(plugin))
		return;

	const auto setInstallPending = [this, &plugin](const bool pending)
	{
		if (plugin.installPending == pending)
			return;
		plugin.installPending = pending;
		invalidatePluginCallbackPresenceCache();
	};

	if (!plugin.enabled && !plugin.disableAfterInstall)
	{
		setInstallPending(true);
		return;
	}
	if (m_loadingDocument || m_pluginInstallDeferred)
	{
		setInstallPending(true);
		return;
	}
	const bool installReady =
	    m_view && m_view->isVisible() && m_view->outputClientWidth() > 0 && m_view->outputClientHeight() > 0;
	if (installReady)
	{
		setInstallPending(false);
		runPluginInstallCallback(plugin);
		invalidatePluginCallbackPresenceCache();
		notifyWorldOutputResized();
		notifyDrawOutputWindow(1, 0);
	}
	else
	{
		setInstallPending(true);
	}
}

void WorldRuntime::runPluginInstallCallback(Plugin &plugin)
{
	if (!plugin.lua || !hasValidPluginId(plugin))
		return;

	++m_forceScriptErrorOutputDepth;
	[[maybe_unused]] const auto restoreScriptErrorOutputBehavior = qScopeGuard(
	    [this]
	    {
		    if (m_forceScriptErrorOutputDepth > 0)
			    --m_forceScriptErrorOutputDepth;
	    });
	plugin.lua->callFunctionNoArgs(QStringLiteral("OnPluginInstall"), nullptr, true);
	if (plugin.disableAfterInstall)
	{
		if (plugin.enabled)
		{
			plugin.enabled = false;
			plugin.attributes.insert(QStringLiteral("enabled"), QStringLiteral("0"));
			plugin.lua->callFunctionNoArgs(QStringLiteral("OnPluginDisable"));
		}
		else
		{
			plugin.attributes.insert(QStringLiteral("enabled"), QStringLiteral("0"));
		}
		plugin.disableAfterInstall = false;
	}
}

void WorldRuntime::installPendingPlugins()
{
	if (m_pluginInstallDeferred)
		return;
	if (!m_view || !m_view->isVisible() || m_view->outputClientWidth() <= 0 ||
	    m_view->outputClientHeight() <= 0)
		return;
	QVector<int> pendingIndices;
	pendingIndices.reserve(m_plugins.size());
	for (int i = 0; i < m_plugins.size(); ++i)
	{
		const Plugin &plugin = m_plugins.at(i);
		if (plugin.installPending && (plugin.enabled || plugin.disableAfterInstall) && plugin.lua &&
		    hasValidPluginId(plugin))
			pendingIndices.push_back(i);
	}
	if (pendingIndices.isEmpty())
		return;

	// Preserve startup behavior: once install starts, plugins can see each other
	// as install-ready during OnPluginInstall callbacks.
	bool installPendingStateChanged = false;
	for (const int index : pendingIndices)
	{
		if (!m_plugins[index].installPending)
			continue;
		m_plugins[index].installPending = false;
		installPendingStateChanged      = true;
	}
	if (installPendingStateChanged)
		invalidatePluginCallbackPresenceCache();

	bool installedAny = false;
	for (const int index : pendingIndices)
	{
		Plugin &plugin = m_plugins[index];
		if (!plugin.lua)
			continue;
		runPluginInstallCallback(plugin);
		installedAny = true;
	}
	if (installedAny)
	{
		invalidatePluginCallbackPresenceCache();
		notifyWorldOutputResized();
		notifyDrawOutputWindow(1, 0);
	}
}

void WorldRuntime::setPluginInstallDeferred(bool deferred)
{
	m_pluginInstallDeferred = deferred;
}

bool WorldRuntime::pluginInstallDeferred() const
{
	return m_pluginInstallDeferred;
}

void WorldRuntime::setBackgroundColour(long colour)
{
	m_backgroundColour = colour;
	if (m_view)
	{
		m_view->applyRuntimeSettings();
		m_view->update();
	}
}

long WorldRuntime::backgroundColour() const
{
	return m_backgroundColour;
}

bool WorldRuntime::doingSimulate() const
{
	return m_doingSimulate;
}

void WorldRuntime::setDoingSimulate(bool value)
{
	m_doingSimulate = value;
}

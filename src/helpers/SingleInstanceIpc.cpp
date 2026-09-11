/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: SingleInstanceIpc.cpp
 * Role: Implements acknowledged, length-prefixed request delivery between QMud processes.
 */

#include "SingleInstanceIpc.h"

#include <QDeadlineTimer>
#include <QDir>
#include <QFileInfo>
// ReSharper disable once CppUnusedIncludeDirective
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QObject>
#include <QtEndian>

#include <algorithm>
#include <chrono>
#include <limits>
#include <utility>

namespace
{
	constexpr qsizetype kFrameHeaderSize = sizeof(quint32);
	const QByteArray    kAcknowledgement = QByteArrayLiteral("QMUD-ACK-1");

	/** @brief Converts a finite deadline's remaining duration for Qt blocking socket APIs. */
	int                 remainingTime(const QDeadlineTimer &deadline)
	{
		return static_cast<int>(std::min<qint64>(deadline.remainingTime(), std::numeric_limits<int>::max()));
	}

	/** @brief Validates and decodes one JSON protocol payload. */
	bool decodeRequest(const QByteArray &payload, QMudSingleInstanceIpc::Request &request)
	{
		if (payload.isEmpty() || payload.size() > QMudSingleInstanceIpc::kMaximumPayloadSize)
			return false;

		QJsonParseError     error;
		const QJsonDocument document = QJsonDocument::fromJson(payload, &error);
		if (error.error != QJsonParseError::NoError || !document.isObject())
			return false;

		const QJsonObject object = document.object();
		if (object.value(QStringLiteral("version")).toInt(-1) != 1)
			return false;

		const QString action = object.value(QStringLiteral("action")).toString();
		if (action == QLatin1String("raise"))
		{
			request = {};
			return true;
		}
		if (action != QLatin1String("open"))
			return false;

		const QJsonValue pathValue = object.value(QStringLiteral("path"));
		if (!pathValue.isString())
			return false;
		const QString path = pathValue.toString();
		if (path.isEmpty() || path.contains(QChar::Null))
			return false;

		request.action              = QMudSingleInstanceIpc::Action::OpenFileAssociation;
		request.fileAssociationPath = path;
		return true;
	}

	/** @brief Owns accumulation and one-shot dispatch for an accepted request socket. */
	class RequestReceiver final : public QObject
	{
		public:
			RequestReceiver(QLocalSocket *socket, QMudSingleInstanceIpc::RequestHandler handler)
			    : QObject(socket), m_socket(socket), m_handler(std::move(handler))
			{
				QObject::connect(m_socket, &QLocalSocket::readyRead, this,
				                 [this] { consumeAvailableData(); });
				QObject::connect(m_socket, &QLocalSocket::disconnected, this,
				                 [this]
				                 {
					                 consumeAvailableData();
					                 m_socket->deleteLater();
				                 });
				consumeAvailableData();
				if (!m_complete && m_socket->state() == QLocalSocket::UnconnectedState)
					m_socket->deleteLater();
			}

		private:
			/** @brief Accumulates available bytes and dispatches once a complete valid frame exists. */
			void consumeAvailableData()
			{
				if (m_complete)
					return;

				constexpr qsizetype maximumFrameSize =
				    kFrameHeaderSize + QMudSingleInstanceIpc::kMaximumPayloadSize;
				const qsizetype availableCapacity = maximumFrameSize + 1 - m_buffer.size();
				if (availableCapacity > 0)
					m_buffer.append(m_socket->read(availableCapacity));
				if (m_buffer.size() > maximumFrameSize)
				{
					reject();
					return;
				}
				if (m_buffer.size() < kFrameHeaderSize)
					return;

				const auto payloadSize =
				    qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(m_buffer.constData()));
				if (payloadSize == 0 || payloadSize > QMudSingleInstanceIpc::kMaximumPayloadSize)
				{
					reject();
					return;
				}

				const qsizetype frameSize = kFrameHeaderSize + static_cast<qsizetype>(payloadSize);
				if (m_buffer.size() < frameSize)
					return;

				QMudSingleInstanceIpc::Request request;
				if (!decodeRequest(m_buffer.sliced(kFrameHeaderSize, payloadSize), request))
				{
					reject();
					return;
				}

				m_complete = true;
				if (m_socket->state() == QLocalSocket::ConnectedState)
				{
					m_socket->write(kAcknowledgement);
					m_socket->flush();
					m_socket->disconnectFromServer();
				}
				if (m_handler)
					m_handler(request);
				if (m_socket->state() == QLocalSocket::UnconnectedState)
					m_socket->deleteLater();
			}

			/** @brief Rejects the current request and closes its socket without dispatch. */
			void reject()
			{
				m_complete = true;
				m_socket->abort();
				m_socket->deleteLater();
			}

			QLocalSocket                         *m_socket;
			QMudSingleInstanceIpc::RequestHandler m_handler;
			QByteArray                            m_buffer;
			bool                                  m_complete{false};
	};
} // namespace

QByteArray QMudSingleInstanceIpc::encodeRequestFrame(const Request &request)
{
	QJsonObject object{
	    {QStringLiteral("version"), 1}
    };
	if (request.action == Action::OpenFileAssociation)
	{
		object.insert(QStringLiteral("action"), QStringLiteral("open"));
		object.insert(QStringLiteral("path"), request.fileAssociationPath);
	}
	else
	{
		object.insert(QStringLiteral("action"), QStringLiteral("raise"));
	}

	const QByteArray payload = QJsonDocument(object).toJson(QJsonDocument::Compact);
	QByteArray       frame(kFrameHeaderSize, Qt::Uninitialized);
	qToBigEndian(static_cast<quint32>(payload.size()), reinterpret_cast<uchar *>(frame.data()));
	frame.append(payload);
	return frame;
}

QMudSingleInstanceIpc::SendResult QMudSingleInstanceIpc::sendRequest(const QString &serverName,
                                                                     const Request &request,
                                                                     const int      connectionTimeoutMs,
                                                                     const int      deliveryTimeoutMs,
                                                                     QString       *errorMessage)
{
	if (errorMessage)
		errorMessage->clear();

	QLocalSocket socket;
	socket.connectToServer(serverName, QIODevice::ReadWrite);
	if (!socket.waitForConnected(connectionTimeoutMs))
	{
		if (socket.error() == QLocalSocket::ServerNotFoundError ||
		    socket.error() == QLocalSocket::ConnectionRefusedError)
		{
			return SendResult::ServerUnavailable;
		}
		if (errorMessage)
			*errorMessage = socket.errorString();
		return SendResult::Failed;
	}

	const QByteArray frame = encodeRequestFrame(request);
	qsizetype        offset{0};
	QDeadlineTimer   deadline(deliveryTimeoutMs);
	while (offset < frame.size())
	{
		const qint64 written = socket.write(frame.constData() + offset, frame.size() - offset);
		if (written < 0)
		{
			if (errorMessage)
				*errorMessage = socket.errorString();
			return SendResult::Failed;
		}
		offset += written;
		if (written == 0 && (deadline.hasExpired() || !socket.waitForBytesWritten(remainingTime(deadline))))
		{
			if (errorMessage)
				*errorMessage = QStringLiteral("Timed out while writing the request.");
			return SendResult::Failed;
		}
	}
	while (socket.bytesToWrite() > 0)
	{
		if (deadline.hasExpired() || !socket.waitForBytesWritten(remainingTime(deadline)))
		{
			if (errorMessage)
				*errorMessage = QStringLiteral("Timed out while writing the request.");
			return SendResult::Failed;
		}
	}

	QByteArray acknowledgement;
	while (acknowledgement.size() < kAcknowledgement.size())
	{
		acknowledgement.append(socket.read(kAcknowledgement.size() - acknowledgement.size()));
		if (acknowledgement.size() >= kAcknowledgement.size())
			break;
		if (deadline.hasExpired() || !socket.waitForReadyRead(remainingTime(deadline)))
		{
			if (errorMessage)
				*errorMessage = QStringLiteral("The running instance did not acknowledge the request.");
			return SendResult::Failed;
		}
	}
	if (acknowledgement != kAcknowledgement)
	{
		if (errorMessage)
			*errorMessage = QStringLiteral("The running instance returned an invalid acknowledgement.");
		return SendResult::Failed;
	}

	socket.disconnectFromServer();
	return SendResult::Delivered;
}

QString QMudSingleInstanceIpc::serverStartupLockFilePath(const QString &serverName)
{
	if (QFileInfo(serverName).isAbsolute())
		return serverName + QStringLiteral(".startup.lock");
	return QDir::temp().filePath(serverName + QStringLiteral(".startup.lock"));
}

QMudSingleInstanceIpc::ServerStartupResult
QMudSingleInstanceIpc::startServerOrForward(QLocalServer &server, const QString &serverName,
                                            const std::optional<Request> &requestToForward,
                                            const int connectionTimeoutMs, const int deliveryTimeoutMs,
                                            const int startupLockTimeoutMs, QString *errorMessage)
{
	if (errorMessage)
		errorMessage->clear();

	QLockFile startupLock(serverStartupLockFilePath(serverName));
	if (!startupLock.tryLock(std::chrono::milliseconds(std::max(0, startupLockTimeoutMs))))
	{
		if (errorMessage)
		{
			*errorMessage = QStringLiteral("Unable to acquire the single-instance startup lock (error %1).")
			                    .arg(static_cast<int>(startupLock.error()));
		}
		return ServerStartupResult::Failed;
	}

	if (requestToForward)
	{
		const SendResult sendResult =
		    sendRequest(serverName, *requestToForward, connectionTimeoutMs, deliveryTimeoutMs, errorMessage);
		if (sendResult == SendResult::Delivered)
			return ServerStartupResult::Forwarded;
		if (sendResult == SendResult::Failed)
			return ServerStartupResult::Failed;
	}

	QLocalServer::removeServer(serverName);
	server.setSocketOptions(QLocalServer::UserAccessOption);
	if (!server.listen(serverName))
	{
		if (errorMessage)
		{
			*errorMessage = QStringLiteral("Unable to listen on the single-instance endpoint '%1': %2")
			                    .arg(serverName, server.errorString());
		}
		return ServerStartupResult::Failed;
	}
	return ServerStartupResult::Listening;
}

void QMudSingleInstanceIpc::receiveRequest(QLocalSocket *socket, RequestHandler handler)
{
	if (!socket)
		return;
	new RequestReceiver(socket, std::move(handler));
}

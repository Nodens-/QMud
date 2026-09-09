/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: SingleInstanceIpc.h
 * Role: Defines the framed local-socket protocol used to forward requests to the running QMud instance.
 */

#ifndef QMUD_SINGLEINSTANCEIPC_H
#define QMUD_SINGLEINSTANCEIPC_H

// ReSharper disable once CppUnusedIncludeDirective
#include <QByteArray>
#include <QString>

#include <functional>
#include <optional>

class QLocalServer;
class QLocalSocket;

namespace QMudSingleInstanceIpc
{
	/** Maximum accepted JSON payload size for one request frame. */
	inline constexpr quint32 kMaximumPayloadSize = 64U * 1024U;

	/**
	 * @brief Operation requested from the running QMud instance.
	 */
	enum class Action
	{
		Raise,
		OpenFileAssociation
	};

	/**
	 * @brief One decoded single-instance request.
	 */
	struct Request
	{
			Action  action{Action::Raise}; ///< Operation requested from the running instance.
			QString fileAssociationPath;   ///< Exact native path supplied for an open request.
	};

	/**
	 * @brief Result of connecting to and delivering a request.
	 */
	enum class SendResult
	{
		Delivered,         ///< The running instance acknowledged the complete request.
		ServerUnavailable, ///< No listening instance exists at the requested endpoint.
		Failed             ///< An endpoint exists or connection setup failed for another reason.
	};

	/**
	 * @brief Outcome of serialized single-instance server startup.
	 */
	enum class ServerStartupResult
	{
		Listening, ///< This process owns and is listening on the endpoint.
		Forwarded, ///< An existing process acknowledged the supplied request.
		Failed     ///< Ownership, forwarding, or listening failed.
	};

	using RequestHandler = std::function<void(const Request &)>;

	/**
	 * @brief Encodes one request as a complete length-prefixed protocol frame.
	 * @param request Request to encode.
	 * @return Complete frame suitable for writing to a local socket.
	 */
	[[nodiscard]] QByteArray          encodeRequestFrame(const Request &request);

	/**
	 * @brief Connects to the running instance, sends one request, and waits for its acknowledgement.
	 * @param serverName Local-server name used by the running instance.
	 * @param request Request to deliver.
	 * @param connectionTimeoutMs Maximum time to wait for an existing server connection.
	 * @param deliveryTimeoutMs Maximum time to wait after connecting for delivery acknowledgement.
	 * @param errorMessage Optional delivery failure detail.
	 * @return Delivery status distinguishing an absent server from a failed established connection.
	 */
	[[nodiscard]] SendResult          sendRequest(const QString &serverName, const Request &request,
	                                              int connectionTimeoutMs, int deliveryTimeoutMs,
	                                              QString *errorMessage = nullptr);

	/**
	 * @brief Returns the deterministic interprocess lock path for one local-server endpoint.
	 * @param serverName Local-server endpoint name.
	 * @return Lock path beside an absolute endpoint or in the production temporary directory for a named endpoint.
	 */
	[[nodiscard]] QString             serverStartupLockFilePath(const QString &serverName);

	/**
	 * @brief Serializes endpoint ownership, forwards to an existing server, or starts this server.
	 * @param server Server that listens when no existing process accepts the request.
	 * @param serverName Local-server endpoint name.
	 * @param requestToForward Request for an existing process, or no value for an intentional endpoint takeover.
	 * @param connectionTimeoutMs Maximum time to wait for an existing server connection.
	 * @param deliveryTimeoutMs Maximum time to wait for delivery acknowledgement.
	 * @param startupLockTimeoutMs Maximum time to wait for another process to finish endpoint startup.
	 * @param errorMessage Optional startup or delivery failure detail.
	 * @return Whether this process is listening, forwarded its request, or failed.
	 *
	 * All processes competing for the endpoint must use this function. It holds an interprocess startup lock
	 * across the existing-server probe, stale endpoint removal, and listen operation so one process cannot
	 * remove an endpoint that another process has just created.
	 */
	[[nodiscard]] ServerStartupResult startServerOrForward(QLocalServer &server, const QString &serverName,
	                                                       const std::optional<Request> &requestToForward,
	                                                       int connectionTimeoutMs, int deliveryTimeoutMs,
	                                                       int      startupLockTimeoutMs,
	                                                       QString *errorMessage = nullptr);

	/**
	 * @brief Attaches framed request handling to an accepted local socket.
	 * @param socket Accepted socket. This function assumes responsibility for deleting it.
	 * @param handler Callback invoked once for a valid complete request.
	 *
	 * The frame length determines completion; peer disconnection is only cleanup. Buffered data is consumed
	 * immediately so a client that disconnected before this function was called is still handled correctly.
	 */
	void                              receiveRequest(QLocalSocket *socket, RequestHandler handler);
} // namespace QMudSingleInstanceIpc

#endif // QMUD_SINGLEINSTANCEIPC_H

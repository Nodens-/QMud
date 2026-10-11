/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: AcceptingWorldSocketService.h
 * Role: Deterministic in-memory world transport for tests that require a physical write boundary.
 */

#pragma once

#include "WorldSocketService.h"

#include <QHostAddress>
#include <QVector>

/**
 * @brief Accepts outbound packets in memory without opening a network socket.
 */
class AcceptingWorldSocketService final : public WorldSocketService
{
	public:
		/**
		 * @brief Creates an initially connected test transport.
		 * @param parent Optional Qt owner.
		 */
		explicit AcceptingWorldSocketService(QObject *parent = nullptr) : WorldSocketService(parent)
		{
		}

		/** @brief Ignores transport UTF-8 configuration in the byte-accepting test seam. */
		void setUseUtf8(bool /*enabled*/) override
		{
		}
		/** @brief Ignores keepalive configuration in the byte-accepting test seam. */
		void setKeepAliveEnabled(bool /*enabled*/) override
		{
		}
		/** @brief Ignores proxy configuration in the byte-accepting test seam. */
		void setProxySettings(int /*proxyType*/, const QString & /*server*/, quint16 /*port*/,
		                      const QString & /*username*/, const QString & /*password*/) override
		{
		}
		/** @brief Ignores connection settings in the byte-accepting test seam. */
		void applyConnectionSettings(const WorldSocketConnectionSettings & /*settings*/) override
		{
		}
		/**
		 * @brief Accepts a simulated TLS upgrade.
		 * @return Always `true`.
		 */
		bool startClientEncryption(QString * /*errorMessage*/) override
		{
			return true;
		}
		/**
		 * @brief Marks the in-memory transport connected.
		 * @return Always `true`.
		 */
		bool connectToHost(const QString & /*host*/, quint16 /*port*/) override
		{
			connected = true;
			return true;
		}
		/** @brief Marks the in-memory transport disconnected. */
		void disconnectFromHost() override
		{
			connected = false;
		}
		/**
		 * @brief Records one accepted packet or returns a configured write failure.
		 * @param payload Packet bytes.
		 * @return Payload size when accepted, otherwise `-1`.
		 */
		qint64 sendPacket(const QByteArray &payload) override
		{
			if (!connected || rejectWrites)
				return -1;
			packets.push_back(payload);
			return payload.size();
		}
		/**
		 * @brief Returns the deterministic loopback peer address.
		 * @return Loopback address text.
		 */
		[[nodiscard]] QString peerAddressString() const override
		{
			return QStringLiteral("127.0.0.1");
		}
		/**
		 * @brief Returns the deterministic loopback IPv4 value.
		 * @return Loopback IPv4 address.
		 */
		[[nodiscard]] quint32 peerAddressV4() const override
		{
			return QHostAddress(QHostAddress::LocalHost).toIPv4Address();
		}
		/**
		 * @brief Reports that no native descriptor backs the in-memory transport.
		 * @return Always `-1`.
		 */
		[[nodiscard]] qintptr nativeSocketDescriptor() const override
		{
			return -1;
		}
		/**
		 * @brief Accepts descriptor adoption as an in-memory connected state.
		 * @return Always `true`.
		 */
		bool adoptConnectedSocketDescriptor(qintptr /*descriptor*/, QString * /*errorMessage*/) override
		{
			connected = true;
			return true;
		}
		/** @brief Marks the in-memory transport disconnected immediately. */
		void abortSocket() override
		{
			connected = false;
		}
		/**
		 * @brief Reports whether an asynchronous connection is pending.
		 * @return Always `false`.
		 */
		[[nodiscard]] bool isConnecting() const override
		{
			return false;
		}
		/**
		 * @brief Reports the configured in-memory connection state.
		 * @return Current connection state.
		 */
		[[nodiscard]] bool isConnected() const override
		{
			return connected;
		}

		QVector<QByteArray> packets;
		bool                connected{true};
		bool                rejectWrites{false};
};

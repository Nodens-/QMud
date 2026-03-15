/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: TelnetProcessor.h
 * Role: Telnet protocol processing interfaces for option negotiation and command/data stream parsing from MUD servers.
 */

// This is a Qt-side port of telnet phase handling from MUSHclient. The original
// code contained extensive commentary and references to RFCs and forum posts;
// those comments are preserved in TelnetProcessor.cpp.

#ifndef QMUD_TELNETPROCESSOR_H
#define QMUD_TELNETPROCESSOR_H

#include <QByteArray>
#include <QMap>
#include <QString>
#include <functional>

/**
 * @brief Stateful telnet stream processor with option negotiation hooks.
 *
 * Consumes incoming bytes, tracks protocol state (including MXP/NAWS/MCCP),
 * and emits parsed text/subnegotiation events through callbacks.
 */
class TelnetProcessor
{
	public:
		enum class MxpDefaultMode
		{
			Open,
			Secure,
			Locked
		};

		/**
		 * @brief Creates telnet processor with default negotiation state.
		 */
		TelnetProcessor();

		/**
		 * @brief Callback table used by the processor to report protocol events.
		 */
		struct Callbacks
		{
				/** @brief Telnet negotiation request callback. */
				std::function<bool(int, const QString &)>                     onTelnetRequest;
				/** @brief Telnet subnegotiation callback. */
				std::function<void(int, const QByteArray &)>                  onTelnetSubnegotiation;
				/** @brief Raw telnet option bytes callback. */
				std::function<void(const QByteArray &)>                       onTelnetOption;
				/** @brief IAC-GA received callback. */
				std::function<void()>                                         onIacGa;
				/** @brief MXP start callback. */
				std::function<void(bool pueblo, bool manual)>                 onMxpStart;
				/** @brief MXP reset callback. */
				std::function<void()>                                         onMxpReset;
				/** @brief MXP stop callback. */
				std::function<void(bool completely, bool puebloActive)>       onMxpStop;
				/** @brief MXP mode change callback. */
				std::function<void(int oldMode, int newMode, bool shouldLog)> onMxpModeChange;
				/** @brief MXP diagnostic callback. */
				std::function<void(int level, long messageNumber, const QString &message)> onMxpDiagnostic;
				/** @brief Predicate indicating if diagnostic level is enabled. */
				std::function<bool(int level)>              onMxpDiagnosticNeeded;
				/** @brief NO-ECHO state changed callback. */
				std::function<void(bool enabled)>           onNoEchoChanged;
				/** @brief Fatal protocol error callback. */
				std::function<void(const QString &message)> onFatalProtocolError;
		};

		/**
		 * @brief Enables or disables UTF-8 handling for incoming text decoding.
		 * @param enabled Enable UTF-8 decode mode when `true`.
		 */
		void       setUseUtf8(bool enabled);
		/**
		 * @brief Converts received GA commands into newline output when enabled.
		 * @param enabled Convert GA to newline when `true`.
		 */
		void       setConvertGAtoNewline(bool enabled);
		/**
		 * @brief Controls whether NO_ECHO is turned off when server negotiation allows it.
		 * @param enabled Turn off NO_ECHO when possible when `true`.
		 */
		void       setNoEchoOff(bool enabled);
		/**
		 * @brief Sets MXP mode preference used during negotiation.
		 * @param mode MXP preference/mode code.
		 */
		void       setUseMxp(int mode);
		/**
		 * @brief Enables or disables NAWS support.
		 * @param enabled Enable NAWS negotiation when `true`.
		 */
		void       setNawsEnabled(bool enabled);
		/**
		 * @brief Updates terminal size values used for NAWS replies.
		 * @param columns Terminal width in columns.
		 * @param rows Terminal height in rows.
		 */
		void       setWindowSize(int columns, int rows);
		/**
		 * @brief Sets terminal identification string returned to the server.
		 * @param value Terminal-identification string.
		 */
		void       setTerminalIdentification(const QString &value);
		/**
		 * @brief Prevents compression startup when enabled.
		 * @param enabled Disable compression when `true`.
		 */
		void       setDisableCompression(bool enabled);
		/**
		 * @brief Installs callback targets used for telnet and MXP events.
		 * @param callbacks Callback table.
		 */
		void       setCallbacks(const Callbacks &callbacks);
		/**
		 * @brief Resets telnet state for a new connection.
		 */
		void       resetConnectionState();
		/**
		 * @brief Queues initial option negotiation bytes for a new session.
		 * @param requestSga Request SGA option when `true`.
		 * @param requestEor Request EOR option when `true`.
		 */
		void       queueInitialNegotiation(bool requestSga, bool requestEor);
		/**
		 * @brief Clears MXP state and custom definitions.
		 */
		void       resetMxp();

		/**
		 * @brief Processes incoming bytes and returns display/log payload bytes.
		 * @param data Incoming raw bytes.
		 * @return Processed/plain payload bytes for output.
		 */
		QByteArray processBytes(const QByteArray &data);
		/**
		 * @brief Returns and clears bytes queued for outbound socket write.
		 * @return Queued outbound bytes.
		 */
		QByteArray takeOutboundData();
		/**
		 * @brief Parsed MXP token emitted during stream processing.
		 */
		struct MxpEvent
		{
				enum Type
				{
					StartTag,
					EndTag,
					Definition
				} type{StartTag};
				int                          offset{0};
				int                          sequence{0};
				QByteArray                   name;
				QByteArray                   raw;
				QMap<QByteArray, QByteArray> attributes;
				bool                         secure{false};
		};
		/**
		 * @brief Returns and clears parsed MXP events since the previous call.
		 * @return Parsed MXP events.
		 */
		QList<MxpEvent>      takeMxpEvents();
		/**
		 * @brief Retrieves a custom MXP entity value by name.
		 * @param name Entity name.
		 * @param value Output entity value.
		 * @return `true` when entity exists.
		 */
		bool                 getCustomEntityValue(const QByteArray &name, QByteArray &value) const;
		/**
		 * @brief Resolves an entity using custom and built-in MXP entity tables.
		 * @param name Entity name.
		 * @param value Output resolved value.
		 * @return `true` when entity resolves successfully.
		 */
		bool                 resolveEntityValue(const QByteArray &name, QByteArray &value) const;
		/**
		 * @brief Registers or replaces a custom MXP entity value.
		 * @param name Entity name.
		 * @param value Entity value.
		 */
		void                 setCustomEntity(const QByteArray &name, const QByteArray &value);
		/**
		 * @brief Reports whether current MXP mode is secure.
		 * @return `true` when MXP mode is secure.
		 */
		[[nodiscard]] bool   isMxpSecure() const;
		/**
		 * @brief Reports whether current MXP mode is open.
		 * @return `true` when MXP mode is open.
		 */
		[[nodiscard]] bool   isMxpOpen() const;
		/**
		 * @brief Reports whether MXP processing is active.
		 * @return `true` when MXP processing is enabled.
		 */
		[[nodiscard]] bool   isMxpEnabled() const;
		/**
		 * @brief Reports whether pueblo mode is currently active.
		 * @return `true` when pueblo mode is active.
		 */
		[[nodiscard]] bool   isPuebloActive() const;
		/**
		 * @brief Forces pueblo mode activation.
		 */
		void                 activatePuebloMode();
		/**
		 * @brief Returns the negotiated MCCP compression type.
		 * @return Active MCCP type code.
		 */
		[[nodiscard]] int    mccpType() const;
		/**
		 * @brief Returns the number of compressed bytes received.
		 * @return Compressed byte count.
		 */
		[[nodiscard]] qint64 totalCompressedBytes() const;
		/**
		 * @brief Returns the number of decompressed output bytes produced.
		 * @return Uncompressed byte count.
		 */
		[[nodiscard]] qint64 totalUncompressedBytes() const;
		/**
		 * @brief Returns total parsed MXP tag count.
		 * @return Parsed MXP tag count.
		 */
		[[nodiscard]] qint64 mxpTagCount() const;
		/**
		 * @brief Returns total parsed MXP entity count.
		 * @return Parsed MXP entity count.
		 */
		[[nodiscard]] qint64 mxpEntityCount() const;
		/**
		 * @brief Returns number of currently registered custom MXP elements.
		 * @return Custom MXP element count.
		 */
		[[nodiscard]] int    customElementCount() const;
		/**
		 * @brief Returns number of currently registered custom MXP entities.
		 * @return Custom MXP entity count.
		 */
		[[nodiscard]] int    customEntityCount() const;
		/**
		 * @brief Returns number of built-in MXP entities.
		 * @return Built-in MXP entity count.
		 */
		static int           builtinEntityCount();
		/**
		 * @brief Reports whether input is currently being decompressed.
		 * @return `true` when MCCP decompression is active.
		 */
		[[nodiscard]] bool   isCompressing() const;
		/**
		 * @brief Disables MXP processing for the current connection state.
		 */
		void                 disableMxp();
		/**
		 * @brief Sets default MXP mode applied when MXP is reset.
		 * @param mode Default MXP mode.
		 */
		void                 setMxpDefaultMode(MxpDefaultMode mode);
		/**
		 * @brief Query result for custom MXP element metadata.
		 */
		struct CustomElementInfo
		{
				QByteArray name;
				bool       open{false};
				bool       command{false};
				int        tag{0};
				QByteArray flag;
				QByteArray definition;
				QByteArray attributes;
		};
		/**
		 * @brief Fetches custom element metadata by name.
		 * @param name Element name.
		 * @param info Output element metadata.
		 * @return `true` when custom element exists.
		 */
		bool getCustomElementInfo(const QByteArray &name, CustomElementInfo &info) const;

	private:
		enum Phase
		{
			NONE,
			HAVE_ESC,
			DOING_CODE,
			HAVE_IAC,
			HAVE_WILL,
			HAVE_WONT,
			HAVE_DO,
			HAVE_DONT,
			HAVE_SB,
			HAVE_SUBNEGOTIATION,
			HAVE_SUBNEGOTIATION_IAC,
			HAVE_COMPRESS,
			HAVE_COMPRESS_WILL
		};

		/**
		 * @brief Parses non-compressed incoming bytes for telnet and MXP processing.
		 * @param data Incoming plain bytes.
		 * @return Processed output bytes.
		 */
		QByteArray                          processPlainBytes(const QByteArray &data);
		/**
		 * @brief Inflates incoming payload when MCCP is active.
		 * @param data Incoming bytes.
		 * @return Inflated/output bytes.
		 */
		QByteArray                          inflateIfNeeded(const QByteArray &data);
		/**
		 * @brief Initializes compression stream state for the given MCCP type.
		 * @param type MCCP type code.
		 * @param errorMessage Optional output error message.
		 * @return `true` when compression initialization succeeds.
		 */
		bool                                startCompression(int type, QString *errorMessage = nullptr);
		/**
		 * @brief Finalizes a collected MXP element token.
		 */
		void                                mxpCollectedElement();
		/**
		 * @brief Finalizes a collected MXP entity token.
		 * @return Resolved entity bytes for replacement output.
		 */
		QByteArray                          mxpCollectedEntity();
		/**
		 * @brief Handles truncated MXP element input.
		 * @param reason Truncation reason text.
		 */
		void                                mxpUnterminatedElement(const char *reason);
		/**
		 * @brief Trims MXP helper strings.
		 * @param input Source bytes.
		 * @return Trimmed bytes.
		 */
		static QByteArray                   trimMxp(const QByteArray &input);
		/**
		 * @brief Validates MXP entity/element name characters.
		 * @param input Candidate name bytes.
		 * @return `true` when name is valid.
		 */
		static bool                         isValidName(const QByteArray &input);
		/**
		 * @brief Extracts one word token from mutable input buffer.
		 * @param result Output extracted word.
		 * @param input Mutable input buffer.
		 * @return `true` when a word was extracted.
		 */
		static bool                         getWord(QByteArray &result, QByteArray &input);
		/**
		 * @brief Parses MXP tag arguments into a key-value map.
		 * @param input Tag argument bytes.
		 * @return Parsed argument map.
		 */
		static QMap<QByteArray, QByteArray> parseTagArguments(QByteArray input);
		/**
		 * @brief Emits MXP diagnostic callback when enabled.
		 * @param level Diagnostic level.
		 * @param messageNumber Diagnostic message id.
		 * @param message Diagnostic text.
		 */
		void emitMxpDiagnostic(int level, long messageNumber, const QString &message) const;
		template <typename MessageBuilder>
		void emitMxpDiagnosticLazy(const int level, const long messageNumber, MessageBuilder &&buildMessage)
		{
			if (!m_callbacks.onMxpDiagnostic)
				return;
			if (m_callbacks.onMxpDiagnosticNeeded && !m_callbacks.onMxpDiagnosticNeeded(level))
				return;
			m_callbacks.onMxpDiagnostic(level, messageNumber, buildMessage());
		}
		/**
		 * @brief Parses one MXP definition command.
		 * @param definition Definition payload bytes.
		 */
		void                     mxpDefinition(QByteArray definition);
		/**
		 * @brief Parses one MXP element declaration.
		 * @param name Element name.
		 * @param tagRemainder Remaining declaration bytes.
		 */
		void                     mxpElement(const QByteArray &name, const QByteArray &tagRemainder);
		/**
		 * @brief Parses one MXP ATTLIST declaration.
		 * @param name Element name.
		 * @param tagRemainder Remaining declaration bytes.
		 */
		void                     mxpAttlist(const QByteArray &name, const QByteArray &tagRemainder);
		/**
		 * @brief Parses one MXP entity declaration.
		 * @param name Entity name.
		 * @param tagRemainder Remaining declaration bytes.
		 */
		void                     mxpEntity(const QByteArray &name, const QByteArray &tagRemainder);
		/**
		 * @brief Resolves an MXP entity from custom or built-in tables.
		 * @param name Entity name.
		 * @return Resolved entity bytes.
		 */
		[[nodiscard]] QByteArray mxpGetEntity(const QByteArray &name) const;
		/**
		 * @brief Enters MXP mode and dispatches start callback.
		 * @param pueblo Activate pueblo mode when `true`.
		 * @param manual Mark as manual activation when `true`.
		 */
		void                     mxpOn(bool pueblo, bool manual);
		/**
		 * @brief Exits MXP mode and dispatches stop callback.
		 * @param completely Stop completely when `true`.
		 */
		void                     mxpOff(bool completely);
		/**
		 * @brief Applies MXP mode transition and notifies callback.
		 * @param newMode Target MXP mode.
		 */
		void                     mxpModeChange(int newMode);
		/**
		 * @brief Restores MXP mode after temporary changes.
		 */
		void                     mxpRestoreMode();
		/**
		 * @brief Reports whether current MXP mode is open.
		 * @return `true` when current mode is open.
		 */
		[[nodiscard]] bool       mxpOpen() const;
		/**
		 * @brief Reports whether current MXP mode is secure.
		 * @return `true` when current mode is secure.
		 */
		[[nodiscard]] bool       mxpSecure() const;
		/**
		 * @brief Queues IAC DO option negotiation command.
		 * @param option Telnet option code.
		 */
		void                     sendIacDo(unsigned char option);
		/**
		 * @brief Queues IAC DONT option negotiation command.
		 * @param option Telnet option code.
		 */
		void                     sendIacDont(unsigned char option);
		/**
		 * @brief Queues IAC WILL option negotiation command.
		 * @param option Telnet option code.
		 */
		void                     sendIacWill(unsigned char option);
		/**
		 * @brief Queues IAC WONT option negotiation command.
		 * @param option Telnet option code.
		 */
		void                     sendIacWont(unsigned char option);
		/**
		 * @brief Sends CHARSET ACCEPTED subnegotiation.
		 * @param charset Accepted charset name.
		 */
		void                     sendCharsetAccepted(const QByteArray &charset);
		/**
		 * @brief Sends CHARSET REJECTED subnegotiation.
		 */
		void                     sendCharsetRejected();
		/**
		 * @brief Sends terminal type response subnegotiation.
		 */
		void                     sendTerminalType();
		/**
		 * @brief Sends current NAWS window size.
		 */
		void                     sendWindowSize();

		Phase                    m_phase{NONE};
		int                      m_code{0};
		bool                     m_convertGAtoNewline{false};
		bool                     m_noEchoOff{false};
		bool                     m_noEcho{false};
		bool                     m_utf8{false};
		int                      m_useMxp{3}; // eNoMXP by default
		bool                     m_naws{false};
		bool                     m_nawsWanted{false};
		int                      m_wrapColumns{0};
		int                      m_wrapRows{0};
		int                      m_ttypeSequence{0};
		QString                  m_terminalIdentification{QStringLiteral("QMud")};

		int                      m_subnegotiationType{0};
		QByteArray               m_subnegotiationData;
		QByteArray               m_outbound;
		QByteArray               m_ansiBuffer;
		QByteArray               m_pendingCompressed;
		QByteArray               m_compressInput;
		QByteArray               m_compressOutputChunk;
		QByteArray               m_postCompressionRemainder;
		int                      m_compressInputOffset{0};
		int                      m_outputSize{0};

		enum MxpPhase
		{
			MXP_NONE,
			HAVE_MXP_ELEMENT,
			HAVE_MXP_COMMENT,
			HAVE_MXP_QUOTE,
			HAVE_MXP_ENTITY
		};

		int                          m_mxpMode{2}; // default locked until enabled
		int                          m_mxpDefaultMode{0};
		int                          m_mxpPreviousMode{0};
		bool                         m_mxpEnabled{false};
		bool                         m_puebloActive{false};
		MxpPhase                     m_mxpPhase{MXP_NONE};
		QByteArray                   m_mxpString;
		unsigned char                m_mxpQuoteTerminator{0};
		QMap<QByteArray, QByteArray> m_customEntities;
		struct CustomElement
		{
				QByteArray name;
				bool       open{false};
				bool       command{false};
				int        tag{0};
				QByteArray flag;
				QByteArray definition;
				QByteArray attributes;
		};
		QMap<QByteArray, CustomElement> m_customElements;
		QList<MxpEvent>                 m_mxpEvents;
		int                             m_mxpEventSequence{0};
		Callbacks                       m_callbacks;

		bool                            m_compress{false};
		int                             m_mccpType{0};
		bool                            m_compressInitOk{false};
		bool                            m_supportsMccp2{false};
		bool                            m_disableCompression{false};
		qint64                          m_totalCompressedBytes{0};
		qint64                          m_totalUncompressedBytes{0};
		qint64                          m_mxpTagCount{0};
		qint64                          m_mxpEntityCount{0};

		bool                            m_seenCompressWillIac{false};
		bool                            m_requestedSga{false};
		bool                            m_requestedEor{false};

		struct ZStreamWrapper;
		ZStreamWrapper *m_zlib{nullptr};
};

#endif // QMUD_TELNETPROCESSOR_H

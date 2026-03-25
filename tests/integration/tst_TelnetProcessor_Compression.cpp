/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_TelnetProcessor_Compression.cpp
 * Role: QTest coverage for TelnetProcessor Compression behavior.
 */

#include "TelnetCallbackSpy.h"
#include "TelnetProcessor.h"

#include <QtTest/QTest>
#include <zlib.h>

namespace
{
	constexpr unsigned char IAC              = 0xFF;
	constexpr unsigned char WILL             = 0xFB;
	constexpr unsigned char WONT             = 0xFC;
	constexpr unsigned char SB               = 0xFA;
	constexpr unsigned char SE               = 0xF0;
	constexpr unsigned char TELOPT_COMPRESS2 = 86;

	QByteArray              bytes(std::initializer_list<unsigned char> raw)
	{
		QByteArray out;
		for (const unsigned char c : raw)
			out.append(static_cast<char>(c));
		return out;
	}

	QByteArray makeQtZlibPayload(const QByteArray &plain)
	{
		QByteArray compressed = qCompress(plain, 9);
		if (compressed.size() < 4)
			return {};
		compressed.remove(0, 4); // qCompress prefixes uncompressed length, MCCP expects raw zlib stream only.
		return compressed;
	}

	QByteArray makeZlibSyncFlushPayload(const QByteArray &plain)
	{
		z_stream stream{};
		if (deflateInit(&stream, Z_BEST_COMPRESSION) != Z_OK)
			return {};
		const auto maxOut = static_cast<int>(deflateBound(&stream, static_cast<uLong>(plain.size())) + 32U);
		if (maxOut <= 0)
		{
			deflateEnd(&stream);
			return {};
		}
		QByteArray payload(maxOut, '\0');
		stream.next_in   = reinterpret_cast<Bytef *>(const_cast<char *>(plain.constData()));
		stream.avail_in  = static_cast<uInt>(plain.size());
		stream.next_out  = reinterpret_cast<Bytef *>(payload.data());
		stream.avail_out = static_cast<uInt>(payload.size());
		const int rc     = deflate(&stream, Z_SYNC_FLUSH);
		deflateEnd(&stream);
		if (rc != Z_OK)
			return {};
		payload.resize(payload.size() - static_cast<int>(stream.avail_out));
		return payload;
	}
} // namespace

/**
 * @brief QTest fixture covering TelnetProcessor Compression scenarios.
 */
class tst_TelnetProcessor_Compression : public QObject
{
		Q_OBJECT

		// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		void disableCompressionRejectsWillCompress2()
		{
			TelnetProcessor processor;
			processor.setDisableCompression(true);

			processor.processBytes(bytes({IAC, WILL, TELOPT_COMPRESS2}));
			QCOMPARE(processor.takeOutboundData(), bytes({IAC, 0xFE, TELOPT_COMPRESS2}));
		}

		void mccp2ActivationAndInflate()
		{
			TelnetProcessor  processor;
			const QByteArray compressed = makeQtZlibPayload(QByteArrayLiteral("hello"));
			QVERIFY(!compressed.isEmpty());

			const QByteArray packet = bytes({IAC, SB, TELOPT_COMPRESS2, IAC, SE}) + compressed;
			const QByteArray output = processor.processBytes(packet);

			QCOMPARE(output, QByteArrayLiteral("hello"));
			QVERIFY(!processor.isCompressing());
			QCOMPARE(processor.mccpType(), 0);
			QVERIFY(processor.totalCompressedBytes() > 0);
			QVERIFY(processor.totalUncompressedBytes() >= 5);
		}

		void malformedCompressedDataEmitsFatalError()
		{
			TelnetProcessor   processor;
			TelnetCallbackSpy spy;
			processor.setCallbacks(spy.callbacks());

			processor.processBytes(bytes({IAC, SB, TELOPT_COMPRESS2, IAC, SE}));
			QVERIFY(processor.isCompressing());

			processor.processBytes(QByteArrayLiteral("not-a-zlib-stream"));
			QVERIFY(!spy.fatalProtocolErrors.isEmpty());
			QVERIFY(!processor.isCompressing());
			QCOMPARE(processor.mccpType(), 0);
		}

		void postTeardownBytesInSameReadAreProcessed()
		{
			TelnetProcessor  processor;
			const QByteArray compressed = makeQtZlibPayload(QByteArrayLiteral("4. This is line four\n"));
			QVERIFY(!compressed.isEmpty());

			QByteArray packet = bytes({IAC, SB, TELOPT_COMPRESS2, IAC, SE});
			packet.append(compressed);
			packet.append(QByteArrayLiteral("5. This is line five\n"));

			const QByteArray output = processor.processBytes(packet);
			QCOMPARE(output, QByteArrayLiteral("4. This is line four\n5. This is line five\n"));
			QVERIFY(!processor.isCompressing());
			QCOMPARE(processor.mccpType(), 0);
		}

		void wontCompress2ClearsCompressionStateImmediately()
		{
			TelnetProcessor processor;

			processor.processBytes(bytes({IAC, SB, TELOPT_COMPRESS2, IAC, SE}));
			QVERIFY(processor.isCompressing());
			QCOMPARE(processor.mccpType(), 2);

			const QByteArray wontSequence   = bytes({IAC, WONT, TELOPT_COMPRESS2});
			const QByteArray compressedWont = makeZlibSyncFlushPayload(wontSequence);
			QVERIFY(!compressedWont.isEmpty());

			const QByteArray output = processor.processBytes(compressedWont);
			QCOMPARE(output, QByteArray());
			QVERIFY(!processor.isCompressing());
			QCOMPARE(processor.mccpType(), 0);
		}

		void mccpRestartAfterStreamEndPreservesCompressedOrdering()
		{
			TelnetProcessor   processor;
			TelnetCallbackSpy spy;
			processor.setCallbacks(spy.callbacks());

			const QByteArray firstStream = makeQtZlibPayload(QByteArrayLiteral("old-stream\n"));
			QVERIFY(!firstStream.isEmpty());

			QByteArray firstPacket = bytes({IAC, SB, TELOPT_COMPRESS2, IAC, SE});
			firstPacket.append(firstStream);
			firstPacket.append(QByteArrayLiteral("copyover-tail\n"));

			const QByteArray firstOutput = processor.processBytes(firstPacket);
			QCOMPARE(firstOutput, QByteArrayLiteral("old-stream\ncopyover-tail\n"));
			QVERIFY(!processor.isCompressing());
			QCOMPARE(processor.mccpType(), 0);

			const QByteArray restartStream = makeQtZlibPayload(QByteArrayLiteral("new-stream\n"));
			QVERIFY(restartStream.size() > 6);
			constexpr int    splitAt      = 4;
			const QByteArray restartPartA = restartStream.left(splitAt);
			const QByteArray restartPartB = restartStream.mid(splitAt);
			QVERIFY(!restartPartA.isEmpty());
			QVERIFY(!restartPartB.isEmpty());

			QByteArray restartNegotiationPacket = QByteArrayLiteral("after-copyover\n");
			restartNegotiationPacket.append(bytes({IAC, SB, TELOPT_COMPRESS2, IAC, SE}));
			restartNegotiationPacket.append(restartPartA);

			const QByteArray restartOutputA = processor.processBytes(restartNegotiationPacket);
			QVERIFY(restartOutputA.startsWith(QByteArrayLiteral("after-copyover\n")));
			QVERIFY(processor.isCompressing());
			QCOMPARE(processor.mccpType(), 2);
			QVERIFY(spy.fatalProtocolErrors.isEmpty());

			const QByteArray restartOutputB = processor.processBytes(restartPartB);
			QCOMPARE(restartOutputA + restartOutputB, QByteArrayLiteral("after-copyover\nnew-stream\n"));
			QVERIFY(!processor.isCompressing());
			QCOMPARE(processor.mccpType(), 0);
			QVERIFY(spy.fatalProtocolErrors.isEmpty());
		}
		// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_APPLESS_MAIN(tst_TelnetProcessor_Compression)


#if __has_include("tst_TelnetProcessor_Compression.moc")
#include "tst_TelnetProcessor_Compression.moc"
#endif

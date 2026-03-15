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

namespace
{
	constexpr unsigned char IAC              = 0xFF;
	constexpr unsigned char WILL             = 0xFB;
	constexpr unsigned char SB               = 0xFA;
	constexpr unsigned char SE               = 0xF0;
	constexpr unsigned char TELOPT_COMPRESS2 = 86;

	QByteArray bytes(std::initializer_list<unsigned char> raw)
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
			TelnetProcessor processor;
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
			TelnetProcessor  processor;
			TelnetCallbackSpy spy;
			processor.setCallbacks(spy.callbacks());

			processor.processBytes(bytes({IAC, SB, TELOPT_COMPRESS2, IAC, SE}));
			QVERIFY(processor.isCompressing());

			processor.processBytes(QByteArrayLiteral("not-a-zlib-stream"));
			QVERIFY(!spy.fatalProtocolErrors.isEmpty());
			QVERIFY(!processor.isCompressing());
			QCOMPARE(processor.mccpType(), 0);
		}
	// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_APPLESS_MAIN(tst_TelnetProcessor_Compression)

#include "tst_TelnetProcessor_Compression.moc"

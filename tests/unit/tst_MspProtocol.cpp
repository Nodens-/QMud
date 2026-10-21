/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_MspProtocol.cpp
 * Role: Unit coverage for native MUD Sound Protocol parsing.
 */

#include "MspProtocol.h"

#include <QtTest/QTest>

namespace
{
	/** @brief QTest fixture covering native MSP parsing and validation. */
	class tst_MspProtocol : public QObject
	{
			Q_OBJECT

		private slots:
			/** @brief Verifies line-form SOUND and MUSIC parameters. */
			static void parsesSoundAndMusicParameters()
			{
				const auto sound = QMudMspProtocol::parseLine(
				    QStringLiteral("!!SOUND(combat/hit V=35 L=3 P=80 T=weapon U=audio.example/)"));
				QVERIFY(sound);
				QCOMPARE(sound->type, QMudMspProtocol::MediaType::Sound);
				QCOMPARE(sound->fileName, QStringLiteral("combat/hit"));
				QCOMPARE(sound->volume, 35);
				QCOMPARE(sound->repeatCount, 3);
				QCOMPARE(sound->priority, 80);
				QCOMPARE(sound->tag, QStringLiteral("weapon"));
				QCOMPARE(sound->url, QStringLiteral("audio.example/"));

				const auto extended =
				    QMudMspProtocol::parseLine(QStringLiteral("!!SOUND(combat/hit X=2.0 V=45)"));
				QVERIFY(extended.has_value());
				QCOMPARE(extended->fileName, QStringLiteral("combat/hit"));
				QCOMPARE(extended->volume, 45);

				const auto windowsPath = QMudMspProtocol::parseLine(
				    QStringLiteral(R"(!!SOUND(C:\MUSHclient\sounds\combat\hit.wav V=55))"));
				QVERIFY(windowsPath);
				QCOMPARE(windowsPath->fileName, QStringLiteral("sounds/combat/hit.wav"));
				QCOMPARE(windowsPath->volume, 55);
				const auto wildcard =
				    QMudMspProtocol::parseLine(QStringLiteral("!!SOUND(combat/alarm*.wav)"));
				QVERIFY(wildcard);
				QCOMPARE(wildcard->fileName, QStringLiteral("combat/alarm*.wav"));

				const auto music =
				    QMudMspProtocol::parseLine(QStringLiteral("!!MUSIC(ambient.mid V=70 L=-1 C=0)"));
				QVERIFY(music);
				QCOMPARE(music->type, QMudMspProtocol::MediaType::Music);
				QCOMPARE(music->repeatCount, -1);
				QVERIFY(!music->continueMusic);
			}

			/** @brief Verifies line-form stop commands. */
			static void parsesStopCommand()
			{
				const auto stop = QMudMspProtocol::parseLine(QStringLiteral("!!SOUND(Off)"));
				QVERIFY(stop);
				QVERIFY(stop->stop);
				QVERIFY(!stop->setDefaultUrl);

				const auto setUrl = QMudMspProtocol::parseLine(
				    QStringLiteral("!!SOUND(Off U=https://audio.example/sounds/)"));
				QVERIFY(setUrl);
				QVERIFY(!setUrl->stop);
				QVERIFY(setUrl->setDefaultUrl);
				QVERIFY(setUrl->urlProvided);
				QCOMPARE(setUrl->url, QStringLiteral("https://audio.example/sounds/"));
			}

			/** @brief Verifies malformed and escaping commands are rejected. */
			static void rejectsMalformedOrUnsafeCommands()
			{
				QVERIFY(!QMudMspProtocol::parseLine(QStringLiteral("prefix !!SOUND(hit.wav)")));
				QVERIFY(!QMudMspProtocol::parseLine(QStringLiteral("!!SOUND(../hit.wav)")));
				QVERIFY(!QMudMspProtocol::parseLine(QStringLiteral("!!SOUND(/hit.wav)")));
				QVERIFY(!QMudMspProtocol::parseLine(QStringLiteral("!!SOUND(hit.wav V=101)")));
				QVERIFY(!QMudMspProtocol::parseLine(QStringLiteral("!!SOUND(hit.wav C=1)")));
				QVERIFY(!QMudMspProtocol::parseLine(QStringLiteral("!!MUSIC(hit.mid P=10)")));
				QVERIFY(!QMudMspProtocol::parseLine(QStringLiteral("!!SOUND(folder*/hit.wav)")));
				QVERIFY(!QMudMspProtocol::parseLine(QStringLiteral("!!SOUND(*hit*.wav)")));
				QVERIFY(!QMudMspProtocol::parseLine(QStringLiteral("!!SOUND(hit*?.wav)")));
				const auto unknownParameter =
				    QMudMspProtocol::parseLine(QStringLiteral("!!SOUND(hit.wav UNKNOWN=1 V=45)"));
				QVERIFY(unknownParameter);
				QCOMPARE(unknownParameter->fileName, QStringLiteral("hit.wav"));
				QCOMPARE(unknownParameter->volume, 45);
				QVERIFY(!QMudMspProtocol::parseLine(QStringLiteral("!!SOUND (hit.wav)")));
				QVERIFY(!QMudMspProtocol::parseLine(QStringLiteral("!!SOUND(hit.wav) ")));
				QVERIFY(!QMudMspProtocol::parseLine(QStringLiteral("!!MUSIC(hit.mid)\t")));
			}
	};
} // namespace

QTEST_MAIN(tst_MspProtocol)

#include "tst_MspProtocol.moc"

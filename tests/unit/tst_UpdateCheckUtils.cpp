/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: tst_UpdateCheckUtils.cpp
 * Role: Unit coverage for update release payload/version evaluation helpers.
 */

#include "UpdateCheckUtils.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSysInfo>
#include <QtTest/QTest>

namespace
{
	/**
	 * @brief Returns deterministic SHA-256 sample digest used by tests.
	 * @return 64-character lowercase hex digest.
	 */
	QString sampleSha256()
	{
		return QStringLiteral("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
	}

	/**
	 * @brief Creates one release-asset JSON object.
	 * @param name Asset name.
	 * @param url Browser download URL.
	 * @param digest Digest text assigned to `digest` field.
	 * @return Asset JSON object.
	 */
	QJsonObject makeAsset(const QString &name, const QString &url, const QString &digest)
	{
		QJsonObject asset;
		asset.insert(QStringLiteral("name"), name);
		asset.insert(QStringLiteral("browser_download_url"), url);
		asset.insert(QStringLiteral("digest"), digest);
		return asset;
	}

	/**
	 * @brief Creates one latest-release payload JSON document.
	 * @param tag Release tag (`tag_name`).
	 * @param assets Release assets list.
	 * @param body Changelog body.
	 * @return Serialized JSON payload.
	 */
	QByteArray makeLatestReleasePayload(const QString &tag, const QJsonArray &assets,
	                                    const QString &body = QStringLiteral("changelog body"))
	{
		QJsonObject root;
		root.insert(QStringLiteral("tag_name"), tag);
		root.insert(QStringLiteral("draft"), false);
		root.insert(QStringLiteral("prerelease"), false);
		root.insert(QStringLiteral("assets"), assets);
		root.insert(QStringLiteral("body"), body);
		return QJsonDocument(root).toJson(QJsonDocument::Compact);
	}
} // namespace

/**
 * @brief Unit fixture for update-check helper functions.
 */
class tst_UpdateCheckUtils : public QObject
{
		Q_OBJECT

		// NOLINTBEGIN(readability-convert-member-functions-to-static)
	private slots:
		/**
		 * @brief Verifies SHA-256 normalization accepts supported digest forms.
		 */
		void normalizeSha256DigestAcceptsCanonicalForms()
		{
			const QString sha = sampleSha256();
			QCOMPARE(QMudUpdateCheck::normalizeSha256Digest(sha), sha);
			QCOMPARE(QMudUpdateCheck::normalizeSha256Digest(QStringLiteral("sha256:%1").arg(sha)), sha);
			QCOMPARE(QMudUpdateCheck::normalizeSha256Digest(QStringLiteral("0x%1").arg(sha.toUpper())), sha);
		}

		/**
		 * @brief Verifies malformed payloads return parse-error status.
		 */
		void evaluateLatestReleasePayloadParseError()
		{
			const auto result = QMudUpdateCheck::evaluateLatestReleasePayload(
			    QByteArrayLiteral("{not-json"), QStringLiteral("10.04"), QString(),
			    QMudUpdateCheck::InstallTarget::LinuxAppImage);
			QCOMPARE(result.status, QMudUpdateCheck::ReleaseEvaluationStatus::ParseError);
		}

		/**
		 * @brief Verifies `-ci` releases are treated as no-stable-release.
		 */
		void evaluateLatestReleasePayloadIgnoresCiRelease()
		{
			const QByteArray payload = makeLatestReleasePayload(QStringLiteral("v10.08-ci"), QJsonArray());
			const auto       result  = QMudUpdateCheck::evaluateLatestReleasePayload(
                payload, QStringLiteral("10.04"), QString(), QMudUpdateCheck::InstallTarget::LinuxAppImage);
			QCOMPARE(result.status, QMudUpdateCheck::ReleaseEvaluationStatus::NoStableRelease);
		}

		/**
		 * @brief Verifies up-to-date outcome when latest version is not newer.
		 */
		void evaluateLatestReleasePayloadUpToDate()
		{
			const QByteArray payload = makeLatestReleasePayload(QStringLiteral("v10.04"), QJsonArray());
			const auto       result  = QMudUpdateCheck::evaluateLatestReleasePayload(
                payload, QStringLiteral("10.04"), QString(), QMudUpdateCheck::InstallTarget::LinuxAppImage);
			QCOMPARE(result.status, QMudUpdateCheck::ReleaseEvaluationStatus::UpToDate);
		}

		/**
		 * @brief Verifies update-available result includes chosen compatible asset and skip reset.
		 */
		void evaluateLatestReleasePayloadUpdateAvailableAndSkipReset()
		{
			QJsonArray assets;
			assets.push_back(makeAsset(QStringLiteral("QMud-10.07-x86_64.AppImage"),
			                           QStringLiteral("https://example.invalid/qmud.appimage"),
			                           QStringLiteral("sha256:%1").arg(sampleSha256())));
			const QByteArray payload = makeLatestReleasePayload(QStringLiteral("v10.07"), assets,
			                                                    QStringLiteral("fixes and features"));

			const auto       result = QMudUpdateCheck::evaluateLatestReleasePayload(
                payload, QStringLiteral("10.04"), QStringLiteral("10.06"),
                QMudUpdateCheck::InstallTarget::LinuxAppImage);

			QCOMPARE(result.status, QMudUpdateCheck::ReleaseEvaluationStatus::UpdateAvailable);
			QCOMPARE(result.releaseVersion, QStringLiteral("10.07"));
			QCOMPARE(result.changelog, QStringLiteral("fixes and features"));
			QCOMPARE(result.asset.name, QStringLiteral("QMud-10.07-x86_64.AppImage"));
			QCOMPARE(result.asset.url, QStringLiteral("https://example.invalid/qmud.appimage"));
			QCOMPARE(result.asset.sha256, sampleSha256());
			QVERIFY(result.clearSkipVersion);
			QVERIFY(!result.isSkippedVersion);
		}

		/**
		 * @brief Verifies skipped-version marker is set when release equals stored skip version.
		 */
		void evaluateLatestReleasePayloadSkippedVersion()
		{
			QJsonArray assets;
			assets.push_back(makeAsset(QStringLiteral("QMud-10.07-x86_64.AppImage"),
			                           QStringLiteral("https://example.invalid/qmud.appimage"),
			                           QStringLiteral("sha256:%1").arg(sampleSha256())));
			const QByteArray payload = makeLatestReleasePayload(QStringLiteral("v10.07"), assets);

			const auto       result = QMudUpdateCheck::evaluateLatestReleasePayload(
                payload, QStringLiteral("10.04"), QStringLiteral("10.07"),
                QMudUpdateCheck::InstallTarget::LinuxAppImage);

			QCOMPARE(result.status, QMudUpdateCheck::ReleaseEvaluationStatus::UpdateAvailable);
			QVERIFY(!result.clearSkipVersion);
			QVERIFY(result.isSkippedVersion);
		}

		/**
		 * @brief Verifies newer releases without compatible+hashed assets are reported accordingly.
		 */
		void evaluateLatestReleasePayloadNoCompatibleAsset()
		{
			QJsonArray assets;
			assets.push_back(makeAsset(QStringLiteral("QMud-10.07-x86_64.AppImage"),
			                           QStringLiteral("https://example.invalid/qmud.appimage"), QString()));
			const QByteArray payload = makeLatestReleasePayload(QStringLiteral("v10.07"), assets);

			const auto       result = QMudUpdateCheck::evaluateLatestReleasePayload(
                payload, QStringLiteral("10.04"), QString(), QMudUpdateCheck::InstallTarget::LinuxAppImage);

			QCOMPARE(result.status, QMudUpdateCheck::ReleaseEvaluationStatus::NoCompatibleAsset);
			QCOMPARE(result.releaseVersion, QStringLiteral("10.07"));
		}

		/**
		 * @brief Verifies explicit architecture mismatches are rejected as incompatible.
		 */
		void evaluateLatestReleasePayloadRejectsMismatchedArchitectureAsset()
		{
			const QString cpu = QSysInfo::currentCpuArchitecture().toLower();
			QString       mismatchedAssetName;
			if (cpu.contains(QStringLiteral("x86_64")) || cpu.contains(QStringLiteral("amd64")) ||
			    cpu.contains(QStringLiteral("x64")))
			{
				mismatchedAssetName = QStringLiteral("QMud-10.07-arm64.AppImage");
			}
			else if (cpu.contains(QStringLiteral("arm64")) || cpu.contains(QStringLiteral("aarch64")))
			{
				mismatchedAssetName = QStringLiteral("QMud-10.07-x86_64.AppImage");
			}
			else
			{
				QSKIP("Current CPU architecture is neither x86_64 nor arm64/aarch64.");
			}

			QJsonArray assets;
			assets.push_back(makeAsset(mismatchedAssetName,
			                           QStringLiteral("https://example.invalid/qmud-mismatch.appimage"),
			                           QStringLiteral("sha256:%1").arg(sampleSha256())));
			const QByteArray payload = makeLatestReleasePayload(QStringLiteral("v10.07"), assets);

			const auto       result = QMudUpdateCheck::evaluateLatestReleasePayload(
                payload, QStringLiteral("10.04"), QString(), QMudUpdateCheck::InstallTarget::LinuxAppImage);

			QCOMPARE(result.status, QMudUpdateCheck::ReleaseEvaluationStatus::NoCompatibleAsset);
		}

		/**
		 * @brief Verifies Windows update target selects setup executable assets.
		 */
		void evaluateLatestReleasePayloadWindowsSelectsSetupExe()
		{
			QJsonArray assets;
			assets.push_back(makeAsset(QStringLiteral("QMud-10.07-windows.zip"),
			                           QStringLiteral("https://example.invalid/qmud-windows.zip"),
			                           QStringLiteral("sha256:%1").arg(sampleSha256())));
			assets.push_back(makeAsset(QStringLiteral("QMud-10.07-win-setup.exe"),
			                           QStringLiteral("https://example.invalid/qmud-win-setup.exe"),
			                           QStringLiteral("sha256:%1").arg(sampleSha256())));

			const QByteArray payload = makeLatestReleasePayload(QStringLiteral("v10.07"), assets);
			const auto       result  = QMudUpdateCheck::evaluateLatestReleasePayload(
                payload, QStringLiteral("10.04"), QString(),
                QMudUpdateCheck::InstallTarget::WindowsInstaller);

			QCOMPARE(result.status, QMudUpdateCheck::ReleaseEvaluationStatus::UpdateAvailable);
			QCOMPARE(result.asset.name, QStringLiteral("QMud-10.07-win-setup.exe"));
			QCOMPARE(result.asset.url, QStringLiteral("https://example.invalid/qmud-win-setup.exe"));
		}

		/**
		 * @brief Verifies Windows target rejects portable-only assets.
		 */
		void evaluateLatestReleasePayloadWindowsRejectsPortableOnlyAssets()
		{
			QJsonArray assets;
			assets.push_back(makeAsset(QStringLiteral("QMud-10.07-windows.zip"),
			                           QStringLiteral("https://example.invalid/qmud-windows.zip"),
			                           QStringLiteral("sha256:%1").arg(sampleSha256())));

			const QByteArray payload = makeLatestReleasePayload(QStringLiteral("v10.07"), assets);
			const auto       result  = QMudUpdateCheck::evaluateLatestReleasePayload(
                payload, QStringLiteral("10.04"), QString(),
                QMudUpdateCheck::InstallTarget::WindowsInstaller);

			QCOMPARE(result.status, QMudUpdateCheck::ReleaseEvaluationStatus::NoCompatibleAsset);
		}
		// NOLINTEND(readability-convert-member-functions-to-static)
};

QTEST_MAIN(tst_UpdateCheckUtils)


#if __has_include("tst_UpdateCheckUtils.moc")
#include "tst_UpdateCheckUtils.moc"
#endif

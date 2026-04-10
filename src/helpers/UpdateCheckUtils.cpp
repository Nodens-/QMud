/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: UpdateCheckUtils.cpp
 * Role: Pure helpers for update release/version evaluation and release-asset selection.
 */

#include "UpdateCheckUtils.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSysInfo>
#include <limits>

namespace
{
	bool assetNameContainsAny(const QString &assetNameLower, const QStringList &tokens)
	{
		return std::ranges::any_of(tokens, [&assetNameLower](const QString &token)
		                           { return assetNameLower.contains(token); });
	}

	QString extractAssetSha256(const QJsonObject &assetObj)
	{
		QString digest =
		    QMudUpdateCheck::normalizeSha256Digest(assetObj.value(QStringLiteral("digest")).toString());
		if (!digest.isEmpty())
			return digest;
		digest = QMudUpdateCheck::normalizeSha256Digest(assetObj.value(QStringLiteral("sha256")).toString());
		return digest;
	}

	int architecturePreferenceScore(const QString &assetNameLower)
	{
		const QString cpu = QSysInfo::currentCpuArchitecture().toLower();
		if (cpu.contains(QStringLiteral("arm64")) || cpu.contains(QStringLiteral("aarch64")))
		{
			if (assetNameContainsAny(assetNameLower, {QStringLiteral("arm64"), QStringLiteral("aarch64")}))
				return 4;
			if (assetNameLower.contains(QStringLiteral("universal")))
				return 3;
			if (assetNameContainsAny(assetNameLower, {QStringLiteral("x86_64"), QStringLiteral("amd64"),
			                                          QStringLiteral("x64")}))
				return -3;
			return 0;
		}
		if (cpu.contains(QStringLiteral("x86_64")) || cpu.contains(QStringLiteral("amd64")) ||
		    cpu.contains(QStringLiteral("x64")))
		{
			if (assetNameContainsAny(assetNameLower, {QStringLiteral("x86_64"), QStringLiteral("amd64"),
			                                          QStringLiteral("x64")}))
				return 4;
			if (assetNameLower.contains(QStringLiteral("universal")))
				return 3;
			if (assetNameContainsAny(assetNameLower, {QStringLiteral("arm64"), QStringLiteral("aarch64")}))
				return -3;
			return 0;
		}
		return 0;
	}
} // namespace

namespace QMudUpdateCheck
{
	QString versionCore(QString text)
	{
		text = text.trimmed();
		if (text.startsWith(QLatin1Char('v'), Qt::CaseInsensitive))
			text.remove(0, 1);

		QString out;
		out.reserve(text.size());
		bool sawDigit = false;
		for (const QChar c : std::as_const(text))
		{
			if (c.isDigit())
			{
				out.push_back(c);
				sawDigit = true;
				continue;
			}
			if (c == QLatin1Char('.') && sawDigit)
			{
				out.push_back(c);
				continue;
			}
			break;
		}
		while (out.endsWith(QLatin1Char('.')))
			out.chop(1);
		return out;
	}

	int compareVersions(const QString &left, const QString &right)
	{
		const auto versionParts = [](const QString &text)
		{
			QVector<int>  out;
			const QString core = versionCore(text);
			if (core.isEmpty())
				return out;
			const QStringList parts = core.split(QLatin1Char('.'), Qt::SkipEmptyParts);
			out.reserve(parts.size());
			for (const QString &part : parts)
			{
				bool      ok = false;
				const int n  = part.toInt(&ok);
				out.push_back(ok ? n : 0);
			}
			return out;
		};

		const QVector<int> a   = versionParts(left);
		const QVector<int> b   = versionParts(right);
		const qsizetype    max = qMax(a.size(), b.size());
		for (qsizetype i = 0; i < max; ++i)
		{
			const int av = i < a.size() ? a.at(i) : 0;
			const int bv = i < b.size() ? b.at(i) : 0;
			if (av < bv)
				return -1;
			if (av > bv)
				return 1;
		}
		return 0;
	}

	QString normalizeSha256Digest(QString digestText)
	{
		digestText = digestText.trimmed();
		if (digestText.isEmpty())
			return {};
		const qsizetype separatorIndex = digestText.indexOf(QLatin1Char(':'));
		if (separatorIndex >= 0)
		{
			const QString algorithm = digestText.left(separatorIndex).trimmed();
			if (algorithm.compare(QStringLiteral("sha256"), Qt::CaseInsensitive) != 0)
				return {};
			digestText = digestText.mid(separatorIndex + 1).trimmed();
		}
		if (digestText.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
			digestText.remove(0, 2);
		digestText = digestText.toLower();
		if (digestText.size() != 64)
			return {};
		for (const QChar c : std::as_const(digestText))
		{
			const bool isHexDigit = (c >= QLatin1Char('0') && c <= QLatin1Char('9')) ||
			                        (c >= QLatin1Char('a') && c <= QLatin1Char('f'));
			if (!isHexDigit)
				return {};
		}
		return digestText;
	}

	ReleaseAssetSelection selectReleaseAssetForPlatform(const QJsonArray &assets, const InstallTarget target)
	{
		ReleaseAssetSelection selected;
		if (target == InstallTarget::Unsupported)
			return selected;

		int selectedScore = std::numeric_limits<int>::min();
		for (const auto &value : assets)
		{
			if (!value.isObject())
				continue;
			const QJsonObject assetObj = value.toObject();
			const QString     name     = assetObj.value(QStringLiteral("name")).toString().trimmed();
			const QString url = assetObj.value(QStringLiteral("browser_download_url")).toString().trimmed();
			const QString sha256 = extractAssetSha256(assetObj);
			if (name.isEmpty() || url.isEmpty() || sha256.isEmpty())
				continue;

			const QString lowerName = name.toLower();
			int           score     = std::numeric_limits<int>::min();
			if (target == InstallTarget::LinuxAppImage)
			{
				if (!lowerName.endsWith(QStringLiteral(".appimage")) ||
				    lowerName.endsWith(QStringLiteral(".appimage.zsync")))
				{
					continue;
				}
				const int archScore = architecturePreferenceScore(lowerName);
				if (archScore < 0)
					continue;
				score = 100 + archScore;
			}
			else if (target == InstallTarget::MacBundle)
			{
				const int archScore = architecturePreferenceScore(lowerName);
				if (archScore < 0)
					continue;
				if (lowerName.endsWith(QStringLiteral(".app.zip")))
				{
					score = 220 + archScore;
				}
				else if (lowerName.endsWith(QStringLiteral(".zip")))
				{
					const bool hasMacMarker =
					    assetNameContainsAny(lowerName, {QStringLiteral("mac"), QStringLiteral("macos"),
					                                     QStringLiteral("osx"), QStringLiteral("darwin")});
					const bool hasWindowsMarker =
					    assetNameContainsAny(lowerName, {QStringLiteral("windows"), QStringLiteral("win32"),
					                                     QStringLiteral("win64"), QStringLiteral("mingw"),
					                                     QStringLiteral(".msi"), QStringLiteral(".exe")});
					if (hasMacMarker && !hasWindowsMarker)
						score = 180 + archScore;
					else
						continue;
				}
				else
				{
					continue;
				}
			}
			else if (target == InstallTarget::WindowsInstaller)
			{
				if (!lowerName.endsWith(QStringLiteral(".exe")))
					continue;

				const bool hasWindowsMarker =
				    assetNameContainsAny(lowerName, {QStringLiteral("windows"), QStringLiteral("win"),
				                                     QStringLiteral("setup"), QStringLiteral("installer")});
				if (!hasWindowsMarker)
					continue;

				const bool hasPortableMarker =
				    assetNameContainsAny(lowerName, {QStringLiteral("portable"), QStringLiteral(".zip")});
				if (hasPortableMarker)
					continue;

				const bool has32BitMarker = assetNameContainsAny(
				    lowerName, {QStringLiteral("x86"), QStringLiteral("win32"), QStringLiteral("i386"),
				                QStringLiteral("32bit"), QStringLiteral("32-bit"), QStringLiteral("x32")});
				if (has32BitMarker)
					continue;

				const int archScore = architecturePreferenceScore(lowerName);
				if (archScore < 0)
					continue;
				score = 260 + archScore;
			}

			if (score > selectedScore)
			{
				selectedScore   = score;
				selected.name   = name;
				selected.url    = url;
				selected.sha256 = sha256;
			}
		}
		return selected;
	}

	ReleaseEvaluationResult evaluateLatestReleasePayload(const QByteArray   &payload,
	                                                     const QString      &currentVersion,
	                                                     const QString      &skipVersion,
	                                                     const InstallTarget target)
	{
		ReleaseEvaluationResult result;

		QJsonParseError         parseError;
		const QJsonDocument     doc = QJsonDocument::fromJson(payload, &parseError);
		if (parseError.error != QJsonParseError::NoError || !doc.isObject())
		{
			result.status = ReleaseEvaluationStatus::ParseError;
			return result;
		}

		const QJsonObject root = doc.object();
		if (root.value(QStringLiteral("draft")).toBool() || root.value(QStringLiteral("prerelease")).toBool())
		{
			result.status = ReleaseEvaluationStatus::NoStableRelease;
			return result;
		}

		QString releaseTag = root.value(QStringLiteral("tag_name")).toString().trimmed();
		if (releaseTag.isEmpty())
			releaseTag = root.value(QStringLiteral("name")).toString().trimmed();
		if (releaseTag.contains(QStringLiteral("-ci"), Qt::CaseInsensitive))
		{
			result.status = ReleaseEvaluationStatus::NoStableRelease;
			return result;
		}

		const QString releaseVersionCore = versionCore(releaseTag);
		const QString currentVersionCore = versionCore(currentVersion);
		if (releaseVersionCore.isEmpty() || currentVersionCore.isEmpty())
		{
			result.status = ReleaseEvaluationStatus::InvalidVersion;
			return result;
		}
		result.releaseVersion = releaseVersionCore;

		if (compareVersions(releaseVersionCore, currentVersionCore) <= 0)
		{
			result.status = ReleaseEvaluationStatus::UpToDate;
			return result;
		}

		const QString skipVersionCore = versionCore(skipVersion);
		if (!skipVersionCore.isEmpty() && compareVersions(releaseVersionCore, skipVersionCore) > 0)
			result.clearSkipVersion = true;
		result.isSkippedVersion =
		    !skipVersionCore.isEmpty() && compareVersions(releaseVersionCore, skipVersionCore) == 0;

		result.asset = selectReleaseAssetForPlatform(root.value(QStringLiteral("assets")).toArray(), target);
		if (result.asset.url.trimmed().isEmpty() || result.asset.name.trimmed().isEmpty() ||
		    result.asset.sha256.trimmed().isEmpty())
		{
			result.status = ReleaseEvaluationStatus::NoCompatibleAsset;
			return result;
		}

		result.changelog = root.value(QStringLiteral("body")).toString();
		result.status    = ReleaseEvaluationStatus::UpdateAvailable;
		return result;
	}
} // namespace QMudUpdateCheck

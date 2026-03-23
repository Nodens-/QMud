/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: UpdateCheckUtils.h
 * Role: Pure helpers for update release/version evaluation and release-asset selection.
 */

#ifndef QMUD_UPDATECHECKUTILS_H
#define QMUD_UPDATECHECKUTILS_H

// ReSharper disable once CppUnusedIncludeDirective
#include <QByteArray>
#include <QJsonArray>
#include <QString>

namespace QMudUpdateCheck
{
	/**
	 * @brief Platform/runtime target used for release-asset selection.
	 */
	enum class InstallTarget
	{
		Unsupported,
		LinuxAppImage,
		MacBundle,
		WindowsInstaller,
	};

	/**
	 * @brief Selected release-asset metadata for one platform target.
	 */
	struct ReleaseAssetSelection
	{
			QString name;
			QString url;
			QString sha256;
	};

	/**
	 * @brief High-level outcome of evaluating a latest-release payload.
	 */
	enum class ReleaseEvaluationStatus
	{
		ParseError,
		NoStableRelease,
		InvalidVersion,
		UpToDate,
		NoCompatibleAsset,
		UpdateAvailable,
	};

	/**
	 * @brief Structured decision result from latest-release payload evaluation.
	 */
	struct ReleaseEvaluationResult
	{
			ReleaseEvaluationStatus status{ReleaseEvaluationStatus::ParseError};
			QString                 releaseVersion;
			QString                 changelog;
			ReleaseAssetSelection   asset;
			bool                    clearSkipVersion{false};
			bool                    isSkippedVersion{false};
	};

	/**
	 * @brief Extracts the numeric core from a version string (for example `v10.07-ci` -> `10.07`).
	 * @param text Source version text.
	 * @return Numeric version core, empty when invalid.
	 */
	[[nodiscard]] QString                 versionCore(QString text);
	/**
	 * @brief Compares two version strings by numeric dot-separated components.
	 * @param left Left-hand version text.
	 * @param right Right-hand version text.
	 * @return `-1`, `0`, or `1` for less/equal/greater.
	 */
	[[nodiscard]] int                     compareVersions(const QString &left, const QString &right);
	/**
	 * @brief Normalizes SHA-256 digest text to 64-char lowercase hex.
	 * @param digestText Source digest text.
	 * @return Normalized digest, or empty on invalid input.
	 */
	[[nodiscard]] QString                 normalizeSha256Digest(QString digestText);
	/**
	 * @brief Selects best matching release asset for the given platform target.
	 * @param assets GitHub release `assets` array.
	 * @param target Runtime install target.
	 * @return Selected asset metadata, empty when no compatible asset exists.
	 */
	[[nodiscard]] ReleaseAssetSelection   selectReleaseAssetForPlatform(const QJsonArray &assets,
	                                                                    InstallTarget     target);
	/**
	 * @brief Evaluates latest-release payload against current/skip versions and target.
	 * @param payload JSON payload from GitHub latest-release endpoint.
	 * @param currentVersion Running application version.
	 * @param skipVersion Stored skipped version, if any.
	 * @param target Runtime install target.
	 * @return Parsed/evaluated release decision.
	 */
	[[nodiscard]] ReleaseEvaluationResult evaluateLatestReleasePayload(const QByteArray &payload,
	                                                                   const QString    &currentVersion,
	                                                                   const QString    &skipVersion,
	                                                                   InstallTarget     target);
} // namespace QMudUpdateCheck

#endif // QMUD_UPDATECHECKUTILS_H

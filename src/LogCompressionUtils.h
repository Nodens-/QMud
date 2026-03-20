/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: LogCompressionUtils.h
 * Role: Shared gzip file-compression helpers used by logging-related features.
 */

#ifndef QMUD_LOGCOMPRESSIONUTILS_H
#define QMUD_LOGCOMPRESSIONUTILS_H

#include <QByteArray>
#include <QFile>
#include <QString>
#include <zlib.h>

/**
 * @brief Compresses one file in place using gzip.
 *
 * Creates `<source>.gz`, then removes the original source file on success.
 *
 * @param sourceFileName Source file path.
 * @param errorMessage Optional output error details on failure.
 * @return `true` when compression and replacement succeed.
 */
inline bool qmudGzipFileInPlace(const QString &sourceFileName, QString *errorMessage)
{
	QFile source(sourceFileName);
	if (!source.open(QIODevice::ReadOnly))
	{
		if (errorMessage)
			*errorMessage = QStringLiteral("Could not open \"%1\" for reading").arg(sourceFileName);
		return false;
	}

	const QString compressedName = sourceFileName + QStringLiteral(".gz");
	QFile::remove(compressedName);

	const QByteArray compressedNameBytes = QFile::encodeName(compressedName);
	gzFile           gz                  = gzopen(compressedNameBytes.constData(), "wb");
	if (!gz)
	{
		if (errorMessage)
			*errorMessage = QStringLiteral("Could not open \"%1\" for gzip writing").arg(compressedName);
		return false;
	}

	constexpr qint64 kChunkSize = 64 * 1024;
	QByteArray       chunk;
	chunk.resize(kChunkSize);
	while (!source.atEnd())
	{
		const qint64 bytesRead = source.read(chunk.data(), kChunkSize);
		if (bytesRead < 0)
		{
			if (errorMessage)
				*errorMessage =
				    QStringLiteral("Could not read \"%1\" for gzip compression").arg(sourceFileName);
			gzclose(gz);
			QFile::remove(compressedName);
			return false;
		}
		if (bytesRead == 0)
			continue;
		const int expected = static_cast<int>(bytesRead);
		const int written  = gzwrite(gz, chunk.constData(), static_cast<unsigned int>(expected));
		if (written != expected)
		{
			int         zError   = Z_OK;
			const char *zMessage = gzerror(gz, &zError);
			if (errorMessage)
				*errorMessage =
				    (zMessage && zError != Z_OK)
				        ? QStringLiteral("gzip write failed: %1").arg(QString::fromLatin1(zMessage))
				        : QStringLiteral("gzip write failed for \"%1\"").arg(sourceFileName);
			gzclose(gz);
			QFile::remove(compressedName);
			return false;
		}
	}

	if (gzclose(gz) != Z_OK)
	{
		if (errorMessage)
			*errorMessage = QStringLiteral("Could not finalize gzip file \"%1\"").arg(compressedName);
		QFile::remove(compressedName);
		return false;
	}

	source.close();
	if (!QFile::remove(sourceFileName))
	{
		if (errorMessage)
			*errorMessage = QStringLiteral("Created \"%1\" but could not remove \"%2\"")
			                    .arg(compressedName, sourceFileName);
		return false;
	}

	return true;
}

#endif // QMUD_LOGCOMPRESSIONUTILS_H

#pragma once

#include <QList>
#include <QPair>
#include <QString>

// Details for Properties: image size, camera and exposure, date taken and
// GPS position; audio and video length, bitrate, frame rate, tags; PDF
// pages and author (core code). Read with exiftool in the sandbox
// (Sandbox::run); without it, images still get their size from Qt.
// Blocking: call it off the GUI thread.
namespace MediaInfo {

using Details = QList<QPair<QString, QString>>; // label, value, in display order

// True for types worth looking at (images, audio, video, PDF).
bool interesting(const QString &mimeType);
Details read(const QString &path, const QString &mimeType);

// Turns exiftool's JSON (one object) into labelled rows (exposed for tests).
Details fromExifToolJson(const QByteArray &json);

} // namespace MediaInfo

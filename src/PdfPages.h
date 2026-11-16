#pragma once

#include <QImage>
#include <QSizeF>
#include <QString>

// The pages of a PDF as pictures, for the reader in the preview pane (core
// code). A PDF is a file that isn't trusted, so it is never read by
// Argonaut itself: `pdfinfo` and `pdftoppm` (poppler-utils) read it, one
// run a question, inside the sandbox (Sandbox::run: no network, nothing of
// the user's visible but that one file). Without the tools or the sandbox
// there is no reader, and the preview is what it is for any other file.
//
// Both calls block (tens of milliseconds a page): call them on a worker.
namespace PdfPages {

bool available(); // pdfinfo and pdftoppm are installed and the sandbox works

struct Info
{
    bool ok = false;   // it could be read (false: not a PDF, damaged, or locked with a password)
    int pages = 0;
    QSizeF pageSize;   // of the first page, in points, turned as it is shown
};
Info info(const QString &path);
// What `pdfinfo` printed, as that (for tests).
Info parseInfo(const QByteArray &output);

// Page `number` (from 1) drawn `width` pixels wide. Null if it can't be.
QImage page(const QString &path, int number, int width);

constexpr int MaxWidth = 4000; // pixels: a page is never drawn wider

} // namespace PdfPages

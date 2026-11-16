#include "PdfPages.h"

#include "Sandbox.h"

#include <QStandardPaths>
#include <QStringList>

namespace PdfPages {

bool available()
{
    static const bool tools = !QStandardPaths::findExecutable(QStringLiteral("pdfinfo")).isEmpty()
                              && !QStandardPaths::findExecutable(QStringLiteral("pdftoppm")).isEmpty();
    return tools && Sandbox::works();
}

Info parseInfo(const QByteArray &output)
{
    Info info;
    int turned = 0;
    const QList<QByteArray> lines = output.split('\n');
    for (const QByteArray &line : lines) {
        const qsizetype colon = line.indexOf(':');
        if (colon <= 0)
            continue;
        const QByteArray key = line.left(colon);
        const QByteArray value = line.mid(colon + 1).simplified();
        if (key == "Pages") {
            info.pages = value.toInt();
        } else if (key == "Page size") { // "595.27 x 841.89 pts (A4)"
            const QList<QByteArray> parts = value.split(' ');
            if (parts.size() >= 3)
                info.pageSize = QSizeF(parts.at(0).toDouble(), parts.at(2).toDouble());
        } else if (key == "Page rot") {
            turned = value.toInt();
        }
    }
    if (turned == 90 || turned == 270)
        info.pageSize.transpose();
    if (info.pageSize.width() < 1 || info.pageSize.height() < 1)
        info.pageSize = QSizeF(595, 842); // not said: A4
    info.ok = info.pages > 0 && info.pages < 1000000;
    return info;
}

Info info(const QString &path)
{
    if (!available())
        return {};
    bool ok = false;
    const QByteArray out = Sandbox::run(QStringLiteral("pdfinfo"), {QStringLiteral("@FILE@")}, path, &ok, 10000, 64 * 1024);
    return ok ? parseInfo(out) : Info();
}

QImage page(const QString &path, int number, int width)
{
    if (!available() || number < 1)
        return {};
    width = qBound(16, width, MaxWidth);
    bool ok = false;
    // One page, as wide as asked and as tall as that makes it, written to
    // standard output as a plain picture (PPM: nothing to compress and unpack).
    const QStringList args{QStringLiteral("-f"), QString::number(number), QStringLiteral("-l"), QString::number(number),
                           QStringLiteral("-scale-to-x"), QString::number(width), QStringLiteral("-scale-to-y"), QStringLiteral("-1"),
                           QStringLiteral("@FILE@")};
    const QByteArray out = Sandbox::run(QStringLiteral("pdftoppm"), args, path, &ok, 20000, 256 * 1024 * 1024);
    if (!ok)
        return {};
    QImage image;
    image.loadFromData(out, "PPM");
    return image;
}

} // namespace PdfPages

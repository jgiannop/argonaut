// Tests for Open With parsing. Never runs the real `gio`.

#include "OpenWith.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QLocale>
#include <QTemporaryDir>
#include <QtTest>

class TestOpenWith : public QObject
{
    Q_OBJECT

private slots:
    void parsesGioOutput();
    void parsesNoDefault();
    void readsDesktopFiles();
};

void TestOpenWith::parsesGioOutput()
{
    const OpenWith::Apps apps = OpenWith::parseGioMime(
        "Default application for “text/plain”: org.gnome.TextEditor.desktop\n"
        "Registered applications:\n"
        "\torg.gnome.TextEditor.desktop\n"
        "\tlibreoffice-writer.desktop\n"
        "\tcode.desktop\n"
        "Recommended applications:\n"
        "\torg.gnome.TextEditor.desktop\n"
        "\tlibreoffice-writer.desktop\n");
    QCOMPARE(apps.defaultId, QStringLiteral("org.gnome.TextEditor.desktop"));
    QCOMPARE(apps.registeredIds, QStringList({"org.gnome.TextEditor.desktop",
                                              "libreoffice-writer.desktop", "code.desktop"}));
    QCOMPARE(apps.recommendedIds, QStringList({"org.gnome.TextEditor.desktop",
                                               "libreoffice-writer.desktop"}));
}

void TestOpenWith::parsesNoDefault()
{
    const OpenWith::Apps apps = OpenWith::parseGioMime(
        "No default applications for “application/x-thing”\n"
        "No registered applications\n"
        "No recommended applications\n");
    QVERIFY(apps.defaultId.isEmpty());
    QVERIFY(apps.registeredIds.isEmpty());
}

void TestOpenWith::readsDesktopFiles()
{
    QTemporaryDir dir;
    auto write = [&](const QString &name, const QByteArray &content) {
        QFile f(dir.path() + '/' + name);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(content);
    };
    write("editor.desktop", "[Desktop Entry]\nType=Application\nName=Text Editor\n"
                            "Name[el]=Επεξεργαστής κειμένου\nIcon=accessories-text-editor\n"
                            "Exec=editor %U\n[Desktop Action New]\nName=Should be ignored\n");
    write("hidden.desktop", "[Desktop Entry]\nName=Helper\nNoDisplay=true\n");

    QLocale::setDefault(QLocale(QLocale::English));
    OpenWith::App app = OpenWith::appFromDesktopFile(dir.path() + "/editor.desktop");
    QCOMPARE(app.name, QStringLiteral("Text Editor"));
    QCOMPARE(app.icon, QStringLiteral("accessories-text-editor"));
    QCOMPARE(app.id, QStringLiteral("editor.desktop"));

    QLocale::setDefault(QLocale(QLocale::Greek, QLocale::Greece));
    app = OpenWith::appFromDesktopFile(dir.path() + "/editor.desktop");
    QCOMPARE(app.name, QStringLiteral("Επεξεργαστής κειμένου")); // translated name wins

    QVERIFY(OpenWith::appFromDesktopFile(dir.path() + "/hidden.desktop").name.isEmpty());
    QVERIFY(OpenWith::appFromDesktopFile(dir.path() + "/missing.desktop").name.isEmpty());
}

QTEST_GUILESS_MAIN(TestOpenWith)
#include "tst_openwith.moc"

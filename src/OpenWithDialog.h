#pragma once

#include "OpenWith.h"

#include <QDialog>
#include <QIcon>

class QCheckBox;
class QListWidget;
class QPushButton;

QIcon appIcon(const OpenWith::App &app);

// "Open With Other Application…": every app that can open the file type,
// with "Always use for … files" to make the choice the default.
class OpenWithDialog : public QDialog
{
    Q_OBJECT

public:
    OpenWithDialog(const QStringList &files, const QString &mimeType, const QString &typeName,
                   QWidget *parent = nullptr);

    OpenWith::App chosen() const;
    bool makeDefault() const;

private:
    QList<OpenWith::App> m_apps;
    QListWidget *m_list = nullptr;
    QCheckBox *m_default = nullptr;
    QPushButton *m_open = nullptr;
};

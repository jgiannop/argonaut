#pragma once

#include "FileJob.h"

#include <QDialog>

class QCheckBox;
class QLineEdit;

// "Replace file “x”?" — asked when a copy/move/restore meets an existing
// item with the same name. Shows both items (icon or thumbnail, size, date)
// and offers Replace / Merge / Skip / Keep Both / Cancel, plus a new name
// and "apply to all".
class ConflictDialog : public QDialog
{
    Q_OBJECT

public:
    ConflictDialog(const QString &source, const QString &existing, QWidget *parent = nullptr);

    FileJob::Resolution resolution() const { return m_resolution; }

private:
    QWidget *itemCard(const QString &heading, const QString &path, bool newer);
    void choose(FileJob::ConflictAction action);

    FileJob::Resolution m_resolution{FileJob::ConflictAction::Cancel, false, QString()};
    QLineEdit *m_name = nullptr;
    QCheckBox *m_applyToAll = nullptr;
    QString m_suggestedName;
};

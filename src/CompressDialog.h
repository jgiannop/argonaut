#pragma once

#include "FileJob.h"

#include <QDialog>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;

// "Compress…": the archive's name and format (remembered between uses).
class CompressDialog : public QDialog
{
    Q_OBJECT

public:
    CompressDialog(const QStringList &paths, const QString &targetDir, QWidget *parent = nullptr);

    QString archiveName() const; // with the suffix, e.g. "Photos.zip"
    FileJob::ArchiveFormat format() const;

private:
    void validate();

    QString m_targetDir;
    QLineEdit *m_name = nullptr;
    QComboBox *m_format = nullptr;
    QLabel *m_problem = nullptr;
    QPushButton *m_create = nullptr;
};

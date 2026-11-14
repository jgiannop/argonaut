#pragma once

#include "Naming.h"

#include <QDateTime>
#include <QDialog>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTreeWidget;

// Renames several items at once (F2 with more than one selected), like
// Nautilus: a template with [Original name] and numbers, or find and
// replace. Only the base name changes; extensions are kept. A live
// preview shows every new name and any clash before anything is renamed.
class BatchRenameDialog : public QDialog
{
    Q_OBJECT

public:
    explicit BatchRenameDialog(const QStringList &paths, QWidget *parent = nullptr);

    // The renames to do, in numbering order (only valid after Accepted).
    QList<Naming::RenameItem> items() const;

private:
    struct Source
    {
        QString path;
        QString name;
        bool isDir = false;
        QDateTime modified;
    };

    void insertTag(const QString &tag);
    void sortSources();
    void updatePreview();

    QList<Source> m_sources; // in numbering order
    QList<Naming::RenameItem> m_items;

    QButtonGroup *m_mode = nullptr;
    QStackedWidget *m_pages = nullptr;
    QLineEdit *m_template = nullptr;
    QSpinBox *m_start = nullptr;
    QComboBox *m_order = nullptr;
    QLineEdit *m_find = nullptr;
    QLineEdit *m_replace = nullptr;
    QCheckBox *m_caseSensitive = nullptr;
    QTreeWidget *m_preview = nullptr;
    QLabel *m_summary = nullptr;
    QPushButton *m_rename = nullptr;
};

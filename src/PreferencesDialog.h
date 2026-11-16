#pragma once

#include <QDialog>

// Application preferences. Changes apply immediately (no OK/Cancel), like
// GNOME apps.
class PreferencesDialog : public QDialog
{
    Q_OBJECT

public:
    explicit PreferencesDialog(QWidget *parent = nullptr);

signals:
    // Show this in the file window: a folder (the themes folder) is opened,
    // a file (a theme just saved) is shown selected in its folder.
    void folderRequested(const QString &path);

private:
    void addClaudeSection(class QVBoxLayout *layout);
};

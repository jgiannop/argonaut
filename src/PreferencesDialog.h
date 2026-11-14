#pragma once

#include <QDialog>

// Application preferences. Changes apply immediately (no OK/Cancel), like
// GNOME apps.
class PreferencesDialog : public QDialog
{
    Q_OBJECT

public:
    explicit PreferencesDialog(QWidget *parent = nullptr);

private:
    void addClaudeSection(class QVBoxLayout *layout);
};

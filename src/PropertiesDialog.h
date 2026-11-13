#pragma once

#include <QDialog>
#include <QStringList>

#include <atomic>
#include <memory>

class QCheckBox;
class QComboBox;
class QLabel;

// Properties of one or more items: type, size (folders are measured in the
// background), dates, free space, and Nautilus-style permissions.
class PropertiesDialog : public QDialog
{
    Q_OBJECT

public:
    explicit PropertiesDialog(const QStringList &paths, QWidget *parent = nullptr);
    ~PropertiesDialog() override;

private:
    void measure();
    void addPermissions(class QFormLayout *form, const QString &path);
    void applyPermissions();

    QStringList m_paths;
    QLabel *m_size = nullptr;
    std::shared_ptr<std::atomic_bool> m_cancel = std::make_shared<std::atomic_bool>(false);

    QComboBox *m_owner = nullptr;
    QComboBox *m_group = nullptr;
    QComboBox *m_others = nullptr;
    QCheckBox *m_exec = nullptr;
    bool m_isDir = false;
};

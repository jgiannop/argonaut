#pragma once

#include <QMainWindow>
#include <QStack>
#include <QString>

class QAction;
class QFileSystemModel;
class QLineEdit;
class QListView;
class QListWidget;
class QModelIndex;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

    // Show the folder at `path`. Returns false if it doesn't exist.
    bool navigateTo(const QString &path, bool addToHistory = true);

private:
    void setupToolbar();
    void setupViews();
    void populateSidebar();

    void goBack();
    void goForward();
    void goUp();
    void openIndex(const QModelIndex &index);
    void setGridMode(bool grid);
    void setShowHidden(bool show);
    void updateActions();
    void updateStatus();

    QFileSystemModel *m_model = nullptr;
    QListView *m_view = nullptr;
    QListWidget *m_sidebar = nullptr;
    QLineEdit *m_pathEdit = nullptr;

    QAction *m_backAct = nullptr;
    QAction *m_forwardAct = nullptr;
    QAction *m_upAct = nullptr;

    QStack<QString> m_backHistory;
    QStack<QString> m_forwardHistory;
    QString m_current;
};

#pragma once

#include <QDialog>
#include <QKeySequence>
#include <QList>

class QLabel;
class QLineEdit;
class QListWidget;
class QTextBrowser;

// Help → User Guide (F1): the guide built into the app, so it works offline
// and always matches this version. A topic list with a search field on the
// left, the page on the right. The Keyboard Shortcuts page is made from the
// window's real actions, so it can't go out of date.
class HelpDialog : public QDialog
{
    Q_OBJECT

public:
    struct Shortcut
    {
        QString action;
        QList<QKeySequence> keys; // shown as "A or B"
        QString note;             // instead of keys, e.g. "Ctrl + mouse wheel"
    };
    struct ShortcutGroup
    {
        QString title;
        QList<Shortcut> shortcuts;
    };

    HelpDialog(const QList<ShortcutGroup> &shortcuts, QWidget *parent = nullptr);

    // Shows a topic by id: "start", "shortcuts", "rename"…
    void showTopic(const QString &id);

protected:
    void changeEvent(QEvent *event) override;

private:
    struct Topic
    {
        QString id;
        QString title;
        QString html; // body, with [[Ctrl+F]] for keys and topic:id links
    };

    void buildTopics();
    void render();             // the current topic, in the current theme
    void applySearch();        // filter topics, highlight matches
    QString pageHtml(const Topic &topic) const;
    QString shortcutsHtml() const;

    QList<ShortcutGroup> m_shortcuts;
    QList<Topic> m_topics;
    QLineEdit *m_search = nullptr;
    QListWidget *m_list = nullptr;
    QTextBrowser *m_page = nullptr;
    QLabel *m_noMatch = nullptr;
};

#pragma once

#include <QObject>
#include <QPointer>
#include <QRect>
#include <QString>

#include <functional>

class QLineEdit;
class QWidget;

// Renaming an item where it is shown (F2): a text field laid over the item's
// name. Enter renames, Esc gives up, clicking elsewhere renames if the name
// can be used and gives up otherwise. A name that is taken or not allowed
// keeps the field open and says why.
//
// One per window; at most one rename at a time.
class InlineRename : public QObject
{
    Q_OBJECT

public:
    struct Host
    {
        // Where the item's name is drawn now, in the viewport the field sits
        // on. A null rectangle means the item is gone (the rename ends).
        std::function<QRect(const QString &path)> nameRect;
        std::function<void(const QString &text)> showMessage;
    };

    explicit InlineRename(const Host &host, QObject *parent = nullptr);

    bool isActive() const { return !m_edit.isNull(); }
    QString path() const { return m_path; } // the item being renamed

    // Opens the field over `path`'s name, inside `viewport`, with the name
    // selected up to its extension (like Nautilus).
    void start(QWidget *viewport, const QString &path, bool isFolder, Qt::Alignment alignment);
    // The view scrolled, resized or repainted: follow the item.
    void reposition();
    // Renames now. False (and the field stays open) if the name can't be used.
    bool commit();
    void cancel();

signals:
    void renamed(const QString &from, const QString &to); // done on disk
    void closed();                                        // the field is gone: the view gets the keyboard back

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    Host m_host;
    QPointer<QLineEdit> m_edit;
    QString m_path;
    // Renaming changes the folder, the view repaints, and reposition() would
    // find the old name gone and close the field under commit()'s feet.
    bool m_committing = false;
};

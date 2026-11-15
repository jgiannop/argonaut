#include "InlineRename.h"

#include "Naming.h"

#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QWidget>

InlineRename::InlineRename(const Host &host, QObject *parent) : QObject(parent), m_host(host) {}

void InlineRename::start(QWidget *viewport, const QString &path, bool isFolder, Qt::Alignment alignment)
{
    cancel();
    m_path = path;
    m_edit = new QLineEdit(viewport);
    m_edit->setObjectName("renameEdit");
    m_edit->setText(QFileInfo(path).fileName());
    m_edit->setAlignment(alignment);
    m_edit->installEventFilter(this);
    reposition();
    if (!m_edit) // the item was already gone
        return;
    m_edit->show();
    m_edit->setFocus(Qt::OtherFocusReason);

    // Select the name without its extension ("photo" of "photo.tar.gz").
    QString extension;
    const QString base = Naming::baseName(m_edit->text(), isFolder, &extension);
    m_edit->setSelection(0, int(base.size()));
}

void InlineRename::reposition()
{
    if (!m_edit || m_committing)
        return;
    QRect rect = m_host.nameRect(m_path);
    if (rect.isNull()) {
        cancel();
        return;
    }
    // Keep it inside the view, also for a name at the edge.
    const int width = m_edit->parentWidget()->width();
    if (rect.right() > width - 3)
        rect.moveRight(width - 3);
    if (rect.left() < 3)
        rect.moveLeft(3);
    m_edit->setGeometry(rect);
}

void InlineRename::cancel()
{
    if (!m_edit)
        return;
    QLineEdit *edit = m_edit;
    m_edit = nullptr;
    m_path.clear();
    edit->removeEventFilter(this);
    edit->hide();
    edit->deleteLater();
    emit closed();
}

bool InlineRename::commit()
{
    if (!m_edit || m_committing)
        return true;
    const QString newName = m_edit->text().trimmed();
    const QFileInfo old(m_path);
    if (newName.isEmpty() || newName == old.fileName()) {
        cancel(); // nothing to do
        return true;
    }

    QString problem = Naming::nameProblem(newName);
    const QString from = m_path;
    const QString to = old.absolutePath() + QLatin1Char('/') + newName;
    const QFileInfo target(to);
    // (target != old: "a.txt" -> "A.txt" on a drive that ignores case.)
    if (problem.isEmpty() && (target.exists() || target.isSymLink()) && target != old)
        problem = tr("“%1” already exists").arg(newName);
    if (problem.isEmpty()) {
        m_committing = true;
        const bool done = QDir().rename(from, to);
        m_committing = false;
        if (!done)
            problem = tr("Couldn't rename “%1”").arg(old.fileName());
    }
    if (!problem.isEmpty()) {
        m_host.showMessage(problem);
        if (m_edit)
            m_edit->setFocus();
        return false;
    }
    cancel();
    emit renamed(from, to);
    return true;
}

// Enter must be consumed here: QLineEdit passes it on, and the view would
// then open the item.
bool InlineRename::eventFilter(QObject *watched, QEvent *event)
{
    if (!m_edit || watched != m_edit)
        return QObject::eventFilter(watched, event);
    if (event->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent *>(event)->key();
        if (key == Qt::Key_Escape) {
            cancel();
            return true;
        }
        if (key == Qt::Key_Return || key == Qt::Key_Enter) {
            commit();
            return true;
        }
    }
    // Clicking elsewhere (but not opening the field's own menu).
    if (event->type() == QEvent::FocusOut && static_cast<QFocusEvent *>(event)->reason() != Qt::PopupFocusReason) {
        if (!commit())
            cancel();
    }
    return QObject::eventFilter(watched, event);
}

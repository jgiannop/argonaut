#include "ExtensionMenu.h"

#include <QHash>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>

namespace ExtensionMenu {

bool run(QWidget *window, const Extensions::Command &command, const QString &folder, const QStringList &selection)
{
    Extensions::Host *host = Extensions::Host::instance();
    const QString title = host->nameOf(command.extension);
    const QString verb = QString(command.label).remove(QStringLiteral("…"));
    if (!command.confirm.isEmpty()) {
        QMessageBox box(QMessageBox::Question, title, command.confirm, QMessageBox::NoButton, window);
        box.setTextFormat(Qt::PlainText);
        QPushButton *go = box.addButton(verb, QMessageBox::AcceptRole);
        box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(QMessageBox::Cancel);
        box.exec();
        if (box.clickedButton() != go)
            return false;
    }
    QString input;
    if (command.asks) {
        QInputDialog dialog(window);
        dialog.setWindowTitle(title);
        dialog.setLabelText(command.askLabel.isEmpty() ? verb : command.askLabel);
        dialog.setInputMode(QInputDialog::TextInput);
        dialog.setOption(QInputDialog::UsePlainTextEditForTextInput, command.askMultiline);
        dialog.setTextValue(command.askText);
        dialog.setOkButtonText(verb);
        dialog.resize(command.askMultiline ? QSize(460, 260) : QSize(420, dialog.sizeHint().height()));
        if (dialog.exec() != QDialog::Accepted)
            return false;
        input = dialog.textValue().trimmed();
        if (input.isEmpty())
            return false; // nothing typed is "never mind"
    }
    host->invoke(command, folder, selection, input);
    return true;
}

int add(QMenu *menu, QWidget *window, const QString &folder, const QList<Extensions::Item> &selection)
{
    QStringList paths;
    for (const Extensions::Item &item : selection)
        paths << item.path;
    const QList<Extensions::Command> commands = Extensions::Host::instance()->commands(folder, selection);
    QHash<QString, QMenu *> groups; // "Git", "Git/Switch Branch"
    for (const Extensions::Command &command : commands) {
        QMenu *target = menu;
        QString path;
        const QStringList levels = command.group.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        for (const QString &level : levels) {
            path += (path.isEmpty() ? QString() : QStringLiteral("/")) + level;
            QMenu *&sub = groups[path];
            if (!sub)
                sub = target->addMenu(QString(level).replace(QLatin1Char('&'), QStringLiteral("&&")));
            target = sub;
        }
        // A name is a name: an "&" in a branch isn't a shortcut key.
        QAction *action = target->addAction(QString(command.label).replace(QLatin1Char('&'), QStringLiteral("&&")));
        if (command.checked) {
            action->setCheckable(true);
            action->setChecked(true);
        }
        QObject::connect(action, &QAction::triggered, window,
                         [window, command, folder, paths] { run(window, command, folder, paths); });
    }
    return int(commands.size());
}

void flatten(QMenu *menu)
{
    const QList<QAction *> actions = menu->actions();
    if (actions.size() != 1 || !actions.first()->menu())
        return;
    QMenu *only = actions.first()->menu();
    menu->removeAction(actions.first());
    const QList<QAction *> inner = only->actions();
    for (QAction *action : inner) {
        only->removeAction(action);
        action->setParent(menu);
        if (QMenu *sub = action->menu())
            sub->setParent(menu, sub->windowFlags());
        menu->addAction(action);
    }
}

} // namespace ExtensionMenu

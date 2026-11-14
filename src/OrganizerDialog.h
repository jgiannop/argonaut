#pragma once

#include "Organizer.h"

#include <QDialog>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QTextBrowser;
class QTreeWidget;
class QWidget;

// File → Organize with Claude…: three steps in one window.
//  1. Start: what will be sent, your wishes, the mode for this folder, and
//     the estimated cost (count_tokens, free). Nothing is sent before Start.
//  2. Working: progress; in Ask mode, "Claude would like to read…" with
//     Allow / Don't share.
//  3. Result: the report, and the plan as a checklist (moves Kosmos can't
//     do are shown with the reason, unticked). Apply returns the kept moves.
class OrganizerDialog : public QDialog
{
    Q_OBJECT

public:
    OrganizerDialog(const QString &folder, QWidget *parent = nullptr);

    QList<Organizer::Move> keptMoves() const;

signals:
    void openPreferences();

private:
    void showStart();
    void showResult(const QString &report, const QList<Organizer::Move> &moves);
    void updateApply();

    Claude::Client *m_client;
    Organizer *m_organizer;
    QList<Organizer::Move> m_moves;

    QStackedWidget *m_pages = nullptr;
    // Start
    QLineEdit *m_wishes = nullptr;
    QComboBox *m_mode = nullptr;
    QLabel *m_estimate = nullptr;
    QPushButton *m_start = nullptr;
    // Working
    QLabel *m_progress = nullptr;
    QWidget *m_askPanel = nullptr;
    QLabel *m_askText = nullptr;
    // Result
    QTextBrowser *m_report = nullptr;
    QTreeWidget *m_plan = nullptr;
    QLabel *m_cost = nullptr;
    QPushButton *m_apply = nullptr;
};

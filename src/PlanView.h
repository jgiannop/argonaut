#pragma once

#include "Extensions.h"

#include <QWidget>

class QLabel;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;
class RichView;

// Changes an extension proposes for a folder (Extensions::Plan), in the
// preview pane: what it found (its report), then every move with a tick,
// then Dismiss and Apply.
//
// Everything on it is Argonaut's: the extension sent data, and can't tick,
// untick or apply. A move Argonaut won't do is shown with the reason and
// can't be ticked. Apply only says which moves are kept (applyRequested):
// the window checks them again and does them through the file engine.
class PlanView : public QWidget
{
    Q_OBJECT

public:
    explicit PlanView(QWidget *parent = nullptr);

    void setPlan(const Extensions::Plan &plan);
    void clear();
    const Extensions::Plan &plan() const { return m_plan; }
    bool isEmpty() const { return m_plan.id.isEmpty(); }

    QList<Extensions::PlanMove> keptMoves() const; // the ticked ones
    int moveCount() const;                         // rows
    // For tests: a row's text and state, and ticking one as a click does.
    QString moveText(int row) const;
    bool isMoveTicked(int row) const;
    bool canMoveBeTicked(int row) const;
    void setMoveTicked(int row, bool on);
    QPushButton *applyButton() const { return m_apply; }
    QPushButton *dismissButton() const { return m_dismiss; }

signals:
    void applyRequested();
    void dismissRequested();
    void itemActivated(const QString &path); // a move was clicked: its file, to be shown in the folder

private:
    void updateCount();

    Extensions::Plan m_plan;
    RichView *m_report;
    QTreeWidget *m_moves;
    QLabel *m_count;
    QPushButton *m_dismiss;
    QPushButton *m_apply;
};

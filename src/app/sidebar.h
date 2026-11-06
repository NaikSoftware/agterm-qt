#pragma once

#include <QSet>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QTreeWidget>

class Store;
struct Settings;
class QPainter;

// A launcher's icon as a template tinted `color`: the terminal glyph, or the agent's logo.
QIcon launcherIcon(const QString &launcherId, const QColor &color);
// agterm's title-bar and footer glyphs: sidebar, split, splitH, filepanel, clock, grid, gridFilled
QIcon chromeIcon(const QString &name, const QColor &color);
// a status's glyph colour and shape (circle square triangle diamond capsule star) from Settings > Agent Status
QColor statusColor(const Settings &settings, const QString &status);
QString statusShape(const Settings &settings, const QString &status);
void drawStatusGlyph(QPainter *p, const QPointF &center, qreal radius, const QString &shape);
// a tint name as the macOS system colour of that name, light or dark by the current palette; invalid for ""
QColor tintColor(const QString &name);

namespace SidebarRole {
enum { Id = Qt::UserRole, Kind, Status, Blink, Agent, Flagged, Expanded, Unseen, Busy, Companion, HasCompanions, FoldedAttention, TintColor, TintOn, Focused, WorkspaceLabel };
enum { WorkspaceRow = 1, SessionRow };
}

class SidebarDelegate : public QStyledItemDelegate
{
public:
    SidebarDelegate(class Sidebar *sidebar);
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    void updateEditorGeometry(QWidget *editor, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    void setEditorData(QWidget *editor, const QModelIndex &index) const override;

private:
    Sidebar *m_sidebar;
};

// The workspace -> session tree, drawn like agterm's sidebar.
class Sidebar : public QTreeWidget
{
    Q_OBJECT
public:
    explicit Sidebar(Store &store, QWidget *parent = nullptr);
    void rebuild();
    void setFlaggedView(bool on);
    bool flaggedView() const;
    bool flatList() const; // the flagged view as one list (flat or plain layout)
    QStringList selectedSessionIds() const;
    bool editing() const { return state() == EditingState; }
    QString lastLauncher() const;

    // geometry shared by painting and hit-testing
    static QRect addButtonRect(const QRect &row);
    static QRect repeatButtonRect(const QRect &row);
    static QRect closeButtonRect(const QRect &row); // a finished session's, on hover
    QModelIndex hovered() const { return m_hovered; }
    bool closable(const QModelIndex &session) const;
    int blinkAlpha() const { return m_blinkAlpha; }
    const Settings &settings() const;

Q_SIGNALS:
    void launcherMenuRequested(const QString &workspaceId, const QPoint &globalPos);
    void repeatRequested(const QString &workspaceId);
    void foldersDropped(const QString &workspaceId, const QStringList &paths);
    void renameRequested(const QString &kind, const QString &id, const QString &name);
    void editFlaggedRequested(); // the empty flagged view's link
    void closeRequested(const QString &sessionId);

protected:
    void drawRow(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    void drawBranches(QPainter *, const QRect &, const QModelIndex &) const override { }
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    QItemSelectionModel::SelectionFlags selectionCommand(const QModelIndex &index, const QEvent *event) const override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void startDrag(Qt::DropActions supportedActions) override;
    void paintEvent(QPaintEvent *event) override;

private:
    struct Drop {
        QString workspaceId;
        int index = -1; // -1 = end
        int workspaceIndex = -1;
        QRect line;
    };
    bool isCloseButton(QTreeWidgetItem *item, const QPoint &pos) const;
    Drop dropTarget(const QPoint &pos, bool workspaceDrag) const;
    QStringList structure() const;
    QStringList m_structure; // what the items were built from; an equal one is updated in place
    QRect m_dropLine;
    Store &m_store;
    bool m_rebuilding = false;
    QModelIndex m_hovered;
    QTimer m_blinkTimer;
    int m_blinkAlpha = 255;
    int m_blinkPhase = 0;
};

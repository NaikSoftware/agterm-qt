#pragma once

#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

#include <optional>

// A sidebar row colour: one of Store::tintColors shown on the icon, the name and/or as a background wash; empty color = none.
struct Tint {
    QString color;
    QStringList on; // icon, name, background in that order; never empty while tinted
    bool operator==(const Tint &) const = default;
};

struct Session {
    QString id;
    QString name; // pinned by a rename; empty = derived from the cwd
    QString cwd;
    QString status = QStringLiteral("idle"); // idle | active | completed | blocked; never persisted
    bool blink = false;
    bool autoReset = false; // a completed status clears once the session is viewed
    bool flagged = false;
    QString title;   // last title a program running in the session set (or its command's name), spinner glyphs stripped
    QString agent;   // launcher id of the coding agent in the foreground, "" for none; not persisted
    bool busy = false; // a program other than the shell holds the main pane's foreground; not persisted
    QString command; // run instead of a shell at creation, not persisted
    QString splitCwd, splitCommand; // the same for the split pane's first spawn, not persisted
    // one session, up to two terminals: the split pane is a second shell sharing the sidebar row
    bool hasSplit = false;
    bool splitVisible = false;
    QString splitAxis = QStringLiteral("vertical"); // vertical = side by side, horizontal = top and bottom
    double splitRatio = 0.5;
    double fontSize = 0, splitFontSize = 0; // points each pane was zoomed to; 0 = the profile's
    int unseen = 0;              // notifications raised while the session was not in front of you
    QString parentId;            // a companion is pinned to its parent, listed right after it
    Tint tint;
    QString context; // a short line of what the session is about, set from outside (session.context)
    // re-run restore: argv captured at quit (persisted), its one-shot replay this launch (not persisted), and a pinned
    // command that beats both (persisted; "" = pinned to nothing)
    QStringList foreground, splitForeground, replay, splitReplay;
    std::optional<QString> restorePin, splitRestorePin;

    QString displayName() const;
};

struct Workspace {
    QString id;
    QString name;
    QString folder; // last directory one of its sessions reported; where new sessions open
    QList<Session> sessions;
    Tint tint; // colours the workspace row; its background wash also covers its open sessions' rows and terminals
};

struct Settings {
    QString newSessionDirectory = QStringLiteral("workspace"); // home | workspace | custom
    QString newSessionCustomDirectory;
    QString newSessionPlacement = QStringLiteral("end"); // end | afterCurrent
    QString statusReset = QStringLiteral("firstKey");    // firstKey | enter | never
    QString lastLauncher = QStringLiteral("terminal");
    bool confirmCloseSession = false;
    bool closeUndo = true; // GUI closes keep a short undo grace
    bool workspaceRowClickExpands = true;
    // Appearance; empty / 0 = the Konsole profile's own
    QString fontFamily, theme;
    double fontSize = 0;
    QString cursorStyle; // block | bar | underline
    QString cursorBlink; // blink | steady
    double sidebarFontSize = 0, interfaceFontSize = 0;
    // Interface: master's InterfaceElement raw names; workspaceName and customCommands are off until shown
    QStringList hiddenInterfaceElements, shownInterfaceElements;
    bool shows(const QString &element) const;
    // Notifications
    bool notificationsEnabled = true, notificationBadgeEnabled = true;
    QString notificationSoundName;               // a freedesktop sound-theme name; "" = none
    QString dockBounce = QStringLiteral("off");   // the taskbar entry asks for attention: off | once | untilFocused
    // Agent Status; "" = agterm's default colour / circle
    QString activeStatusColorHex, blockedStatusColorHex, completedStatusColorHex;
    QString activeStatusShape, blockedStatusShape, completedStatusShape; // circle square triangle diamond capsule star
    QString blockedStatusSoundName;
    bool attentionIncludesActive = false;
    QString autoFollowAttention = QStringLiteral("off"); // off s5 s10 s30 s60 m5: idle time before the window follows a blocked session
    bool autoFollowStayOnActive = false;                  // hold the selection while the current session is active
    QString flaggedViewLayout = QStringLiteral("flat");   // flat (session : workspace) | plain | tree
    // Key Mapping: the directory holding keymap.conf and hooks.conf; "" = the default
    QString configDirectory;
    QString restoreMode = QStringLiteral("none"); // none | rerun | live; takes effect at the next launch

    QJsonObject toJson() const;
    static Settings fromJson(const QJsonObject &o);
};

// The workspace -> session tree. Owns no terminals: the window reacts to its signals.
class Store : public QObject
{
    Q_OBJECT
public:
    enum class Key { Plain, Submit, Interrupt };
    static const QStringList statuses;
    static const QStringList tintColors; // red orange yellow green blue purple gray
    static QStringList tintParts(const QStringList &raw); // ordered icon,name,background; empty when empty or unknown

    QList<Workspace> workspaces;
    QString selected;
    QStringList recent; // session ids, most recently selected first
    struct Closed {
        QString workspaceId, name, cwd;
        int index;
        QString id;          // the closed session's id, which recent.open accepts
        qint64 closedAt = 0; // ms since the epoch
    };
    QList<Closed> closed; // most recent first; Reopen Closed Item brings the shell back where it was
    // a close inside its undo grace: the sessions are out of the tree, their terminals still running
    struct Parked {
        QString workspaceId;
        int index;
        Session session;
    };
    struct PendingClose {
        int serial = 0;
        QList<Parked> sessions;
        Workspace workspace; // a whole workspace close; id empty otherwise
        int workspaceIndex = -1;
        bool focusMember = false;
        QString select;
    };
    QList<PendingClose> pending; // newest last
    int graceMs = 3000;
    Settings settings;
    QString restoreModeAtLaunch = QStringLiteral("none"); // frozen by the first fromJson
    // the line to type into a pane's new shell: the pin, else the capture if restorable; consumed once
    QString takeReplay(const QString &id, bool split, const QStringList &denylist);
    bool flaggedView = false; // the sidebar shows flagged sessions only; navigation follows it
    QSet<QString> collapsed;  // folded workspace rows and companion parents, shared by the tree and the flagged tree
    // the workspace filter: a marked set, applied or suspended; never applied while empty
    QStringList focused;
    bool focusEnabled = false;

    Session *session(const QString &id);
    Workspace *workspace(const QString &id);
    Workspace *workspaceOf(const QString &sessionId);
    QList<Session *> allSessions();
    QList<Workspace *> visibleWorkspaces();  // the marked set while the filter applies, else all
    QList<Session *> navigableSessions();    // flagged ones in the flagged view, else those of visible workspaces
    QStringList recentOthers(int limit = 10); // the MRU sessions on screen, the current one excluded
    bool isSoleFocus(const QString &workspaceId) const { return focusEnabled && focused == QStringList{workspaceId}; }

    // target: "" or "active" -> current; else exact id, unique id prefix, or unique exact name.
    QString resolveSession(const QString &target, QString *error);
    QString resolveWorkspace(const QString &target, QString *error);
    QString workspaceRemembering(const QString &folder) const;
    QString newSessionCwd(const QString &workspaceId, const QString &currentCwd) const;

    QString addWorkspace(const QString &name);
    // index -1 follows settings.newSessionPlacement
    QString addSession(const QString &workspaceId, const QString &name, const QString &cwd, const QString &command = {}, int index = -1);
    QString duplicateSession(const QString &id, const QString &cwd);
    // a companion of `parentId` (a companion target means its parent), after the parent's last companion
    QString addCompanion(const QString &parentId, const QString &name, const QString &cwd, const QString &command = {});
    QStringList companionsOf(const QString &parentId) const;
    void removeSession(const QString &id);
    QString reopenClosed(const QString &closedId = {}); // "" when nothing (or no such item) was closed
    bool softClose(const QStringList &ids); // closes with the undo grace; finalized by a timer
    bool softRemoveWorkspace(const QString &id);
    bool undoClose();                       // the newest pending close back in place, shells intact
    void finalizePending(int serial = -1);  // -1: every pending close, as at quit
    void removeWorkspace(const QString &id);
    void renameSession(const QString &id, const QString &name); // "" reverts to the cwd-derived name
    void renameWorkspace(const QString &id, const QString &name);
    bool moveSession(const QString &id, const QString &workspaceId, int index); // index -1 = end
    bool moveWorkspace(const QString &id, int index);
    void select(const QString &id);
    bool setStatus(const QString &id, const QString &status, bool autoReset = false, bool blink = false);
    void setFlag(const QString &id, bool flagged);
    bool focusWorkspace(const QString &id, const QString &mode); // on | off | toggle | add; false on another mode
    void setFilter(bool on);
    // id names a session or a workspace. color "" clears; parts empty keep the row's placement; an untinted row gets icon,name,background
    void setTint(const QString &id, const QString &color, const QStringList &parts = {});
    void placeTint(const QString &id, const QStringList &parts); // an untinted row stays untinted
    void setCwd(const QString &id, const QString &cwd);
    void setTitle(const QString &id, const QString &title);
    void setAgent(const QString &id, const QString &agent);
    void setBusy(const QString &id, bool busy);
    void setSettings(const Settings &s);
    void keyPressed(const QString &id, Key key);
    // ⌘D / ⌘⇧D: create, transpose (other axis), or hide (same axis)
    void toggleSplit(const QString &id, const QString &axis);
    void setSplit(const QString &id, bool hasSplit, bool visible, const QString &axis);
    void requestSwap(const QString &id) { Q_EMIT swapRequested(id); }
    void notify(const QString &id, const QString &title, const QString &body) { Q_EMIT notified(id, title, body); }
    void setUnseen(const QString &id, int count);
    QString go(const QString &direction, QString *error); // next | prev | first | last | next-attention | prev-attention (and previous…)

    QJsonObject toJson() const;
    void fromJson(const QJsonObject &o);

    static QString stripTitleSpinner(const QString &title);

Q_SIGNALS:
    void changed();
    void sessionAdded(const QString &id);
    void sessionRemoved(const QString &id);
    void selectionChanged(const QString &id);
    void statusChanged(const QString &id, const QString &status);
    void titleChanged(const QString &id);
    void paneLayoutChanged(const QString &id);
    void swapRequested(const QString &id);
    void notified(const QString &id, const QString &title, const QString &body);
    void settingsChanged();

private:
    static QString newId();
    QString goTo(const QString &direction, QString *error);
    Tint *tintOf(const QString &id);
    int indexIn(const Workspace &w, const QString &sessionId) const;
    void commitFocus(QStringList ids, bool enabled);
    void reselect(const Workspace *w, int i); // after the selected session left w at row i; w null when w went too
    bool park(const PendingClose &close);
    int m_pendingSerial = 0;
    bool m_loaded = false;
};

#include "store.h"

#include <QDateTime>
#include "restore.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QSet>
#include <QTimer>
#include <QUuid>

const QStringList Store::statuses = {QStringLiteral("idle"), QStringLiteral("active"), QStringLiteral("completed"),
                                     QStringLiteral("blocked")};
const QStringList Store::tintColors = {QStringLiteral("red"),  QStringLiteral("orange"), QStringLiteral("yellow"), QStringLiteral("green"),
                                       QStringLiteral("blue"), QStringLiteral("purple"), QStringLiteral("gray")};

QStringList Store::tintParts(const QStringList &raw)
{
    QStringList parts;
    for (const QString &p : raw) {
        const QString part = p.trimmed().toLower();
        if (part != QLatin1String("icon") && part != QLatin1String("name") && part != QLatin1String("background"))
            return {};
        parts << part;
    }
    QStringList ordered;
    for (const char *p : {"icon", "name", "background"})
        if (parts.contains(QLatin1String(p)))
            ordered << QLatin1String(p);
    return ordered;
}

static QJsonValue tintJson(const Tint &t)
{
    return t.color.isEmpty() ? QJsonValue() : QJsonObject{{"color", t.color}, {"on", QJsonArray::fromStringList(t.on)}};
}

static Tint tintFromJson(const QJsonValue &v)
{
    // an unknown colour drops the tint; unknown parts are skipped and none left reads as the icon
    const QString color = v.toObject().value("color").toString();
    if (!Store::tintColors.contains(color))
        return {};
    QStringList on;
    for (const auto &p : v.toObject().value("on").toArray())
        on += Store::tintParts({p.toString()});
    on = Store::tintParts(on);
    return {color, on.isEmpty() ? QStringList{QStringLiteral("icon")} : on};
}

QString Session::displayName() const
{
    // a pinned name, else the title its program set (or the command it runs), else the folder it is in
    if (!name.isEmpty())
        return name;
    if (!title.trimmed().isEmpty())
        return title.trimmed();
    if (QDir::cleanPath(cwd) == QDir::homePath())
        return QStringLiteral("~");
    const QString folder = QFileInfo(cwd).fileName();
    return folder.isEmpty() ? QStringLiteral("New session") : folder;
}

QString Store::newId()
{
    return QUuid::createUuid().toString(QUuid::Id128).left(12);
}

QString Store::stripTitleSpinner(const QString &title)
{
    // agents animate a status glyph (✳, ◑, braille frames) at the start of the title
    int i = 0;
    for (; i < title.size(); ++i) {
        const char16_t c = title[i].unicode();
        const bool spinner = (c >= 0x2800 && c <= 0x28FF) || (c >= 0x25CB && c <= 0x25D3) || (c >= 0x2722 && c <= 0x2725)
                             || (c >= 0x2731 && c <= 0x273F) || c == 0x00B7;
        if (!spinner && !title[i].isSpace())
            break;
    }
    return i == title.size() ? title : title.mid(i);
}

Session *Store::session(const QString &id)
{
    for (auto &w : workspaces)
        for (auto &s : w.sessions)
            if (s.id == id)
                return &s;
    return nullptr;
}

Workspace *Store::workspace(const QString &id)
{
    for (auto &w : workspaces)
        if (w.id == id)
            return &w;
    return nullptr;
}

Workspace *Store::workspaceOf(const QString &sessionId)
{
    for (auto &w : workspaces)
        for (auto &s : w.sessions)
            if (s.id == sessionId)
                return &w;
    return nullptr;
}

int Store::indexIn(const Workspace &w, const QString &sessionId) const
{
    for (int i = 0; i < w.sessions.size(); ++i)
        if (w.sessions[i].id == sessionId)
            return i;
    return -1;
}

QList<Session *> Store::allSessions()
{
    QList<Session *> out;
    for (auto &w : workspaces)
        for (auto &s : w.sessions)
            out << &s;
    return out;
}

QList<Workspace *> Store::visibleWorkspaces()
{
    QList<Workspace *> out;
    for (auto &w : workspaces)
        if (!focusEnabled || focused.contains(w.id))
            out << &w;
    return out;
}

QList<Session *> Store::navigableSessions()
{
    QList<Session *> out;
    if (flaggedView) { // the flagged view is cross-workspace and ignores the filter
        for (auto *s : allSessions())
            if (s->flagged)
                out << s;
        return out;
    }
    for (auto *w : visibleWorkspaces())
        for (auto &s : w->sessions)
            out << &s;
    return out;
}

QStringList Store::recentOthers(int limit)
{
    QSet<QString> shown;
    for (auto *s : navigableSessions())
        shown << s->id;
    QStringList out;
    for (const QString &id : std::as_const(recent))
        if (id != selected && shown.contains(id) && out.size() < limit)
            out << id;
    return out;
}

template<typename T, typename Name>
static QString resolve(const QList<T *> &items, const QString &target, const char *kind, Name name, QString *error)
{
    QList<T *> prefix, named;
    for (T *i : items) {
        if (i->id == target)
            return i->id;
        if (target.size() >= 3 && i->id.startsWith(target))
            prefix << i;
        if (name(i) == target)
            named << i;
    }
    const auto &hits = prefix.isEmpty() ? named : prefix;
    if (hits.size() == 1)
        return hits.first()->id;
    *error = hits.isEmpty() ? QStringLiteral("no %1 matches '%2'").arg(QLatin1String(kind), target)
                            : QStringLiteral("'%1' matches %2 %3s").arg(target).arg(hits.size()).arg(QLatin1String(kind));
    return {};
}

QString Store::resolveSession(const QString &target, QString *error)
{
    if (target.isEmpty() || target == QLatin1String("active")) {
        if (selected.isEmpty())
            *error = QStringLiteral("no active session");
        return selected;
    }
    return resolve(allSessions(), target, "session", [](Session *s) { return s->displayName(); }, error);
}

QString Store::resolveWorkspace(const QString &target, QString *error)
{
    if (target.isEmpty() || target == QLatin1String("active")) {
        if (auto *w = workspaceOf(selected))
            return w->id;
        if (!workspaces.isEmpty())
            return workspaces.first().id;
        *error = QStringLiteral("no workspaces");
        return {};
    }
    QList<Workspace *> ws;
    for (auto &w : workspaces)
        ws << &w;
    return resolve(ws, target, "workspace", [](Workspace *w) { return w->name; }, error);
}

QString Store::workspaceRemembering(const QString &folder) const
{
    const QString f = QDir::cleanPath(folder);
    for (const auto &w : workspaces)
        if (!w.folder.isEmpty() && w.folder == f)
            return w.id;
    return {};
}

QString Store::newSessionCwd(const QString &workspaceId, const QString &currentCwd) const
{
    const QString home = QDir::homePath();
    if (settings.newSessionDirectory == QLatin1String("custom"))
        return QFileInfo(settings.newSessionCustomDirectory).isDir() ? settings.newSessionCustomDirectory : home;
    if (settings.newSessionDirectory == QLatin1String("workspace")) {
        for (const auto &w : workspaces)
            if (w.id == workspaceId && QFileInfo(w.folder).isDir())
                return w.folder; // a vanished folder is skipped, not cleared
        return currentCwd.isEmpty() ? home : currentCwd;
    }
    return home;
}

QString Store::addWorkspace(const QString &name)
{
    Workspace w{newId(), name.isEmpty() ? QStringLiteral("workspace %1").arg(workspaces.size() + 1) : name, {}, {}};
    workspaces << w;
    if (focusEnabled)
        focused << w.id; // a workspace created under the filter joins it rather than vanishing
    Q_EMIT changed();
    return w.id;
}

QString Store::addSession(const QString &workspaceId, const QString &name, const QString &cwd, const QString &command, int index)
{
    auto *w = workspace(workspaceId);
    if (!w)
        return {};
    Session s;
    s.id = newId();
    s.cwd = cwd.isEmpty() ? QDir::homePath() : cwd;
    s.name = name;
    s.command = command;
    if (index < 0 && settings.newSessionPlacement == QLatin1String("afterCurrent"))
        if (const int at = indexIn(*w, selected); at >= 0)
            index = at + 1;
    w->sessions.insert(index < 0 || index > w->sessions.size() ? w->sessions.size() : index, s);
    Q_EMIT sessionAdded(s.id);
    Q_EMIT changed();
    return s.id;
}

QString Store::duplicateSession(const QString &id, const QString &cwd)
{
    auto *w = workspaceOf(id);
    if (!w)
        return {};
    const Session *s = session(id);
    return addSession(w->id, {}, cwd.isEmpty() ? s->cwd : cwd, {}, indexIn(*w, id) + 1);
}

QStringList Store::companionsOf(const QString &parentId) const
{
    QStringList ids;
    for (const auto &w : workspaces)
        for (const auto &s : w.sessions)
            if (s.parentId == parentId && !parentId.isEmpty())
                ids << s.id;
    return ids;
}

QString Store::addCompanion(const QString &parentId, const QString &name, const QString &cwd, const QString &command)
{
    const Session *p = session(parentId);
    if (!p)
        return {};
    const QString parent = p->parentId.isEmpty() ? parentId : p->parentId;
    auto *w = workspaceOf(parent);
    int at = indexIn(*w, parent) + 1;
    while (at < w->sessions.size() && w->sessions[at].parentId == parent)
        ++at;
    Session s;
    s.id = newId();
    s.cwd = cwd.isEmpty() ? session(parent)->cwd : cwd;
    s.name = name;
    s.command = command;
    s.parentId = parent;
    w->sessions.insert(at, s);
    Q_EMIT sessionAdded(s.id);
    Q_EMIT changed();
    return s.id;
}

void Store::removeSession(const QString &id)
{
    auto *w = workspaceOf(id);
    if (!w)
        return;
    for (const QString &c : companionsOf(id))
        removeSession(c); // closing the parent closes the group
    const int i = indexIn(*w, id);
    closed.prepend({w->id, w->sessions[i].name, w->sessions[i].cwd, i, w->sessions[i].id, QDateTime::currentMSecsSinceEpoch()});
    if (closed.size() > 20)
        closed.removeLast();
    w->sessions.removeAt(i);
    recent.removeAll(id);
    if (selected == id)
        reselect(w, i);
    Q_EMIT sessionRemoved(id);
    Q_EMIT changed();
}

void Store::reselect(const Workspace *w, int i)
{
    // most recent in the same workspace, the adjacent row, then most recent anywhere: each kept on screen first
    QSet<QString> shown;
    for (auto *s : navigableSessions())
        shown << s->id;
    QString next;
    for (const auto &r : std::as_const(recent))
        if (w && indexIn(*w, r) >= 0 && shown.contains(r)) {
            next = r;
            break;
        }
    for (int k = i; w && next.isEmpty() && k < w->sessions.size(); ++k)
        if (shown.contains(w->sessions[k].id))
            next = w->sessions[k].id;
    for (int k = w ? qMin(i, int(w->sessions.size())) - 1 : -1; next.isEmpty() && k >= 0; --k)
        if (shown.contains(w->sessions[k].id))
            next = w->sessions[k].id;
    for (const auto &r : std::as_const(recent))
        if (next.isEmpty() && shown.contains(r))
            next = r;
    if (next.isEmpty() && w && !w->sessions.isEmpty())
        next = w->sessions[qMin(i, int(w->sessions.size()) - 1)].id;
    if (next.isEmpty() && !recent.isEmpty())
        next = recent.first();
    if (next.isEmpty())
        if (auto all = allSessions(); !all.isEmpty())
            next = all.first()->id;
    selected.clear();
    if (!next.isEmpty())
        select(next);
    else
        Q_EMIT selectionChanged({});
}

bool Store::softClose(const QStringList &ids)
{
    // a parent takes its companions; the panes stay alive, hidden, until the grace expires
    QStringList group;
    for (const QString &id : ids)
        if (session(id))
            group << id << companionsOf(id);
    PendingClose close{++m_pendingSerial};
    for (auto *s : allSessions())
        if (group.contains(s->id)) {
            auto *w = workspaceOf(s->id);
            close.sessions << Parked{w->id, indexIn(*w, s->id), *s};
        }
    if (close.sessions.isEmpty())
        return false;
    const Parked first = close.sessions.first();
    close.select = group.contains(selected) ? selected : first.session.id;
    const bool wasSelected = group.contains(selected);
    for (auto it = close.sessions.crbegin(); it != close.sessions.crend(); ++it) {
        workspace(it->workspaceId)->sessions.removeAt(it->index);
        recent.removeAll(it->session.id);
    }
    if (wasSelected) {
        const Parked &at = *std::find_if(close.sessions.cbegin(), close.sessions.cend(), [this](const Parked &p) { return p.session.id == selected; });
        reselect(workspace(at.workspaceId), at.index);
    }
    return park(close);
}

bool Store::softRemoveWorkspace(const QString &id)
{
    const auto at = std::find_if(workspaces.cbegin(), workspaces.cend(), [&id](const Workspace &w) { return w.id == id; });
    if (at == workspaces.cend())
        return false;
    PendingClose close{++m_pendingSerial};
    close.workspace = *at;
    close.workspaceIndex = int(at - workspaces.cbegin());
    close.focusMember = focused.contains(id);
    focused.removeAll(id);
    focusEnabled = focusEnabled && !focused.isEmpty();
    const bool wasSelected = workspaceOf(selected) && workspaceOf(selected)->id == id;
    workspaces.removeAt(close.workspaceIndex);
    for (const auto &s : close.workspace.sessions)
        recent.removeAll(s.id);
    close.select = wasSelected ? selected : QString();
    if (wasSelected)
        reselect(nullptr, 0);
    return park(close);
}

bool Store::park(const PendingClose &close)
{
    pending << close;
    QTimer::singleShot(graceMs, this, [this, serial = close.serial] { finalizePending(serial); });
    Q_EMIT changed();
    return true;
}

void Store::finalizePending(int serial)
{
    for (int i = 0; i < pending.size(); ++i) {
        if (serial >= 0 && pending[i].serial != serial)
            continue;
        const PendingClose c = pending.takeAt(i--);
        QList<Parked> gone = c.sessions;
        for (int k = 0; k < c.workspace.sessions.size(); ++k)
            gone << Parked{c.workspace.id, k, c.workspace.sessions[k]};
        for (const Parked &p : std::as_const(gone)) {
            closed.prepend({p.workspaceId, p.session.name, p.session.cwd, p.index, p.session.id, QDateTime::currentMSecsSinceEpoch()});
            Q_EMIT sessionRemoved(p.session.id);
        }
        while (closed.size() > 20)
            closed.removeLast();
        Q_EMIT changed();
    }
}

bool Store::undoClose()
{
    if (pending.isEmpty())
        return false;
    const PendingClose c = pending.takeLast();
    if (!c.workspace.id.isEmpty()) {
        workspaces.insert(qMin(c.workspaceIndex, int(workspaces.size())), c.workspace);
        if (c.focusMember)
            focused << c.workspace.id; // membership only: the filter flag is current state
    }
    for (const Parked &p : c.sessions) {
        Workspace *w = workspace(p.workspaceId);
        if (!w) // its workspace went meanwhile
            w = workspace(workspaces.isEmpty() ? addWorkspace(QStringLiteral("main")) : workspaces.first().id);
        w->sessions.insert(qMin(p.index, int(w->sessions.size())), p.session);
    }
    if (!c.select.isEmpty())
        select(c.select);
    Q_EMIT changed();
    return true;
}

QString Store::reopenClosed(const QString &closedId)
{
    qsizetype at = 0;
    while (!closedId.isEmpty() && at < closed.size() && closed[at].id != closedId)
        ++at;
    if (at >= closed.size())
        return {};
    const Closed c = closed.takeAt(at);
    QString ws = c.workspaceId;
    if (!workspace(ws))
        ws = workspaces.isEmpty() ? addWorkspace(QStringLiteral("main")) : workspaces.first().id;
    const QString id = addSession(ws, c.name, c.cwd, {}, c.index);
    select(id);
    return id;
}

void Store::removeWorkspace(const QString &id)
{
    auto *w = workspace(id);
    if (!w)
        return;
    focused.removeAll(id);
    focusEnabled = focusEnabled && !focused.isEmpty();
    QStringList ids;
    for (const auto &s : w->sessions)
        ids << s.id;
    for (const auto &sid : ids)
        removeSession(sid);
    for (int i = 0; i < workspaces.size(); ++i)
        if (workspaces[i].id == id)
            workspaces.removeAt(i--);
    Q_EMIT changed();
}

void Store::renameSession(const QString &id, const QString &name)
{
    if (auto *s = session(id)) {
        s->name = name.trimmed();
        Q_EMIT changed();
    }
}

void Store::renameWorkspace(const QString &id, const QString &name)
{
    if (auto *w = workspace(id); w && !name.trimmed().isEmpty()) {
        w->name = name.trimmed();
        Q_EMIT changed();
    }
}

bool Store::moveSession(const QString &id, const QString &workspaceId, int index)
{
    auto *from = workspaceOf(id);
    if (!from || !workspace(workspaceId))
        return false;
    const int at = indexIn(*from, id);
    const QString parent = from->sessions[at].parentId;
    if (!parent.isEmpty()) {
        // a companion reorders only among its parent's companions
        if (from->id != workspaceId)
            return false;
        const int first = indexIn(*from, parent) + 1;
        const int end = first + int(companionsOf(parent).size());
        index = qBound(first, index < 0 ? end : index, end);
    } else if (index >= 0) {
        auto *to = workspace(workspaceId);
        while (index < to->sessions.size() && index > 0 && !to->sessions[index].parentId.isEmpty())
            ++index; // never land inside another session's companion group
    }
    // a parent carries its companions
    int count = 1;
    while (at + count < from->sessions.size() && from->sessions[at + count].parentId == id)
        ++count;
    QList<Session> block = from->sessions.mid(at, count);
    from->sessions.remove(at, count);
    auto *to = workspace(workspaceId);
    if (to == from && index > at)
        index -= qMin(count, index - at); // the removal shifted the target
    int dest = index < 0 || index > to->sessions.size() ? int(to->sessions.size()) : index;
    for (const Session &s : block)
        to->sessions.insert(dest++, s);
    Q_EMIT changed();
    return true;
}

bool Store::moveWorkspace(const QString &id, int index)
{
    for (int i = 0; i < workspaces.size(); ++i)
        if (workspaces[i].id == id) {
            Workspace w = workspaces.takeAt(i);
            if (index > i)
                --index;
            workspaces.insert(index < 0 || index > workspaces.size() ? workspaces.size() : index, w);
            Q_EMIT changed();
            return true;
        }
    return false;
}

void Store::select(const QString &id)
{
    auto *s = session(id);
    if (!s || selected == id)
        return;
    if (focusEnabled && !flaggedView && !focused.contains(workspaceOf(id)->id))
        focusEnabled = false; // selecting outside the set suspends the filter, keeping the set
    selected = id;
    recent.removeAll(id);
    recent.prepend(id);
    s->unseen = 0; // focusing a session acknowledges its notifications
    if (s->status == QLatin1String("completed") && s->autoReset)
        setStatus(id, QStringLiteral("idle"));
    Q_EMIT selectionChanged(id);
    Q_EMIT changed();
}

bool Store::setStatus(const QString &id, const QString &status, bool autoReset, bool blink)
{
    auto *s = session(id);
    if (!s || !statuses.contains(status))
        return false;
    const bool same = s->status == status && s->blink == blink;
    s->autoReset = autoReset;
    s->blink = blink && status != QLatin1String("idle");
    if (same)
        return true;
    s->status = status;
    Q_EMIT statusChanged(id, status);
    Q_EMIT changed();
    return true;
}

void Store::setUnseen(const QString &id, int count)
{
    if (auto *s = session(id); s && s->unseen != count) {
        s->unseen = count;
        Q_EMIT titleChanged(id); // a repaint, not a save: unseen counts are not persisted
    }
}

void Store::setFlag(const QString &id, bool flagged)
{
    if (auto *s = session(id); s && s->flagged != flagged) {
        s->flagged = flagged;
        Q_EMIT changed();
    }
}

bool Store::focusWorkspace(const QString &id, const QString &mode)
{
    // marking (add) never applies the filter on its own, so a set is built with the whole tree on screen
    const bool exists = workspace(id);
    QStringList ids = focused;
    if (mode == QLatin1String("on") || (mode == QLatin1String("toggle") && !isSoleFocus(id))) {
        if (exists)
            commitFocus({id}, true);
    } else if (mode == QLatin1String("toggle")) {
        commitFocus({}, false);
    } else if (mode == QLatin1String("add")) {
        if (exists && !ids.contains(id))
            commitFocus(ids << id, focusEnabled);
    } else if (mode == QLatin1String("off")) {
        ids.removeAll(id);
        commitFocus(ids, focusEnabled);
    } else {
        return false;
    }
    return true;
}

void Store::setFilter(bool on)
{
    commitFocus(focused, on);
}

void Store::commitFocus(QStringList ids, bool enabled)
{
    enabled = enabled && !ids.isEmpty();
    if (ids == focused && enabled == focusEnabled)
        return;
    focused = ids;
    focusEnabled = enabled;
    // a narrowing that hid the active session moves to the most recent one still on screen
    const auto nav = navigableSessions();
    const bool hidden = !selected.isEmpty() && std::none_of(nav.begin(), nav.end(), [this](Session *s) { return s->id == selected; });
    if (hidden && !nav.isEmpty()) {
        QString next = nav.first()->id;
        for (const auto &r : std::as_const(recent))
            if (std::any_of(nav.begin(), nav.end(), [&r](Session *s) { return s->id == r; })) {
                next = r;
                break;
            }
        select(next);
    }
    Q_EMIT changed();
}

Tint *Store::tintOf(const QString &id)
{
    if (auto *s = session(id))
        return &s->tint;
    auto *w = workspace(id);
    return w ? &w->tint : nullptr;
}

void Store::setTint(const QString &id, const QString &color, const QStringList &parts)
{
    Tint *t = tintOf(id);
    if (!t || (!color.isEmpty() && !tintColors.contains(color)))
        return;
    // an untinted row starts as a background wash: its row, a workspace's open sessions and the terminal
    const QStringList fresh{QStringLiteral("background")};
    Tint next;
    if (!color.isEmpty())
        next = {color, !tintParts(parts).isEmpty() ? tintParts(parts) : t->on.isEmpty() ? fresh : t->on};
    if (next == *t)
        return;
    *t = next;
    Q_EMIT changed();
}

void Store::placeTint(const QString &id, const QStringList &parts)
{
    if (Tint *t = tintOf(id); t && !t->color.isEmpty() && !tintParts(parts).isEmpty())
        setTint(id, t->color, parts);
}

void Store::setCwd(const QString &id, const QString &cwd)
{
    auto *s = session(id);
    if (!s || cwd.isEmpty())
        return;
    const QString c = QDir::cleanPath(cwd);
    auto *w = workspaceOf(id);
    if (s->cwd == c && w->folder == c)
        return;
    s->cwd = c;
    w->folder = c; // the workspace remembers where its sessions last were
    Q_EMIT changed();
}

void Store::setTitle(const QString &id, const QString &title)
{
    const QString t = stripTitleSpinner(title);
    if (auto *s = session(id); s && s->title != t) {
        s->title = t; // a program's title or a command's name: once per command at most, so saving is cheap
        Q_EMIT changed();
    }
}

void Store::setAgent(const QString &id, const QString &agent)
{
    if (auto *s = session(id); s && s->agent != agent) {
        s->agent = agent;
        Q_EMIT titleChanged(id); // a repaint, not a save
    }
}

void Store::setBusy(const QString &id, bool busy)
{
    if (auto *s = session(id); s && s->busy != busy) {
        s->busy = busy;
        Q_EMIT titleChanged(id); // a repaint, not a save
    }
}

void Store::setSettings(const Settings &s)
{
    settings = s;
    Q_EMIT settingsChanged();
    Q_EMIT changed();
}

// every field by its master name; empty strings and defaults stay out of the file
#define AGTERM_SETTINGS_STRINGS(X)                                                                                                  \
    X(newSessionDirectory) X(newSessionCustomDirectory) X(newSessionPlacement) X(statusReset) X(lastLauncher) X(fontFamily) X(theme) \
    X(cursorStyle) X(cursorBlink) X(notificationSoundName) X(dockBounce) X(activeStatusColorHex) X(blockedStatusColorHex)            \
    X(completedStatusColorHex) X(activeStatusShape) X(blockedStatusShape) X(completedStatusShape) X(blockedStatusSoundName)            \
    X(configDirectory) X(restoreMode) X(autoFollowAttention) X(flaggedViewLayout)
#define AGTERM_SETTINGS_BOOLS(X)                                                                                                  \
    X(confirmCloseSession) X(closeUndo) X(workspaceRowClickExpands) X(notificationsEnabled) X(notificationBadgeEnabled) X(attentionIncludesActive) X(autoFollowStayOnActive)
#define AGTERM_SETTINGS_DOUBLES(X) X(fontSize) X(sidebarFontSize) X(interfaceFontSize)
#define AGTERM_SETTINGS_LISTS(X) X(hiddenInterfaceElements) X(shownInterfaceElements)

QJsonObject Settings::toJson() const
{
    QJsonObject o;
#define X(f) o.insert(QStringLiteral(#f), f);
    AGTERM_SETTINGS_STRINGS(X) AGTERM_SETTINGS_BOOLS(X) AGTERM_SETTINGS_DOUBLES(X)
#undef X
#define X(f) o.insert(QStringLiteral(#f), QJsonArray::fromStringList(f));
    AGTERM_SETTINGS_LISTS(X)
#undef X
    return o;
}

Settings Settings::fromJson(const QJsonObject &o)
{
    Settings s;
#define X(f) s.f = o.value(QStringLiteral(#f)).toString(s.f);
    AGTERM_SETTINGS_STRINGS(X)
#undef X
#define X(f) s.f = o.value(QStringLiteral(#f)).toBool(s.f);
    AGTERM_SETTINGS_BOOLS(X)
#undef X
#define X(f) s.f = o.value(QStringLiteral(#f)).toDouble(s.f);
    AGTERM_SETTINGS_DOUBLES(X)
#undef X
#define X(f)                                                                                                                       \
    for (const auto &v : o.value(QStringLiteral(#f)).toArray())                                                                    \
        s.f << v.toString();
    AGTERM_SETTINGS_LISTS(X)
#undef X
    return s;
}

bool Settings::shows(const QString &element) const
{
    static const QStringList offByDefault{QStringLiteral("workspaceName"), QStringLiteral("customCommands")};
    return offByDefault.contains(element) ? shownInterfaceElements.contains(element) : !hiddenInterfaceElements.contains(element);
}

void Store::keyPressed(const QString &id, Key key)
{
    auto *s = session(id);
    if (!s)
        return;
    bool clear = false;
    if (s->status == QLatin1String("active"))
        clear = key == Key::Interrupt;
    else if (s->status == QLatin1String("blocked") || s->status == QLatin1String("completed")) {
        if (settings.statusReset == QLatin1String("firstKey"))
            clear = true;
        else if (settings.statusReset == QLatin1String("enter"))
            clear = key == Key::Submit || (s->status == QLatin1String("blocked") && key == Key::Interrupt);
    }
    if (clear)
        setStatus(id, QStringLiteral("idle"));
}

void Store::toggleSplit(const QString &id, const QString &axis)
{
    Session *s = session(id);
    if (!s)
        return;
    if (!s->hasSplit || !s->splitVisible)
        return setSplit(id, true, true, axis);
    setSplit(id, true, s->splitAxis != axis, axis);
}

void Store::setSplit(const QString &id, bool hasSplit, bool visible, const QString &axis)
{
    Session *s = session(id);
    if (!s || (axis != QLatin1String("vertical") && axis != QLatin1String("horizontal")))
        return;
    s->hasSplit = hasSplit;
    s->splitVisible = hasSplit && visible;
    s->splitAxis = axis;
    Q_EMIT paneLayoutChanged(id);
    Q_EMIT changed();
}

QString Store::go(const QString &direction, QString *error)
{
    static const QStringList known{"next", "prev", "first", "last", "next-attention", "prev-attention"};
    const QString to = direction == QLatin1String("previous") ? QStringLiteral("prev")
                       : direction == QLatin1String("previous-attention") ? QStringLiteral("prev-attention") : direction;
    if (!known.contains(to)) {
        *error = QStringLiteral("session.go requires --to next|prev|first|last|next-attention|prev-attention");
        return {};
    }
    return goTo(to, error);
}

QString Store::goTo(const QString &direction, QString *error)
{
    const auto all = navigableSessions();
    if (all.isEmpty()) {
        *error = QStringLiteral("no session to navigate");
        return {};
    }
    if (direction == QLatin1String("first") || direction == QLatin1String("last")) {
        const QString id = (direction == QLatin1String("first") ? all.first() : all.last())->id;
        select(id);
        return id;
    }
    const bool attention = direction.endsWith(QLatin1String("-attention"));
    const bool back = direction.startsWith(QLatin1String("prev"));
    int at = -1;
    for (int i = 0; i < all.size(); ++i)
        if (all[i]->id == selected)
            at = i;
    const int n = all.size();
    for (int step = 1; step <= n; ++step) {
        const int idx = at < 0 ? (back ? n - step : step - 1) : ((at + (back ? -step : step)) % n + n) % n;
        auto *s = all[idx];
        if (!attention || s->status == QLatin1String("blocked") || s->status == QLatin1String("completed")
            || (settings.attentionIncludesActive && s->status == QLatin1String("active"))) {
            select(s->id);
            return s->id;
        }
    }
    if (selected.isEmpty()) // nothing needs attention: the selection stays, as master
        *error = QStringLiteral("no session to navigate");
    return selected;
}

QString Store::takeReplay(const QString &id, bool split, const QStringList &denylist)
{
    Session *s = session(id);
    if (!s)
        return {};
    const QStringList argv = std::exchange(split ? s->splitReplay : s->replay, {});
    if (restoreModeAtLaunch != QLatin1String("rerun") && restoreModeAtLaunch != QLatin1String("live")) // live: for a daemon that is gone
        return {};
    if (const auto &pin = split ? s->splitRestorePin : s->restorePin)
        return *pin; // typed verbatim, past the denylist
    return Restore::shouldRestore(argv, denylist) ? Restore::shellQuoted(argv) : QString();
}

QJsonObject Store::toJson() const
{
    QJsonArray ws;
    for (const auto &w : workspaces) {
        QJsonArray ss;
        for (const auto &s : w.sessions) {
            QJsonObject so{{"id", s.id},           {"name", s.name},
                           {"cwd", s.cwd}, {"title", s.title}, {"flagged", s.flagged}, {"parentId", s.parentId},
                           {"hasSplit", s.hasSplit}, {"splitVisible", s.splitVisible},
                           {"splitAxis", s.splitAxis}, {"splitRatio", s.splitRatio},
                           {"fontSize", s.fontSize}, {"splitFontSize", s.splitFontSize}};
            if (!s.tint.color.isEmpty())
                so.insert("tint", tintJson(s.tint));
            if (!s.foreground.isEmpty())
                so.insert("foregroundCommand", QJsonArray::fromStringList(s.foreground));
            if (!s.splitForeground.isEmpty())
                so.insert("splitForegroundCommand", QJsonArray::fromStringList(s.splitForeground));
            if (s.restorePin)
                so.insert("restoreCommand", *s.restorePin);
            if (s.splitRestorePin)
                so.insert("splitRestoreCommand", *s.splitRestorePin);
            if (!s.context.isEmpty())
                so.insert("context", s.context);
            ss << so;
        }
        QJsonObject wo{{"id", w.id}, {"name", w.name}, {"folder", w.folder}, {"sessions", ss}};
        if (!w.tint.color.isEmpty())
            wo.insert("tint", tintJson(w.tint));
        ws << wo;
    }
    const QJsonObject st = settings.toJson();
    return {{"version", 2},      {"selected", selected}, {"recent", QJsonArray::fromStringList(recent)},
            {"settings", st},    {"workspaces", ws},     {"focusedWorkspaces", QJsonArray::fromStringList(focused)},
            {"focusEnabled", focusEnabled}};
}

void Store::fromJson(const QJsonObject &o)
{
    const int version = o.value("version").toInt(1);
    workspaces.clear();
    for (const auto &wv : o.value("workspaces").toArray()) {
        const auto wo = wv.toObject();
        Workspace w{wo.value("id").toString(), wo.value("name").toString(), wo.value("folder").toString(), {}, tintFromJson(wo.value("tint"))};
        if (w.id.isEmpty())
            continue;
        for (const auto &sv : wo.value("sessions").toArray()) {
            const auto so = sv.toObject();
            Session s;
            s.id = so.value("id").toString();
            s.cwd = so.value("cwd").toString();
            s.name = so.value("name").toString();
            if (version < 2 && s.name == QFileInfo(s.cwd).fileName()) // v1 stored the derived name
                s.name.clear();
            s.title = so.value("title").toString();
            s.flagged = so.value("flagged").toBool();
            s.context = so.value("context").toString();
            s.parentId = so.value("parentId").toString();
            s.tint = tintFromJson(so.value("tint"));
            s.fontSize = so.value("fontSize").toDouble();
            s.splitFontSize = so.value("splitFontSize").toDouble();
            s.hasSplit = so.value("hasSplit").toBool();
            s.splitVisible = s.hasSplit && so.value("splitVisible").toBool();
            s.splitAxis = so.value("splitAxis").toString(QStringLiteral("vertical"));
            s.splitRatio = qBound(0.05, so.value("splitRatio").toDouble(0.5), 0.95);
            // the capture moves into a one-shot slot: the next save writes it as gone, so it replays exactly once
            for (const auto &v : so.value("foregroundCommand").toArray())
                s.replay << v.toString();
            for (const auto &v : so.value("splitForegroundCommand").toArray())
                s.splitReplay << v.toString();
            if (so.contains("restoreCommand"))
                s.restorePin = so.value("restoreCommand").toString();
            if (so.contains("splitRestoreCommand"))
                s.splitRestorePin = so.value("splitRestoreCommand").toString();
            if (!s.id.isEmpty())
                w.sessions << s;
        }
        workspaces << w;
    }
    settings = Settings::fromJson(o.value("settings").toObject());
    if (!std::exchange(m_loaded, true))
        restoreModeAtLaunch = settings.restoreMode;
    focused.clear();
    for (const auto &f : o.value("focusedWorkspaces").toArray())
        if (workspace(f.toString())) // stale ids are pruned, so the filter never restores enabled and empty
            focused << f.toString();
    focusEnabled = o.value("focusEnabled").toBool() && !focused.isEmpty();
    recent.clear();
    for (const auto &r : o.value("recent").toArray())
        if (session(r.toString()))
            recent << r.toString();
    selected = session(o.value("selected").toString()) ? o.value("selected").toString() : QString();
    if (selected.isEmpty())
        if (auto all = allSessions(); !all.isEmpty())
            selected = all.first()->id;
}

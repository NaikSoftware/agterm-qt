// Control commands added to reach master's catalog: events, navigation, sidebar, context, recent, messages, and the
// app-side ones routed through Terminals::appCommand.
#include "control.h"
#include "hooks.h"
#include "store.h"

#include <QDateTime>
#include <algorithm>
#include <QJsonArray>
#include <QRegularExpression>
#include <QUuid>

static QJsonObject ok(const QJsonObject &result = {})
{
    return {{"ok", true}, {"result", result}};
}

static QJsonObject fail(const QString &error)
{
    return {{"ok", false}, {"error", error}};
}

QString unsupportedReason(const QString &cmd)
{
    static const QList<std::pair<QString, QString>> absent{
        {"zmx", "agterm-qt's live sessions run on agterm-session-host, not zmx"},
        {"session.restart", "it is not ported to agterm-qt's live sessions"},
        {"session.lead", "pane leads belong to remote sessions, which agterm-qt does not have"},
        {"browser", "agterm-qt has no built-in browser: page overlays need Qt WebEngine"},
        {"session.companion.page", "page companions need Qt WebEngine, which agterm-qt does not have"},
        {"session.overlay.reload", "page overlays need Qt WebEngine, which agterm-qt does not have"},
        {"session.overlay.navigate", "page overlays need Qt WebEngine, which agterm-qt does not have"},
        {"session.overlay.submit", "page overlays need Qt WebEngine, which agterm-qt does not have"},
        {"session.overlay.copy", "page overlays need Qt WebEngine, which agterm-qt does not have"},
        {"session.overlay.job.run", "remote overlay jobs need a remote viewer, which agterm-qt does not have"},
        {"session.overlay.page.serve", "page overlays need Qt WebEngine, which agterm-qt does not have"},
        {"quick", "agterm-qt has no quick terminal"},
        {"session.scratch", "agterm-qt has no scratch terminal"},
        {"session.background", "Konsole takes a pane's background from its profile, not per session"},
        {"surface.cursor", "Konsole's terminal part does not expose the cursor position"},
        {"config.reload", "agterm-qt has no Ghostty config; keymap reload and hooks reload re-read its own files"},
        {"debug.appearance", "it is a macOS UI-test hook"},
    };
    for (const auto &[name, why] : absent)
        if (cmd == name || cmd.startsWith(name + QLatin1Char('.')))
            return QStringLiteral("%1 is not supported on this platform (%2)").arg(cmd, why);
    return {};
}

static const QStringList leftPane{"left", "top", "primary", "main"}, rightPane{"right", "bottom", "split"}; // main: AGTERM_PANE

// workspace.go: the visible workspaces in order, wrapping; lands on the workspace's first session
static QJsonObject workspaceGo(Store &store, const QJsonObject &args)
{
    const QString to = args.value("to").toString();
    if (to != QLatin1String("next") && to != QLatin1String("prev") && to != QLatin1String("previous"))
        return fail(QStringLiteral("workspace.go requires --to next|prev"));
    const QList<Workspace *> visible = store.visibleWorkspaces();
    if (store.flaggedView || visible.size() < 2)
        return fail(QStringLiteral("no other workspace to navigate to"));
    const Workspace *cur = store.workspaceOf(store.selected);
    const qsizetype at = cur ? visible.indexOf(cur) : -1;
    const qsizetype n = visible.size();
    const bool next = to == QLatin1String("next");
    Workspace *dest = at < 0 ? (next ? visible.first() : visible.last()) : visible[((at + (next ? 1 : -1)) % n + n) % n];
    if (!dest->sessions.isEmpty())
        store.select(dest->sessions.first().id);
    return ok({{"id", dest->id}});
}

static QJsonObject sessionContext(Store &store, const QString &id, const QJsonObject &args)
{
    const QString mode = args.value("mode").toString();
    Session *s = store.session(id);
    if (mode == QLatin1String("clear")) {
        if (args.contains("text"))
            return fail(QStringLiteral("session.context clear takes no text"));
        s->context.clear();
    } else if (mode == QLatin1String("set")) {
        if (!args.contains("text"))
            return fail(QStringLiteral("session.context set requires text"));
        const QString raw = args.value("text").toString(), text = raw.trimmed();
        for (QChar c : raw)
            if (c.category() == QChar::Other_Control || c == QChar(0x2028) || c == QChar(0x2029))
                return fail(QStringLiteral("context must not contain control characters or line breaks"));
        if (text.isEmpty())
            return fail(QStringLiteral("context must not be empty (use --clear to remove it)"));
        if (text.toUtf8().size() > 256)
            return fail(QStringLiteral("context must be at most 256 UTF-8 bytes"));
        if (s->context == text)
            return ok({{"id", id}});
        s->context = text;
    } else {
        return fail(QStringLiteral("invalid context mode: %1 (set|clear)").arg(mode));
    }
    Q_EMIT store.changed();
    return ok({{"id", id}});
}

static QJsonObject sessionResize(Store &store, const QString &id, const QJsonObject &args)
{
    const bool absolute = args.contains("ratio"), delta = args.contains("ratioDelta");
    if (!absolute && !delta)
        return fail(QStringLiteral("session.resize requires --split-ratio, --grow-left, or --grow-right"));
    if (absolute && delta)
        return fail(QStringLiteral("session.resize: --split-ratio is mutually exclusive with --grow-left/--grow-right"));
    Session *s = store.session(id);
    if (!s->hasSplit)
        return fail(QStringLiteral("session has no split"));
    s->splitRatio = qBound(0.05, absolute ? args.value("ratio").toDouble() : s->splitRatio + args.value("ratioDelta").toDouble(), 0.95);
    Q_EMIT store.paneLayoutChanged(id);
    Q_EMIT store.changed();
    return ok({{"id", id}, {"ratio", s->splitRatio}});
}

static QJsonObject sessionText(Store &store, Terminals &terminals, const QString &id, const QJsonObject &args)
{
    const bool all = args.value("all").toBool();
    if (all && args.contains("lines"))
        return fail(QStringLiteral("use either --all or --lines, not both"));
    const int lines = args.value("lines").toInt();
    if (args.contains("lines") && lines <= 0)
        return fail(QStringLiteral("--lines must be greater than 0"));
    const QString raw = args.value("pane").toString(QStringLiteral("left"));
    if (!leftPane.contains(raw) && !rightPane.contains(raw))
        return fail(QStringLiteral("invalid pane: %1").arg(raw));
    const QString pane = leftPane.contains(raw) ? QStringLiteral("left") : QStringLiteral("right");
    if (pane == QLatin1String("right") && !store.session(id)->hasSplit)
        return fail(QStringLiteral("session has no split pane"));
    QString error;
    QString text = terminals.text(id, pane, all || lines > 0, &error); // default: the visible screen only
    if (!error.isEmpty())
        return fail(error);
    if (lines > 0) {
        QStringList rows = text.split(QLatin1Char('\n'));
        while (!rows.isEmpty() && rows.last().trimmed().isEmpty())
            rows.removeLast();
        text = rows.mid(qMax(qsizetype(0), rows.size() - lines)).join(QLatin1Char('\n'));
    }
    return ok({{"id", id}, {"text", text}, {"pane", pane}});
}

static QJsonObject sidebarCommand(Store &store, const QString &cmd, const QJsonObject &args)
{
    const QString mode = args.value("mode").toString(QStringLiteral("toggle"));
    if (cmd == QLatin1String("sidebar.mode")) {
        if (mode != QLatin1String("tree") && mode != QLatin1String("flagged") && mode != QLatin1String("toggle"))
            return fail(QStringLiteral("invalid sidebar mode: %1").arg(mode));
        store.flaggedView = mode == QLatin1String("toggle") ? !store.flaggedView : mode == QLatin1String("flagged");
        Q_EMIT store.changed();
        return ok({{"sidebarMode", store.flaggedView ? "flagged" : "tree"}});
    }
    if (cmd == QLatin1String("sidebar.flagged-layout")) {
        static const QStringList layouts{"flat", "plain", "tree"};
        if (!layouts.contains(mode) && mode != QLatin1String("toggle"))
            return fail(QStringLiteral("invalid flagged layout: %1").arg(mode));
        Settings s = store.settings;
        s.flaggedViewLayout = mode != QLatin1String("toggle") ? mode : s.flaggedViewLayout == QLatin1String("tree") ? QStringLiteral("flat") : QStringLiteral("tree");
        store.setSettings(s);
        return ok({{"text", s.flaggedViewLayout}});
    }
    // sidebar.expand | sidebar.collapse: workspace rows; collapse keeps the current workspace open
    const Workspace *cur = store.workspaceOf(store.selected);
    for (const Workspace &w : std::as_const(store.workspaces))
        cmd == QLatin1String("sidebar.expand") || (cur && cur->id == w.id) ? void(store.collapsed.remove(w.id)) : void(store.collapsed.insert(w.id));
    Q_EMIT store.changed();
    return ok();
}

static QJsonObject recentCommand(Store &store, const QString &cmd, const QString &target)
{
    if (cmd == QLatin1String("recent.open")) {
        if (store.closed.isEmpty())
            return fail(QStringLiteral("no recently closed items"));
        QString id = target;
        if (!id.isEmpty()) { // an exact id or a unique prefix of one
            QStringList hits;
            for (const auto &c : std::as_const(store.closed))
                if (c.id.startsWith(target, Qt::CaseInsensitive))
                    hits << c.id;
            if (hits.isEmpty())
                return fail(QStringLiteral("no such recent item: %1").arg(target));
            if (hits.size() > 1 && !hits.contains(target))
                return fail(QStringLiteral("ambiguous recent item prefix '%1' → %2, %3").arg(target, hits[0].left(8), hits[1].left(8)));
            id = hits.contains(target) ? target : hits.first();
        }
        const QString reopened = store.reopenClosed(id);
        return reopened.isEmpty() ? fail(QStringLiteral("no recently closed items")) : ok({{"id", reopened}});
    }
    if (!target.isEmpty())
        return fail(QStringLiteral("%1 takes no target or --window").arg(cmd));
    if (cmd == QLatin1String("recent.clear")) {
        store.closed.clear();
        return ok();
    }
    QJsonArray out;
    for (const auto &c : std::as_const(store.closed))
        out << QJsonObject{{"id", c.id}, {"kind", "session"}, {"title", c.title.isEmpty() ? QStringLiteral("New session") : c.title}, {"subtitle", c.cwd},
                           {"closedAt", c.closedAt / 1000.0}, {"workspaceID", c.workspaceId}, {"sessionIDs", QJsonArray{c.id}}};
    return ok({{"recent", out}});
}

// msg.*: in-memory mailboxes per pane id, app-wide
struct Message {
    QString id, from, to, body;
    double ts;
};
static QHash<QString, QList<Message>> s_mail;

// SESSION:PANE to "session:paneId"; a role picks the live pane in that position. "" with *error on a bad address.
static QString resolveAddress(Store &store, Terminals &terminals, const QString &address, bool identity, QString *error)
{
    const QStringList parts = address.split(QLatin1Char(':'));
    if (parts.size() != 2 || parts[0].isEmpty() || parts[1].isEmpty())
        return *error = identity ? QStringLiteral("identity \"%1\" must name a pane id, not a role").arg(address)
                                 : QStringLiteral("invalid address \"%1\": expected SESSION:PANE").arg(address),
               QString();
    const bool role = leftPane.contains(parts[1]) || rightPane.contains(parts[1]);
    if (identity && role)
        return *error = QStringLiteral("identity \"%1\" must name a pane id, not a role").arg(address), QString();
    if (!role && QUuid::fromString(parts[1]).isNull())
        return *error = QStringLiteral("invalid address \"%1\": expected SESSION:PANE").arg(address), QString();
    const QString sid = store.resolveSession(parts[0], error);
    if (sid.isEmpty())
        return {};
    const QStringList panes = terminals.paneIds(sid);
    const QString pane = !role ? parts[1] : leftPane.contains(parts[1]) ? panes.value(0) : panes.value(1);
    if (pane.isEmpty() || !panes.contains(pane, Qt::CaseInsensitive)) {
        if (identity)
            *error = QStringLiteral("pane %1 is not a live pane of session %2").arg(parts[1], sid);
        return {};
    }
    return sid + QLatin1Char(':') + pane.toLower();
}

static QJsonObject messageCommand(Store &store, Terminals &terminals, const QString &cmd, const QJsonObject &args)
{
    QString error, me;
    if (args.contains("identity") && (me = resolveAddress(store, terminals, args.value("identity").toString(), true, &error)).isEmpty())
        return fail(error.isEmpty() ? QStringLiteral("unknown pane") : error);
    if (cmd == QLatin1String("msg.send")) {
        const QString body = args.value("body").toString();
        const bool all = args.value("all").toBool();
        const QJsonArray to = args.value("recipients").toArray();
        if (body.isEmpty())
            return fail(QStringLiteral("msg.send requires a body"));
        if (all && !to.isEmpty())
            return fail(QStringLiteral("msg.send takes --to or --all, not both"));
        if (!all && to.isEmpty())
            return fail(QStringLiteral("msg.send requires --to or --all"));
        if (args.contains("workspace") && !all)
            return fail(QStringLiteral("msg.send: --workspace narrows --all only"));
        if (body.toUtf8().size() > 65536)
            return fail(QStringLiteral("message body too large (%1 bytes, max 65536)").arg(body.toUtf8().size()));
        QStringList targets;
        QJsonArray rejected;
        if (all) {
            QString ws;
            if (args.contains("workspace") && (ws = store.resolveWorkspace(args.value("workspace").toString(), &error)).isEmpty())
                return fail(error);
            // shortcut: --all reaches the panes of the addressed window only; master spans every open window
            for (const Session *s : store.allSessions())
                if (ws.isEmpty() || store.workspaceOf(s->id)->id == ws)
                    for (const QString &p : terminals.paneIds(s->id))
                        targets << s->id + QLatin1Char(':') + p.toLower();
            targets.removeAll(me);
        } else {
            for (const auto &v : to) {
                const QString address = resolveAddress(store, terminals, v.toString(), false, &error);
                if (!error.isEmpty())
                    return fail(error);
                if (address.isEmpty())
                    rejected << QJsonObject{{"address", v.toString()}, {"reason", "unknown pane"}};
                else if (!targets.contains(address))
                    targets << address;
            }
        }
        const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QJsonArray delivered;
        for (const QString &t : std::as_const(targets)) {
            QList<Message> &box = s_mail[t.section(QLatin1Char(':'), 1)];
            qsizetype bytes = body.size();
            for (const Message &m : std::as_const(box))
                bytes += m.body.size();
            if (box.size() >= 256 || bytes > (1 << 20)) {
                rejected << QJsonObject{{"address", t}, {"reason", "mailbox full"}};
                continue;
            }
            box.append({id, me, t, body, QDateTime::currentMSecsSinceEpoch() / 1000.0});
            delivered << t;
            const QString sid = t.section(QLatin1Char(':'), 0, 0);
            QJsonObject payload{{"name", store.session(sid)->displayName()}, {"paneID", t.section(QLatin1Char(':'), 1)}, {"messageID", id}};
            if (!me.isEmpty())
                payload.insert("from", me);
            terminals.emitEvent(QStringLiteral("message"), sid, payload);
        }
        return ok({{"messageID", id}, {"delivered", delivered}, {"rejected", rejected}});
    }
    if (me.isEmpty())
        return fail(QStringLiteral("%1 requires an identity: --as SESSION:PANE-ID").arg(cmd));
    QList<Message> &box = s_mail[me.section(QLatin1Char(':'), 1)];
    if (cmd == QLatin1String("msg.ack")) {
        const QJsonArray ids = args.value("ids").toArray();
        if (ids.isEmpty())
            return fail(QStringLiteral("msg.ack requires message ids"));
        QStringList wanted;
        for (const auto &v : ids) {
            if (QUuid::fromString(v.toString()).isNull())
                return fail(QStringLiteral("invalid message id: %1").arg(v.toString()));
            wanted << v.toString().toLower();
        }
        QJsonArray acked;
        box.removeIf([&](const Message &m) { return wanted.contains(m.id) && (acked << m.id, true); });
        return ok({{"acked", acked}});
    }
    QJsonArray messages, acked;
    const QList<Message> batch = box.mid(0, 32);
    for (const Message &m : batch) {
        QJsonObject o{{"id", m.id}, {"to", m.to}, {"ts", m.ts}, {"body", m.body}};
        if (!m.from.isEmpty())
            o.insert("from", m.from);
        messages << o;
        if (args.value("ack").toBool())
            acked << m.id;
    }
    if (args.value("ack").toBool())
        box.remove(0, batch.size());
    QJsonObject result{{"messages", messages}, {"remaining", int(box.size() - (args.value("ack").toBool() ? 0 : batch.size()))}};
    if (args.value("ack").toBool())
        result.insert("acked", acked);
    return ok(result);
}

// session.close / session.move with repeated --target (args.targets); null when the request is a single target
static std::optional<QJsonObject> batch(Store &store, const QString &cmd, const QJsonObject &args)
{
    const QJsonArray raw = args.value("targets").toArray();
    if (!args.contains("targets") || raw.size() == 1)
        return std::nullopt;
    if (raw.isEmpty())
        return fail(QStringLiteral("%1 requires at least one --target").arg(cmd));
    QStringList ids;
    QString error;
    for (const auto &v : raw) {
        const QString id = store.resolveSession(v.toString(), &error);
        if (id.isEmpty())
            return fail(error);
        if (!ids.contains(id))
            ids << id;
    }
    if (cmd == QLatin1String("session.close")) {
        bool soft = false;
        if (args.value("undoable").toBool() && store.settings.closeUndo)
            soft = store.softClose(ids);
        if (!soft)
            for (const QString &id : ids)
                if (store.session(id)) // an earlier close may have taken it with its parent
                    store.removeSession(id);
        QJsonObject r{{"affected", int(ids.size())}, {"closed", QJsonArray::fromStringList(ids)}};
        if (args.contains("undoable"))
            r.insert("undoable", soft);
        return ok(r);
    }
    const QString ws = store.resolveWorkspace(args.value("workspace").toString(), &error);
    if (ws.isEmpty())
        return fail(error);
    int index = args.contains("index") ? args.value("index").toInt() : -1, affected = 0;
    for (const QString &id : ids) {
        affected += store.moveSession(id, ws, index) ? 1 : 0;
        if (index >= 0)
            ++index;
    }
    return ok({{"affected", affected}});
}

static bool hasControl(const QString &t, bool allowLines = false)
{
    for (QChar c : t)
        if ((c.unicode() < 0x20 || c.unicode() == 0x7f) && !(allowLines && c == QLatin1Char('\n')))
            return true;
    return false;
}

QString pickArgsError(const QJsonObject &args)
{
    if (!args.contains("items"))
        return QStringLiteral("pick.open requires items");
    const QJsonArray items = args.value("items").toArray();
    if (items.isEmpty() && !args.value("allowCustom").toBool())
        return QStringLiteral("pick.open requires at least one item");
    if (items.size() > 1000)
        return QStringLiteral("too many items (max 1000)");
    QStringList ids;
    static const QStringList statuses{"idle", "active", "completed", "blocked"}, shapes{"circle", "square", "triangle", "diamond", "capsule", "star"};
    static const QRegularExpression hex(QStringLiteral("^#[0-9a-fA-F]{6}$"));
    for (const auto &v : items) {
        const QJsonObject it = v.toObject();
        if (it.value("label").toString().isEmpty())
            return QStringLiteral("pick item label must not be empty");
        if (ids.contains(it.value("id").toString()))
            return QStringLiteral("pick item ids must be unique");
        ids << it.value("id").toString();
        if (hasControl(it.value("label").toString()) || hasControl(it.value("subtitle").toString()))
            return QStringLiteral("item text must not contain control characters");
        if (it.contains("status") && !statuses.contains(it.value("status").toString()))
            return QStringLiteral("invalid pick item status (idle|active|completed|blocked)");
        if (it.contains("statusColor") && !hex.match(it.value("statusColor").toString()).hasMatch())
            return QStringLiteral("invalid pick item color (expected #rrggbb)");
        if (it.contains("statusShape") && !shapes.contains(it.value("statusShape").toString()))
            return QStringLiteral("invalid pick item shape (circle|square|triangle|diamond|capsule|star)");
    }
    if (args.contains("selection") && !ids.contains(args.value("selection").toString()))
        return QStringLiteral("pick select must name an item id");
    return {};
}

QString askArgsError(QJsonObject *a)
{
    QJsonObject &args = *a;
    if (args.value("title").toString().trimmed().isEmpty())
        return QStringLiteral("ask.open requires a title");
    const bool input = args.contains("input"), choices = args.contains("choices");
    if (input && choices)
        return QStringLiteral("input and choices are exclusive");
    for (const char *k : {"value", "placeholder"})
        if (args.contains(QLatin1String(k)) && !input)
            return QStringLiteral("%1 requires input").arg(QLatin1String(k));
    for (const char *k : {"checked", "multiple", "other"})
        if (args.contains(QLatin1String(k)) && !choices)
            return QStringLiteral("%1 requires choices").arg(QLatin1String(k));
    const bool multiline = args.value("input").toString() == QLatin1String("multiline");
    if (input && !multiline && args.value("input").toString() != QLatin1String("line"))
        return QStringLiteral("unknown input");
    if (args.value("value").toString().toUtf8().size() > 65536)
        return QStringLiteral("ask text too long (max 65536 bytes)");
    if (hasControl(args.value("value").toString(), multiline))
        return QStringLiteral("ask text must not contain control characters");
    if (choices) {
        const QJsonArray list = args.value("choices").toArray();
        if (list.isEmpty() || list.size() > 50)
            return QStringLiteral("ask choices must be 1 to 50");
        QStringList ids;
        for (const auto &v : list) {
            if (v.toObject().value("label").toString().isEmpty())
                return QStringLiteral("ask choice label must not be empty");
            if (ids.contains(v.toObject().value("id").toString()))
                return QStringLiteral("ask choice ids must be unique");
            ids << v.toObject().value("id").toString();
        }
        QStringList checked;
        for (const auto &v : args.value("checked").toArray()) {
            if (checked.contains(v.toString()))
                return QStringLiteral("ask checked ids must be unique");
            if (!ids.contains(v.toString()))
                return QStringLiteral("unknown checked choice: %1").arg(v.toString());
            checked << v.toString();
        }
        if (checked.size() > 1 && !args.value("multiple").toBool())
            return QStringLiteral("radio choices accept one checked id");
        if (args.contains("other") && args.value("other").toString().trimmed().isEmpty())
            return QStringLiteral("ask other placeholder must not be blank");
    }
    if (!args.contains("buttons")) {
        if (!input && !choices)
            return QStringLiteral("ask.open requires buttons");
        args.insert("buttons", QJsonArray{QJsonObject{{"id", "ok"}, {"label", "OK"}}, QJsonObject{{"id", "cancel"}, {"label", "Cancel"}}});
        args.insert("defaultButton", "ok");
        args.insert("cancelButton", "cancel");
    }
    const QJsonArray buttons = args.value("buttons").toArray();
    if (buttons.isEmpty())
        return QStringLiteral("ask.open requires at least one button");
    if (buttons.size() > 6)
        return QStringLiteral("too many buttons (max 6)");
    QStringList ids, keys;
    bool hotkeys = false, controls = hasControl(args.value("title").toString()) || hasControl(args.value("message").toString(), true);
    for (const auto &v : buttons) {
        const QJsonObject b = v.toObject();
        if (b.value("label").toString().isEmpty())
            return QStringLiteral("ask button label must not be empty");
        if (ids.contains(b.value("id").toString()))
            return QStringLiteral("ask button ids must be unique");
        ids << b.value("id").toString();
        hotkeys |= b.contains("hotkey");
        controls |= hasControl(b.value("label").toString());
    }
    if (controls)
        return QStringLiteral("ask text must not contain control characters");
    for (const char *role : {"default", "destructive", "cancel"})
        if (const QString k = QLatin1String(role) + QStringLiteral("Button"); args.contains(k) && !ids.contains(args.value(k).toString()))
            return QStringLiteral("unknown %1 button: %2").arg(QLatin1String(role), args.value(k).toString());
    if (args.contains("defaultButton") && args.value("defaultButton") == args.value("destructiveButton"))
        return QStringLiteral("default button must not be destructive");
    if (hotkeys && input)
        return QStringLiteral("ask input does not take hotkeys");
    if (args.contains("style") && args.value("style").toString() != QLatin1String("terminal") && args.value("style").toString() != QLatin1String("gui"))
        return QStringLiteral("unknown style");
    if (args.contains("align") && !QStringList{"left", "center", "right"}.contains(args.value("align").toString()))
        return QStringLiteral("unknown align");
    if (args.contains("width") && (args.value("width").toInt() < 10 || args.value("width").toInt() > 100))
        return QStringLiteral("width must be 10 to 100");
    for (const auto &v : buttons) {
        if (!v.toObject().contains("hotkey"))
            continue;
        const QString key = v.toObject().value("hotkey").toString().toLower();
        if (key.size() != 1 || key[0] < QLatin1Char('a') || key[0] > QLatin1Char('z'))
            return QStringLiteral("ask button hotkey must be one ASCII letter");
        if (keys.contains(key))
            return QStringLiteral("ask button hotkeys must be unique");
        keys << key;
    }
    if (args.contains("pane") && !leftPane.contains(args.value("pane").toString()) && !rightPane.contains(args.value("pane").toString()))
        return QStringLiteral("--pane must be left or right");
    return {};
}

QJsonObject moreCommands(Store &store, Terminals &terminals, const QString &cmd, const QString &target, const QJsonObject &args, bool *handled)
{
    *handled = true;
    QString error;
    if (cmd == QLatin1String("events.read")) {
        const QJsonObject events = EventSource::read(args, &error);
        return error.isEmpty() ? ok({{"events", events}}) : QJsonObject{{"ok", false}, {"error", error}, {"result", QJsonObject{{"events", events}}}};
    }
    if (cmd == QLatin1String("workspace.go"))
        return workspaceGo(store, args);
    if (cmd.startsWith(QLatin1String("sidebar.")) && cmd != QLatin1String("sidebar.width"))
        return sidebarCommand(store, cmd, args);
    if (cmd.startsWith(QLatin1String("recent.")))
        return recentCommand(store, cmd, target);
    if (cmd.startsWith(QLatin1String("msg.")))
        return messageCommand(store, terminals, cmd, args);
    if (cmd == QLatin1String("session.close") || cmd == QLatin1String("session.move"))
        if (auto r = batch(store, cmd, args))
            return *r;
    if (cmd == QLatin1String("notify")) {
        const QString id = store.resolveSession(target, &error);
        if (id.isEmpty())
            return fail(error);
        if (args.value("body").toString().isEmpty())
            return fail(QStringLiteral("notify requires a body"));
        store.notify(id, args.value("title").toString(), args.value("body").toString());
        if (!store.settings.notificationsEnabled) // the badge still rises; only the banner is off
            return ok({{"id", id}, {"text", QStringLiteral("badge updated, but \"Show notification banners\" is off, so no banner was posted")}});
        return ok({{"id", id}});
    }
    // the app owns these: the sidebar widget, panes' selection and search, dialogs, zoom
    static const QStringList app{"sidebar", "sidebar.width", "surface.zoom", "pick.open", "pick.result", "pick.cancel", "ask.result", "ask.cancel"};
    if (cmd == QLatin1String("pick.open"))
        if (const QString e = pickArgsError(args); !e.isEmpty())
            return fail(e);
    if (QStringList{"pick.result", "pick.cancel", "ask.result", "ask.cancel"}.contains(cmd) && target.isEmpty())
        return fail(QStringLiteral("%1 requires %2 id").arg(cmd, cmd.startsWith(QLatin1String("pick")) ? QStringLiteral("a pick") : QStringLiteral("an ask")));
    if (app.contains(cmd)) {
        QJsonObject result;
        return terminals.appCommand(cmd, target, args, &result, &error) ? ok(result) : fail(error);
    }
    if (cmd.startsWith(QLatin1String("workspace.")) && QStringList{"workspace.collapse", "workspace.expand", "workspace.flag"}.contains(cmd)) {
        const QString id = store.resolveWorkspace(target, &error);
        if (id.isEmpty())
            return fail(error);
        if (cmd == QLatin1String("workspace.flag")) {
            const QString mode = args.value("mode").toString(QStringLiteral("toggle"));
            if (mode != QLatin1String("on") && mode != QLatin1String("off") && mode != QLatin1String("toggle"))
                return fail(QStringLiteral("invalid workspace flag mode: %1").arg(mode));
            const auto &ss = store.workspace(id)->sessions;
            const bool allFlagged = std::all_of(ss.begin(), ss.end(), [](const Session &s) { return s.flagged; });
            const bool on = mode == QLatin1String("toggle") ? !allFlagged : mode == QLatin1String("on");
            for (const QString &sid : [&] { QStringList l; for (const auto &s : ss) l << s.id; return l; }())
                store.setFlag(sid, on);
        } else {
            cmd == QLatin1String("workspace.collapse") ? void(store.collapsed.insert(id)) : void(store.collapsed.remove(id));
            Q_EMIT store.changed();
        }
        return ok({{"id", id}});
    }
    static const QStringList sessionCmds{"session.context", "session.collapse", "session.expand", "session.resize", "session.text", "session.focus",
                                         "session.copy", "session.paste", "session.selectall", "session.search", "session.reveal", "ask.open"};
    if (!sessionCmds.contains(cmd)) {
        *handled = false;
        return {};
    }
    // ask.open's GUI style needs no session
    const bool optional = cmd == QLatin1String("ask.open") && args.value("style").toString() == QLatin1String("gui") && target.isEmpty();
    const QString id = optional ? QString() : store.resolveSession(target, &error);
    if (id.isEmpty() && !optional)
        return fail(error);
    if (cmd == QLatin1String("session.context"))
        return sessionContext(store, id, args);
    if (cmd == QLatin1String("session.resize"))
        return sessionResize(store, id, args);
    if (cmd == QLatin1String("session.text"))
        return sessionText(store, terminals, id, args);
    if (cmd == QLatin1String("session.collapse") || cmd == QLatin1String("session.expand")) {
        if (store.companionsOf(id).isEmpty())
            return fail(QStringLiteral("session has no companions"));
        cmd == QLatin1String("session.collapse") ? void(store.collapsed.insert(id)) : void(store.collapsed.remove(id));
        Q_EMIT store.changed();
        return ok({{"id", id}});
    }
    QJsonObject a = args;
    if (cmd == QLatin1String("ask.open")) {
        if (const QString e = askArgsError(&a); !e.isEmpty())
            return fail(e);
    } else if (cmd == QLatin1String("session.search") && args.contains("to") && !QStringList{"next", "prev", "close"}.contains(args.value("to").toString())) {
        return fail(QStringLiteral("session.search --to must be next|prev|close"));
    }
    QJsonObject result{{"id", id}};
    return terminals.appCommand(cmd, id, a, &result, &error) ? ok(result) : fail(error);
}

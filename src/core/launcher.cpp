#include "launcher.h"
#include "restore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlQuery>

const QList<Launcher> &launchers()
{
    static const QList<Launcher> all{{"terminal", "Terminal", {}},
                                     {"claude", "Claude Code", "claude"},
                                     {"codex", "Codex", "codex"},
                                     {"opencode", "OpenCode", "opencode"},
                                     {"antigravity", "Antigravity", "agy"}};
    return all;
}

const Launcher *launcher(const QString &id)
{
    for (const auto &l : launchers())
        if (l.id == id)
            return &l;
    return nullptr;
}

static QString named(const QString &arg)
{
    const QString base = arg.section(QLatin1Char('/'), -1);
    for (const auto &l : launchers())
        if (!l.command.isEmpty() && l.command == base)
            return l.id;
    return {};
}

QString agentForArgv(QStringList argv)
{
    if (argv.isEmpty())
        return {};
    if (argv[0].startsWith(QLatin1Char('-')))
        argv[0] = argv[0].mid(1); // login shells report -zsh
    if (const QString a = named(argv[0]); !a.isEmpty())
        return a;
    static const QStringList interpreters{"node", "bun", "deno"};
    if (!interpreters.contains(argv[0].section(QLatin1Char('/'), -1)))
        return {};
    for (int i = 1; i < argv.size(); ++i)
        if (!argv[i].startsWith(QLatin1Char('-')))
            return named(argv[i]);
    return {};
}

QString agentForGroup(const QList<QStringList> &group)
{
    for (const auto &argv : group)
        if (const QString a = agentForArgv(argv); !a.isEmpty())
            return a;
    return {};
}

QString commandTitle(QStringList argv)
{
    if (argv.isEmpty())
        return {};
    if (argv[0].startsWith(QLatin1Char('-')))
        argv[0] = argv[0].mid(1);
    const QString program = argv[0].section(QLatin1Char('/'), -1);
    static const QStringList interpreters{"python", "python3", "node", "bun", "deno", "ruby", "perl", "php", "bash", "sh", "zsh", "fish"};
    if (!interpreters.contains(program))
        return program;
    for (int i = 1; i < argv.size(); ++i) {
        if (argv[i] == QLatin1String("-c") || argv[i] == QLatin1String("-e"))
            break;
        if (!argv[i].startsWith(QLatin1Char('-')))
            return argv[i].section(QLatin1Char('/'), -1);
    }
    return program;
}

QString antigravityTitle(const QString &fdDir, const QString &dataDir)
{
    QString id;
    const QString presence = QDir(dataDir).absoluteFilePath(QStringLiteral("presence")) + QLatin1Char('/');
    for (const QString &fd : QDir(fdDir).entryList(QDir::AllEntries | QDir::System | QDir::NoDotAndDotDot)) {
        const QString target = QFile::symLinkTarget(fdDir + QLatin1Char('/') + fd);
        if (target.startsWith(presence) && target.endsWith(QLatin1String(".lock"))) {
            id = QFileInfo(target).completeBaseName();
            break;
        }
    }
    if (id.isEmpty())
        return {};
    const QString connection = QStringLiteral("agterm-antigravity");
    QString title;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=100")); // never block agy's writes
        db.setDatabaseName(QDir(dataDir).absoluteFilePath(QStringLiteral("conversation_summaries.db")));
        if (db.open()) {
            QSqlQuery q(db);
            q.prepare(QStringLiteral("SELECT title FROM conversation_summaries WHERE conversation_id = ?"));
            q.addBindValue(id);
            if (q.exec() && q.next())
                title = q.value(0).toString().trimmed();
        }
    }
    QSqlDatabase::removeDatabase(connection);
    return title;
}

QString claudeResumeLine(const QString &sessionsDir, int pid)
{
    QFile f(QStringLiteral("%1/%2.json").arg(sessionsDir).arg(pid));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    const QString id = o.value(QStringLiteral("sessionId")).toString(), cwd = o.value(QStringLiteral("cwd")).toString();
    // the id is typed into a shell: only a plain token passes
    static const QRegularExpression token(QStringLiteral("^[A-Za-z0-9_-]+$"));
    if (!token.match(id).hasMatch())
        return {};
    // claude finds a conversation under the folder it began in, which the shell may have left since
    // shortcut: fish reads a backslash inside single quotes as an escape, so such a folder breaks there; quote per shell if that shows up
    return (cwd.isEmpty() ? QString() : QStringLiteral("cd ") + Restore::shellQuoted({cwd}) + QStringLiteral(" && ")) + QStringLiteral("claude --resume ") + id;
}

QStringList availableLaunchers(const QString &path, const std::function<bool(const QString &)> &isExecutable)
{
    const QStringList dirs = path.split(QLatin1Char(':'), Qt::SkipEmptyParts);
    QStringList out;
    for (const auto &l : launchers()) {
        bool found = l.command.isEmpty();
        for (int i = 0; !found && i < dirs.size(); ++i)
            found = isExecutable(dirs[i] + QLatin1Char('/') + l.command);
        if (found)
            out << l.id;
    }
    return out;
}

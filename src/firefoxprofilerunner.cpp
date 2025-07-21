#include "firefoxprofilerunner.h"

#include <KConfigGroup>
#include <KLocalizedString>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QIcon>
#include <QProcess>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUrl>
#include <QSqlError>
#include <QThread>
#include <QTemporaryFile>
#include <QDateTime>

ZenBookmarkRunner::ZenBookmarkRunner(QObject *parent, const KPluginMetaData &data, const QVariantList &)
#if KRUNNER_VERSION_MAJOR == 5
    : AbstractRunner(parent, data, QVariantList{})
    , zenIcon("bookmarks")
#else
    : AbstractRunner(parent, data)
    , zenIcon("bookmarks")
#endif
{
}

void ZenBookmarkRunner::reloadConfiguration()
{
    // Set up Zen browser bookmarks database path
    QString homeDir = QDir::homePath();
    zenBookmarksPath = homeDir + "/.var/app/app.zen_browser.zen/.zen/cr6uussi.Default (release)/places.sqlite";

    QList<RunnerSyntax> syntaxes;
    syntaxes.append(RunnerSyntax("b :q:", "Plugin gets triggered by b... search for bookmarks by title or URL"));
    syntaxes.append(RunnerSyntax("bookmark :q:", "Plugin gets triggered by bookmark... search for bookmarks by title or URL"));
    setSyntaxes(syntaxes);
}

void ZenBookmarkRunner::match(RunnerContext &context)
{
    QString term = context.query();
    if (!context.isValid()) {
        return;
    }
    if (!term.startsWith(shortPrefix, Qt::CaseInsensitive) && !term.startsWith(mediumPrefix, Qt::CaseInsensitive)) {
        return;
    }

    const QString filter = filterRegex.match(term).captured(1);
    QList<QueryMatch> matches = createBookmarkMatches(filter);
    context.addMatches(matches);
}

void ZenBookmarkRunner::run(const RunnerContext & /*context*/, const QueryMatch &match)
{
    const QMap<QString, QVariant> data = match.data().toMap();
    QString url = data.value("url").toString();
    
    // Launch Zen browser with the bookmark URL using flatpak
    QStringList args = {"run", "app.zen_browser.zen", url};
    QProcess::startDetached("flatpak", args);
}

QueryMatch ZenBookmarkRunner::createMatch(const QString &text, const QMap<QString, QVariant> &data, float relevance)
{
    QueryMatch match(this);
    match.setIconName(zenIcon);
    match.setText(text);
    match.setData(data);
    match.setRelevance(relevance);
#if KRUNNER_VERSION_MAJOR == 5
    match.setType(QueryMatch::ExactMatch);
#else
    match.setCategoryRelevance(QueryMatch::CategoryRelevance::Highest);
#endif
    return match;
}

QList<QueryMatch> ZenBookmarkRunner::createBookmarkMatches(const QString &filter)
{
    QList<::QueryMatch> matches;
    
    // Check if places.sqlite exists
    qDebug() << "Looking for database at:" << zenBookmarksPath;
    if (!QFile::exists(zenBookmarksPath)) {
        qDebug() << "Zen bookmarks database not found at:" << zenBookmarksPath;
        return matches;
    }
    qDebug() << "Database file exists";
    
    // Create a unique connection name to avoid conflicts
    QString connectionName = QString("zen_bookmarks_%1").arg(reinterpret_cast<qintptr>(QThread::currentThread()));
    
    // Always use a temporary copy to avoid locking issues
    QString tempDbPath = QDir::temp().filePath(QString("zen_bookmarks_%1.db").arg(QDateTime::currentMSecsSinceEpoch()));
    
    // Copy the main database file
    if (!QFile::copy(zenBookmarksPath, tempDbPath)) {
        qDebug() << "Failed to copy database to temp location";
        return matches;
    }
    
    // Also copy WAL file if it exists (this contains recent changes)
    QString walPath = zenBookmarksPath + "-wal";
    QString tempWalPath = tempDbPath + "-wal";
    if (QFile::exists(walPath)) {
        QFile::copy(walPath, tempWalPath);
        qDebug() << "Copied WAL file";
    }
    
    // Also copy SHM file if it exists
    QString shmPath = zenBookmarksPath + "-shm";
    QString tempShmPath = tempDbPath + "-shm";
    if (QFile::exists(shmPath)) {
        QFile::copy(shmPath, tempShmPath);
        qDebug() << "Copied SHM file";
    }
    
    QSqlDatabase db;
    if (QSqlDatabase::contains(connectionName)) {
        db = QSqlDatabase::database(connectionName);
    } else {
        db = QSqlDatabase::addDatabase("QSQLITE", connectionName);
    }
    
    db.setDatabaseName(tempDbPath);
    
    if (!db.open()) {
        qDebug() << "Failed to open temp database:" << db.lastError().text();
        QFile::remove(tempDbPath);
        QFile::remove(tempWalPath);
        QFile::remove(tempShmPath);
        QSqlDatabase::removeDatabase(connectionName);
        return matches;
    }
    
    qDebug() << "Using temporary database copy at:" << tempDbPath;
    
    QString queryStr = "SELECT moz_bookmarks.title, moz_places.url FROM moz_bookmarks "
                      "JOIN moz_places ON moz_bookmarks.fk = moz_places.id "
                      "WHERE moz_bookmarks.title IS NOT NULL AND moz_bookmarks.title != '' ";
    
    if (!filter.isEmpty()) {
        queryStr += "AND (moz_bookmarks.title LIKE ? OR moz_places.url LIKE ?)";
    }
    
    queryStr += " ORDER BY moz_bookmarks.title";
    
    QSqlQuery query(db);
    query.prepare(queryStr);
    
    if (!filter.isEmpty()) {
        QString likeFilter = "%" + filter + "%";
        query.addBindValue(likeFilter);
        query.addBindValue(likeFilter);
    }
    
    qDebug() << "Executing query:" << queryStr;
    qDebug() << "Filter:" << filter;
    
    if (query.exec()) {
        qDebug() << "Query executed successfully";
        int count = 0;
        while (query.next()) {
            count++;
            QString title = query.value(0).toString();
            QString url = query.value(1).toString();
            
            if (title.isEmpty() || url.isEmpty()) {
                continue;
            }
            
            QMap<QString, QVariant> data;
            data.insert("url", url);
            
            QString displayText = title;
            if (!url.isEmpty()) {
                displayText += " - " + url;
            }
            
            float relevance = 0.8;
            if (!filter.isEmpty()) {
                if (title.contains(filter, Qt::CaseInsensitive)) {
                    relevance = 0.9;
                }
                if (title.startsWith(filter, Qt::CaseInsensitive)) {
                    relevance = 1.0;
                }
            }
            
            matches.append(createMatch(displayText, data, relevance));
        }
        qDebug() << "Found" << count << "bookmarks";
    } else {
        qDebug() << "Failed to execute bookmark query:" << query.lastError().text();
        qDebug() << "Query was:" << queryStr;
    }
    
    // Clean up
    QString dbName = db.databaseName();
    db.close();
    QSqlDatabase::removeDatabase(connectionName);
    
    // Remove temp files
    QFile::remove(dbName);
    QFile::remove(dbName + "-wal");
    QFile::remove(dbName + "-shm");
    
    return matches;
}

K_PLUGIN_CLASS_WITH_JSON(ZenBookmarkRunner, "firefoxprofilerunner.json")

#include "firefoxprofilerunner.moc"
#include "moc_firefoxprofilerunner.cpp"

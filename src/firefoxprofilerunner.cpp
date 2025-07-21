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
#include <QIODevice>

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
    
    // Also set favicon database path
    QString zenProfilePath = homeDir + "/.var/app/app.zen_browser.zen/.zen/cr6uussi.Default (release)";
    zenFaviconsPath = zenProfilePath + "/favicons.sqlite";

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
    
    // Use favicon if available, otherwise use default icon
    if (data.contains("favicon") && !data.value("favicon").toString().isEmpty()) {
        QString faviconPath = data.value("favicon").toString();
        qDebug() << "Setting favicon icon from path:" << faviconPath;
        QIcon faviconIcon(faviconPath);
        if (faviconIcon.isNull()) {
            qDebug() << "Failed to load favicon icon, using default";
            match.setIconName(zenIcon);
        } else {
            qDebug() << "Successfully loaded favicon icon";
            match.setIcon(faviconIcon);
        }
    } else {
        qDebug() << "No favicon data in match, using default icon";
        match.setIconName(zenIcon);
    }
    
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
    qint64 timestamp = QDateTime::currentMSecsSinceEpoch();
    QString tempDbPath = QDir::temp().filePath(QString("zen_bookmarks_%1.db").arg(timestamp));
    QString tempFaviconsPath = QDir::temp().filePath(QString("zen_favicons_%1.db").arg(timestamp));
    
    // Copy the main database file
    if (!QFile::copy(zenBookmarksPath, tempDbPath)) {
        qDebug() << "Failed to copy database to temp location";
        return matches;
    }
    
    // Copy the favicons database file
    bool hasFavicons = false;
    if (QFile::exists(zenFaviconsPath) && QFile::copy(zenFaviconsPath, tempFaviconsPath)) {
        hasFavicons = true;
        qDebug() << "Copied favicons database";
        
        // Also copy WAL and SHM files for favicons if they exist
        QString faviconWalPath = zenFaviconsPath + "-wal";
        QString tempFaviconWalPath = tempFaviconsPath + "-wal";
        if (QFile::exists(faviconWalPath)) {
            QFile::copy(faviconWalPath, tempFaviconWalPath);
        }
        
        QString faviconShmPath = zenFaviconsPath + "-shm";
        QString tempFaviconShmPath = tempFaviconsPath + "-shm";
        if (QFile::exists(faviconShmPath)) {
            QFile::copy(faviconShmPath, tempFaviconShmPath);
        }
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
            
            // Get favicon for this URL
            QString faviconPath;
            if (hasFavicons) {
                qDebug() << "Trying to get favicon for URL:" << url;
                faviconPath = getFaviconForUrl(url, tempFaviconsPath);
                if (!faviconPath.isEmpty()) {
                    qDebug() << "Got favicon path:" << faviconPath;
                    data.insert("favicon", faviconPath);
                } else {
                    qDebug() << "No favicon found for URL:" << url;
                }
            } else {
                qDebug() << "No favicons database available";
            }
            
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
    
    // Remove favicon temp files
    if (hasFavicons) {
        QFile::remove(tempFaviconsPath);
        QFile::remove(tempFaviconsPath + "-wal");
        QFile::remove(tempFaviconsPath + "-shm");
    }
    
    return matches;
}

QString ZenBookmarkRunner::getFaviconForUrl(const QString &url, const QString &tempFaviconsDbPath)
{
    // Use a unique connection name for each call to avoid conflicts
    QString faviconConnectionName = QString("zen_favicons_%1_%2").arg(reinterpret_cast<qintptr>(QThread::currentThread())).arg(QDateTime::currentMSecsSinceEpoch());
    
    QSqlDatabase faviconDb = QSqlDatabase::addDatabase("QSQLITE", faviconConnectionName);
    faviconDb.setDatabaseName(tempFaviconsDbPath);
    
    if (!faviconDb.open()) {
        qDebug() << "Failed to open favicons database:" << faviconDb.lastError().text();
        return QString();
    }
    
    qDebug() << "Opened favicons database successfully";
    
    // Firefox favicon structure: moz_pages_w_icons -> moz_icons_to_pages -> moz_icons
    // Query to get favicon data for a specific URL
    QString queryStr = "SELECT i.data FROM moz_icons i "
                      "JOIN moz_icons_to_pages itp ON i.id = itp.icon_id "
                      "JOIN moz_pages_w_icons p ON itp.page_id = p.id "
                      "WHERE p.page_url = ? AND i.data IS NOT NULL "
                      "ORDER BY i.width DESC LIMIT 1";
    
    QSqlQuery faviconQuery(faviconDb);
    faviconQuery.prepare(queryStr);
    faviconQuery.addBindValue(url);
    
    qDebug() << "Executing favicon query for URL:" << url;
    
    if (faviconQuery.exec()) {
        qDebug() << "Favicon query executed successfully";
        if (faviconQuery.next()) {
            qDebug() << "Found favicon data for URL:" << url;
            QByteArray iconData = faviconQuery.value(0).toByteArray();
        if (!iconData.isEmpty()) {
            // Save icon data to a temporary file
            QString tempIconPath = QDir::temp().filePath(QString("zen_favicon_%1_%2.ico")
                                                        .arg(QDateTime::currentMSecsSinceEpoch())
                                                        .arg(qHash(url)));
            QFile iconFile(tempIconPath);
            if (iconFile.open(QIODevice::WriteOnly)) {
                iconFile.write(iconData);
                iconFile.close();
                qDebug() << "Created favicon file:" << tempIconPath;
                faviconDb.close();
                QSqlDatabase::removeDatabase(faviconConnectionName);
                return tempIconPath;
            }
        }
        } else {
            qDebug() << "No favicon data found for exact URL:" << url;
        }
    } else {
        qDebug() << "Favicon query failed:" << faviconQuery.lastError().text();
        // Try fallback query with host matching
        QUrl parsedUrl(url);
        QString host = parsedUrl.host();
        if (!host.isEmpty()) {
            QString fallbackQuery = "SELECT i.data FROM moz_icons i "
                                   "JOIN moz_icons_to_pages itp ON i.id = itp.icon_id "
                                   "JOIN moz_pages_w_icons p ON itp.page_id = p.id "
                                   "WHERE p.page_url LIKE ? AND i.data IS NOT NULL "
                                   "ORDER BY i.width DESC LIMIT 1";
            
            QSqlQuery fallbackFaviconQuery(faviconDb);
            fallbackFaviconQuery.prepare(fallbackQuery);
            fallbackFaviconQuery.addBindValue("%" + host + "%");
            
            if (fallbackFaviconQuery.exec() && fallbackFaviconQuery.next()) {
                QByteArray iconData = fallbackFaviconQuery.value(0).toByteArray();
                if (!iconData.isEmpty()) {
                    QString tempIconPath = QDir::temp().filePath(QString("zen_favicon_%1_%2.ico")
                                                                .arg(QDateTime::currentMSecsSinceEpoch())
                                                                .arg(qHash(url)));
                    QFile iconFile(tempIconPath);
                    if (iconFile.open(QIODevice::WriteOnly)) {
                        iconFile.write(iconData);
                        iconFile.close();
                        qDebug() << "Created fallback favicon file:" << tempIconPath;
                        faviconDb.close();
                        QSqlDatabase::removeDatabase(faviconConnectionName);
                        return tempIconPath;
                    }
                }
            }
        }
    }
    
    faviconDb.close();
    QSqlDatabase::removeDatabase(faviconConnectionName);
    return QString();
}

K_PLUGIN_CLASS_WITH_JSON(ZenBookmarkRunner, "firefoxprofilerunner.json")

#include "firefoxprofilerunner.moc"
#include "moc_firefoxprofilerunner.cpp"

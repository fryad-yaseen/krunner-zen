#pragma once

// Removed profile includes as not needed for zen-bookmark
#include <KRunner/AbstractRunner>
// Removed QFileSystemWatcher as not needed
#include <QRegularExpression>
#include <QString>
#include <krunner_version.h>

struct Bookmark {
    QString title;
    QString url;
};

#if KRUNNER_VERSION_MAJOR == 5
using namespace Plasma;
#include <QAction>
#else
using namespace KRunner;
#include <KRunner/Action>
#endif

class ZenBookmarkRunner : public AbstractRunner
{
    Q_OBJECT

public:
    ZenBookmarkRunner(QObject *parent, const KPluginMetaData &data, const QVariantList &args);

    // NOTE: Prefixes need to be included in filterRegex.
    QLatin1String shortPrefix = QLatin1String("b");
    QLatin1String mediumPrefix = QLatin1String("bookmark");
    const QRegularExpression filterRegex = QRegularExpression(R"(^(?:b|bookmark\w*)(?: (.+))$)", QRegularExpression::CaseInsensitiveOption);
    // Removed private window flag regex as not needed for bookmarks

    QString zenBookmarksPath;
    QString zenIcon;
// Removed matchActions as not needed for zen-bookmark

    QList<QueryMatch> createBookmarkMatches(const QString &filter);
    QueryMatch createMatch(const QString &text, const QMap<QString, QVariant> &data, float relevance);

public: // AbstractRunner API
    void reloadConfiguration() override;
    void match(RunnerContext &context) override;
    void run(const RunnerContext &context, const QueryMatch &match) override;
};

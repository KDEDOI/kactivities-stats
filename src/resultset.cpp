/*
 *   Copyright (C) 2015, 2016 Ivan Cukic <ivan.cukic(at)kde.org>
 *
 *   This library is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU Lesser General Public
 *   License as published by the Free Software Foundation; either
 *   version 2.1 of the License, or (at your option) version 3, or any
 *   later version accepted by the membership of KDE e.V. (or its
 *   successor approved by the membership of KDE e.V.), which shall
 *   act as a proxy defined in Section 6 of version 3 of the license.
 *
 *   This library is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *   Lesser General Public License for more details.
 *
 *   You should have received a copy of the GNU Lesser General Public
 *   License along with this library.
 *   If not, see <http://www.gnu.org/licenses/>.
 */

#include "resultset.h"

// Qt
#include <QSqlQuery>
#include <QSqlError>
#include <QCoreApplication>

// Local
#include <common/database/Database.h>
#include <utils/debug_and_return.h>
#include <utils/qsqlquery_iterator.h>

// Boost and STL
#include <boost/range/algorithm/transform.hpp>
#include <iterator>
#include <functional>
#include <mutex>

// KActivities
#include "activitiessync_p.h"

#define DEBUG_QUERIES 0

namespace KActivities {
namespace Stats {

using namespace Terms;

class ResultSet_ResultPrivate {
public:
    QString resource;
    QString title;
    QString mimetype;
    double  score;
    uint    lastUpdate;
    uint    firstUpdate;
    ResultSet::Result::LinkStatus linkStatus;
    QStringList linkedActivities;

};

ResultSet::Result::Result()
    : d(new ResultSet_ResultPrivate())
{
}

ResultSet::Result::Result(Result &&result)
    : d(result.d)
{
    result.d = nullptr;
}

ResultSet::Result::Result(const Result &result)
    : d(new ResultSet_ResultPrivate(*result.d))
{
}

ResultSet::Result &ResultSet::Result::operator=(Result result)
{
    std::swap(d, result.d);

    return *this;
}

ResultSet::Result::~Result()
{
    delete d;
}

#define CREATE_GETTER_AND_SETTER(Type, Name, Set)                              \
    Type ResultSet::Result::Name() const                                       \
    {                                                                          \
        return d->Name;                                                        \
    }                                                                          \
                                                                               \
    void ResultSet::Result::Set(Type Name)                                     \
    {                                                                          \
        d->Name = Name;                                                        \
    }

CREATE_GETTER_AND_SETTER(QString, resource, setResource)
CREATE_GETTER_AND_SETTER(QString, title, setTitle)
CREATE_GETTER_AND_SETTER(QString, mimetype, setMimetype)
CREATE_GETTER_AND_SETTER(double, score, setScore)
CREATE_GETTER_AND_SETTER(uint, lastUpdate, setLastUpdate)
CREATE_GETTER_AND_SETTER(uint, firstUpdate, setFirstUpdate)
CREATE_GETTER_AND_SETTER(ResultSet::Result::LinkStatus, linkStatus, setLinkStatus)
CREATE_GETTER_AND_SETTER(QStringList, linkedActivities, setLinkedActivities)

#undef CREATE_GETTER_AND_SETTER


class ResultSetPrivate {
public:
    Common::Database::Ptr database;
    QSqlQuery query;
    Query queryDefinition;

    mutable ActivitiesSync::ConsumerPtr activities;

    void initQuery()
    {
        if (!database || query.isActive()) {
            return;
        }

        auto selection = queryDefinition.selection();

        query = database->execQuery(replaceQueryParameters(
                selection == LinkedResources ? linkedResourcesQuery()
              : selection == UsedResources   ? usedResourcesQuery()
              : selection == AllResources    ? allResourcesQuery()
              : QString()));

        if (query.lastError().isValid()) {
            qWarning() << "[Error at ResultSetPrivate::initQuery]: " << query.lastError();
        }
    }

    QString agentClause(const QString &agent) const
    {
        if (agent == QLatin1String(":any")) return QStringLiteral("1");

        return "agent = '" + (
                agent == QLatin1String(":current") ? QCoreApplication::instance()->applicationName() :
                                      agent
            ) + "'";
    }

    QString activityClause(const QString &activity) const
    {
        if (activity == QLatin1String(":any")) return QStringLiteral("1");

        return "activity = '" + (
                activity == QLatin1String(":current") ? ActivitiesSync::currentActivity(activities) :
                                         activity
            ) + "'";
    }

    inline QString starPattern(const QString &pattern) const
    {
        return Common::parseStarPattern(pattern, QStringLiteral("%"), [] (QString str) {
            return str.replace(QLatin1String("%"), QLatin1String("\\%")).replace(QLatin1String("_"), QLatin1String("\\_"));
        });
    }

    QString urlFilterClause(const QString &urlFilter) const
    {
        if (urlFilter == QLatin1String("*")) return QStringLiteral("1");

        return "resource LIKE '" + Common::starPatternToLike(urlFilter) + "' ESCAPE '\\'";
    }

    QString mimetypeClause(const QString &mimetype) const
    {
        if (mimetype == QLatin1String(":any") || mimetype == QLatin1String("*")) return QStringLiteral("1");

        return "mimetype LIKE '" + Common::starPatternToLike(mimetype) + "' ESCAPE '\\'";
    }

    /**
     * Transforms the input list's elements with the f member method,
     * and returns the resulting list
     */
    template <typename F>
    inline
    QStringList transformedList(const QStringList &input, F f) const
    {
        using namespace std::placeholders;

        QStringList result;
        boost::transform(input,
                         std::back_inserter(result),
                         std::bind(f, this, _1));

        return result;
    }

    QString limitOffsetSuffix() const
    {
        QString result;

        const int limit = queryDefinition.limit();
        if (limit > 0) {
            result += " LIMIT " + QString::number(limit);

            const int offset = queryDefinition.offset();
            if (offset > 0) {
                result += " OFFSET " + QString::number(offset);
            }
        }

        return result;
    }

    inline QString replaceQueryParameters(const QString &_query) const
    {
        // ORDER BY column
        auto ordering = queryDefinition.ordering();
        QString orderingColumn = QStringLiteral("linkStatus DESC, ") + (
                ordering == HighScoredFirst      ? QStringLiteral("score DESC,")
              : ordering == RecentlyCreatedFirst ? QStringLiteral("firstUpdate DESC,")
              : ordering == RecentlyUsedFirst    ? QStringLiteral("lastUpdate DESC,")
              : ordering == OrderByTitle         ? QStringLiteral("title ASC,")
              : QString()
            );


        // WHERE clause for filtering on agents
        QStringList agentsFilter = transformedList(
                queryDefinition.agents(), &ResultSetPrivate::agentClause);

        // WHERE clause for filtering on activities
        QStringList activitiesFilter = transformedList(
                queryDefinition.activities(), &ResultSetPrivate::activityClause);

        // WHERE clause for filtering on resource URLs
        QStringList urlFilter = transformedList(
                queryDefinition.urlFilters(), &ResultSetPrivate::urlFilterClause);

        // WHERE clause for filtering on resource mime
        QStringList mimetypeFilter = transformedList(
                queryDefinition.types(), &ResultSetPrivate::mimetypeClause);

        auto queryString = _query;

        queryString.replace("ORDER_BY_CLAUSE", "ORDER BY $orderingColumn resource ASC")
                   .replace("LIMIT_CLAUSE", limitOffsetSuffix());

        return kamd::utils::debug_and_return(DEBUG_QUERIES, "Query: ",
            queryString
                .replace(QLatin1String("$orderingColumn"), orderingColumn)
                .replace(QLatin1String("$agentsFilter"), agentsFilter.join(QStringLiteral(" OR ")))
                .replace(QLatin1String("$activitiesFilter"), activitiesFilter.join(QStringLiteral(" OR ")))
                .replace(QLatin1String("$urlFilter"), urlFilter.join(QStringLiteral(" OR ")))
                .replace(QLatin1String("$mimetypeFilter"), mimetypeFilter.join(QStringLiteral(" OR ")))
            );
    }

    static const QString &linkedResourcesQuery()
    {
        // TODO: We need to correct the scores based on the time that passed
        //       since the cache was last updated, although, for this query,
        //       scores are not that important.
        static const QString queryString =
            R"sql(
            SELECT
                rl.targettedResource as resource
              , SUM(rsc.cachedScore) as score
              , MIN(rsc.firstUpdate) as firstUpdate
              , MAX(rsc.lastUpdate)  as lastUpdate
              , rl.usedActivity      as activity
              , rl.initiatingAgent   as agent
              , COALESCE(ri.title, rl.targettedResource) as title
              , ri.mimetype as mimetype
              , 2 as linkStatus

            FROM
                ResourceLink rl
            LEFT JOIN
                ResourceScoreCache rsc
                ON rl.targettedResource = rsc.targettedResource
                AND rl.usedActivity     = rsc.usedActivity
                AND rl.initiatingAgent  = rsc.initiatingAgent
            LEFT JOIN
                ResourceInfo ri
                ON rl.targettedResource = ri.targettedResource

            WHERE
                ($agentsFilter)
                AND ($activitiesFilter)
                AND ($urlFilter)
                AND ($mimetypeFilter)

            GROUP BY resource, title

            ORDER_BY_CLAUSE
            LIMIT_CLAUSE
            )sql"
            ;

        return queryString;
    }

    static const QString &usedResourcesQuery()
    {
        // TODO: We need to correct the scores based on the time that passed
        //       since the cache was last updated
        static const QString queryString =
            R"sql(
            SELECT
                rsc.targettedResource as resource
              , SUM(rsc.cachedScore)  as score
              , MIN(rsc.firstUpdate)  as firstUpdate
              , MAX(rsc.lastUpdate)   as lastUpdate
              , rsc.usedActivity      as activity
              , rsc.initiatingAgent   as agent
              , COALESCE(ri.title, rsc.targettedResource) as title
              , ri.mimetype as mimetype
              , 1 as linkStatus

            FROM
                ResourceScoreCache rsc
            LEFT JOIN
                ResourceInfo ri
                ON rsc.targettedResource = ri.targettedResource

            WHERE
                ($agentsFilter)
                AND ($activitiesFilter)
                AND ($urlFilter)
                AND ($mimetypeFilter)

            GROUP BY resource, title

            ORDER_BY_CLAUSE
            LIMIT_CLAUSE
            )sql"
            ;

        return queryString;
    }

    static const QString &allResourcesQuery()
    {
        // TODO: We need to correct the scores based on the time that passed
        //       since the cache was last updated, although, for this query,
        //       scores are not that important.
        static const QString queryString =
            R"sql(
            WITH
                LinkedResourcesResults AS (
                    SELECT rl.targettedResource as resource
                         , rsc.cachedScore      as score
                         , rsc.firstUpdate      as firstUpdate
                         , rsc.lastUpdate       as lastUpdate
                         , rl.usedActivity      as activity
                         , rl.initiatingAgent   as agent
                         , 2 as linkStatus

                    FROM
                        ResourceLink rl

                    LEFT JOIN
                        ResourceScoreCache rsc
                        ON  rl.targettedResource = rsc.targettedResource
                        AND rl.usedActivity      = rsc.usedActivity
                        AND rl.initiatingAgent   = rsc.initiatingAgent

                    WHERE
                        ($agentsFilter)
                        AND ($activitiesFilter)
                        AND ($urlFilter)
                        AND ($mimetypeFilter)
                ),

                UsedResourcesResults AS (
                    SELECT rsc.targettedResource as resource
                         , rsc.cachedScore       as score
                         , rsc.firstUpdate       as firstUpdate
                         , rsc.lastUpdate        as lastUpdate
                         , rsc.usedActivity      as activity
                         , rsc.initiatingAgent   as agent
                         , 0 as linkStatus

                    FROM
                        ResourceScoreCache rsc

                    WHERE
                        ($agentsFilter)
                        AND ($activitiesFilter)
                        AND ($urlFilter)
                        AND ($mimetypeFilter)
                ),

                CollectedResults AS (
                    SELECT *
                    FROM LinkedResourcesResults

                    UNION

                    SELECT *
                    FROM UsedResourcesResults
                    WHERE resource NOT IN (SELECT resource FROM LinkedResourcesResults)
                )

                SELECT
                    resource
                  , SUM(score) as score
                  , MIN(firstUpdate) as firstUpdate
                  , MAX(lastUpdate) as lastUpdate
                  , activity
                  , agent
                  , COALESCE(ri.title, resource) as title
                  , ri.mimetype as mimetype
                  , linkStatus

                FROM CollectedResults cr

                LEFT JOIN
                    ResourceInfo ri
                    ON cr.resource = ri.targettedResource

                GROUP BY resource, title

                ORDER_BY_CLAUSE
                LIMIT_CLAUSE
            )sql"
            ;

        return queryString;
    }

    ResultSet::Result currentResult() const
    {
        ResultSet::Result result;

        if (!database || !query.isActive()) return result;

        result.setResource(query.value(QStringLiteral("resource")).toString());
        result.setTitle(query.value(QStringLiteral("title")).toString());
        result.setMimetype(query.value(QStringLiteral("mimetype")).toString());
        result.setScore(query.value(QStringLiteral("score")).toDouble());
        result.setLastUpdate(query.value(QStringLiteral("lastUpdate")).toInt());
        result.setFirstUpdate(query.value(QStringLiteral("firstUpdate")).toInt());


        result.setLinkStatus(
            (ResultSet::Result::LinkStatus)query.value(QStringLiteral("linkStatus")).toInt());

        auto linkedActivitiesQuery = database->createQuery();

        linkedActivitiesQuery.prepare(R"sql(
            SELECT usedActivity
            FROM   ResourceLink
            WHERE  targettedResource = :resource
            )sql");

        linkedActivitiesQuery.bindValue(":resource", result.resource());
        linkedActivitiesQuery.exec();

        QStringList linkedActivities;
        for (const auto &item: linkedActivitiesQuery) {
            linkedActivities << item[0].toString();
        }

        result.setLinkedActivities(linkedActivities);
        // qDebug() << result.resource() << "linked to activities" << result.linkedActivities();

        return result;
    }
};

ResultSet::ResultSet(Query queryDefinition)
    : d(new ResultSetPrivate())
{
    using namespace Common;

    d->database = Database::instance(Database::ResourcesDatabase, Database::ReadOnly);

    if (!(d->database)) {
        qWarning() << "KActivities ERROR: There is no database. This probably means "
                      "that you do not have the Activity Manager running, or that "
                      "something else is broken on your system. Recent documents and "
                      "alike will not work!";
    }

    d->queryDefinition = queryDefinition;

    d->initQuery();
}

ResultSet::ResultSet(ResultSet &&source)
    : d(nullptr)
{
    std::swap(d, source.d);
}

ResultSet::ResultSet(const ResultSet &source)
    : d(new ResultSetPrivate(*source.d))
{
}

ResultSet &ResultSet::operator= (ResultSet source)
{
    std::swap(d, source.d);
    return *this;
}

ResultSet::~ResultSet()
{
    delete d;
}

ResultSet::Result ResultSet::at(int index) const
{
    if (!d->query.isActive()) return Result();

    d->query.seek(index);

    return d->currentResult();
}

} // namespace Stats
} // namespace KActivities

#include "resultset_iterator.cpp"


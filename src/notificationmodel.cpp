// SPDX-FileCopyrightText: 2021 Carl Schwan <carlschwan@kde.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "notificationmodel.h"
#include "accountmodel.h"
#include "threadmodel.h"
#include <KLocalizedString>
#include <QtMath>

NotificationModel::NotificationModel(QObject *parent)
    : AbstractTimelineModel(parent)
{
    m_account = AccountManager::instance().selectedAccount();

    QObject::connect(&AccountManager::instance(), &AccountManager::accountSelected, this, [=](Account *account) {
        if (m_account == account) {
            return;
        }
        m_account = account;

        beginResetModel();
        m_notifications.clear();
        endResetModel();

        fillTimeline();
    });
    QObject::connect(&AccountManager::instance(), &AccountManager::invalidated, this, [=](Account *account) {
        if (m_account == account) {
            qDebug() << "Invalidating account" << account;

            beginResetModel();
            m_notifications.clear();
            endResetModel();

            fillTimeline();
        }
    });
    connect(this, &NotificationModel::excludeTypesChanged, this, [this] {
        beginResetModel();
        m_notifications.clear();
        endResetModel();

        fillTimeline();
    });

    fillTimeline();
}

QStringList NotificationModel::excludeTypes() const
{
    return m_excludeTypes;
}

void NotificationModel::setExcludesTypes(const QStringList &excludeTypes)
{
    if (m_excludeTypes == excludeTypes) {
        return;
    }

    m_excludeTypes = excludeTypes;
    Q_EMIT excludeTypesChanged();
}

void NotificationModel::fillTimeline(const QUrl &next)
{
    m_fetching = true;

    if (!m_account) {
        return;
    }
    QUrl uri;
    if (next.isEmpty()) {
        uri = QUrl::fromUserInput(m_account->instanceUri());
        uri.setPath(QStringLiteral("/api/v1/notifications"));
    } else {
        uri = next;
    }
    QUrlQuery urlQuery(uri);
    for (const auto &excludeType : m_excludeTypes) {
        urlQuery.addQueryItem("exclude_types[]", excludeType);
    }
    uri.setQuery(urlQuery);

    m_account->get(uri, true, [=](QNetworkReply *reply) {
        const auto data = reply->readAll();
        const auto doc = QJsonDocument::fromJson(data);

        if (m_loading) {
            m_loading = false;
            Q_EMIT loadingChanged();
        }

        if (!doc.isArray()) {
            m_account->errorOccured(i18n("Error occurred when fetching the latest notification."));
            return;
        }
        static QRegularExpression re("<(.*)>; rel=\"next\"");
        auto next = reply->rawHeader(QByteArrayLiteral("Link"));
        auto match = re.match(next);
        m_next = QUrl::fromUserInput(match.captured(1));

        QList<std::shared_ptr<Notification>> notifications;
        for (const auto &value : doc.array()) {
            QJsonObject obj = value.toObject();

            auto notification = std::make_shared<Notification>(m_account, obj);
            notifications.push_back(notification);
        }
        fetchedNotifications(notifications);
    });
}

void NotificationModel::fetchMore(const QModelIndex &parent)
{
    Q_UNUSED(parent);

    if (m_notifications.isEmpty() || !m_next.isValid()) {
        return;
    }

    fillTimeline(m_next);
}

bool NotificationModel::canFetchMore(const QModelIndex &parent) const
{
    Q_UNUSED(parent);

    // Todo detect when there is nothing left
    return !m_fetching;
}

void NotificationModel::fetchedNotifications(QList<std::shared_ptr<Notification>> notifications)
{
    m_fetching = false;

    if (notifications.isEmpty()) {
        return;
    }

    beginInsertRows({}, m_notifications.count(), m_notifications.count() + notifications.count() - 1);
    m_notifications.append(notifications);
    endInsertRows();
}

int NotificationModel::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent)

    return m_notifications.size();
}

// this is even more extremely cursed
std::shared_ptr<Notification> NotificationModel::internalData(const QModelIndex &index) const
{
    int row = index.row();
    return m_notifications[row];
}

QHash<int, QByteArray> NotificationModel::roleNames() const
{
    return {
        {Qt::DisplayRole, QByteArrayLiteral("display")},
        {AvatarRole, QByteArrayLiteral("avatar")},
        {AuthorDisplayNameRole, QByteArrayLiteral("authorDisplayName")},
        {PinnedRole, QByteArrayLiteral("pinned")},
        {AuthorIdRole, QByteArrayLiteral("authorId")},
        {PublishedAtRole, QByteArrayLiteral("publishedAt")},
        {RelativeTimeRole, QByteArrayLiteral("relativeTime")},
        {SensitiveRole, QByteArrayLiteral("sensitive")},
        {SpoilerTextRole, QByteArrayLiteral("spoilerText")},
        {RebloggedRole, QByteArrayLiteral("reblogged")},
        {WasRebloggedRole, QByteArrayLiteral("wasReblogged")},
        {RebloggedDisplayNameRole, QByteArrayLiteral("rebloggedDisplayName")},
        {RebloggedIdRole, QByteArrayLiteral("rebloggedId")},
        {AttachmentsRole, QByteArrayLiteral("attachments")},
        {ReblogsCountRole, QByteArrayLiteral("reblogsCount")},
        {RepliesCountRole, QByteArrayLiteral("repliesCount")},
        {FavoritedRole, QByteArrayLiteral("favorite")},
        {FavoritesCountRole, QByteArrayLiteral("favoritesCount")},
        {UrlRole, QByteArrayLiteral("url")},
        {ThreadModelRole, QByteArrayLiteral("threadModel")},
        {AccountModelRole, QByteArrayLiteral("accountModel")},
        {TypeRole, QByteArrayLiteral("type")},
        {CardRole, QByteArrayLiteral("card")},
        {ActorDisplayNameRole, QByteArrayLiteral("actorDisplayName")},
    };
}

QVariant NotificationModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid()) {
        return {};
    }
    int row = index.row();
    auto notification = m_notifications[row];
    auto post = notification->post();

    switch (role) {
    case TypeRole:
        return notification->type();
    case ActorDisplayNameRole:
        return notification->identity()->displayName();
    case Qt::DisplayRole:
        return post->m_content;
    case AvatarRole:
        return post->m_author_identity->m_avatarUrl;
    case AuthorDisplayNameRole:
        return post->m_author_identity->m_display_name;
    case AuthorIdRole:
        return post->m_author_identity->m_acct;
    case PublishedAtRole:
        return post->m_published_at;
    case WasRebloggedRole:
        return post->m_repeat || notification->type() == Notification::Repeat;
    case RebloggedDisplayNameRole:
        if (post->m_repeat_identity) {
            return post->m_repeat_identity->m_display_name;
        }
        if (notification->type() == Notification::Repeat) {
            return notification->identity()->displayName();
        }
        return {};
    case RebloggedIdRole:
        if (post->m_repeat_identity) {
            return post->m_repeat_identity->m_acct;
        }
        if (notification->type() == Notification::Repeat) {
            return notification->identity()->m_acct;
        }
        return {};
    case RebloggedRole:
        return post->m_isRepeated;
    case ReblogsCountRole:
        return post->m_repeatedCount;
    case FavoritedRole:
        return post->m_isFavorite;
    case PinnedRole:
        return post->m_pinned;
    case SensitiveRole:
        return post->m_isSensitive;
    case SpoilerTextRole:
        return post->m_subject;
    case AttachmentsRole:
        return QVariant::fromValue<QList<Attachment *>>(post->m_attachments);
    case ThreadModelRole:
        return QVariant::fromValue<QAbstractListModel *>(new ThreadModel(m_manager, post->m_post_id));
    case CardRole:
        if (post->card().has_value()) {
            return QVariant::fromValue<Card>(*post->card());
        }
        return false;
    case AccountModelRole:
        return QVariant::fromValue<QAbstractListModel *>(new AccountModel(m_manager, post->m_author_identity->m_id, post->m_author_identity->m_acct));
    case RelativeTimeRole: {
        const auto current = QDateTime::currentDateTime();
        auto secsTo = post->m_published_at.secsTo(current);
        if (secsTo < 60 * 60) {
            const auto hours = post->m_published_at.time().hour();
            const auto minutes = post->m_published_at.time().minute();
            return i18nc("hour:minute",
                         "%1:%2",
                         hours < 10 ? QChar('0') + QString::number(hours) : QString::number(hours),
                         minutes < 10 ? QChar('0') + QString::number(minutes) : QString::number(minutes));
        } else if (secsTo < 60 * 60 * 24) {
            return i18n("%1h", qCeil(secsTo / (60 * 60)));
        } else if (secsTo < 60 * 60 * 24 * 7) {
            return i18n("%1d", qCeil(secsTo / (60 * 60 * 24)));
        }
        return QLocale::system().toString(post->m_published_at.date(), QLocale::ShortFormat);
    }
    }

    return {};
}

void NotificationModel::actionReply(const QModelIndex &index)
{
    int row = index.row();
    auto p = m_notifications[row]->post();

    Q_EMIT wantReply(m_account, p, index);
}

void NotificationModel::actionMenu(const QModelIndex &index)
{
    int row = index.row();
    auto p = m_notifications[row]->post();

    Q_EMIT wantMenu(m_account, p, index);
}

void NotificationModel::actionFavorite(const QModelIndex &index)
{
    int row = index.row();
    auto p = m_notifications[row]->post();

    if (!p->m_isFavorite) {
        m_account->favorite(p);
        p->m_isFavorite = true;
    } else {
        m_account->unfavorite(p);
        p->m_isFavorite = false;
    }

    Q_EMIT dataChanged(index, index);
}

void NotificationModel::actionRepeat(const QModelIndex &index)
{
    int row = index.row();
    auto p = m_notifications[row]->post();

    if (!p->m_isRepeated) {
        m_account->repeat(p);
        p->m_isRepeated = true;
    } else {
        m_account->unrepeat(p);
        p->m_isRepeated = false;
    }

    Q_EMIT dataChanged(index, index);
}

void NotificationModel::actionVis(const QModelIndex &index)
{
    int row = index.row();
    auto p = m_notifications[row]->post();

    p->m_attachments_visible ^= true;

    Q_EMIT dataChanged(index, index);
}

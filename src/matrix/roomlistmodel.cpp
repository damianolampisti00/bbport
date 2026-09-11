#include "roomlistmodel.hpp"
#include "matrixapi.hpp"
#include "mediamanager.hpp"

#include <bb/cascades/ArrayDataModel>

#include <QNetworkReply>
#include <QUrl>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QTimer>

using namespace bb::cascades;

RoomListModel::RoomListModel(MatrixApi *api, MediaManager *media, QObject *parent) :
        QObject(parent),
        m_api(api),
        m_media(media),
        m_model(new ArrayDataModel(this)),
        m_totalUnreadCount(0),
        m_searchDebounce(new QTimer(this))
{
    connect(m_media, SIGNAL(thumbnailReady(QString,QString)), this, SLOT(onThumbnailReady(QString,QString)));
    m_hiddenRoomsFilePath = QDir::homePath() + "/hidden_rooms.txt";
    loadHiddenRooms();

    m_searchDebounce->setSingleShot(true);
    connect(m_searchDebounce, SIGNAL(timeout()), this, SLOT(rebuildVisible()));
}

RoomListModel::~RoomListModel()
{
}

QObject* RoomListModel::model() const
{
    return m_model;
}

QString RoomListModel::searchQuery() const
{
    return m_searchQuery;
}

int RoomListModel::totalUnreadCount() const
{
    return m_totalUnreadCount;
}

QString RoomListModel::pendingOpenRoomId() const
{
    return m_pendingOpenRoomId;
}

void RoomListModel::requestOpenRoom(const QString &roomId)
{
    m_pendingOpenRoomId = roomId;
    emit pendingOpenRoomIdChanged();
}

// Summed across every room including hidden ones, since hiding is only an
// inbox-declutter preference, not a "mark as read". Called whenever
// m_allRooms membership or an unreadCount within it can have changed.
void RoomListModel::recomputeTotalUnreadCount()
{
    int total = 0;
    for (int i = 0; i < m_allRooms.size(); ++i) {
        total += m_allRooms.at(i).value("unreadCount").toInt();
    }
    if (total == m_totalUnreadCount) return;
    m_totalUnreadCount = total;
    emit totalUnreadCountChanged();
}

void RoomListModel::setSearchQuery(const QString &query)
{
    if (m_searchQuery == query) return;
    m_searchQuery = query;
    emit searchQueryChanged();
    // Debounced: each keystroke restarts this single-shot timer rather than
    // rebuilding (a full re-sort + ArrayDataModel clear/append, redrawing
    // the whole ListView) immediately, so typing a query triggers one
    // rebuild shortly after the user pauses instead of one per character.
    m_searchDebounce->start(250);
}

int RoomListModel::indexOfRoom(const QString &roomId) const
{
    return m_indexByRoomId.value(roomId, -1);
}

QVariantMap RoomListModel::roomAt(int index) const
{
    if (index < 0 || index >= m_model->size()) return QVariantMap();
    return m_model->value(index).toMap();
}

void RoomListModel::loadHiddenRooms()
{
    QFile file(m_hiddenRoomsFilePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return;
    QTextStream in(&file);
    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (!line.isEmpty()) m_hiddenRoomIds.insert(line);
    }
}

void RoomListModel::saveHiddenRooms() const
{
    QFile file(m_hiddenRoomsFilePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return;
    QTextStream out(&file);
    foreach (const QString &roomId, m_hiddenRoomIds) {
        out << roomId << "\n";
    }
}

void RoomListModel::hideRoom(const QString &roomId)
{
    if (m_hiddenRoomIds.contains(roomId)) return;
    m_hiddenRoomIds.insert(roomId);
    saveHiddenRooms();
    rebuildVisible();
}

void RoomListModel::unhideRoom(const QString &roomId)
{
    if (!m_hiddenRoomIds.remove(roomId)) return;
    saveHiddenRooms();
    rebuildVisible();
}

bool RoomListModel::isRoomHidden(const QString &roomId) const
{
    return m_hiddenRoomIds.contains(roomId);
}

QVariantList RoomListModel::allRoomsForManagement() const
{
    QVariantList result;
    for (int i = 0; i < m_allRooms.size(); ++i) {
        QVariantMap item = m_allRooms.at(i);
        if (item.value("isInvite").toBool()) continue;
        item["hidden"] = m_hiddenRoomIds.contains(item.value("roomId").toString());
        result << item;
    }
    return result;
}

// Rebuilds the visible/sorted m_model projection from m_allRooms, applying
// the hidden-room and search-query filters (invites always bypass both --
// a fresh invite shouldn't silently disappear because of an old hide or an
// unrelated search in progress), then the same "invites first, then by
// lastTs descending" order the old resort() used.
void RoomListModel::rebuildVisible()
{
    QString q = m_searchQuery.trimmed().toLower();

    QList<QVariantMap> items;
    for (int i = 0; i < m_allRooms.size(); ++i) {
        const QVariantMap &item = m_allRooms.at(i);
        bool isInvite = item.value("isInvite").toBool();
        if (!isInvite) {
            if (m_hiddenRoomIds.contains(item.value("roomId").toString())) continue;
            if (!q.isEmpty()) {
                QString name = item.value("name").toString().toLower();
                QString lastBody = item.value("lastBody").toString().toLower();
                if (!name.contains(q) && !lastBody.contains(q)) continue;
            }
        }
        items << item;
    }

    for (int i = 1; i < items.size(); ++i) {
        QVariantMap key = items.at(i);
        int j = i - 1;
        while (j >= 0) {
            const QVariantMap &other = items.at(j);
            bool keyIsInvite = key.value("isInvite").toBool();
            bool otherIsInvite = other.value("isInvite").toBool();
            bool shouldMoveUp;
            if (keyIsInvite != otherIsInvite) {
                shouldMoveUp = keyIsInvite;
            } else {
                shouldMoveUp = key.value("lastTs").toLongLong() > other.value("lastTs").toLongLong();
            }
            if (!shouldMoveUp) break;
            items[j + 1] = items[j];
            j--;
        }
        items[j + 1] = key;
    }

    m_model->clear();
    m_indexByRoomId.clear();
    QVariantList asVariantList;
    for (int i = 0; i < items.size(); ++i) asVariantList << items.at(i);
    m_model->append(asVariantList);
    for (int i = 0; i < items.size(); ++i) {
        m_indexByRoomId[items.at(i).value("roomId").toString()] = i;
    }
}

void RoomListModel::upsertRoom(const QString &roomId, const QVariantMap &summary)
{
    QVariantMap item = summary;
    item["roomId"] = roomId;
    item["isInvite"] = false;

    QString avatarMxc = item.value("avatarMxc").toString();
    if (!avatarMxc.isEmpty()) {
        item["avatarLocalUrl"] = m_media->resolveThumbnail(avatarMxc);
    }

    int allIdx = m_allIndexByRoomId.value(roomId, -1);
    if (allIdx >= 0) {
        const QVariantMap &previous = m_allRooms.at(allIdx);
        // Preserve a live typing flag across summary refreshes.
        item["isTyping"] = previous.value("isTyping").toBool();
        // A resumed/incremental sync's summary can legitimately have no
        // avatarMxc for a room that already had one resolved (state that
        // didn't change isn't re-sent) -- without this, that update would
        // silently blank out an avatar already showing in the list.
        if (avatarMxc.isEmpty()) {
            item["avatarMxc"] = previous.value("avatarMxc").toString();
            item["avatarLocalUrl"] = previous.value("avatarLocalUrl").toString();
        }
        m_allRooms[allIdx] = item;
    } else {
        m_allIndexByRoomId[roomId] = m_allRooms.size();
        m_allRooms << item;
    }
    recomputeTotalUnreadCount();
    rebuildVisible();
}

void RoomListModel::setTyping(const QString &roomId, const QStringList &userIds)
{
    bool typing = !userIds.isEmpty();

    int allIdx = m_allIndexByRoomId.value(roomId, -1);
    if (allIdx >= 0) {
        QVariantMap item = m_allRooms.at(allIdx);
        item["isTyping"] = typing;
        m_allRooms[allIdx] = item;
    }

    // Typing doesn't affect sort order or filtering, so patch the visible
    // model directly rather than paying for a full rebuild on every event.
    int idx = m_indexByRoomId.value(roomId, -1);
    if (idx < 0) return;
    QVariantMap item = m_model->value(idx).toMap();
    item["isTyping"] = typing;
    m_model->replace(idx, item);
}

void RoomListModel::addInvite(const QString &roomId, const QString &inviterId, const QString &roomName)
{
    if (m_allIndexByRoomId.contains(roomId)) return;
    QVariantMap item;
    item["roomId"] = roomId;
    item["name"] = roomName;
    item["isInvite"] = true;
    item["inviterId"] = inviterId;
    item["lastTs"] = (qlonglong)0;
    item["unreadCount"] = 0;
    m_allIndexByRoomId[roomId] = m_allRooms.size();
    m_allRooms << item;
    recomputeTotalUnreadCount();
    rebuildVisible();
}

void RoomListModel::removeRoom(const QString &roomId)
{
    int allIdx = m_allIndexByRoomId.value(roomId, -1);
    if (allIdx < 0) return;
    m_allRooms.removeAt(allIdx);
    m_allIndexByRoomId.clear();
    for (int i = 0; i < m_allRooms.size(); ++i) {
        m_allIndexByRoomId[m_allRooms.at(i).value("roomId").toString()] = i;
    }
    recomputeTotalUnreadCount();
    rebuildVisible();
}

void RoomListModel::acceptInvite(const QString &roomId)
{
    QString path = QString("/join/%1").arg(QString(QUrl::toPercentEncoding(roomId)));
    QNetworkReply *reply = m_api->apiPost(path, QVariantMap());
    connect(reply, SIGNAL(finished()), this, SLOT(onJoinReplyFinished()));
    // The room will flip from "invite" to "join" on the next /sync response,
    // which upsertRoom() will then reconcile with a real summary.
    removeRoom(roomId);
}

void RoomListModel::declineInvite(const QString &roomId)
{
    QString path = QString("/rooms/%1/leave").arg(QString(QUrl::toPercentEncoding(roomId)));
    QNetworkReply *reply = m_api->apiPost(path, QVariantMap());
    connect(reply, SIGNAL(finished()), reply, SLOT(deleteLater()));
    removeRoom(roomId);
}

void RoomListModel::onJoinReplyFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (reply) reply->deleteLater();
}

void RoomListModel::onThumbnailReady(const QString &mxcUri, const QString &localFileUrl)
{
    for (int i = 0; i < m_allRooms.size(); ++i) {
        if (m_allRooms.at(i).value("avatarMxc").toString() == mxcUri) {
            QVariantMap item = m_allRooms.at(i);
            item["avatarLocalUrl"] = localFileUrl;
            m_allRooms[i] = item;
        }
    }
    for (int i = 0; i < m_model->size(); ++i) {
        QVariantMap item = m_model->value(i).toMap();
        if (item.value("avatarMxc").toString() == mxcUri) {
            item["avatarLocalUrl"] = localFileUrl;
            m_model->replace(i, item);
        }
    }
}

/*
 * Copyright (c) 2011-2015 BlackBerry Limited.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "applicationui.hpp"

#include "matrix/matrixapi.hpp"
#include "matrix/keybackupmanager.hpp"
#include "matrix/olmcryptomanager.hpp"
#include "matrix/syncengine.hpp"
#include "matrix/timelinestore.hpp"
#include "matrix/mediamanager.hpp"
#include "matrix/roomlistmodel.hpp"
#include "matrix/messagelistmodel.hpp"
#include "matrix/nativevideoplayer.hpp"
#include "matrix/notificationmanager.hpp"

#include <QtDeclarative/qdeclarative.h>
#include <QFile>
#include <QTextStream>

#include <bb/cascades/Application>
#include <bb/cascades/QmlDocument>
#include <bb/cascades/AbstractPane>
#include <bb/cascades/LocaleHandler>
#include <bb/cascades/SceneCover>
#include <bb/cascades/Container>
#include <bb/system/InvokeManager>
#include <bb/system/InvokeRequest>

using namespace bb::cascades;

ApplicationUI::ApplicationUI() :
        QObject()
{
    // --- Matrix backend wiring ---
    m_matrixApi = new MatrixApi(this);
    m_keyBackupManager = new KeyBackupManager(m_matrixApi, this);
    m_olmCryptoManager = new OlmCryptoManager(m_matrixApi, m_keyBackupManager, this);
    m_matrixApi->setPreferredDeviceId(m_olmCryptoManager->deviceId());
    m_syncEngine = new SyncEngine(m_matrixApi, m_keyBackupManager, this);
    m_timelineStore = new TimelineStore(this);
    m_mediaManager = new MediaManager(m_matrixApi, this);
    m_roomListModel = new RoomListModel(m_matrixApi, m_mediaManager, this);
    m_messageListModel = new MessageListModel(m_matrixApi, m_timelineStore, m_mediaManager, m_keyBackupManager, m_olmCryptoManager, this);
    m_notificationManager = new NotificationManager(m_matrixApi, m_messageListModel, m_roomListModel, m_syncEngine, this);

    connect(m_matrixApi, SIGNAL(loginSucceeded()), m_syncEngine, SLOT(start()));
    connect(m_matrixApi, SIGNAL(loginSucceeded()), m_olmCryptoManager, SLOT(start()));

    connect(m_syncEngine, SIGNAL(roomUpdated(QString,QVariantMap)), m_roomListModel, SLOT(upsertRoom(QString,QVariantMap)));
    connect(m_syncEngine, SIGNAL(typingUpdated(QString,QStringList)), m_roomListModel, SLOT(setTyping(QString,QStringList)));
    connect(m_syncEngine, SIGNAL(inviteReceived(QString,QString,QString)), m_roomListModel, SLOT(addInvite(QString,QString,QString)));

    connect(m_syncEngine, SIGNAL(timelineEvent(QString,QVariantMap)), m_timelineStore, SLOT(onTimelineEvent(QString,QVariantMap)));
    connect(m_syncEngine, SIGNAL(timelineEventUpdated(QString,QVariantMap)), m_timelineStore, SLOT(onTimelineEventUpdated(QString,QVariantMap)));
    connect(m_syncEngine, SIGNAL(typingUpdated(QString,QStringList)), m_timelineStore, SLOT(onTypingUpdated(QString,QStringList)));
    connect(m_syncEngine, SIGNAL(receiptUpdated(QString,QString,QStringList)), m_timelineStore, SLOT(onReceiptUpdated(QString,QString,QStringList)));
    connect(m_syncEngine, SIGNAL(reactionAdded(QString,QString,QString,QString,QString)), m_timelineStore, SLOT(onReactionAdded(QString,QString,QString,QString,QString)));
    connect(m_syncEngine, SIGNAL(roomHistoryAnchor(QString,QString)), m_timelineStore, SLOT(onRoomHistoryAnchor(QString,QString)));
    connect(m_syncEngine, SIGNAL(eventRedacted(QString,QString)), m_timelineStore, SLOT(onEventRedacted(QString,QString)));

    connect(m_syncEngine, SIGNAL(timelineEvent(QString,QVariantMap)), m_notificationManager, SLOT(onTimelineEvent(QString,QVariantMap)));

    connect(m_syncEngine, SIGNAL(roomUpdated(QString,QVariantMap)), m_olmCryptoManager, SLOT(onRoomUpdated(QString,QVariantMap)));
    connect(m_syncEngine, SIGNAL(toDeviceEvent(QVariantMap)), m_olmCryptoManager, SLOT(handleToDeviceEvent(QVariantMap)));

    // prepare the localization
    m_pTranslator = new QTranslator(this);
    m_pLocaleHandler = new LocaleHandler(this);

    bool res = QObject::connect(m_pLocaleHandler, SIGNAL(systemLanguageChanged()), this, SLOT(onSystemLanguageChanged()));
    // This is only available in Debug builds
    Q_ASSERT(res);
    // Since the variable is not used in the app, this is added to avoid a
    // compiler warning
    Q_UNUSED(res);

    // initial load
    onSystemLanguageChanged();

    // bb::multimedia::MediaPlayer + ForeignWindowControl (the documented
    // Cascades way to play video) binds correctly on-device (windowAttached,
    // videoDimensions all fire) but renders solid black regardless of
    // codec/resolution/config -- confirmed via a real device A/B test that a
    // bare mm-renderer + Screen sample shows video fine on the same phone,
    // isolating the bug to Cascades' own MediaPlayer wrapper specifically.
    // NativeVideoPlayer talks to mm-renderer directly instead.
    qmlRegisterType<NativeVideoPlayer>("it.bbport", 1, 0, "NativeVideoPlayer");

    // Create scene document from main.qml asset, the parent is set
    // to ensure the document gets destroyed properly at shut down.
    QmlDocument *qml = QmlDocument::create("asset:///main.qml").parent(this);

    qml->setContextProperty("matrixApi", m_matrixApi);
    qml->setContextProperty("keyBackupManager", m_keyBackupManager);
    qml->setContextProperty("olmCryptoManager", m_olmCryptoManager);
    qml->setContextProperty("syncEngine", m_syncEngine);
    qml->setContextProperty("mediaManager", m_mediaManager);
    qml->setContextProperty("roomListModel", m_roomListModel);
    qml->setContextProperty("messageListModel", m_messageListModel);

    // Create root object for the UI
    AbstractPane *root = qml->createRootObject<AbstractPane>();

    if (qml->hasErrors() || !root) {
        // A QML parse/binding error here previously went straight into
        // Application::instance()->setScene(0) (or a half-built root) with
        // no diagnostic at all -- on a device with no signed debug token
        // there's no other way to see what broke. Written to shared/misc,
        // same place/convention as every other on-device diagnostic log
        // this project uses (retrievable via Term49 `cat`).
        QFile errFile("/accounts/1000/shared/misc/bbport_qml_load_error.txt");
        if (errFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            QTextStream out(&errFile);
            out << "main.qml failed to load. hasErrors=" << qml->hasErrors()
                << " root=" << (root ? "non-null" : "null") << "\n";
        }
        if (!root) return;
    }

    // Set created root object as the application scene
    Application::instance()->setScene(root);

    // Active Frame (the multitasking/"open apps" preview shown when the app
    // is minimized): a small always-live QML view showing the total unread
    // message count, bound straight to roomListModel.totalUnreadCount so it
    // stays current with no manual refresh plumbing needed.
    QmlDocument *coverQml = QmlDocument::create("asset:///cover.qml").parent(this);
    coverQml->setContextProperty("roomListModel", m_roomListModel);
    Container *coverContent = coverQml->createRootObject<Container>();
    m_cover = new SceneCover(this);
    m_cover->setContent(coverContent);
    Application::instance()->setCover(m_cover);

    // Tapping a notification (see NotificationManager) re-invokes this app
    // with the roomId as payload; invoked() fires the same way whether the
    // app was already running or this is the launch that started it, so one
    // connection covers both cases.
    m_invokeManager = new bb::system::InvokeManager(this);
    connect(m_invokeManager, SIGNAL(invoked(bb::system::InvokeRequest)), this, SLOT(onInvoked(bb::system::InvokeRequest)));
}

void ApplicationUI::onInvoked(const bb::system::InvokeRequest &request)
{
    QString roomId = QString::fromUtf8(request.data());
    if (roomId.isEmpty()) return;
    m_roomListModel->requestOpenRoom(roomId);
}

void ApplicationUI::onSystemLanguageChanged()
{
    QCoreApplication::instance()->removeTranslator(m_pTranslator);
    // Initiate, load and install the application translation files.
    QString locale_string = QLocale().name();
    QString file_name = QString("BBport_%1").arg(locale_string);
    if (m_pTranslator->load(file_name, "app/native/qm")) {
        QCoreApplication::instance()->installTranslator(m_pTranslator);
    }
}

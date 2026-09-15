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

#ifndef ApplicationUI_HPP_
#define ApplicationUI_HPP_

#include <QObject>

namespace bb
{
    namespace cascades
    {
        class LocaleHandler;
        class SceneCover;
    }
    namespace system
    {
        class InvokeManager;
        class InvokeRequest;
    }
}

class QTranslator;
class MatrixApi;
class SyncEngine;
class TimelineStore;
class MediaManager;
class RoomListModel;
class MessageListModel;
class KeyBackupManager;
class OlmCryptoManager;
class NotificationManager;

/*!
 * @brief Application UI object
 *
 * Use this object to create and init app UI, to create context objects, to register the new meta types etc.
 */
class ApplicationUI : public QObject
{
    Q_OBJECT
public:
    ApplicationUI();
    virtual ~ApplicationUI() {}
private slots:
    void onSystemLanguageChanged();
    // Fires when the app is invoked (cold-launched or already running) via
    // the InvokeRequest NotificationManager attaches to each notification
    // -- extracts the roomId payload and hands it to RoomListModel so the
    // QML side (watching pendingOpenRoomId) can push that conversation open.
    void onInvoked(const bb::system::InvokeRequest &request);
    // Logs the outcome of registerTimer() (see the constructor) -- the only
    // way to see on a real device whether the recurring headless-wakeup
    // timer actually registered, since registerTimer() is asynchronous and
    // this project has no console attached to a standalone install.
    void onHeadlessTimerRegistered();
private:
    QTranslator* m_pTranslator;
    bb::cascades::LocaleHandler* m_pLocaleHandler;

    MatrixApi *m_matrixApi;
    KeyBackupManager *m_keyBackupManager;
    OlmCryptoManager *m_olmCryptoManager;
    SyncEngine *m_syncEngine;
    TimelineStore *m_timelineStore;
    MediaManager *m_mediaManager;
    RoomListModel *m_roomListModel;
    MessageListModel *m_messageListModel;
    NotificationManager *m_notificationManager;
    bb::cascades::SceneCover *m_cover;
    bb::system::InvokeManager *m_invokeManager;
};

#endif /* ApplicationUI_HPP_ */

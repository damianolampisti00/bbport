APP_NAME = BBport

CONFIG += qt warn_on cascades10
QT += network
LIBS += -lbbdata

include(config.pri)

# --- libolm (Megolm/Olm E2EE decryption via Matrix Secure Key Backup) ---
INCLUDEPATH += $$quote($$_PRO_FILE_PWD_/third_party/olm/include)

device {
    LIBS += $$quote($$_PRO_FILE_PWD_/third_party/olm/lib/armv7/libolm.a)
}
simulator {
    LIBS += $$quote($$_PRO_FILE_PWD_/third_party/olm/lib/x86/libolm.a)
}

# --- libopus (decodes OGG/Opus voice notes, e.g. from WhatsApp via Beeper,
# which BB10's own MediaPlayer can't play at all -- no Opus codec support on
# this OS) ---
INCLUDEPATH += $$quote($$_PRO_FILE_PWD_/third_party/opus/include)

device {
    LIBS += $$quote($$_PRO_FILE_PWD_/third_party/opus/lib/armv7/libopus.a)
}
simulator {
    LIBS += $$quote($$_PRO_FILE_PWD_/third_party/opus/lib/x86/libopus.a)
}

# --- mbedTLS (native TLS 1.2 for MatrixApi, device only -- see
# src/matrix/tlsnetworkreply.cpp. BB10's system OpenSSL is TLS 1.0-only and
# Qt 4.8's QSslSocket can't be pointed at a newer one, so real homeservers
# are otherwise unreachable without an external TLS-bridging proxy) ---
device {
    DEFINES += BBPORT_HAVE_NATIVE_TLS
    INCLUDEPATH += $$quote($$_PRO_FILE_PWD_/third_party/mbedtls/include)
    LIBS += $$quote($$_PRO_FILE_PWD_/third_party/mbedtls/lib/armv7/libmbedtls_all.a)
    OTHER_FILES += $$quote($$_PRO_FILE_PWD_/assets/cacert.pem)
}

# --- mm-renderer client (NativeVideoPlayer) -- talks to mm-renderer
# directly, bypassing bb::multimedia::MediaPlayer, which renders video as
# solid black on-device for reasons isolated to Cascades' own wrapper (a
# bare mm-renderer/Screen sample plays video fine on the same phone). Part
# of the standard NDK target lib set, not vendored.
LIBS += -lmmrndclient -lstrm

# --- bb::platform::Notification (NotificationManager) ---
LIBS += -lbbplatform

# --- bb::system::InvokeManager/InvokeRequest (tap-a-notification-to-open-
# the-right-chat, ApplicationUI + NotificationManager) ---
LIBS += -lbbsystem

# base58.cpp/hpp and keybackupmanager.cpp/hpp are auto-registered by the IDE
# into config.pri's config_pri_source_group1 (it scans src/ on build) --
# listing them here too would duplicate them on the link line.

APP_NAME = BBportHeadless

# No cascades10 CONFIG and no QtDeclarative -- this is a plain headless
# background process (bb::Application, not bb::cascades::Application), not
# a UI app, so it needs none of Cascades' QML bootstrap. Matches the
# structure of BlackBerry's own official headlesserviceui/headlesservice
# sample (blackberry/Cascades-Samples on GitHub) -- fetched and inspected
# this session since BlackBerry's own docs site for this is now dead, same
# as the debug-token signing service.
CONFIG += qt warn_on
QT += network
LIBS += -lbb -lbbdata -lbbsystem -lbbplatform -lsocket -lm

INCLUDEPATH += $$quote($$_PRO_FILE_PWD_/src) $$quote($$_PRO_FILE_PWD_/../src/matrix)

# Same native-TLS story as BBport.pro itself (see that file's own comment):
# BB10's system OpenSSL only speaks TLS 1.0, which real homeservers reject,
# so MatrixApi needs mbedTLS here too -- device only, matching BBport.pro
# (this sub-project has never been built for Simulator).
device {
    DEFINES += BBPORT_HAVE_NATIVE_TLS
    INCLUDEPATH += $$quote($$_PRO_FILE_PWD_/../third_party/mbedtls/include)
    LIBS += $$quote($$_PRO_FILE_PWD_/../third_party/mbedtls/lib/armv7/libmbedtls_all.a)
    OTHER_FILES += $$quote($$_PRO_FILE_PWD_/../assets/cacert.pem)
}

# libolm (Megolm/Olm E2EE), same static lib BBport.pro itself links --
# OlmCryptoManager/KeyBackupManager need it here too so headless can decrypt
# incoming messages for a Hub notification preview the same way the
# foreground app does.
INCLUDEPATH += $$quote($$_PRO_FILE_PWD_/../third_party/olm/include)
device {
    LIBS += $$quote($$_PRO_FILE_PWD_/../third_party/olm/lib/armv7/libolm.a)
}

SOURCES += \
    $$quote($$_PRO_FILE_PWD_/src/headless_main.cpp) \
    $$quote($$_PRO_FILE_PWD_/src/applicationheadless.cpp) \
    $$quote($$_PRO_FILE_PWD_/../src/matrix/matrixapi.cpp) \
    $$quote($$_PRO_FILE_PWD_/../src/matrix/keybackupmanager.cpp) \
    $$quote($$_PRO_FILE_PWD_/../src/matrix/olmcryptomanager.cpp) \
    $$quote($$_PRO_FILE_PWD_/../src/matrix/syncengine.cpp) \
    $$quote($$_PRO_FILE_PWD_/../src/matrix/notificationmanager.cpp) \
    $$quote($$_PRO_FILE_PWD_/../src/matrix/base58.cpp) \
    $$quote($$_PRO_FILE_PWD_/../src/matrix/tlsnetworkreply.cpp) \
    $$quote($$_PRO_FILE_PWD_/../src/matrix/tlsnetworkaccessmanager.cpp)

HEADERS += \
    $$quote($$_PRO_FILE_PWD_/src/applicationheadless.hpp) \
    $$quote($$_PRO_FILE_PWD_/../src/matrix/matrixapi.hpp) \
    $$quote($$_PRO_FILE_PWD_/../src/matrix/keybackupmanager.hpp) \
    $$quote($$_PRO_FILE_PWD_/../src/matrix/olmcryptomanager.hpp) \
    $$quote($$_PRO_FILE_PWD_/../src/matrix/syncengine.hpp) \
    $$quote($$_PRO_FILE_PWD_/../src/matrix/notificationmanager.hpp) \
    $$quote($$_PRO_FILE_PWD_/../src/matrix/base58.hpp) \
    $$quote($$_PRO_FILE_PWD_/../src/matrix/tlsnetworkreply.hpp) \
    $$quote($$_PRO_FILE_PWD_/../src/matrix/tlsnetworkaccessmanager.hpp) \
    $$quote($$_PRO_FILE_PWD_/../src/matrix/bbportlog.hpp)

TRANSLATIONS = $$quote($${TARGET}.ts)

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
LIBS += -lbb -lbbdata -lbbsystem -lbbplatform

INCLUDEPATH += $$quote($$_PRO_FILE_PWD_/src) $$quote($$_PRO_FILE_PWD_/../src/matrix)

SOURCES += \
    $$quote($$_PRO_FILE_PWD_/src/headless_main.cpp) \
    $$quote($$_PRO_FILE_PWD_/src/applicationheadless.cpp)

HEADERS += \
    $$quote($$_PRO_FILE_PWD_/src/applicationheadless.hpp)

TRANSLATIONS = $$quote($${TARGET}.ts)

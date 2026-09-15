#include "applicationheadless.hpp"

#include <bb/Application>

using namespace bb;

Q_DECL_EXPORT int main(int argc, char **argv)
{
    Application app(argc, argv);

    ApplicationHeadless appHeadless;

    return Application::exec();
}

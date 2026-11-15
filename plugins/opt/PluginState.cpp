// The command-line compiler owns these globals in its executable. The opt
// plugin is a separate DSO, so it carries its own isolated copies of the
// optimisation-option globals.
#include "common.h"

namespace freshet {
Options g_opts;
std::string g_backend;
std::string g_targetTriple;
std::string g_dataLayout;
} // namespace freshet

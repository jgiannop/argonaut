#pragma once

// `argonaut --tui` and `argonaut-tui`: Argonaut in the terminal. Returns the
// program's exit code. Arguments after the program's name (and --tui) are
// the folders of the left and the right pane.
namespace Tui {

int run(int argc, char **argv);

} // namespace Tui

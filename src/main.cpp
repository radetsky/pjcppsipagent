#include "args.h"
#include "call_executor.h"
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    // --version: quick exit before full arg parsing
    if (argc == 2 && std::string(argv[1]) == "--version") {
        std::cout << "pjcppagent 0.1.0" << std::endl;
        return 0;
    }

    // Read first line of STDIN (audio source spec)
    std::string stdin_line;
    std::getline(std::cin, stdin_line);

    // Parse args
    ArgParser parser;
    Config config = parser.parse(argc, argv, stdin_line);

    // Only CLI mode is implemented in M3
    if (config.mode != Mode::CLI) {
        std::cerr << "error: server mode not implemented yet" << std::endl;
        return 1;
    }

    // Sink: print JSON events to stdout
    auto sink = [](const AgentEvent& ev) {
        std::cout << ev.toJson() << std::endl;
    };

    CallExecutor executor(config, sink);
    AgentEvent result = executor.run();

    return statusToExitCode(result.status);
}

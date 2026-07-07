#include "args.h"
#include "call_executor.h"
#include "grpc_server.h"
#include <cstring>
#include <iostream>
#include <string>

// STDIN carries the audio source only in CLI mode; the server must not
// block reading it. Detect the mode before arg parsing.
static bool isServerMode(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            return std::strcmp(argv[i + 1], "server") == 0;
        }
        if (std::strcmp(argv[i], "--mode=server") == 0) {
            return true;
        }
    }
    return false;
}

int main(int argc, char* argv[]) {
    // --version: quick exit before full arg parsing
    if (argc == 2 && std::string(argv[1]) == "--version") {
        std::cout << "pjcppagent 0.1.0" << std::endl;
        return 0;
    }

    std::string stdin_line;
    if (!isServerMode(argc, argv)) {
        std::getline(std::cin, stdin_line);
    }

    ArgParser parser;
    Config config = parser.parse(argc, argv, stdin_line);

    if (config.mode == Mode::SERVER) {
        return runGrpcServer(config);
    }

    // CLI mode: print JSON events to stdout
    auto sink = [](const AgentEvent& ev) {
        std::cout << ev.toJson() << std::endl;
    };

    CallExecutor executor(config, sink);
    AgentEvent result = executor.run();

    return statusToExitCode(result.status);
}

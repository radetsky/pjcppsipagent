#ifndef ARGS_H
#define ARGS_H

#include <cstdint>
#include <string>

enum class Transport { UDP, TCP, TLS };

struct AudioSource {
    std::string type; // "wav" or "text"
    std::string path_or_text;
};

enum class Mode { CLI, SERVER };

// Pure data; no PJSIP or gRPC includes.
struct Config {
    Mode mode = Mode::CLI;
    std::string sip_host;
    uint16_t sip_port = 5060;
    Transport sip_transport = Transport::UDP;
    std::string sip_user;
    std::string sip_pass; // raw password; never log this
    std::string destination;
    std::string caller_id;
    uint32_t answer_delay_s = 1;
    uint32_t silence_s = 10;
    bool record = true;
    std::string record_dir = "./recordings";
    std::string grpc_listen = "0.0.0.0:50051";
    uint32_t idle_shutdown_s = 300;
    int log_level = 3;
    int silence_threshold = 10;
    AudioSource audio_source;

    std::string toString() const;
};

class ArgParser {
public:
    Config parse(int argc, char* argv[], const std::string& stdin_line);
};

#endif // ARGS_H

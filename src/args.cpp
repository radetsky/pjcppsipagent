#include "args.h"

#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif defined(__GNUC__) || defined(__GNUG__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
#include <CLI/CLI.hpp>
#if defined(__clang__)
#  pragma clang diagnostic pop
#elif defined(__GNUC__) || defined(__GNUG__)
#  pragma GCC diagnostic pop
#endif
#include <nlohmann/json.hpp>
#include <iostream>
#include <cstdlib>
#include <cstring>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string envOr(const char* name, const std::string& fallback) {
    const char* v = getenv(name);
    return v ? std::string(v) : fallback;
}

// ---------------------------------------------------------------------------
// Config::toString — redacts password
// ---------------------------------------------------------------------------

std::string Config::toString() const {
    nlohmann::json j;
    j["mode"]             = mode == Mode::CLI ? "cli" : "server";
    j["sip_host"]         = sip_host;
    j["sip_port"]         = static_cast<int>(sip_port);
    j["sip_transport"]    = sip_transport == Transport::UDP  ? "udp"
                          : sip_transport == Transport::TCP  ? "tcp"
                          :                                    "tls";
    j["sip_user"]         = sip_user;
    j["sip_pass"]         = "[REDACTED]";
    j["destination"]      = destination;
    j["caller_id"]        = caller_id;
    j["answer_delay_s"]   = answer_delay_s;
    j["answer_timeout_s"] = answer_timeout_s;
    j["silence_s"]        = silence_s;
    j["record"]           = record;
    j["record_dir"]       = record_dir;
    j["grpc_listen"]      = grpc_listen;
    j["idle_shutdown_s"]  = idle_shutdown_s;
    j["log_level"]        = log_level;
    j["silence_threshold"] = silence_threshold;
    if (!audio_source.type.empty()) {
        std::string d = audio_source.path_or_text;
        auto pos = d.rfind('/');
        if (pos != std::string::npos) d = d.substr(pos + 1);
        j["audio_source"] = d + " (" + audio_source.type + ")";
    } else {
        j["audio_source"] = "(none)";
    }
    return j.dump(2);
}

// ---------------------------------------------------------------------------
// ArgParser::parse
// ---------------------------------------------------------------------------

Config ArgParser::parse(int argc, char* argv[], const std::string& stdin_line) {
    Config config;

    CLI::App app{"pjcppagent — headless SIP call agent (PoC)"};
    app.set_version_flag("--version", "pjcppagent 0.1.0");

    // -- mode ---------------------------------------------------------------
    std::string mode_str = "cli";
    app.add_option("--mode", mode_str, "Operating mode: cli | server");

    // -- SIP connection -----------------------------------------------------
    std::string sip_host, sip_user, sip_pass, destination;
    int sip_port = 5060;
    std::string transport_str = "udp";

    app.add_option("--sip-host", sip_host, "SIP server host");
    auto* port_opt = app.add_option("--sip-port", sip_port, "SIP server port (default 5060)");
    app.add_option("--sip-transport", transport_str, "Transport: udp | tcp | tls");
    app.add_option("--sip-user", sip_user, "SIP auth username");
    // sip_pass deliberately has NO CLI flag — env only (security)

    app.add_option("--dest", destination, "Destination extension or E.164");
    app.add_option("--caller-id", config.caller_id, "Caller ID (optional)");

    // -- call behaviour -----------------------------------------------------
    app.add_option("--answer-delay",     config.answer_delay_s,  "Seconds after answer before playback (default 1)");
    app.add_option("--answer-timeout",   config.answer_timeout_s, "Max ringing seconds before cancel -> NO_ANSWER (default 30)");
    app.add_option("--silence-timeout",  config.silence_s,       "Seconds of silence after playback -> hangup (default 10)");
    app.add_flag("--record",             config.record,          "Enable recording (default true)");
    app.add_option("--record-dir",       config.record_dir,      "Recording output directory");

    // -- server mode --------------------------------------------------------
    app.add_option("--grpc-listen",      config.grpc_listen,     "gRPC listen address (server mode)");
    app.add_option("--idle-shutdown",    config.idle_shutdown_s, "Idle timeout before shutdown (server mode)");

    // -- logging ------------------------------------------------------------
    app.add_option("--log-level", config.log_level, "PJSIP log level 0–5 (default 3)");

    // -- hidden flags -------------------------------------------------------
    int st = 10;
    app.add_option("--silence-threshold", st, "Silence level threshold 0–255 (hidden)");

    // Parse CLI
    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        std::exit(app.exit(e)); // prints help / error to stderr
    }

    // -- mode ---------------------------------------------------------------
    if (mode_str == "server")      config.mode = Mode::SERVER;
    else if (mode_str == "cli")   config.mode = Mode::CLI;
    else { std::cerr << "error: --mode must be 'cli' or 'server' (got '" << mode_str << "')\n"; std::exit(2); }

    // -- ENV fallback (CLI flag beats ENV, ENV beats default) --------------
    if (sip_host.empty())       sip_host = envOr("AGENT_SIP_HOST", "");
    if (sip_user.empty())       sip_user = envOr("AGENT_SIP_USER", "");
    if (sip_pass.empty())       sip_pass = envOr("AGENT_SIP_PASS", "");
    if (destination.empty())    destination = envOr("AGENT_DEST", "");
    // AGENT_SIP_PORT: only apply if --sip-port was not given on CLI
    if (port_opt->count() == 0) {
        const char* port_env = getenv("AGENT_SIP_PORT");
        if (port_env) sip_port = std::stoi(port_env);
    }

    // -- validation (cli mode) ----------------------------------------------
    if (config.mode == Mode::CLI) {
        bool ok = true;
        if (sip_host.empty())     { std::cerr << "error: --sip-host (or AGENT_SIP_HOST) is required in cli mode\n"; ok = false; }
        if (sip_user.empty())     { std::cerr << "error: --sip-user (or AGENT_SIP_USER) is required in cli mode\n"; ok = false; }
        if (sip_pass.empty())     { std::cerr << "error: AGENT_SIP_PASS is required in cli mode\n"; ok = false; }
        if (destination.empty())  { std::cerr << "error: --dest (or AGENT_DEST) is required in cli mode\n"; ok = false; }
        if (!ok) std::exit(2);
    }

    // -- copy values into config -------------------------------------------
    config.sip_host  = sip_host;
    config.sip_user  = sip_user;
    config.sip_pass  = sip_pass;
    config.destination = destination;

    if (sip_port < 1 || sip_port > 65535) {
        std::cerr << "error: --sip-port must be 1–65535\n"; std::exit(2);
    }
    config.sip_port = static_cast<uint16_t>(sip_port);

    if (transport_str == "udp")      config.sip_transport = Transport::UDP;
    else if (transport_str == "tcp") config.sip_transport = Transport::TCP;
    else if (transport_str == "tls") config.sip_transport = Transport::TLS;
    else { std::cerr << "error: --sip-transport must be udp, tcp, or tls\n"; std::exit(2); }

    if (config.log_level < 0 || config.log_level > 5) {
        std::cerr << "error: --log-level must be 0–5\n"; std::exit(2);
    }

    config.silence_threshold = st;

    // -- STDIN audio source ------------------------------------------------
    if (!stdin_line.empty()) {
        if (stdin_line.substr(0, 4) == "wav:") {
            config.audio_source.type = "wav";
            config.audio_source.path_or_text = stdin_line.substr(4);
        } else if (stdin_line.substr(0, 5) == "text:") {
            config.audio_source.type = "text";
            config.audio_source.path_or_text = stdin_line.substr(5);
        } else {
            std::cerr << "error: invalid audio source; must start with 'wav:' or 'text:'\n";
            std::exit(2);
        }
    }

    return config;
}

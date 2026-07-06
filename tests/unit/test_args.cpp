#include <gtest/gtest.h>
#include <gtest/gtest-death-test.h>
#include "args.h"
#include <cstdlib>
#include <cstring>
#include <vector>

// ---------------------------------------------------------------------------
// Helpers: build argv array, set/clear env
// ---------------------------------------------------------------------------

static std::vector<char*> make_argv(const std::vector<const char*>& args) {
    std::vector<char*> argv;
    for (auto* a : args) argv.push_back(const_cast<char*>(a));
    return argv;
}

class ScopedEnv {
    std::string name_;
    bool was_set_ = false;
    std::string old_;
public:
    ScopedEnv(const char* name, const char* val) : name_(name) {
        const char* old = getenv(name);
        if (old) { was_set_ = true; old_ = old; }
        setenv(name, val, 1);
    }
    ~ScopedEnv() {
        if (was_set_) setenv(name_.c_str(), old_.c_str(), 1);
        else unsetenv(name_.c_str());
    }
};

// ---------------------------------------------------------------------------
// Defaults
// ---------------------------------------------------------------------------

TEST(ArgsTest, Defaults) {
    ScopedEnv host("AGENT_SIP_HOST", "h");
    ScopedEnv user("AGENT_SIP_USER", "u");
    ScopedEnv pass("AGENT_SIP_PASS", "p");
    ScopedEnv dest("AGENT_DEST", "d");
    ArgParser p;
    auto argv = make_argv({"prog"});
    Config c = p.parse(static_cast<int>(argv.size()), argv.data(), "");

    EXPECT_EQ(c.mode, Mode::CLI);
    EXPECT_EQ(c.sip_port, 5060);
    EXPECT_EQ(c.sip_transport, Transport::UDP);
    EXPECT_EQ(c.answer_delay_s, 1u);
    EXPECT_EQ(c.silence_s, 10u);
    EXPECT_EQ(c.record, true);
    EXPECT_EQ(c.record_dir, "./recordings");
    EXPECT_EQ(c.grpc_listen, "0.0.0.0:50051");
    EXPECT_EQ(c.idle_shutdown_s, 300u);
    EXPECT_EQ(c.log_level, 3);
    EXPECT_EQ(c.silence_threshold, 10);
    EXPECT_TRUE(c.caller_id.empty());
    EXPECT_TRUE(c.audio_source.type.empty());
}

// ---------------------------------------------------------------------------
// ENV fallback
// ---------------------------------------------------------------------------

TEST(ArgsTest, EnvFallback) {
    ScopedEnv host("AGENT_SIP_HOST", "sip.example.com");
    ScopedEnv user("AGENT_SIP_USER", "alice");
    ScopedEnv pass("AGENT_SIP_PASS", "secret123");
    ScopedEnv dest("AGENT_DEST", "100");
    ScopedEnv port("AGENT_SIP_PORT", "5070");

    ArgParser p;
    auto argv = make_argv({"prog", "--dest", "200"});  // CLI dest beats env
    Config c = p.parse(static_cast<int>(argv.size()), argv.data(), "");

    EXPECT_EQ(c.sip_host, "sip.example.com");
    EXPECT_EQ(c.sip_user, "alice");
    EXPECT_EQ(c.sip_pass, "secret123");
    EXPECT_EQ(c.destination, "200");        // CLI wins over env
    EXPECT_EQ(c.sip_port, 5070);            // env over default
}

// ---------------------------------------------------------------------------
// CLI-over-ENV precedence
// ---------------------------------------------------------------------------

TEST(ArgsTest, CliOverEnv) {
    ScopedEnv host("AGENT_SIP_HOST", "env-host");
    ScopedEnv user("AGENT_SIP_USER", "env-user");
    ScopedEnv pass("AGENT_SIP_PASS", "env-pass");
    ScopedEnv dest("AGENT_DEST", "env-dest");

    ArgParser p;
    auto argv = make_argv({"prog", "--sip-host", "cli-host",
                           "--sip-user", "cli-user",
                           "--dest", "cli-dest",
                           "--sip-port", "5080"});
    Config c = p.parse(static_cast<int>(argv.size()), argv.data(), "");

    // CLI values must win over env
    EXPECT_EQ(c.sip_host, "cli-host");
    EXPECT_EQ(c.sip_user, "cli-user");
    EXPECT_EQ(c.destination, "cli-dest");
    EXPECT_EQ(c.sip_port, 5080);
    // sip_pass has no CLI flag, so should come from env
    EXPECT_EQ(c.sip_pass, "env-pass");
}

// ---------------------------------------------------------------------------
// Missing required arg -> death
// ---------------------------------------------------------------------------

TEST(ArgsTest, MissingSipHostDies) {
    ScopedEnv user("AGENT_SIP_USER", "u");
    ScopedEnv pass("AGENT_SIP_PASS", "p");
    ScopedEnv dest("AGENT_DEST", "100");
    ArgParser p;
    auto argv = make_argv({"prog"});
    EXPECT_EXIT({ p.parse(static_cast<int>(argv.size()), argv.data(), ""); },
                ::testing::ExitedWithCode(2), "sip-host");
}

TEST(ArgsTest, MissingSipUserDies) {
    ScopedEnv host("AGENT_SIP_HOST", "h");
    ScopedEnv pass("AGENT_SIP_PASS", "p");
    ScopedEnv dest("AGENT_DEST", "100");
    ArgParser p;
    auto argv = make_argv({"prog"});
    EXPECT_EXIT({ p.parse(static_cast<int>(argv.size()), argv.data(), ""); },
                ::testing::ExitedWithCode(2), "sip-user");
}

TEST(ArgsTest, MissingSipPassDies) {
    ScopedEnv host("AGENT_SIP_HOST", "h");
    ScopedEnv user("AGENT_SIP_USER", "u");
    ScopedEnv dest("AGENT_DEST", "100");
    ArgParser p;
    auto argv = make_argv({"prog"});
    EXPECT_EXIT({ p.parse(static_cast<int>(argv.size()), argv.data(), ""); },
                ::testing::ExitedWithCode(2), "AGENT_SIP_PASS");
}

TEST(ArgsTest, MissingDestDies) {
    ScopedEnv host("AGENT_SIP_HOST", "h");
    ScopedEnv user("AGENT_SIP_USER", "u");
    ScopedEnv pass("AGENT_SIP_PASS", "p");
    ArgParser p;
    auto argv = make_argv({"prog"});
    EXPECT_EXIT({ p.parse(static_cast<int>(argv.size()), argv.data(), ""); },
                ::testing::ExitedWithCode(2), "dest");
}

// ---------------------------------------------------------------------------
// STDIN line parsing
// ---------------------------------------------------------------------------

TEST(ArgsTest, StdinWav) {
    ScopedEnv host("AGENT_SIP_HOST", "h");
    ScopedEnv user("AGENT_SIP_USER", "u");
    ScopedEnv pass("AGENT_SIP_PASS", "p");
    ScopedEnv dest("AGENT_DEST", "d");
    ArgParser p;
    auto argv = make_argv({"prog"});
    Config c = p.parse(static_cast<int>(argv.size()), argv.data(), "wav:/tmp/hello.wav");
    EXPECT_EQ(c.audio_source.type, "wav");
    EXPECT_EQ(c.audio_source.path_or_text, "/tmp/hello.wav");
}

TEST(ArgsTest, StdinText) {
    ScopedEnv host("AGENT_SIP_HOST", "h");
    ScopedEnv user("AGENT_SIP_USER", "u");
    ScopedEnv pass("AGENT_SIP_PASS", "p");
    ScopedEnv dest("AGENT_DEST", "d");
    ArgParser p;
    auto argv = make_argv({"prog"});
    Config c = p.parse(static_cast<int>(argv.size()), argv.data(), "text:hello world");
    EXPECT_EQ(c.audio_source.type, "text");
    EXPECT_EQ(c.audio_source.path_or_text, "hello world");
}

TEST(ArgsTest, StdinGarbageDies) {
    ScopedEnv host("AGENT_SIP_HOST", "h");
    ScopedEnv user("AGENT_SIP_USER", "u");
    ScopedEnv pass("AGENT_SIP_PASS", "p");
    ScopedEnv dest("AGENT_DEST", "d");
    ArgParser p;
    auto argv = make_argv({"prog"});
    EXPECT_EXIT({ p.parse(static_cast<int>(argv.size()), argv.data(), "garbage"); },
                ::testing::ExitedWithCode(2), "invalid audio source");
}

// ---------------------------------------------------------------------------
// toString does NOT contain raw password
// ---------------------------------------------------------------------------

TEST(ArgsTest, ToStringRedactsPassword) {
    ScopedEnv host("AGENT_SIP_HOST", "h");
    ScopedEnv user("AGENT_SIP_USER", "u");
    ScopedEnv pass("AGENT_SIP_PASS", "super-secret");
    ScopedEnv dest("AGENT_DEST", "100");

    ArgParser p;
    auto argv = make_argv({"prog"});
    Config c = p.parse(static_cast<int>(argv.size()), argv.data(), "");
    std::string s = c.toString();
    EXPECT_EQ(s.find("super-secret"), std::string::npos);
    EXPECT_NE(s.find("[REDACTED]"), std::string::npos);
}

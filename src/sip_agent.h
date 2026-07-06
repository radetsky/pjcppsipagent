#ifndef SIP_AGENT_H
#define SIP_AGENT_H

#include "events.h"
#include "args.h"
#include <pjsua2.hpp>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <string>

// ---------------------------------------------------------------------------
// SipAgent — owns PJSIP lifecycle, provides thread-safe event queue
// ---------------------------------------------------------------------------

class SipAgent {
public:
    explicit SipAgent(const Config& config);
    ~SipAgent();

    // Init endpoint, register account, block until REGISTERED or timeout
    bool init();

    // PJSIP event queue (thread-safe)
    void pushEvent(InternalEvent ev);
    bool waitEvent(InternalEvent& ev, unsigned timeout_ms);
    void wakeDispatcher();

    // PJSIP API
    void placeCall();
    void hangup();
    void startPlayer(const std::string& wav_path);
    void startRecorder(const std::string& wav_path);
    void stopRecorder();
    void setCallStartTime(uint64_t t); // ms epoch

    // Silence detection: poll received audio level (0..255)
    unsigned getRxLevel();

    // Accessors
    pj::Endpoint& endpoint() { return *ep_; }
    uint64_t callStartTime() const { return call_start_ms_; }
    int lastSipCode() const { return last_sip_code_; }
    bool isRegistered() const { return registered_; }
    const std::string& callId() const { return call_id_; }
    const Config& config() const { return config_; }

private:
    // ---- PJSIP callback subclasses ----

    class MyAccount : public pj::Account {
    public:
        MyAccount(SipAgent& agent) : agent_(agent) {}
    private:
        void onRegState(pj::OnRegStateParam& prm) override;
        SipAgent& agent_;
    };

    class MyCall : public pj::Call {
    public:
        MyCall(SipAgent& agent, pj::Account& acc, int call_id = PJSUA_INVALID_ID);
    private:
        void onCallState(pj::OnCallStateParam& prm) override;
        void onCallMediaState(pj::OnCallMediaStateParam& prm) override;
        SipAgent& agent_;
    };

    class MyPlayer : public pj::AudioMediaPlayer {
    public:
        MyPlayer(SipAgent& agent) : agent_(agent) {}
    private:
        void onEof2() override;
        SipAgent& agent_;
    };

    // ---- helpers ----
    void libRegisterThread(const std::string& name);

    // ---- config ----
    Config config_;

    // ---- PJSIP objects ----
    std::unique_ptr<pj::Endpoint> ep_;
    std::unique_ptr<MyAccount> account_;
    std::unique_ptr<MyCall> call_;
    std::unique_ptr<MyPlayer> player_;
    std::unique_ptr<pj::AudioMediaRecorder> recorder_;

    // ---- state ----
    bool registered_ = false;
    int last_sip_code_ = 0;
    uint64_t call_start_ms_ = 0;
    std::string call_id_;

    // ---- event queue ----
    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<InternalEvent> queue_;
    bool dispatcher_wake_ = false;

    // ---- threading ----
    bool thread_registered_ = false;
};

#endif // SIP_AGENT_H

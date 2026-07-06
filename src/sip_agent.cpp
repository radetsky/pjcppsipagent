#include "sip_agent.h"
#include <iostream>

// =========================================================================
// SipAgent
// =========================================================================

SipAgent::SipAgent(const Config& config)
    : config_(config)
{
    call_id_ = config.destination + "@" + config.sip_host;
}

SipAgent::~SipAgent() {
    player_.reset();
    recorder_.reset();
    call_.reset();
    if (account_) {
        try { account_->shutdown(); } catch (...) {}
    }
    account_.reset();
    if (ep_) {
        try { ep_->libDestroy(); } catch (...) {}
    }
    ep_.reset();
}

void SipAgent::libRegisterThread(const std::string& name) {
    if (!thread_registered_) {
        ep_->libRegisterThread(name);
        thread_registered_ = true;
    }
}

// ---------------------------------------------------------------------------
// init
// ---------------------------------------------------------------------------

bool SipAgent::init() {
    ep_ = std::make_unique<pj::Endpoint>();

    ep_->libCreate();

    pj::EpConfig ep_cfg;
    ep_cfg.logConfig.level = config_.log_level;
    ep_cfg.logConfig.consoleLevel = config_.log_level;

    ep_->libInit(ep_cfg);

    pj::TransportConfig tcfg;
    tcfg.port = 0;
    ep_->transportCreate(PJSIP_TRANSPORT_UDP, tcfg);

    ep_->libStart();

    // Null audio device (headless!)
    ep_->audDevManager().setNullDev();

    // Account
    pj::AccountConfig acc_cfg;

    std::string id_uri = "sip:" + config_.sip_user + "@" + config_.sip_host;
    acc_cfg.idUri = id_uri;

    std::string reg_uri = "sip:" + config_.sip_host;
    if (config_.sip_port != 5060) {
        reg_uri += ":" + std::to_string(config_.sip_port);
    }
    std::string transport_str;
    switch (config_.sip_transport) {
        case Transport::UDP: transport_str = "udp"; break;
        case Transport::TCP: transport_str = "tcp"; break;
        case Transport::TLS: transport_str = "tls"; break;
    }
    reg_uri += ";transport=" + transport_str;
    acc_cfg.regConfig.registrarUri = reg_uri;

    pj::AuthCredInfo cred("digest", "*", config_.sip_user, 0, config_.sip_pass);
    acc_cfg.sipConfig.authCreds.push_back(cred);

    account_ = std::make_unique<MyAccount>(*this);
    account_->create(acc_cfg);

    return true;
}

// ---------------------------------------------------------------------------
// Event queue
// ---------------------------------------------------------------------------

void SipAgent::pushEvent(InternalEvent ev) {
    std::lock_guard<std::mutex> lock(mtx_);
    queue_.push_back(ev);
    cv_.notify_one();
}

bool SipAgent::waitEvent(InternalEvent& ev, unsigned timeout_ms) {
    std::unique_lock<std::mutex> lock(mtx_);
    dispatcher_wake_ = false;
    cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] {
        return !queue_.empty() || dispatcher_wake_;
    });
    if (queue_.empty()) return false;
    ev = queue_.front();
    queue_.pop_front();
    return true;
}

void SipAgent::wakeDispatcher() {
    std::lock_guard<std::mutex> lock(mtx_);
    dispatcher_wake_ = true;
    cv_.notify_one();
}

// ---------------------------------------------------------------------------
// PJSIP API
// ---------------------------------------------------------------------------

void SipAgent::placeCall() {
    libRegisterThread("pjcppagent-executor");

    pj::CallOpParam call_param;
    call_param.opt.audioCount = 1;
    call_param.opt.videoCount = 0;

    std::string dest = "sip:" + config_.destination + "@" + config_.sip_host;
    if (config_.sip_port != 5060) {
        dest += ":" + std::to_string(config_.sip_port);
    }
    dest += ";transport=";
    switch (config_.sip_transport) {
        case Transport::UDP: dest += "udp"; break;
        case Transport::TCP: dest += "tcp"; break;
        case Transport::TLS: dest += "tls"; break;
    }

    call_ = std::make_unique<MyCall>(*this, *account_);
    call_->makeCall(dest, call_param);
}

void SipAgent::hangup() {
    libRegisterThread("pjcppagent-executor");
    if (call_) {
        pj::CallOpParam param;
        param.statusCode = PJSIP_SC_DECLINE;
        try { call_->hangup(param); } catch (...) {}
    }
}

void SipAgent::startPlayer(const std::string& wav_path) {
    libRegisterThread("pjcppagent-executor");

    player_ = std::make_unique<MyPlayer>(*this);
    player_->createPlayer(wav_path, PJMEDIA_FILE_NO_LOOP);

    // Connect player -> call's audio media
    if (call_) {
        try {
            pj::AudioMedia call_media = call_->getAudioMedia(-1);
            player_->startTransmit(call_media);
        } catch (...) {
            std::cerr << "error: no audio media in call to start player" << std::endl;
            return;
        }
    }
}

void SipAgent::startRecorder(const std::string& wav_path) {
    libRegisterThread("pjcppagent-executor");

    recorder_ = std::make_unique<pj::AudioMediaRecorder>();
    recorder_->createRecorder(wav_path);

    // Connect call's audio media -> recorder
    if (call_) {
        try {
            pj::AudioMedia call_media = call_->getAudioMedia(-1);
            call_media.startTransmit(*recorder_);
        } catch (...) {
            std::cerr << "error: no audio media in call to start recorder" << std::endl;
            return;
        }
    }
}

void SipAgent::stopRecorder() {
    recorder_.reset();
}

unsigned SipAgent::getRxLevel() {
    libRegisterThread("pjcppagent-executor");
    if (!call_) return 0;
    try {
        pj::AudioMedia m = call_->getAudioMedia(-1);
        return m.getRxLevel();
    } catch (...) {
        return 0;
    }
}

void SipAgent::setCallStartTime(uint64_t t) {
    call_start_ms_ = t;
}

// =========================================================================
// MyAccount
// =========================================================================

void SipAgent::MyAccount::onRegState(pj::OnRegStateParam& prm) {
    InternalEvent ev;
    ev.type = InternalType::REG_STATE;
    ev.connected = (prm.code >= 200 && prm.code < 300);
    ev.sip_code = prm.code;
    ev.reason = prm.reason;
    agent_.pushEvent(ev);
}

// =========================================================================
// MyCall
// =========================================================================

SipAgent::MyCall::MyCall(SipAgent& agent, pj::Account& acc, int call_id)
    : pj::Call(acc, call_id), agent_(agent)
{}

void SipAgent::MyCall::onCallState(pj::OnCallStateParam& prm) {
    (void)prm;
    InternalEvent ev;
    ev.type = InternalType::CALL_STATE;

    pj::CallInfo ci;
    try {
        ci = getInfo();
    } catch (...) {
        ev.call_state = PJSIP_INV_STATE_DISCONNECTED;
        ev.sip_code = 0;
        ev.reason = "call destroyed";
        agent_.pushEvent(ev);
        return;
    }

    ev.call_state = ci.state;
    ev.sip_code = ci.lastStatusCode;
    ev.reason = ci.lastReason;

    if (ci.state == PJSIP_INV_STATE_DISCONNECTED) {
        agent_.last_sip_code_ = ci.lastStatusCode;
    }

    agent_.pushEvent(ev);
}

void SipAgent::MyCall::onCallMediaState(pj::OnCallMediaStateParam& prm) {
    (void)prm;
    InternalEvent ev;
    ev.type = InternalType::MEDIA_STATE;
    ev.media_active = false;

    pj::CallInfo ci;
    try {
        ci = getInfo();
    } catch (...) {
        agent_.pushEvent(ev);
        return;
    }

    for (unsigned i = 0; i < ci.media.size(); ++i) {
        if (ci.media[i].type == PJMEDIA_TYPE_AUDIO &&
            ci.media[i].status == PJSUA_CALL_MEDIA_ACTIVE) {
            ev.media_active = true;
            break;
        }
    }

    agent_.pushEvent(ev);
}

// =========================================================================
// MyPlayer
// =========================================================================

void SipAgent::MyPlayer::onEof2() {
    InternalEvent ev;
    ev.type = InternalType::PLAYBACK_EOF;
    agent_.pushEvent(ev);
}

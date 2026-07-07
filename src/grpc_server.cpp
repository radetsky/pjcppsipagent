#include "grpc_server.h"
#include "call_executor.h"
#include "events.h"

#include <grpcpp/grpcpp.h>
#ifndef PJCPP_NO_REFLECTION
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#endif
#include <google/protobuf/timestamp.pb.h>
#include "call_agent.grpc.pb.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace v1 = automatedcalls::callagent::v1;

static constexpr const char* kAgentVersion = "0.1.0";

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static uint64_t epochMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

static void setTimestamp(google::protobuf::Timestamp* ts, uint64_t ms) {
    ts->set_seconds(static_cast<int64_t>(ms / 1000));
    ts->set_nanos(static_cast<int32_t>((ms % 1000) * 1000000));
}

static v1::StateChange::State toProtoState(CallStatus s) {
    switch (s) {
        case CallStatus::REGISTERING: return v1::StateChange::REGISTERING;
        case CallStatus::REGISTERED:  return v1::StateChange::REGISTERED;
        case CallStatus::DIALING:     return v1::StateChange::DIALING;
        case CallStatus::RINGING:     return v1::StateChange::RINGING;
        case CallStatus::ANSWERED:    return v1::StateChange::ANSWERED;
        case CallStatus::PLAYING:     return v1::StateChange::PLAYING;
        case CallStatus::PLAYED:      return v1::StateChange::PLAYED;
        case CallStatus::HANGING_UP:  return v1::StateChange::HANGING_UP;
        default:                      return v1::StateChange::STATE_UNSPECIFIED;
    }
}

static v1::CallResult::Status toProtoResultStatus(CallStatus s) {
    switch (s) {
        case CallStatus::ANSWERED:  return v1::CallResult::ANSWERED;
        case CallStatus::BUSY:      return v1::CallResult::BUSY;
        case CallStatus::NO_ANSWER: return v1::CallResult::NO_ANSWER;
        case CallStatus::CANCELLED: return v1::CallResult::CANCELLED;
        default:                    return v1::CallResult::FAILED;
    }
}

// Convert an internal AgentEvent to the wire CallEvent. call_id is echoed
// from the request (the internal id is dest@host, not the backend UUID).
static v1::CallEvent toProtoEvent(const AgentEvent& ev, const std::string& call_id,
                                  uint64_t started_at_ms) {
    v1::CallEvent out;
    out.set_call_id(call_id);
    uint64_t now = epochMs();
    setTimestamp(out.mutable_occurred_at(), now);

    bool is_final = ev.is_result ||
                    ev.status == CallStatus::BUSY ||
                    ev.status == CallStatus::NO_ANSWER ||
                    ev.status == CallStatus::CANCELLED ||
                    ev.status == CallStatus::FAILED;
    if (is_final) {
        auto* res = out.mutable_result();
        res->set_status(toProtoResultStatus(ev.status));
        res->set_billing_seconds(ev.billing_seconds);
        res->set_sip_code(static_cast<uint32_t>(ev.sip_code));
        res->set_error_message(ev.reason);
        res->set_recording_uri(ev.recording_path);
        if (started_at_ms > 0) setTimestamp(res->mutable_started_at(), started_at_ms);
        setTimestamp(res->mutable_ended_at(), now);
    } else if (ev.status == CallStatus::SILENCE_DETECTED) {
        out.mutable_silence_detected()->set_silence_seconds(ev.billing_seconds);
    } else if (ev.status == CallStatus::RECORDING_READY) {
        auto* rec = out.mutable_recording_ready();
        rec->set_recording_uri(ev.recording_path);
        rec->set_duration_seconds(ev.billing_seconds);
    } else {
        out.mutable_state_change()->set_state(toProtoState(ev.status));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Service implementation (sync API — sufficient for max 1 concurrent call)
// ---------------------------------------------------------------------------

class CallAgentServiceImpl final : public v1::CallAgent::Service {
public:
    explicit CallAgentServiceImpl(const Config& base_config)
        : base_config_(base_config), last_activity_ms_(epochMs()) {}

    grpc::Status Health(grpc::ServerContext*, const v1::HealthRequest*,
                        v1::HealthResponse* response) override {
        uint64_t idle_ms = epochMs() - last_activity_ms_.load();
        touchActivity();

        if (shutting_down_.load()) {
            response->set_state(v1::HealthResponse::SHUTTING_DOWN);
        } else if (call_active_.load()) {
            response->set_state(v1::HealthResponse::BUSY);
        } else {
            response->set_state(v1::HealthResponse::READY);
        }
        response->set_active_calls(call_active_.load() ? 1 : 0);
        response->set_idle_seconds(static_cast<uint32_t>(idle_ms / 1000));
        response->set_agent_version(kAgentVersion);
        return grpc::Status::OK;
    }

    grpc::Status ExecuteCall(grpc::ServerContext* context,
                             const v1::CallRequest* request,
                             grpc::ServerWriter<v1::CallEvent>* writer) override {
        touchActivity();

        // Single-call PoC: reject a second concurrent call.
        bool expected = false;
        if (!call_active_.compare_exchange_strong(expected, true)) {
            return grpc::Status(grpc::StatusCode::ALREADY_EXISTS,
                                "a call is already in progress");
        }
        struct ActiveGuard {
            CallAgentServiceImpl& s;
            ~ActiveGuard() { s.call_active_.store(false); s.touchActivity(); }
        } guard{*this};

        std::string verr = validate(*request);
        if (!verr.empty()) {
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, verr);
        }

        Config cfg = makeCallConfig(*request);

        // Events flow: executor thread -> queue -> this RPC thread -> writer.
        // gRPC Write() must never be called from PJSIP/executor threads.
        std::mutex mtx;
        std::condition_variable cv;
        std::deque<AgentEvent> queue;
        bool done = false;

        CallExecutor executor(cfg, [&](const AgentEvent& ev) {
            std::lock_guard<std::mutex> lock(mtx);
            queue.push_back(ev);
            cv.notify_one();
        });

        std::thread call_thread([&] {
            executor.run();
            std::lock_guard<std::mutex> lock(mtx);
            done = true;
            cv.notify_one();
        });

        uint64_t started_at_ms = 0;
        bool cancel_sent = false;
        for (;;) {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait_for(lock, std::chrono::milliseconds(200),
                        [&] { return done || !queue.empty(); });
            std::deque<AgentEvent> batch;
            batch.swap(queue);
            bool finished = done && batch.empty();
            lock.unlock();

            for (const AgentEvent& ev : batch) {
                if (ev.status == CallStatus::ANSWERED && !ev.is_result) {
                    started_at_ms = epochMs();
                }
                writer->Write(toProtoEvent(ev, request->call_id(), started_at_ms));
            }

            if (finished) break;

            if (!cancel_sent && context->IsCancelled()) {
                cancel_sent = true;
                executor.cancel();
            }
        }

        call_thread.join();
        // Call-level failures live in CallResult.status; gRPC status codes are
        // reserved for protocol misuse.
        return grpc::Status::OK;
    }

    grpc::Status StreamAudio(grpc::ServerContext*,
                             grpc::ServerReaderWriter<v1::AudioFrame, v1::AudioFrame>*)
        override {
        touchActivity();
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "PoC");
    }

    void touchActivity() { last_activity_ms_.store(epochMs()); }
    uint64_t idleSeconds() const { return (epochMs() - last_activity_ms_.load()) / 1000; }
    bool callActive() const { return call_active_.load(); }
    void setShuttingDown() { shutting_down_.store(true); }

private:
    static std::string validate(const v1::CallRequest& req) {
        if (req.call_id().empty()) return "call_id is required";
        if (!req.has_sip()) return "sip credentials are required";
        if (req.sip().username().empty()) return "sip.username is required";
        if (req.sip().password().empty()) return "sip.password is required";
        if (req.sip().server_host().empty()) return "sip.server_host is required";
        if (req.destination().empty()) return "destination is required";
        if (req.audio_source_case() == v1::CallRequest::AUDIO_SOURCE_NOT_SET) {
            return "exactly one audio_source (wav_url or tts_text) is required";
        }
        return "";
    }

    Config makeCallConfig(const v1::CallRequest& req) const {
        Config cfg = base_config_; // keeps record_dir, log_level, answer_timeout_s
        cfg.mode = Mode::CLI;      // executor semantics are mode-independent
        cfg.sip_host = req.sip().server_host();
        cfg.sip_port = req.sip().server_port() ? static_cast<uint16_t>(req.sip().server_port())
                                               : 5060;
        cfg.sip_user = req.sip().username();
        cfg.sip_pass = req.sip().password();
        switch (req.sip().transport()) {
            case v1::SipCredentials::TCP: cfg.sip_transport = Transport::TCP; break;
            case v1::SipCredentials::TLS: cfg.sip_transport = Transport::TLS; break;
            default:                      cfg.sip_transport = Transport::UDP; break;
        }
        cfg.destination = req.destination();
        cfg.caller_id = req.caller_id();
        // proto3 zero values mean "use default", as documented in the proto
        cfg.answer_delay_s = req.answer_delay_seconds() ? req.answer_delay_seconds() : 1;
        cfg.silence_s = req.silence_timeout_seconds() ? req.silence_timeout_seconds() : 10;
        cfg.record = req.record();
        if (req.audio_source_case() == v1::CallRequest::kWavUrl) {
            cfg.audio_source.type = "wav";
            // PoC: wav_url is a local filesystem path; fetching http(s) URLs
            // is a later integration.
            cfg.audio_source.path_or_text = req.wav_url();
        } else {
            cfg.audio_source.type = "text";
            cfg.audio_source.path_or_text = req.tts_text();
        }
        return cfg;
    }

    Config base_config_;
    std::atomic<bool> call_active_{false};
    std::atomic<bool> shutting_down_{false};
    std::atomic<uint64_t> last_activity_ms_;
};

// ---------------------------------------------------------------------------
// runGrpcServer — build, serve, idle-watchdog shutdown
// ---------------------------------------------------------------------------

int runGrpcServer(const Config& config) {
    CallAgentServiceImpl service(config);

#ifndef PJCPP_NO_REFLECTION
    grpc::reflection::InitProtoReflectionServerBuilderPlugin();
#endif
    grpc::ServerBuilder builder;
    // TODO(security): mTLS or per-worker token (docs/SIP-AGENT.md)
    builder.AddListeningPort(config.grpc_listen, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
    if (!server) {
        std::cerr << "error: failed to listen on " << config.grpc_listen << std::endl;
        return 1;
    }
    std::cerr << "pjcppagent " << kAgentVersion << " gRPC server on "
              << config.grpc_listen << " (idle shutdown "
              << config.idle_shutdown_s << " s)" << std::endl;

    // Idle watchdog: the server must never kill an in-progress call.
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        if (!service.callActive() && service.idleSeconds() >= config.idle_shutdown_s) {
            service.setShuttingDown();
            std::cerr << "idle for " << service.idleSeconds()
                      << " s, shutting down" << std::endl;
            break;
        }
    }

    server->Shutdown();
    server->Wait();
    return 0;
}

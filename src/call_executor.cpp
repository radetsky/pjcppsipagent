#include "call_executor.h"
#include "silence.h"
#include "tts_stub.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <sys/stat.h>
#include <sys/types.h>

// =========================================================================
// Helpers
// =========================================================================

static uint64_t epochMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

// =========================================================================
// CallExecutor
// =========================================================================

CallExecutor::CallExecutor(const Config& config, EventSink sink)
    : config_(config), sink_(std::move(sink)), agent_(config)
{}

uint64_t CallExecutor::nowMs() const {
    return epochMs();
}

void CallExecutor::emitEvent(AgentEvent ev) {
    if (sink_) sink_(ev);
}

// ---------------------------------------------------------------------------
// run — main state machine
// ---------------------------------------------------------------------------

AgentEvent CallExecutor::run() {
    result_.status = CallStatus::FAILED;

    // --- Step 1: Init SipAgent ---
    if (!agent_.init()) {
        result_.status = CallStatus::FAILED;
        result_.reason = "failed to initialize SIP agent";
        emitEvent({CallStatus::FAILED, "", "", 0, 0, result_.reason});
        return result_;
    }

    // --- Step 2: Wait for registration ---
    emitEvent({CallStatus::REGISTERING, agent_.callId(), "", 0, 0, ""});

    uint64_t reg_start = nowMs();
    constexpr uint64_t REG_TIMEOUT_MS = 15000;

    while (!cancelled_) {
        InternalEvent ev;
        if (!agent_.waitEvent(ev, 500)) {
            if (nowMs() - reg_start >= REG_TIMEOUT_MS) {
                result_.status = CallStatus::FAILED;
                result_.sip_code = 0;
                result_.reason = "registration timeout";
                emitEvent({CallStatus::FAILED, agent_.callId(), "", 0, 0, "registration timeout"});
                return result_;
            }
            continue;
        }

        if (ev.type == InternalType::REG_STATE) {
            if (ev.connected) {
                emitEvent({CallStatus::REGISTERED, agent_.callId(), "", 0, 0, ""});
                break;
            } else {
                result_.status = CallStatus::FAILED;
                result_.sip_code = ev.sip_code;
                result_.reason = ev.reason;
                emitEvent({CallStatus::FAILED, agent_.callId(), "", 0, ev.sip_code, ev.reason});
                return result_;
            }
        }
    }

    if (cancelled_) {
        result_.status = CallStatus::CANCELLED;
        result_.reason = "cancelled before call placed";
        return result_;
    }

    // --- Step 3: Place call ---
    emitEvent({CallStatus::DIALING, agent_.callId(), "", 0, 0, ""});
    agent_.placeCall();

    // --- Step 4: Wait for call states ---
    bool answered = false;
    bool media_active = false;
    bool answer_timeout_hit = false;
    const uint64_t answer_deadline_ms = nowMs() + config_.answer_timeout_s * 1000ULL;

    while (!cancelled_) {
        // Agent-initiated NO_ANSWER path: no CONFIRMED within answer_timeout_s
        // -> hangup() sends CANCEL, remote replies 487 -> NO_ANSWER via mapping.
        if (!answered && !answer_timeout_hit && nowMs() >= answer_deadline_ms) {
            answer_timeout_hit = true;
            agent_.hangup();
        }
        // Safety net: remote never acknowledged our CANCEL with a final response
        if (answer_timeout_hit && nowMs() >= answer_deadline_ms + 10000) {
            result_.status = CallStatus::NO_ANSWER;
            result_.reason = "answer timeout";
            emitEvent({CallStatus::NO_ANSWER, agent_.callId(), "", 0, 0, result_.reason, true});
            return result_;
        }

        InternalEvent ev;
        if (!agent_.waitEvent(ev, 500)) continue;

        if (ev.type == InternalType::CALL_STATE) {
            if (ev.call_state == INV_STATE_CALLING) {
                // DIALING already emitted right before placeCall()
            } else if (ev.call_state == INV_STATE_EARLY) {
                emitEvent({CallStatus::RINGING, agent_.callId(), "", 0, 0, ""});
            } else if (ev.call_state == INV_STATE_CONFIRMED) {
                answered = true;
                // Provisional final status; handleDisconnect refines it later.
                result_.status = CallStatus::ANSWERED;
                call_connected_ms_ = nowMs();
                agent_.setCallStartTime(call_connected_ms_);
                emitEvent({CallStatus::ANSWERED, agent_.callId(), "", 0, 0, ""});
            } else if (ev.call_state == INV_STATE_DISCONNECTED) {
                if (answer_timeout_hit) {
                    // PJSIP records a local hangup of an early call as
                    // "603 Decline"; the wire-level outcome of our CANCEL is
                    // 487, which maps to NO_ANSWER.
                    handleDisconnect(487, "answer timeout");
                } else {
                    handleDisconnect(ev.sip_code, ev.reason);
                }
                return result_;
            }
        } else if (ev.type == InternalType::MEDIA_STATE) {
            if (ev.media_active) media_active = true;
        }

        if (answered && media_active) break;
    }

    if (cancelled_) {
        agent_.hangup();
        result_.status = CallStatus::CANCELLED;
        result_.reason = "cancelled by user";
        return result_;
    }

    if (result_.status == CallStatus::FAILED ||
        result_.status == CallStatus::BUSY ||
        result_.status == CallStatus::NO_ANSWER) {
        return result_;
    }

    // --- Step 5: Answer delay, setup recording, playback ---
    mkdir(config_.record_dir.c_str(), 0755);

    std::string rec_path = config_.record_dir + "/" + agent_.callId() + ".wav";
    agent_.startRecorder(rec_path);
    result_.recording_path = rec_path;

    // Determine audio source
    std::string play_path;
    if (config_.audio_source.type == "wav") {
        play_path = config_.audio_source.path_or_text;
    } else if (config_.audio_source.type == "text") {
        play_path = synth(config_.audio_source.path_or_text, config_.record_dir);
    }

    // Wait answer_delay_s before playing
    uint64_t answer_ms = call_connected_ms_;
    uint64_t play_at_ms = answer_ms + (uint64_t)config_.answer_delay_s * 1000;
    while (nowMs() < play_at_ms && !cancelled_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (cancelled_) {
        agent_.hangup();
        result_.status = CallStatus::CANCELLED;
        return result_;
    }

    // Start playback
    emitEvent({CallStatus::PLAYING, agent_.callId(), "", 0, 0, ""});
    playback_started_ms_ = nowMs();
    agent_.startPlayer(play_path);

    // --- Step 6: Wait for playback EOF ---
    bool played = false;
    while (!played && !cancelled_) {
        InternalEvent ev;
        if (!agent_.waitEvent(ev, 500)) {
            if (nowMs() - call_connected_ms_ >= 600000) {
                std::cerr << "max call duration reached" << std::endl;
                agent_.hangup();
                while (!cancelled_) {
                    if (!agent_.waitEvent(ev, 500)) continue;
                    if (ev.type == InternalType::CALL_STATE &&
                        ev.call_state == INV_STATE_DISCONNECTED) {
                        handleDisconnect(ev.sip_code, ev.reason);
                        return result_;
                    }
                }
                break;
            }
            continue;
        }

        if (ev.type == InternalType::PLAYBACK_EOF) {
            played = true;
            emitEvent({CallStatus::PLAYED, agent_.callId(), "", 0, 0, ""});
            break;
        } else if (ev.type == InternalType::CALL_STATE &&
                   ev.call_state == INV_STATE_DISCONNECTED) {
            handleDisconnect(ev.sip_code, ev.reason);
            return result_;
        }
    }

    if (cancelled_) {
        agent_.hangup();
        result_.status = CallStatus::CANCELLED;
        return result_;
    }

    if (result_.status == CallStatus::FAILED ||
        result_.status == CallStatus::BUSY ||
        result_.status == CallStatus::NO_ANSWER) {
        return result_;
    }

    // --- Step 7: Silence detection via SilenceDetector ---
    {
        SilenceDetector sd(
            static_cast<unsigned>(config_.silence_threshold),
            static_cast<unsigned>(config_.silence_s) * 1000
        );
        sd.arm();

        bool silent = false;

        while (!silent && !cancelled_) {
            InternalEvent ev;
            if (agent_.waitEvent(ev, 100)) {
                if (ev.type == InternalType::CALL_STATE &&
                    ev.call_state == INV_STATE_DISCONNECTED) {
                    handleDisconnect(ev.sip_code, ev.reason);
                    return result_;
                }
            }

            // Poll received audio level and feed detector
            unsigned level = agent_.getRxLevel();
            if (sd.feed(level, nowMs())) {
                silent = true;
            }

            // Check max call duration (600s hard cap)
            if (nowMs() - call_connected_ms_ >= 600000) {
                std::cerr << "max call duration reached" << std::endl;
                silent = true;
                break;
            }
        }

        if (cancelled_) {
            result_.status = CallStatus::CANCELLED;
            return result_;
        }

        // Silence detected -> hangup
        emitEvent({CallStatus::SILENCE_DETECTED, agent_.callId(), "",
                   config_.silence_s, 0, ""});
        emitEvent({CallStatus::HANGING_UP, agent_.callId(), "", 0, 0, ""});
        agent_.hangup();
    }

    // --- Step 8: Wait for disconnect ---
    while (!cancelled_) {
        InternalEvent ev;
        if (!agent_.waitEvent(ev, 500)) continue;
        if (ev.type == InternalType::CALL_STATE &&
            ev.call_state == INV_STATE_DISCONNECTED) {
            handleDisconnect(ev.sip_code, ev.reason);
            return result_;
        }
    }

    result_.status = CallStatus::CANCELLED;
    return result_;
}

// ---------------------------------------------------------------------------
// handleDisconnect
// ---------------------------------------------------------------------------

void CallExecutor::handleDisconnect(int sip_code, const std::string& reason) {
    uint32_t bill_sec = 0;
    if (call_connected_ms_ > 0) {
        uint64_t elapsed = nowMs() - call_connected_ms_;
        bill_sec = static_cast<uint32_t>((elapsed + 999) / 1000);
    }

    // Mapping table row 1: answered then hung up (any side) -> ANSWERED.
    // Local hangup of a confirmed call is recorded by PJSIP as "603 Decline",
    // so the sip-code mapping only applies to never-answered calls.
    CallStatus status = (call_connected_ms_ > 0)
        ? CallStatus::ANSWERED
        : sipCodeToCallStatus(sip_code);

    result_.status = status;
    result_.sip_code = sip_code;
    result_.reason = reason;
    result_.billing_seconds = bill_sec;

    // Stop recorder explicitly, then emit RECORDING_READY before result
    agent_.stopRecorder();
    if (!result_.recording_path.empty()) {
        emitEvent({
            CallStatus::RECORDING_READY,
            agent_.callId(),
            result_.recording_path,
            bill_sec,
            0,
            ""
        });
    }

    // Final result
    emitEvent({
        status,
        agent_.callId(),
        result_.recording_path,
        bill_sec,
        sip_code,
        reason,
        true
    });
}

// ---------------------------------------------------------------------------
// cancel
// ---------------------------------------------------------------------------

void CallExecutor::cancel() {
    cancelled_ = true;
    agent_.wakeDispatcher();
}

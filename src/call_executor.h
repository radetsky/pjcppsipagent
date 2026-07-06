#ifndef CALL_EXECUTOR_H
#define CALL_EXECUTOR_H

#include "args.h"
#include "events.h"
#include "sip_agent.h"
#include <functional>
#include <string>

// ---------------------------------------------------------------------------
// CallExecutor — single-call state machine
// ---------------------------------------------------------------------------

class CallExecutor {
public:
    using EventSink = std::function<void(const AgentEvent&)>;

    CallExecutor(const Config& config, EventSink sink);

    // Run the full call lifecycle. Blocks until done.
    // Returns the final AgentEvent (result).
    AgentEvent run();

    // Cancel an in-progress call
    void cancel();

private:
    void emitEvent(AgentEvent ev);
    void handleDisconnect(int sip_code, const std::string& reason);
    void onTimeout(uint32_t timeout_id);

    Config config_;
    EventSink sink_;
    SipAgent agent_;
    bool cancelled_ = false;
    AgentEvent result_;

    // Timing
    uint64_t nowMs() const;
    uint64_t call_connected_ms_ = 0;
    uint64_t playback_started_ms_ = 0;
};

#endif // CALL_EXECUTOR_H

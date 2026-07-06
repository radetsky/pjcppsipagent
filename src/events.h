#ifndef EVENTS_H
#define EVENTS_H

#include <cstdint>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Call lifecycle status (public-facing, sink callback receives these)
// ---------------------------------------------------------------------------

enum class CallStatus {
  REGISTERING,
  REGISTERED,
  DIALING,
  RINGING,
  ANSWERED,
  PLAYING,
  PLAYED,
  SILENCE_DETECTED,
  RECORDING_READY,
  BUSY,
  NO_ANSWER,
  CANCELLED,
  FAILED,
  HANGING_UP,
  DISCONNECTED
};

std::string callStatusToString(CallStatus s);

// ---------------------------------------------------------------------------
// CallResult — final outcome
// ---------------------------------------------------------------------------

struct CallResult {
  CallStatus status = CallStatus::FAILED;
  int sip_code = 0;
  std::string reason;
  uint32_t billing_seconds = 0;
  std::string recording_path;
};

int statusToExitCode(CallStatus s);

// ---------------------------------------------------------------------------
// Disconnect mapping: SIP code -> CallStatus
// ---------------------------------------------------------------------------

CallStatus sipCodeToCallStatus(int sip_code);

// ---------------------------------------------------------------------------
// AgentEvent — emitted to the sink callback
// ---------------------------------------------------------------------------

struct AgentEvent {
  CallStatus status;
  std::string call_id;
  std::string recording_path;     // for RECORDING_READY / result
  uint32_t billing_seconds = 0;   // for result (or duration for RECORDING_READY)
  int sip_code = 0;
  std::string reason;

  std::string toJson() const;
};

// ---------------------------------------------------------------------------
// InternalEvent — pushed from PJSIP callbacks to the executor thread
// ---------------------------------------------------------------------------

enum class InternalType {
  REG_STATE,
  CALL_STATE,
  MEDIA_STATE,
  PLAYBACK_EOF,
  TIMEOUT,
  CANCEL,
  HANGUP_DONE
};

struct InternalEvent {
  InternalType type;
  bool connected = false;      // reg state
  int sip_code = 0;            // call status code / reg failure code
  std::string reason;
  int call_state = 0;          // pjsip_inv_state (PJSIP_INV_STATE_*)
  bool media_active = false;   // for MEDIA_STATE
  // TIMEOUT payload
  uint32_t timeout_id = 0;     // 0=reg timeout, 1=max_call_duration
};

// ---------------------------------------------------------------------------
// PJSIP call state constants (mirror pjsip_inv_state, no PJSIP include needed)
// ---------------------------------------------------------------------------

constexpr int INV_STATE_NULL         = 0;
constexpr int INV_STATE_CALLING      = 1;
constexpr int INV_STATE_EARLY        = 2;
constexpr int INV_STATE_CONNECTING   = 3;
constexpr int INV_STATE_CONFIRMED    = 4;
constexpr int INV_STATE_DISCONNECTED = 5;

#endif // EVENTS_H

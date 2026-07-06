#include "events.h"
#include <nlohmann/json.hpp>
#include <sstream>
#include <iomanip>
#include <chrono>

// ---------------------------------------------------------------------------
// callStatusToString
// ---------------------------------------------------------------------------

std::string callStatusToString(CallStatus s) {
    switch (s) {
        case CallStatus::REGISTERING:      return "REGISTERING";
        case CallStatus::REGISTERED:       return "REGISTERED";
        case CallStatus::DIALING:          return "DIALING";
        case CallStatus::RINGING:          return "RINGING";
        case CallStatus::ANSWERED:         return "ANSWERED";
        case CallStatus::PLAYING:          return "PLAYING";
        case CallStatus::PLAYED:           return "PLAYED";
        case CallStatus::SILENCE_DETECTED: return "SILENCE_DETECTED";
        case CallStatus::RECORDING_READY:  return "RECORDING_READY";
        case CallStatus::BUSY:             return "BUSY";
        case CallStatus::NO_ANSWER:        return "NO_ANSWER";
        case CallStatus::CANCELLED:        return "CANCELLED";
        case CallStatus::FAILED:           return "FAILED";
        case CallStatus::HANGING_UP:       return "HANGING_UP";
        case CallStatus::DISCONNECTED:     return "DISCONNECTED";
    }
    return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// statusToExitCode
// ---------------------------------------------------------------------------

int statusToExitCode(CallStatus s) {
    switch (s) {
        case CallStatus::ANSWERED:   return 0;
        case CallStatus::BUSY:       return 3;
        case CallStatus::NO_ANSWER:  return 4;
        case CallStatus::FAILED:     return 5;
        case CallStatus::CANCELLED:  return 6;
        default:                     return 1;
    }
}

// ---------------------------------------------------------------------------
// sipCodeToCallStatus  — disconnect mapping table
// ---------------------------------------------------------------------------

CallStatus sipCodeToCallStatus(int sip_code) {
    switch (sip_code) {
        case 200:                    return CallStatus::ANSWERED;
        case 486: case 600: case 603: return CallStatus::BUSY;
        case 408: case 480: case 487: return CallStatus::NO_ANSWER;
        default:                     return CallStatus::FAILED;
    }
}

// ---------------------------------------------------------------------------
// AgentEvent::toJson
// ---------------------------------------------------------------------------

static std::string isoNow() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) % 1000;
    std::tm tm;
    gmtime_r(&t, &tm);
    std::ostringstream os;
    os << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S.")
       << std::setfill('0') << std::setw(3) << ms.count()
       << "Z";
    return os.str();
}

std::string AgentEvent::toJson() const {
    nlohmann::json j;
    if (status == CallStatus::REGISTERING ||
        status == CallStatus::REGISTERED) {
        j["event"] = "state";
        j["state"] = callStatusToString(status);
        j["call_id"] = call_id;
        j["ts"] = isoNow();
    } else if (status == CallStatus::RECORDING_READY) {
        j["event"] = "recording_ready";
        j["uri"] = recording_path;
        j["duration"] = billing_seconds;
        j["call_id"] = call_id;
        j["ts"] = isoNow();
    } else if (status == CallStatus::ANSWERED ||
               status == CallStatus::BUSY ||
               status == CallStatus::NO_ANSWER ||
               status == CallStatus::CANCELLED ||
               status == CallStatus::FAILED ||
               status == CallStatus::DISCONNECTED) {
        j["event"] = "result";
        j["status"] = callStatusToString(status);
        j["sip_code"] = sip_code;
        if (!reason.empty()) j["reason"] = reason;
        j["billing_seconds"] = billing_seconds;
        if (!recording_path.empty()) j["recording"] = recording_path;
        j["call_id"] = call_id;
        j["ts"] = isoNow();
    } else {
        j["event"] = "state";
        j["state"] = callStatusToString(status);
        j["call_id"] = call_id;
        j["ts"] = isoNow();
    }
    return j.dump();
}

#pragma once

// Status sentences for the input panel. The shell owns every user-visible
// string: the Rust core reports a category and a raw description, and only
// what is defined here is ever shown.

#include <cstdint>
#include <string>

#include "ainput_core.h"

namespace ainput {

/// Folds line breaks into spaces, so one status is always one panel line.
inline std::string oneLine(const std::string &text) {
    std::string out = text;
    for (char &ch : out) {
        if (ch == '\n' || ch == '\r' || ch == '\t') {
            ch = ' ';
        }
    }
    return out;
}

/// Truncates to at most `limit` bytes without splitting a UTF-8 character.
inline std::string truncate(const std::string &text, std::size_t limit) {
    if (text.size() <= limit) {
        return text;
    }
    std::size_t end = limit;
    // A continuation byte (10xxxxxx) at the cut means the character in front
    // of it is incomplete; step back to its leading byte.
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) {
        --end;
    }
    return text.substr(0, end) + "...";
}

/// One actionable sentence per error category, i.e. per terminal outcome
/// without text. Every one of them goes to the input panel, or to the
/// desktop notification when a running status owns the panel.
///
/// "Nothing was transcribed" says where that was decided, because the three
/// cases call for different reactions:
///   - the local gate rejected the recording, so it was never sent;
///   - the backend processed the audio and found no speech;
///   - the backend did not answer at all (network error or timeout).
inline std::string statusForError(int32_t category, const std::string &detail) {
    switch (category) {
    case AINPUT_ERR_CONFIG_MISSING:
    case AINPUT_ERR_CONFIG_INVALID:
    case AINPUT_ERR_AUDIO_DATA:
        return "Ainput: " + detail;
    case AINPUT_ERR_AUTH_REJECTED:
        return "Ainput: API key rejected; check provider config";
    case AINPUT_ERR_MODEL_NOT_FOUND:
        return "Ainput: model not found; check provider config";
    case AINPUT_ERR_LOCAL_SETUP:
        return "Ainput: local engine setup error; " + detail;
    case AINPUT_ERR_NETWORK:
        return "Ainput: ASR backend did not respond (network error); try again";
    case AINPUT_ERR_SERVICE_UNAVAILABLE:
        return "Ainput: recognition service unavailable; try again";
    case AINPUT_ERR_TIMEOUT:
        return "Ainput: ASR backend did not respond (recognition timed out); try again";
    case AINPUT_ERR_NO_SPEECH:
        return "Ainput: no speech detected locally; audio was not sent";
    case AINPUT_ERR_EMPTY_RESULT:
        return "Ainput: no speech recognized by the ASR backend";
    case AINPUT_ERR_INVALID_REQUEST:
    case AINPUT_ERR_RUNTIME:
    case AINPUT_ERR_UNKNOWN:
    default:
        return "Ainput: " + truncate(oneLine(detail), 120);
    }
}

} // namespace ainput

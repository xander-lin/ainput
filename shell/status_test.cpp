#include "status.h"

#include <cstdlib>
#include <initializer_list>
#include <string>

namespace {

void check(bool condition) {
    if (!condition) std::abort();
}

bool contains(const std::string &text, const char *needle) {
    return text.find(needle) != std::string::npos;
}

std::string repeat(const std::string &unit, std::size_t count) {
    std::string out;
    for (std::size_t index = 0; index < count; ++index) out += unit;
    return out;
}

} // namespace

int main() {
    // "Nothing was transcribed" says where that was decided.
    const std::string local =
        ainput::statusForError(AINPUT_ERR_NO_SPEECH, "no speech detected");
    const std::string backend = ainput::statusForError(
        AINPUT_ERR_EMPTY_RESULT, "Qwen HTTP 400 CLIENT_ERROR: ASR_RESPONSE_HAVE_NO_WORDS");
    check(contains(local, "locally") && contains(local, "not sent"));
    check(contains(backend, "ASR backend"));
    // The vendor's raw reply stays out of the panel; the journal keeps it.
    check(!contains(backend, "HTTP 400"));
    check(local != backend);

    // A backend that never answered is distinct from one that answered.
    const std::string network = ainput::statusForError(AINPUT_ERR_NETWORK, "");
    const std::string timeout = ainput::statusForError(AINPUT_ERR_TIMEOUT, "");
    check(contains(network, "did not respond") && contains(network, "network error"));
    check(contains(timeout, "did not respond") && contains(timeout, "recognition timed out"));
    check(network != timeout && network != backend && network != local);

    // Only "nothing to transcribe" is kept out of the desktop notification.
    check(ainput::isNoResult(AINPUT_ERR_NO_SPEECH));
    check(ainput::isNoResult(AINPUT_ERR_EMPTY_RESULT));
    for (const int32_t category :
         {AINPUT_ERR_CONFIG_MISSING, AINPUT_ERR_CONFIG_INVALID, AINPUT_ERR_AUTH_REJECTED,
          AINPUT_ERR_MODEL_NOT_FOUND, AINPUT_ERR_LOCAL_SETUP, AINPUT_ERR_INVALID_REQUEST,
          AINPUT_ERR_NETWORK, AINPUT_ERR_SERVICE_UNAVAILABLE, AINPUT_ERR_TIMEOUT,
          AINPUT_ERR_AUDIO_DATA, AINPUT_ERR_RUNTIME, AINPUT_ERR_UNKNOWN}) {
        check(!ainput::isNoResult(category));
    }

    // Every category has exactly one prefixed sentence.
    for (int32_t category = AINPUT_ERR_CONFIG_MISSING; category <= AINPUT_ERR_UNKNOWN;
         ++category) {
        check(ainput::statusForError(category, "detail").rfind("Ainput: ", 0) == 0);
    }
    for (const int32_t category :
         {AINPUT_ERR_INVALID_REQUEST, AINPUT_ERR_RUNTIME, AINPUT_ERR_UNKNOWN}) {
        check(ainput::statusForError(category, "first\nsecond\r\tthird") ==
              "Ainput: first second  third");
    }

    // A long detail is cut at a character boundary, never inside one.
    const std::string zhong = "\xe4\xb8\xad";
    const std::string raw =
        ainput::statusForError(AINPUT_ERR_UNKNOWN, "a" + repeat(zhong, 45));
    check(raw == "Ainput: a" + repeat(zhong, 39) + "...");
    check(ainput::truncate("short", 120) == "short");
}

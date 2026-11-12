// macOS and Linux: keys are only obfuscated for now. TODO: the Keychain on macOS
// and libsecret on Linux.

#include "core/platform.h"
#include "core/util.h"

std::string ProtectSecret(const std::string& plain) {
    if (plain.empty()) return std::string();
    return "plain:" + Base64Encode(plain);
}

std::string UnprotectSecret(const std::string& stored) {
    if (StartsWith(stored, "plain:")) return Base64Decode(stored.substr(6));
    return stored;
}

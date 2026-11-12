// API keys encrypted for the current Windows user with DPAPI. The settings file
// holds "dpapi:<base64>", which only this user on this machine can decrypt.

#include "core/platform.h"
#include "core/util.h"

#include <windows.h>
#include <wincrypt.h>

std::string ProtectSecret(const std::string& plain) {
    if (plain.empty()) return std::string();
    DATA_BLOB in{static_cast<DWORD>(plain.size()), reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()))};
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"FastChat API key", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        return "plain:" + Base64Encode(plain);
    }
    std::string blob(reinterpret_cast<char*>(out.pbData), out.cbData);
    LocalFree(out.pbData);
    return "dpapi:" + Base64Encode(blob);
}

std::string UnprotectSecret(const std::string& stored) {
    if (stored.empty()) return std::string();
    if (StartsWith(stored, "plain:")) return Base64Decode(stored.substr(6));
    if (!StartsWith(stored, "dpapi:")) return stored;  // written by hand
    std::string blob = Base64Decode(stored.substr(6));
    DATA_BLOB in{static_cast<DWORD>(blob.size()), reinterpret_cast<BYTE*>(blob.data())};
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        return std::string();
    }
    std::string plain(reinterpret_cast<char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return plain;
}

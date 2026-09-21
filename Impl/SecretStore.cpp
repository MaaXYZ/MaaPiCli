#include "SecretStore.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string_view>
#include <system_error>
#include <vector>

#include "MaaUtils/Logger.h"

#if defined(_WIN32)
#include "MaaUtils/SafeWindows.hpp"
#include <wincrypt.h>
#elif defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#include <Security/Security.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/evp.h>
#include <openssl/rand.h>
#endif

MAA_PROJECT_INTERFACE_NS_BEGIN

namespace
{
constexpr std::string_view kWindowsPrefix = "dpapi:";
constexpr std::string_view kKeychainPrefix = "keychain:";
constexpr std::string_view kAesGcmPrefix = "gcm:";
constexpr std::string_view kServiceName = "MaaPiCli";

#if defined(_WIN32)
constexpr std::string_view kCurrentPlatformPrefix = kWindowsPrefix;
#elif defined(__APPLE__)
constexpr std::string_view kCurrentPlatformPrefix = kKeychainPrefix;
#else
constexpr std::string_view kCurrentPlatformPrefix = kAesGcmPrefix;
#endif

bool starts_with(const std::string& value, std::string_view prefix)
{
    return value.starts_with(prefix);
}

std::vector<std::uint8_t> to_bytes(const std::string& value)
{
    return { value.begin(), value.end() };
}

std::string to_string(const std::vector<std::uint8_t>& bytes)
{
    return { bytes.begin(), bytes.end() };
}

std::string with_prefix(std::string_view prefix, const std::vector<std::uint8_t>& bytes)
{
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result(prefix);
    result.reserve(result.size() + (bytes.size() + 2) / 3 * 4);

    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const std::uint32_t byte0 = bytes[i];
        const std::uint32_t byte1 = i + 1 < bytes.size() ? bytes[i + 1] : 0;
        const std::uint32_t byte2 = i + 2 < bytes.size() ? bytes[i + 2] : 0;
        const std::uint32_t triple = (byte0 << 16) | (byte1 << 8) | byte2;

        result.push_back(alphabet[(triple >> 18) & 0x3f]);
        result.push_back(alphabet[(triple >> 12) & 0x3f]);
        result.push_back(i + 1 < bytes.size() ? alphabet[(triple >> 6) & 0x3f] : '=');
        result.push_back(i + 2 < bytes.size() ? alphabet[triple & 0x3f] : '=');
    }
    return result;
}

std::optional<std::vector<std::uint8_t>> decode_after_prefix(const std::string& value, std::string_view prefix)
{
    if (!starts_with(value, prefix)) {
        return std::nullopt;
    }

    const std::string_view encoded(value.data() + prefix.size(), value.size() - prefix.size());
    if (encoded.empty() || encoded.size() % 4 != 0) {
        return std::nullopt;
    }

    auto digit = [](char ch) -> std::optional<std::uint8_t> {
        if (ch >= 'A' && ch <= 'Z') {
            return ch - 'A';
        }
        if (ch >= 'a' && ch <= 'z') {
            return ch - 'a' + 26;
        }
        if (ch >= '0' && ch <= '9') {
            return ch - '0' + 52;
        }
        if (ch == '+') {
            return 62;
        }
        if (ch == '/') {
            return 63;
        }
        return std::nullopt;
    };

    std::vector<std::uint8_t> bytes;
    bytes.reserve(encoded.size() / 4 * 3);
    for (std::size_t i = 0; i < encoded.size(); i += 4) {
        const auto d0 = digit(encoded[i]);
        const auto d1 = digit(encoded[i + 1]);
        const bool padding1 = encoded[i + 2] == '=';
        const bool padding2 = encoded[i + 3] == '=';
        const auto d2 = padding1 ? std::optional<std::uint8_t>(0) : digit(encoded[i + 2]);
        const auto d3 = padding2 ? std::optional<std::uint8_t>(0) : digit(encoded[i + 3]);

        if (!d0 || !d1 || !d2 || !d3 || (padding1 && !padding2)) {
            return std::nullopt;
        }

        const std::uint32_t triple = (*d0 << 18) | (*d1 << 12) | (*d2 << 6) | *d3;
        bytes.push_back(static_cast<std::uint8_t>((triple >> 16) & 0xff));
        if (!padding1) {
            bytes.push_back(static_cast<std::uint8_t>((triple >> 8) & 0xff));
        }
        if (!padding2) {
            bytes.push_back(static_cast<std::uint8_t>(triple & 0xff));
        }
    }
    return bytes;
}

#if defined(_WIN32)

DATA_BLOB entropy_blob(const std::string& context)
{
    DATA_BLOB entropy { };
    entropy.cbData = static_cast<DWORD>(context.size());
    entropy.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(context.data()));
    return entropy;
}

std::optional<std::vector<std::uint8_t>> protect(const std::string& context, const std::vector<std::uint8_t>& bytes)
{
    DATA_BLOB input { };
    auto entropy = entropy_blob(context);
    input.cbData = static_cast<DWORD>(bytes.size());
    input.pbData = reinterpret_cast<BYTE*>(const_cast<std::uint8_t*>(bytes.data()));

    DATA_BLOB output { };
    if (!CryptProtectData(&input, L"MaaPiCli password", &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        return std::nullopt;
    }

    std::vector<std::uint8_t> result(output.pbData, output.pbData + output.cbData);
    LocalFree(output.pbData);
    return result;
}

std::optional<std::vector<std::uint8_t>> unprotect(const std::string& context, const std::vector<std::uint8_t>& bytes)
{
    DATA_BLOB input { };
    auto entropy = entropy_blob(context);
    input.cbData = static_cast<DWORD>(bytes.size());
    input.pbData = reinterpret_cast<BYTE*>(const_cast<std::uint8_t*>(bytes.data()));

    DATA_BLOB output { };
    if (!CryptUnprotectData(&input, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        return std::nullopt;
    }

    std::vector<std::uint8_t> result(output.pbData, output.pbData + output.cbData);
    LocalFree(output.pbData);
    return result;
}

#elif defined(__APPLE__)

std::string keychain_account(const std::string& context)
{
    std::array<std::uint8_t, CC_SHA256_DIGEST_LENGTH> digest { };
    CC_SHA256(context.data(), static_cast<CC_LONG>(context.size()), digest.data());

    static constexpr char hex[] = "0123456789abcdef";
    std::string account;
    account.reserve(digest.size() * 2);
    for (const auto byte : digest) {
        account.push_back(hex[byte >> 4]);
        account.push_back(hex[byte & 0x0f]);
    }
    return account;
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

// SecItem does not reliably read file-backed keychains used by CI. The legacy
// generic-password API explicitly addresses the current default keychain.
bool keychain_store(const std::string& account, const std::vector<std::uint8_t>& value)
{
    SecKeychainRef keychain = nullptr;
    if (SecKeychainCopyDefault(&keychain) != errSecSuccess) {
        return false;
    }

    UInt32 length = 0;
    void* data = nullptr;
    SecKeychainItemRef item = nullptr;
    OSStatus status = SecKeychainFindGenericPassword(
        keychain,
        kServiceName.size(),
        kServiceName.data(),
        account.size(),
        account.data(),
        &length,
        &data,
        &item);
    if (status == errSecSuccess) {
        status = SecKeychainItemModifyAttributesAndData(item, nullptr, static_cast<UInt32>(value.size()), value.data());
        SecKeychainItemFreeContent(nullptr, data);
    }
    else if (status == errSecItemNotFound) {
        status = SecKeychainAddGenericPassword(
            keychain,
            kServiceName.size(),
            kServiceName.data(),
            account.size(),
            account.data(),
            static_cast<UInt32>(value.size()),
            value.data(),
            &item);
    }

    if (item) {
        CFRelease(item);
    }
    CFRelease(keychain);
    if (status != errSecSuccess) {
        LogError << "Failed to store keychain item" << VAR(status);
    }
    return status == errSecSuccess;
}

std::optional<std::vector<std::uint8_t>> keychain_load(const std::string& account)
{
    if (account.size() != 64) {
        return std::nullopt;
    }

    SecKeychainRef keychain = nullptr;
    if (SecKeychainCopyDefault(&keychain) != errSecSuccess) {
        return std::nullopt;
    }

    UInt32 length = 0;
    void* data = nullptr;
    const OSStatus status = SecKeychainFindGenericPassword(
        keychain,
        kServiceName.size(),
        kServiceName.data(),
        account.size(),
        account.data(),
        &length,
        &data,
        nullptr);
    CFRelease(keychain);
    if (status != errSecSuccess || !data) {
        LogError << "Failed to load keychain item" << VAR(status);
        return std::nullopt;
    }

    const auto bytes = static_cast<const std::uint8_t*>(data);
    std::vector<std::uint8_t> value(bytes, bytes + length);
    SecKeychainItemFreeContent(nullptr, data);
    return value;
}

#pragma clang diagnostic pop

#else

std::filesystem::path master_key_path()
{
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        return std::filesystem::path(xdg) / "MaaPiCli" / "master.key";
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".config" / "MaaPiCli" / "master.key";
    }
    return { };
}

std::optional<std::vector<std::uint8_t>> read_or_create_key(bool allow_create)
{
    constexpr std::size_t key_size = 32;
    const auto path = master_key_path();
    if (path.empty()) {
        return std::nullopt;
    }

    std::ifstream input(path, std::ios::binary);
    if (input.is_open()) {
        const std::vector<std::uint8_t> key { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
        if (key.size() == key_size) {
            return key;
        }
        LogError << "Invalid password master key" << VAR(path);
        return std::nullopt;
    }

    if (allow_create) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            LogError << "Failed to create password key directory" << VAR(path);
            return std::nullopt;
        }

        std::vector<std::uint8_t> key(key_size);
        if (RAND_bytes(key.data(), static_cast<int>(key.size())) != 1) {
            LogError << "Failed to generate password master key";
            return std::nullopt;
        }

        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0) {
            LogError << "Failed to create password master key" << VAR(path);
            return std::nullopt;
        }

        const ssize_t written = ::write(fd, key.data(), key.size());
        ::close(fd);
        if (written != static_cast<ssize_t>(key.size())) {
            LogError << "Failed to write password master key" << VAR(path);
            return std::nullopt;
        }
        return key;
    }

    LogError << "Password master key not found" << VAR(path);
    return std::nullopt;
}

std::optional<std::vector<std::uint8_t>>
    aes_gcm_encrypt(const std::vector<std::uint8_t>& key, const std::string& context, const std::string& value)
{
    std::vector<std::uint8_t> iv(12);
    if (RAND_bytes(iv.data(), static_cast<int>(iv.size())) != 1) {
        return std::nullopt;
    }

    EVP_CIPHER_CTX* cipher_context = EVP_CIPHER_CTX_new();
    if (!cipher_context) {
        return std::nullopt;
    }

    const auto plaintext = to_bytes(value);
    std::vector<std::uint8_t> encrypted(plaintext.size());
    int associated_length = 0;
    int length = 0;
    int final_length = 0;
    bool ok =
        EVP_EncryptInit_ex(cipher_context, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1
        && EVP_CIPHER_CTX_ctrl(cipher_context, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(iv.size()), nullptr) == 1
        && EVP_EncryptInit_ex(cipher_context, nullptr, nullptr, key.data(), iv.data()) == 1
        && EVP_EncryptUpdate(
               cipher_context,
               nullptr,
               &associated_length,
               reinterpret_cast<const std::uint8_t*>(context.data()),
               static_cast<int>(context.size()))
               == 1
        && (plaintext.empty() || EVP_EncryptUpdate(cipher_context, encrypted.data(), &length, plaintext.data(), plaintext.size()) == 1)
        && EVP_EncryptFinal_ex(cipher_context, encrypted.data() + length, &final_length) == 1;

    std::array<std::uint8_t, 16> tag { };
    ok = ok && EVP_CIPHER_CTX_ctrl(cipher_context, EVP_CTRL_GCM_GET_TAG, tag.size(), tag.data()) == 1;
    EVP_CIPHER_CTX_free(cipher_context);
    if (!ok || final_length != 0) {
        return std::nullopt;
    }

    encrypted.resize(length);
    std::vector<std::uint8_t> result;
    result.reserve(iv.size() + tag.size() + encrypted.size());
    result.insert(result.end(), iv.begin(), iv.end());
    result.insert(result.end(), tag.begin(), tag.end());
    result.insert(result.end(), encrypted.begin(), encrypted.end());
    return result;
}

std::optional<std::vector<std::uint8_t>>
    aes_gcm_decrypt(const std::vector<std::uint8_t>& key, const std::string& context, const std::vector<std::uint8_t>& payload)
{
    constexpr std::size_t iv_size = 12;
    constexpr std::size_t tag_size = 16;
    if (payload.size() < iv_size + tag_size) {
        return std::nullopt;
    }

    std::vector<std::uint8_t> iv(payload.begin(), payload.begin() + iv_size);
    std::array<std::uint8_t, tag_size> tag { };
    std::copy_n(payload.begin() + iv_size, tag_size, tag.begin());
    const std::vector<std::uint8_t> encrypted(payload.begin() + iv_size + tag_size, payload.end());

    EVP_CIPHER_CTX* cipher_context = EVP_CIPHER_CTX_new();
    if (!cipher_context) {
        return std::nullopt;
    }

    std::vector<std::uint8_t> decrypted(encrypted.size());
    int associated_length = 0;
    int length = 0;
    int final_length = 0;
    bool ok =
        EVP_DecryptInit_ex(cipher_context, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1
        && EVP_CIPHER_CTX_ctrl(cipher_context, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(iv.size()), nullptr) == 1
        && EVP_DecryptInit_ex(cipher_context, nullptr, nullptr, key.data(), iv.data()) == 1
        && EVP_DecryptUpdate(
               cipher_context,
               nullptr,
               &associated_length,
               reinterpret_cast<const std::uint8_t*>(context.data()),
               static_cast<int>(context.size()))
               == 1
        && (encrypted.empty() || EVP_DecryptUpdate(cipher_context, decrypted.data(), &length, encrypted.data(), encrypted.size()) == 1)
        && EVP_CIPHER_CTX_ctrl(cipher_context, EVP_CTRL_GCM_SET_TAG, tag_size, tag.data()) == 1
        && EVP_DecryptFinal_ex(cipher_context, decrypted.data() + length, &final_length) == 1;
    EVP_CIPHER_CTX_free(cipher_context);
    if (!ok || final_length != 0) {
        return std::nullopt;
    }

    decrypted.resize(length);
    return decrypted;
}

#endif

std::string encrypt_value(const std::string& context, const std::string& value)
{
#if defined(_WIN32)
    const auto encrypted = protect(context, to_bytes(value));
    if (!encrypted) {
        return { };
    }
    return with_prefix(kWindowsPrefix, *encrypted);
#elif defined(__APPLE__)
    const auto account = keychain_account(context);
    if (!keychain_store(account, to_bytes(value))) {
        return { };
    }
    return std::string(kKeychainPrefix) + account;
#else
    const auto key = read_or_create_key(true);
    const auto encrypted = key ? aes_gcm_encrypt(*key, context, value) : std::nullopt;
    if (!encrypted) {
        return { };
    }
    return with_prefix(kAesGcmPrefix, *encrypted);
#endif
}

std::string decrypt_value(const std::string& context, const std::string& stored)
{
#if defined(_WIN32)
    const auto payload = decode_after_prefix(stored, kWindowsPrefix);
    const auto decrypted = payload ? unprotect(context, *payload) : std::nullopt;
    if (!decrypted) {
        return { };
    }
    return to_string(*decrypted);
#elif defined(__APPLE__)
    if (!starts_with(stored, kKeychainPrefix)) {
        return { };
    }
    const auto decrypted = keychain_load(stored.substr(kKeychainPrefix.size()));
    if (!decrypted) {
        return { };
    }
    return to_string(*decrypted);
#else
    const auto payload = decode_after_prefix(stored, kAesGcmPrefix);
    const auto key = payload ? read_or_create_key(false) : std::nullopt;
    const auto decrypted = key ? aes_gcm_decrypt(*key, context, *payload) : std::nullopt;
    if (!decrypted) {
        return { };
    }
    return to_string(*decrypted);
#endif
}

} // namespace

std::string SecretStore::encrypt(const std::string& context, const std::string& value)
{
    if (value.empty()) {
        return value;
    }

    auto encrypted = encrypt_value(context, value);
    if (encrypted.empty()) {
        LogError << "Failed to encrypt password";
        return { };
    }
    return encrypted;
}

std::string SecretStore::decrypt(const std::string& context, const std::string& value)
{
    if (value.empty()) {
        return value;
    }

    for (const auto prefix : { kWindowsPrefix, kKeychainPrefix, kAesGcmPrefix }) {
        if (!starts_with(value, prefix)) {
            continue;
        }
        if (prefix != kCurrentPlatformPrefix) {
            LogError << "Password was encrypted on another platform";
            return { };
        }
        break;
    }

    auto decrypted = decrypt_value(context, value);
    if (decrypted.empty()) {
        LogError << "Failed to decrypt password";
        return { };
    }
    return decrypted;
}

MAA_PROJECT_INTERFACE_NS_END

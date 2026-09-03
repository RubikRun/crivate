#include "crypto.h"

#include <limits>
#include <vector>

#include <windows.h>
#include <bcrypt.h>

namespace {

struct AlgHandle {
    BCRYPT_ALG_HANDLE h = nullptr;

    AlgHandle() = default;
    AlgHandle(const AlgHandle&) = delete;
    AlgHandle& operator=(const AlgHandle&) = delete;

    ~AlgHandle() {
        if (h != nullptr) {
            BCryptCloseAlgorithmProvider(h, 0);
            h = nullptr;
        }
    }
};

struct KeyHandle {
    BCRYPT_KEY_HANDLE h = nullptr;
    std::vector<uint8_t> obj;

    KeyHandle() = default;
    KeyHandle(const KeyHandle&) = delete;
    KeyHandle& operator=(const KeyHandle&) = delete;

    ~KeyHandle() {
        if (h != nullptr) {
            BCryptDestroyKey(h);
            h = nullptr;
        }
        if (!obj.empty()) {
            SecureZeroMemory(obj.data(), obj.size());
            obj.clear();
        }
    }
};

bool open_aes_gcm_key(const AesKey& key, AlgHandle* alg, KeyHandle* key_handle) {
    NTSTATUS st = BCryptOpenAlgorithmProvider(&alg->h, BCRYPT_AES_ALGORITHM, nullptr, 0);
    if (!BCRYPT_SUCCESS(st)) {
        return false;
    }

    st = BCryptSetProperty(alg->h, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM,
                           sizeof(BCRYPT_CHAIN_MODE_GCM), 0);
    if (!BCRYPT_SUCCESS(st)) {
        return false;
    }

    DWORD obj_len = 0;
    DWORD cb = 0;
    st = BCryptGetProperty(alg->h, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&obj_len),
                           sizeof(obj_len), &cb, 0);
    if (!BCRYPT_SUCCESS(st)) {
        return false;
    }

    key_handle->obj.assign(obj_len, 0);
    PUCHAR obj_ptr = obj_len == 0 ? nullptr : key_handle->obj.data();

    st = BCryptGenerateSymmetricKey(alg->h, &key_handle->h, obj_ptr, obj_len,
                                    const_cast<PUCHAR>(key.bytes),
                                    static_cast<ULONG>(kAesKeyBytes), 0);
    return BCRYPT_SUCCESS(st);
}

bool gcm_crypt(bool encrypt, const AesKey& key, const uint8_t nonce[kGcmNonceBytes],
               const uint8_t* aad, size_t aad_len, const uint8_t* input, size_t len,
               uint8_t* output, uint8_t tag[kGcmTagBytes]) {
    if (nonce == nullptr || tag == nullptr) {
        return false;
    }
    if (len > 0 && (input == nullptr || output == nullptr)) {
        return false;
    }
    if (aad_len > 0 && aad == nullptr) {
        return false;
    }
    if (len > (std::numeric_limits<ULONG>::max)() ||
        aad_len > (std::numeric_limits<ULONG>::max)()) {
        return false;
    }

    AlgHandle alg;
    KeyHandle key_handle;
    if (!open_aes_gcm_key(key, &alg, &key_handle)) {
        return false;
    }

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = const_cast<PUCHAR>(nonce);
    info.cbNonce = static_cast<ULONG>(kGcmNonceBytes);
    info.pbAuthData = aad_len == 0 ? nullptr : const_cast<PUCHAR>(aad);
    info.cbAuthData = static_cast<ULONG>(aad_len);
    info.pbTag = tag;
    info.cbTag = static_cast<ULONG>(kGcmTagBytes);

    ULONG cb_result = 0;
    const NTSTATUS st = encrypt
        ? BCryptEncrypt(key_handle.h, const_cast<PUCHAR>(input), static_cast<ULONG>(len),
                        &info, nullptr, 0, output, static_cast<ULONG>(len), &cb_result, 0)
        : BCryptDecrypt(key_handle.h, const_cast<PUCHAR>(input), static_cast<ULONG>(len),
                        &info, nullptr, 0, output, static_cast<ULONG>(len), &cb_result, 0);

    if (!BCRYPT_SUCCESS(st) || cb_result != len) {
        if (!encrypt && output != nullptr && len > 0) {
            SecureZeroMemory(output, len);
        }
        return false;
    }
    return true;
}

}  // namespace

void crypto_wipe(void* p, size_t n) {
    if (p == nullptr || n == 0) {
        return;
    }
    SecureZeroMemory(p, n);
}

void crypto_wipe_key(AesKey* key) {
    if (key == nullptr) {
        return;
    }
    SecureZeroMemory(key->bytes, sizeof(key->bytes));
}

bool crypto_random(void* buf, size_t n) {
    if (buf == nullptr || n == 0 || n > (std::numeric_limits<ULONG>::max)()) {
        return false;
    }
    const NTSTATUS st = BCryptGenRandom(nullptr, static_cast<PUCHAR>(buf), static_cast<ULONG>(n),
                                        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return BCRYPT_SUCCESS(st);
}

bool crypto_derive_key(const uint8_t* password, size_t password_len,
                       const uint8_t* salt, size_t salt_len,
                       uint32_t iterations, AesKey* out_key) {
    if (out_key == nullptr || password == nullptr || salt == nullptr) {
        return false;
    }
    if (password_len == 0 || salt_len == 0 || iterations == 0) {
        return false;
    }
    if (password_len > (std::numeric_limits<ULONG>::max)() ||
        salt_len > (std::numeric_limits<ULONG>::max)()) {
        return false;
    }

    crypto_wipe_key(out_key);

    AlgHandle prf;
    NTSTATUS st = BCryptOpenAlgorithmProvider(&prf.h, BCRYPT_SHA256_ALGORITHM, nullptr,
                                              BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (!BCRYPT_SUCCESS(st)) {
        return false;
    }

    st = BCryptDeriveKeyPBKDF2(prf.h, const_cast<PUCHAR>(password),
                               static_cast<ULONG>(password_len),
                               const_cast<PUCHAR>(salt), static_cast<ULONG>(salt_len),
                               iterations, out_key->bytes, static_cast<ULONG>(kAesKeyBytes), 0);
    if (!BCRYPT_SUCCESS(st)) {
        crypto_wipe_key(out_key);
        return false;
    }
    return true;
}

bool crypto_gcm_encrypt(const AesKey& key, const uint8_t nonce[kGcmNonceBytes],
                        const uint8_t* aad, size_t aad_len,
                        const uint8_t* plaintext, size_t plaintext_len,
                        uint8_t* ciphertext, uint8_t tag[kGcmTagBytes]) {
    return gcm_crypt(true, key, nonce, aad, aad_len, plaintext, plaintext_len, ciphertext, tag);
}

bool crypto_gcm_decrypt(const AesKey& key, const uint8_t nonce[kGcmNonceBytes],
                        const uint8_t* aad, size_t aad_len,
                        const uint8_t* ciphertext, size_t ciphertext_len,
                        const uint8_t tag[kGcmTagBytes], uint8_t* plaintext) {
    return gcm_crypt(false, key, nonce, aad, aad_len,
                     ciphertext, ciphertext_len, plaintext, const_cast<uint8_t*>(tag));
}

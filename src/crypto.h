#pragma once

#include <cstddef>
#include <cstdint>

constexpr size_t kAesKeyBytes = 32;
constexpr size_t kGcmNonceBytes = 12;
constexpr size_t kGcmTagBytes = 16;
constexpr size_t kSaltBytes = 16;

struct AesKey {
    uint8_t bytes[kAesKeyBytes]{};
};

void crypto_wipe(void* p, size_t n);
void crypto_wipe_key(AesKey* key);

bool crypto_random(void* buf, size_t n);

bool crypto_derive_key(const uint8_t* password, size_t password_len,
                       const uint8_t* salt, size_t salt_len,
                       uint32_t iterations, AesKey* out_key);

bool crypto_gcm_encrypt(const AesKey& key,
                        const uint8_t nonce[kGcmNonceBytes],
                        const uint8_t* aad, size_t aad_len,
                        const uint8_t* plaintext, size_t plaintext_len,
                        uint8_t* ciphertext,
                        uint8_t tag[kGcmTagBytes]);

bool crypto_gcm_decrypt(const AesKey& key,
                        const uint8_t nonce[kGcmNonceBytes],
                        const uint8_t* aad, size_t aad_len,
                        const uint8_t* ciphertext, size_t ciphertext_len,
                        const uint8_t tag[kGcmTagBytes],
                        uint8_t* plaintext);

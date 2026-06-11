#pragma once

// PBKDF2-HMAC-SHA256 password hashing for identity login.
//
// Reticulum's libcrypto already includes HMAC-SHA256, so PBKDF2 is just
// a chained-HMAC loop on top. We use a single 32-byte block (DK length
// = hash length) so the outer chain of PBKDF2 collapses to:
//
//   U_1 = HMAC(password, salt || 0x00000001)
//   U_i = HMAC(password, U_{i-1})
//   T   = U_1 XOR U_2 XOR ... XOR U_n
//
// where n = iterations. The result is the password hash; salt is per-
// identity, generated at creation, and stored alongside the hash.
//
// Iteration count is intentionally large enough to make brute-forcing
// slow on a desktop attacker, but bounded so login on the ESP32-S3
// stays sub-second.

#include <Cryptography/HMAC.h>
#include <Cryptography/Random.h>
#include <Bytes.h>

#include <string>
#include <stdint.h>

namespace Web {

  class PasswordHash {
  public:
    static constexpr uint32_t ITERATIONS = 20000;
    static constexpr size_t   SALT_BYTES = 16;
    static constexpr size_t   HASH_BYTES = 32;  // SHA-256 output
    static constexpr size_t   MIN_PASSWORD_LEN = 8;

    // Produce a random 16-byte salt suitable for use with derive().
    static RNS::Bytes new_salt() {
      return RNS::Cryptography::random(SALT_BYTES);
    }

    // Derive a 32-byte hash from password + salt. Iteration count fixed
    // at ITERATIONS - change requires re-hashing every identity, so it's
    // effectively a one-shot decision.
    static RNS::Bytes derive(const std::string& password, const RNS::Bytes& salt) {
      RNS::Bytes pw_bytes((const uint8_t*)password.c_str(), password.length());

      // U_1 = HMAC(password, salt || INT_BE_32(1))
      RNS::Bytes salt_idx;
      salt_idx.append(salt.data(), salt.size());
      const uint8_t idx[4] = { 0x00, 0x00, 0x00, 0x01 };
      salt_idx.append(idx, 4);

      RNS::Cryptography::HMAC h1(pw_bytes);
      h1.update(salt_idx);
      RNS::Bytes U = h1.digest();
      RNS::Bytes T = U;

      for (uint32_t i = 1; i < ITERATIONS; ++i) {
        RNS::Cryptography::HMAC h(pw_bytes);
        h.update(U);
        U = h.digest();
        // T ^= U
        for (size_t j = 0; j < T.size() && j < U.size(); ++j) {
          T[j] ^= U[j];
        }
      }
      return T;
    }

    // Constant-time comparison of two hashes. Both should be HASH_BYTES.
    static bool verify(const std::string& candidate_password,
                       const RNS::Bytes& salt,
                       const RNS::Bytes& expected_hash) {
      if (expected_hash.size() != HASH_BYTES) return false;
      if (salt.size() != SALT_BYTES) return false;
      RNS::Bytes got = derive(candidate_password, salt);
      if (got.size() != expected_hash.size()) return false;
      uint8_t diff = 0;
      for (size_t i = 0; i < got.size(); ++i) {
        diff |= got[i] ^ expected_hash[i];
      }
      return diff == 0;
    }
  };

}

#pragma once

// Per-identity X25519 ratchet ring.
//
// Reticulum's ratchet model: each announce advertises a fresh X25519 public
// key. Senders encrypt to that pubkey. To stay decryptable for messages
// already in flight when we rotate, we keep a window of the last N ratchet
// privkeys.  On decrypt we try them newest-first, falling back to the
// static identity key only if every ratchet fails.
//
// Persistence is a tiny binary file at /lxmf/identities/<id>/ratchet.dat:
//   magic(4)  version(1)  count(1)
//   for each entry: created_ms(8 LE) privkey(32) pubkey(32)
//
// All cryptography goes through RNS::Cryptography::X25519PrivateKey so the
// same X25519 implementation that the rest of microReticulum uses is in
// effect here — no separate code paths.

#include <Bytes.h>
#include <Cryptography/X25519.h>
#include <microStore/FileSystem.h>

#include <stdint.h>
#include <string>
#include <vector>

#ifndef LXMF_RATCHET_RING_MAX
#define LXMF_RATCHET_RING_MAX 16
#endif

namespace LXMF {

  class RatchetStore {
  public:
    struct Entry {
      uint64_t created_ms = 0;
      RNS::Bytes privkey;  // 32 bytes
      RNS::Bytes pubkey;   // 32 bytes
    };

    static constexpr size_t RATCHET_BYTES = 32;
    static constexpr uint32_t FILE_MAGIC  = 0x52415443;  // "RATC"
    static constexpr uint8_t  FILE_VERSION = 1;

    // Generate a fresh keypair, push to the back of the ring, drop the
    // oldest if we're over LXMF_RATCHET_RING_MAX. Returns a const ref to
    // the new entry (which is at back()).
    const Entry& rotate(uint64_t now_ms) {
      auto prv = RNS::Cryptography::X25519PrivateKey::generate();
      Entry e;
      e.created_ms = now_ms;
      e.privkey = prv->private_bytes();
      e.pubkey  = prv->public_key()->public_bytes();
      _entries.push_back(std::move(e));
      while (_entries.size() > LXMF_RATCHET_RING_MAX) {
        _entries.erase(_entries.begin());
      }
      return _entries.back();
    }

    bool empty() const { return _entries.empty(); }
    size_t size() const { return _entries.size(); }
    const Entry& back() const { return _entries.back(); }

    // index 0 = newest, increasing back in time.
    const Entry* at_newest_first(size_t index) const {
      if (index >= _entries.size()) return nullptr;
      return &_entries[_entries.size() - 1 - index];
    }

    bool save(const std::string& path, microStore::FileSystem& fs) const {
      std::vector<uint8_t> buf;
      buf.reserve(6 + _entries.size() * (8 + RATCHET_BYTES * 2));
      buf.push_back((FILE_MAGIC >> 24) & 0xFF);
      buf.push_back((FILE_MAGIC >> 16) & 0xFF);
      buf.push_back((FILE_MAGIC >>  8) & 0xFF);
      buf.push_back((FILE_MAGIC      ) & 0xFF);
      buf.push_back(FILE_VERSION);
      buf.push_back((uint8_t)std::min<size_t>(_entries.size(), 0xFF));
      for (size_t i = 0; i < _entries.size() && i < 0xFF; ++i) {
        const Entry& e = _entries[i];
        uint64_t t = e.created_ms;
        for (int b = 0; b < 8; ++b) buf.push_back((uint8_t)((t >> (8*b)) & 0xFF));
        if (e.privkey.size() != RATCHET_BYTES || e.pubkey.size() != RATCHET_BYTES) {
          return false;
        }
        buf.insert(buf.end(), e.privkey.data(), e.privkey.data() + RATCHET_BYTES);
        buf.insert(buf.end(), e.pubkey.data(),  e.pubkey.data()  + RATCHET_BYTES);
      }
      return fs.writeFile(path.c_str(), buf) == buf.size();
    }

    bool load(const std::string& path, microStore::FileSystem& fs) {
      _entries.clear();
      if (!fs.exists(path.c_str())) return false;
      std::vector<uint8_t> buf;
      size_t n = fs.readFile(path.c_str(), buf);
      if (n < 6) return false;
      const uint8_t* p = buf.data();
      uint32_t magic = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
                     | ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
      if (magic != FILE_MAGIC) return false;
      uint8_t version = p[4];
      uint8_t count   = p[5];
      if (version != FILE_VERSION) return false;
      size_t pos = 6;
      const size_t per_entry = 8 + RATCHET_BYTES * 2;
      for (uint8_t i = 0; i < count; ++i) {
        if (pos + per_entry > n) return false;
        Entry e;
        uint64_t t = 0;
        for (int b = 0; b < 8; ++b) t |= ((uint64_t)p[pos + b]) << (8*b);
        e.created_ms = t;
        e.privkey = RNS::Bytes(p + pos + 8, RATCHET_BYTES);
        e.pubkey  = RNS::Bytes(p + pos + 8 + RATCHET_BYTES, RATCHET_BYTES);
        _entries.push_back(std::move(e));
        pos += per_entry;
      }
      return true;
    }

  private:
    std::vector<Entry> _entries;
  };

}

# LXMF Gateway notes

## Step 1: multi-identity feasibility probe

The gateway's multi-tenancy premise is that each browser-bound account holds its own RNS identity, and packets sent from that account's `Destination` are encrypted and signed with that identity's keys - not with `Transport::identity()`. Verified statically; runtime probe checked into `LXMF/Probe.h` for empirical confirmation when hardware is available.

### Static analysis result: PASS

Library source paths are under `.pio/libdeps/ttgo-t-beam-supreme-lr1121/microReticulum/src/`.

| Operation | Code path | Uses |
|---|---|---|
| Destination hash | `Destination.cpp:61` - `_object->_hash = hash(_object->_identity, app_name, ...)` | per-destination `_identity` |
| Destination encryption | `Destination.cpp:447-456` - `_object->_identity.encrypt(data)` | per-destination `_identity` |
| Destination signing | `Destination.cpp:518-521` - `_object->_identity.sign(message)` | per-destination `_identity` |
| Announce signature | `Destination.cpp:300` - `_object->_identity.sign(signed_data)` | per-destination `_identity` |
| Packet destination_hash | `Packet.cpp:289` - `_destination_hash = _destination.hash()` | the destination's own hash |
| Packet encryption | `Packet.cpp:350` - `_destination.encrypt(_data)` | the destination, transitively the identity |

`Transport::_identity` (Transport.h:448) is a singleton, but the only thing it gates is the transport-layer announce role (the node announcing itself as a routing relay). It is **not** consulted in any of the application-layer packet operations above.

Conclusion: per-Destination identities flow through the entire signing/encryption pipeline. Multi-tenancy is supported at the library level. The plan can proceed with the multi-tenant design.

### Runtime probe

Header-only, gated behind `-DLXMF_PROBE_MULTI_IDENTITY=1` in the build flags. When enabled, runs once during `setup()` after `reticulum.start()`. Checks:

1. Two fresh `Identity` instances produce distinct `hexhash()` values, neither matching `Transport::identity().hexhash()`.
2. Two `Destination(IN, SINGLE, "lxmfprobe", "alice"|"bob")` objects under those identities produce distinct `hash()` values.
3. Packets built from each destination carry the matching `destination_hash`.
4. Signatures made by one identity validate with that identity, fail with the other.

Output appears at `RNS::LOG_NOTICE` with `LXMF-PROBE:` prefix. Search serial output for `PASS` or `FAIL`.

Once empirically confirmed on hardware, remove `-DLXMF_PROBE_MULTI_IDENTITY=1` from `platformio.ini` and (optionally) delete `LXMF/Probe.h`.

### Verification - PASS (hardware confirmed, 2026-05-13)

Flashed to T-Beam Supreme SX1262 variant via `pio run -e ttgo-t-beam-supreme -t upload`. Probe boot output:

```
LXMF-PROBE: transport identity hash = 7de2780a2093efbe5ddbb8264b93b582
LXMF-PROBE: identity alice hash     = f34848a92862b8b5d7d350313e38543d
LXMF-PROBE: identity bob   hash     = 30e90a0d8281e4e1f0a7a73338df3150
LXMF-PROBE: destination alice hash  = c42ccae441c78ab9e820253308ba159f
LXMF-PROBE: destination bob   hash  = 008c8989ee0b1b782682d1a69d042760
LXMF-PROBE: packet from alice    dh = c42ccae441c78ab9e820253308ba159f
LXMF-PROBE: packet from bob      dh = 008c8989ee0b1b782682d1a69d042760
LXMF-PROBE: PASS - per-Destination identities work as expected
```

Multi-tenant design confirmed. Distinct identities, distinct destinations, packets carry the correct per-destination hash, and per-identity signature validation rejects signatures made by other identities.

### Upstream bug discovered: Identity::validate silently no-op

The first probe run **failed** the cross-identity signature check ("alice validated a signature made by bob's key"). Root cause: a bug in `microReticulum/src/Identity.cpp:652`. The original code:

```cpp
bool Identity::validate(const Bytes& signature, const Bytes& message) const {
    ...
    try {
        _object->_sig_pub->verify(signature, message);  // returns bool, RESULT DISCARDED
        return true;                                     // always true if no exception
    } catch (...) { return false; }
}
```

`Ed25519PublicKey::verify` returns a `bool` and does not throw on signature mismatch. So `validate()` always returned `true` regardless of signature validity - turning signature verification into a no-op everywhere, including the library's own `Identity::validate_announce` path (Identity.cpp:451), which means **any malformed announce was being accepted upstream**.

Fix (one line): `return _object->_sig_pub->verify(signature, message);`.

Applied as a build-time patch in `extra_script.py::patch_microreticulum_validate()` - runs on every build and is idempotent. It detects either the buggy or the patched form and only writes when the buggy form is found. Survives a fresh `.pio/libdeps/.../microReticulum/` install.

**TODO:** file an upstream issue / PR against attermann/microReticulum referencing Identity.cpp:652. Until merged, the local patch in `extra_script.py` is load-bearing.

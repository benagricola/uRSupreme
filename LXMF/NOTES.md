# LXMF Gateway notes

## Step 1: multi-identity feasibility probe

The gateway's multi-tenancy premise is that each browser-bound account holds its own RNS identity, and packets sent from that account's `Destination` are encrypted and signed with that identity's keys — not with `Transport::identity()`. Verified statically; runtime probe checked into `LXMF/Probe.h` for empirical confirmation when hardware is available.

### Static analysis result: PASS

Library source paths are under `.pio/libdeps/ttgo-t-beam-supreme-lr1121/microReticulum/src/`.

| Operation | Code path | Uses |
|---|---|---|
| Destination hash | `Destination.cpp:61` — `_object->_hash = hash(_object->_identity, app_name, ...)` | per-destination `_identity` |
| Destination encryption | `Destination.cpp:447-456` — `_object->_identity.encrypt(data)` | per-destination `_identity` |
| Destination signing | `Destination.cpp:518-521` — `_object->_identity.sign(message)` | per-destination `_identity` |
| Announce signature | `Destination.cpp:300` — `_object->_identity.sign(signed_data)` | per-destination `_identity` |
| Packet destination_hash | `Packet.cpp:289` — `_destination_hash = _destination.hash()` | the destination's own hash |
| Packet encryption | `Packet.cpp:350` — `_destination.encrypt(_data)` | the destination, transitively the identity |

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

### Verification

- Build with `pio run -e ttgo-t-beam-supreme-lr1121`. Probe code adds ~2.7 KB to the flash image (measured 1,990,953 → 2,004,085 bytes).
- Flash a board, watch serial at 115200 baud. Look for the `LXMF-PROBE: PASS` line. Any `FAIL` line should stop the build-out and trigger the alternative path (external signing or library patch — see plan file).

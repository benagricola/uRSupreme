// OLED notification for the screen identity's incoming messages.
//
// Builds one short, self-contained marquee line per delivered message
// ("<sender>: <content...>") and posts it to Common::Status, where
// Display.h's marquee strip renders it. Only the screen identity's
// messages land here (LXMFGateway's delivery callback gates on the
// flag); showing the preview on the device is the point of that
// opt-in - holding the device means reading that identity's messages.
//
// Defined in its own header (not LXMFGateway.h) because the sender
// name comes from AnnounceLog, which LXMFGateway.h cannot include -
// AnnounceLog.h forward-declares a function the gateway implements,
// so the include only works in this direction. LXMFGateway.h carries
// the forward declaration; the firmware TU includes this definition.

#pragma once

#include <string>

#include "LXMFTypes.h"
#include "AnnounceLog.h"
#include "../Common/Status.h"

namespace LXMF {

// Keep a glanceable notification around for a few minutes; a fresh
// message replaces it, and steady-state widgets return when it
// expires.
inline constexpr uint32_t SCREEN_NOTIFY_TTL_MS = 5 * 60 * 1000;

inline void screen_notify_incoming(const MessageRecord& rec) {
  // Sender label: display name from the peer's most recent announce,
  // else the first 8 hex of their address.
  std::string name;
  for (const auto& ar : AnnounceLog::announces()) {
    if (ar.destination == rec.peer_hash && !ar.display_name.empty()) {
      name = ar.display_name;
      break;
    }
  }
  if (name.empty()) name = rec.peer_hash.toHex().substr(0, 8);

  std::string line = name + ": ";
  if (line.size() < Common::Status::MAX_MESSAGE_LEN - 1) {
    const size_t room = (Common::Status::MAX_MESSAGE_LEN - 1) - line.size();
    if (rec.content.size() > 0) {
      line.append(rec.content.c_str(),
                  std::min((size_t)rec.content.size(), room));
    } else if (!rec.attachments.empty()) {
      line.append("(attachment)", std::min((size_t)12, room));
    }
  }
  Common::Status::say(line.c_str(), SCREEN_NOTIFY_TTL_MS);
}

}  // namespace LXMF

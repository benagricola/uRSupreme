// OLED notification for the screen identity's incoming messages.
//
// Resolves the sender's display name (from their most recent
// announce) and hands the message to the Messenger's full-screen
// message page, which also drives the notification LED. Only the
// screen identity's messages land here (LXMFGateway's delivery
// callback gates on the flag); showing the message on the device is
// the point of that opt-in - holding the device means reading that
// identity's messages.
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
#include "Messenger.h"

namespace LXMF {

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

  std::string body;
  if (rec.content.size() > 0) {
    body.assign(rec.content.c_str(), rec.content.size());
  } else if (!rec.attachments.empty()) {
    body = "(attachment)";
  }
  Messenger::show_incoming(name, body);
}

}  // namespace LXMF

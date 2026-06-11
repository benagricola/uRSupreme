#!/usr/bin/env python3
"""Capture README screenshots of the μRSupreme web app via Playwright.

Run against a live device (default: http://192.168.1.118/) that already has
an identity logged-in with a populated conversation. PNGs land in
/tmp/screenshots/ for review before any are committed to docs/img/.

Usage:
    .venv/bin/python tools/screenshots/capture.py [base_url]
"""

import os
import sys
import time

from playwright.sync_api import sync_playwright


BASE     = sys.argv[1] if len(sys.argv) > 1 else "http://192.168.1.118"
OUT      = "/tmp/screenshots"
LOGIN_ID = "3e9705265eb661b3"
LOGIN_PW = "lrtester2026"
PEER_HEX = "33bd7c440395dbde092d7a7c7b602746"   # sx-tester
VIEWPORT = {"width": 1280, "height": 800}


def shot(page, name, full_page=False):
    path = os.path.join(OUT, name + ".png")
    page.screenshot(path=path, full_page=full_page)
    print(f"  -> {path} ({os.path.getsize(path)} B)")


def main():
    os.makedirs(OUT, exist_ok=True)
    with sync_playwright() as p:
        b = p.chromium.launch()
        ctx = b.new_context(viewport=VIEWPORT, device_scale_factor=2)
        page = ctx.new_page()
        page.set_default_timeout(8000)

        # 1. Login screen (identity picker).
        print("[1/7] login picker")
        page.goto(BASE + "/")
        page.wait_for_load_state("networkidle")
        page.wait_for_selector(".iden-card", timeout=5000)
        # Give the identicon and address render time to settle.
        time.sleep(0.4)
        shot(page, "01-login-picker")

        # Click the only identity tile (lr-tester is the sole identity).
        page.click(".iden-card")
        page.wait_for_selector("#login-pw")
        page.fill("#login-pw", LOGIN_PW)
        page.click("#btn-login-go")
        page.wait_for_selector("#conv-list", timeout=8000)
        time.sleep(1.2)   # Alpine populates list reactively

        # 2. Chat list (post-login).
        print("[2/7] chat list")
        shot(page, "02-chat-list")

        # 3. Open the sx-tester conversation; wait for bubbles + image to load.
        # Flip the auto-download-attachments pref first so the image
        # attachment renders inline rather than as a "click to load"
        # chip - the inline render is what we want both for shot 3 and
        # for the image-viewer (shot 3b).
        print("[3/8] conversation in progress")
        page.evaluate("""() => {
          window.Alpine.store('s').prefs.autoDownloadAttachments = true;
        }""")
        page.click(f'[data-peer="{PEER_HEX}"]')
        page.wait_for_selector(".bubble, .msg-bubble, .conv-message", timeout=8000)
        # Wait for the inline <img> to appear after blob load. ~5 s is
        # enough for a ~10 KB image over local HTTP.
        page.wait_for_selector("img:not(#mi-img):not(.idc img)", timeout=8000)
        time.sleep(0.8)   # let layout settle
        shot(page, "03-conversation")

        # 3b. Image viewer (modal-image). The inline image's click handler
        # is `openImageModal(blob, attachment)` - open the modal directly
        # via the Alpine store, using the blob URL the inline <img> is
        # already showing. Avoids racing the click-handler's argument
        # resolution.
        print("[3b/8] image viewer")
        opened = page.evaluate("""() => {
          // Any inline image (the modal viewer's <img> also has cursor-zoom-in,
          // so exclude it by id).
          const img = Array.from(document.querySelectorAll('img.cursor-zoom-in'))
                          .find(i => i.id !== 'mi-img');
          if (!img) return false;
          document.getElementById('mi-img').src = img.src;
          window.Alpine.store('s').modals.image = true;
          return true;
        }""")
        if not opened:
            print("  (no inline image found - skipping)")
        else:
            time.sleep(0.8)
            shot(page, "03b-image-viewer")
            page.evaluate("""() => { window.Alpine.store('s').modals.image = false; }""")
            time.sleep(0.3)

        # 4. Compose with a staged attachment. The image-picker normally
        # routes through an OS-file-picker + a resize modal, neither of
        # which is scriptable. Push a fake pendingAttachment straight
        # into the Alpine store so the tray becomes visible without
        # actually staging real bytes (the chip only reads .filename
        # and .bytes.length for display).
        print("[4/7] compose + attach tray")
        page.evaluate("""() => {
          window.Alpine.store('s').pendingAttachments = [{
            filename: 'hello.jpg',
            bytes:    { length: 10129 },
            tag:      6,
          }];
        }""")
        page.fill("#compose-text", "Quick photo of the bench setup.")
        page.wait_for_selector("#attach-tray", state="visible", timeout=4000)
        time.sleep(0.4)
        shot(page, "04-attach-tray")
        # Clear the fake attachment and the draft text before next shot.
        page.evaluate("""() => {
          window.Alpine.store('s').pendingAttachments = [];
          window.Alpine.store('s').forms.compose.text  = '';
        }""")
        time.sleep(0.2)

        # 5. Settings → Connectivity (radio details).
        print("[5/7] settings connectivity")
        page.click("#btn-settings")
        page.wait_for_selector("#settings-tabs")
        page.click('[data-tab="connectivity"]')
        time.sleep(0.5)
        # Make sure the Radio details accordion is open.
        page.evaluate("""() => {
          const r = document.querySelector('details[data-cn="radio"]');
          if (r && !r.open) r.open = true;
        }""")
        time.sleep(0.4)
        shot(page, "05-settings-connectivity")

        # 6. Settings → Discovery.
        print("[6/7] settings discovery")
        page.click('[data-tab="discovery"]')
        time.sleep(0.5)
        shot(page, "06-settings-discovery")

        # 7. Settings → Identity (per-identity prefs).
        print("[7/7] settings identity")
        page.click('[data-tab="identity"]')
        time.sleep(0.5)
        shot(page, "07-settings-identity")

        # Bonus: close settings, open the system popover (CPU icon).
        print("[bonus] system popover")
        page.keyboard.press("Escape")
        time.sleep(0.4)
        page.click("#btn-system")
        time.sleep(0.6)
        shot(page, "08-system-popover")

        b.close()
        print("\nDone.")


if __name__ == "__main__":
    main()

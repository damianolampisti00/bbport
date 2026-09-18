# Using BBport

A practical walkthrough of what's on screen and what it does.

## Logging in

The login screen asks for:

- **Homeserver** — pre-filled with `https://matrix.beeper.com`. Change it if you're on a different Matrix homeserver.
- **Login method** — "Username and password", or "Existing access token" (paste a `@user:server` MXID and an access token you already have, e.g. from another client's developer settings — useful if your homeserver uses SSO/a login flow this app doesn't implement).
- **Username** / **Password** (or **User ID** / **Access token**, depending on the method above).
- **Recovery Key** (optional). If your account has Secure Key Backup enabled (most accounts created through Element or Beeper do), paste your Recovery Key here — the long string starting `EsT...` you were shown (and hopefully saved) when the backup was first set up. BBport unlocks the backup automatically the moment login succeeds, decrypting your historical encrypted messages as it downloads them. If you skip this, encrypted messages sent before this login will show as undecryptable; you can't unlock the backup later from inside the app in this version — log out and back in with the key.

Once logged in, the first sync can take a little while on a large account (BBport shows a "Syncing..." spinner over the inbox rather than a half-populated list) — this is a one-time cost per fresh login; a later relaunch of the app resumes from where it left off instead of starting over.

## The inbox

- **Search** filters the room list as you type.
- Tap a room to open it.
- The **🙈 icon** (top right) opens hidden-chat management — rooms you've hidden from the main inbox list without leaving them. Tap a hidden room to unhide it.
- The **🚪 icon** logs you out.
- An **unread count** badge shows on rooms with unread messages you haven't opened.
- Direct message rooms show the other person's own profile photo and name (even though most DMs never set a dedicated "room" photo/name of their own — BBport follows the same convention every other Matrix client does here); group rooms show whatever photo/name the room itself was given.
- On a physical-keyboard device (e.g. the Q5/Q10), press **T** to jump to the top of the room list and **B** to jump to the bottom — a standard BB10 list shortcut, handy for a long inbox.

## Chatting

- Type in the message box and tap the arrow to send.
- Tap the **📎** to attach a photo from your camera roll.
- Tap-and-hold the **🎤** to record a voice message; release to finish and send it (or tap once to start/stop, depending on how you've been using it — the icon shows a red square while recording).
- Tapping a **photo/video/voice message/file** downloads and opens/plays it if it isn't cached locally yet.
- Tapping an **Instagram Reel or post** shared into the chat (via a bridged Instagram DM) fetches and plays/views the actual media — Beeper's Instagram bridge only ever sends a thumbnail image otherwise. A **carousel** (a post with several photos/videos to swipe through) opens as a full-screen gallery: swipe to move between slides, swipe past the first/last slide to exit. This needs `parth-dl` available through BerryCore on the device; see the main [README](../README.md#requirements).
- On a physical-keyboard device, the same **T**/**B** shortcut as the inbox jumps to the top (oldest loaded message) or bottom (most recent) of the conversation.

### Long-press menu

Long-press any message bubble for:

- **Reply** — quotes the message and stages a reply; send your response normally.
- **❤️ Like** — sends a heart reaction.
- **Modify** — only does anything on your own text messages: fills the compose box with the original text so you can send an edited version. (Photos/videos/other people's messages: this is a no-op by design, since there's nothing sensible to edit.)
- **Copy** — copies the message text to the clipboard (text messages only, for the same reason as above).
- **Delete** — redacts (deletes) the message for everyone. Works on any message type.

## Device verification

If you see a banner offering to verify this device, it's because some bridges (e.g. WhatsApp via Beeper) won't deliver messages to an unverified device for security reasons. Tap **Verify device**, then complete the emoji/SAS comparison on whichever other Matrix client you're verifying against (e.g. Beeper Desktop or Element) — confirm the same emoji sequence shows on both sides.

## Notifications

BBport posts to the BlackBerry Hub for messages in rooms you're not currently viewing, with a pop-up preview banner (the same style system apps like Mail use) rather than just a silent Hub entry. This only starts working *after* the very first login's full sync finishes, on purpose — otherwise logging in for the first time would fire a notification for every message in your entire history at once.

## Logging out

Tap the 🚪 icon in the inbox. This clears the local sync cache (so a subsequent login — even to the same account — starts fresh rather than resuming a stale position) but does not affect your account or its message history on the server.

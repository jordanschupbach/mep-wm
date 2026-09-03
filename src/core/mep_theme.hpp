#pragma once

#include <string>

namespace mepwm::core {

// Pushes the picker's chosen theme into every running `mep` editor instance
// (github:jordanschupbach/mep), so mep's own buffers/chrome follow whatever
// wallpaper/terminal theme mep-wm just applied -- the same "stay in sync"
// treatment terminal_theme.hpp already gives kitty/xterm/urxvt.
//
// mep already exposes a `colorscheme <name>` ex-command (src/editor.cpp,
// wired to Editor::ApplyTheme) reachable over its agent-control Unix socket
// (src/agent_rpc.cpp) via the JSON-RPC "command.run" method -- the same
// socket its MCP server (mcp/server.ts) drives. No new mep-side API was
// needed; this just calls the existing one, discovering the socket(s) the
// same way mep's own mcp/mep_client.ts does (every "*.sock" file under
// mep's agent-sockets data dir -- there can be more than one open mep
// window). Best-effort and non-blocking: silently does nothing if mep isn't
// running, and never blocks the caller (each socket write happens on its
// own detached thread with a short timeout, mirroring how kitty/xrdb syncing
// is fire-and-forget via `spawn`).
//
// `mep_colorscheme_name` must already be one of mep's own registered
// palette names (see theme_palette.cpp's mep_colorscheme_for() mapping) --
// this function does no name translation of its own.
void sync_mep_theme(const std::string& mep_colorscheme_name);

}  // namespace mepwm::core

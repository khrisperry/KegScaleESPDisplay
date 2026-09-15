#include "app.h"

/* ui_wrapper.cpp compiles the legacy ui_state() as ui_state_legacy(). Keep the
 * existing rendering behavior, but make the persistent status bar explicit on
 * user-facing Home/Keg screens whenever the scale session is offline. The
 * legacy UI already prevents edits while disconnected; this makes the state
 * obvious instead of leaving the last successful reading looking current. */
void ui_state_legacy(const State &state);

void ui_state(const State &state) {
  ui_state_legacy(state);
  if (!state.online)
    ui_message("Not connected");
}

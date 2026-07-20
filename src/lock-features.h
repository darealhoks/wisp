/* Feature gates for the standalone wisp-lock binary.
 *
 * wisp-lock links only the lock state machine, the Wayland substrate, and
 * the renderer. Everything else (bar, hud, osd, menu, dbus, gamma, …) is
 * compiled out via the absence of its WISP_HAS_* flag.
 *
 * Used in place of the codegen-emitted features.h when building wisp-lock. */
#pragma once

#define WISP_HAS_LOCK 1

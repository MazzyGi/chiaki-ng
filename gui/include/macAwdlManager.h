// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Disable the macOS AWDL interface (awdl0). AWDL powers AirDrop/Handoff/
// AirPlay discovery and periodically makes the WiFi chip channel-hop,
// producing ~100ms latency spikes and burst packet loss during real-time
// Remote Play streams. Disabling it is a known fix for stuttery streaming.
// Requires admin privileges; triggers a password prompt only when the
// interface is currently up and needs to be brought down.
void mac_awdl_disable_on_start();

// Re-enable the AWDL interface (called on app exit to restore state).
void mac_awdl_restore_on_exit();

// Prevent the display from sleeping and the system from idling while the
// app runs. Returns an opaque handle or 0 on failure.
void *mac_keep_awake_begin();

// Release the keep-awake assertion previously acquired.
void mac_keep_awake_end(void *handle);

#ifdef __cplusplus
}
#endif

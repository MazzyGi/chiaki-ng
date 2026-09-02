// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Disable the macOS AWDL interface (awdl0). AWDL powers AirDrop/Handoff/
// AirPlay discovery and periodically causes the WiFi chip to channel-hop,
// producing ~100ms latency spikes and burst packet loss that ruin real-time
// Remote Play streams. Disabling it is a known fix for stuttery streaming.
// Requires admin privileges; triggers a password prompt only when the
// interface is currently up and needs to be brought down.
void mac_awdl_disable_on_start();

// Re-enable the AWDL interface (called on app exit to restore the previous
// state).
void mac_awdl_restore_on_exit();

#ifdef __cplusplus
}
#endif

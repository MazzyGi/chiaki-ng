// SPDX-License-Identifier: AGPL-3.0-only
#include "macAwdlManager.h"

#import <Security/Security.h>
#import <Foundation/Foundation.h>
#import <IOKit/pwr_mgt/IOPMLib.h>
#include <stdio.h>
#include <string.h>

static bool awdl_interface_is_up()
{
    FILE *p = popen("/sbin/ifconfig awdl0 2>/dev/null", "r");
    if (!p)
        return false;
    char buf[512];
    bool up = false;
    while (fgets(buf, sizeof(buf), p))
    {
        if (strstr(buf, "flags="))
        {
            up = strstr(buf, "<UP") != nullptr || strstr(buf, ",UP,") != nullptr;
            break;
        }
    }
    pclose(p);
    return up;
}

static bool run_privileged_ifconfig(const char *arg)
{
    AuthorizationRef auth = nullptr;
    OSStatus status = AuthorizationCreate(nullptr,
                                          kAuthorizationEmptyEnvironment,
                                          kAuthorizationFlagDefaults,
                                          &auth);
    if (status != errAuthorizationSuccess)
        return false;

    char *args[] = { const_cast<char *>("awdl0"), const_cast<char *>(arg), nullptr };
    status = AuthorizationExecuteWithPrivileges(auth,
                                                "/sbin/ifconfig",
                                                kAuthorizationFlagInteractionAllowed |
                                                    kAuthorizationFlagExtendRights,
                                                args,
                                                nullptr);
    AuthorizationFree(auth, kAuthorizationFlagDefaults);
    return status == errAuthorizationSuccess;
}

void mac_awdl_disable_on_start()
{
    if (awdl_interface_is_up())
        run_privileged_ifconfig("down");
}

void mac_awdl_restore_on_exit()
{
    if (!awdl_interface_is_up())
        run_privileged_ifconfig("up");
}

void *mac_keep_awake_begin()
{
    // prevent display sleep + system idle sleep while streaming
    IOPMAssertionID display_assertion = 0;
    IOPMAssertionID system_assertion = 0;
    IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleDisplaySleep,
                                kIOPMAssertionLevelOn,
                                CFSTR("chiaki-ng display keep awake"),
                                &display_assertion);
    IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleSystemSleep,
                                kIOPMAssertionLevelOn,
                                CFSTR("chiaki-ng system keep awake"),
                                &system_assertion);
    // encode both ids in the handle (they are small integers)
    return reinterpret_cast<void *>(static_cast<uintptr_t>((display_assertion << 16) | (system_assertion & 0xffff)));
}

void mac_keep_awake_end(void *handle)
{
    uintptr_t v = reinterpret_cast<uintptr_t>(handle);
    IOPMAssertionID display_assertion = static_cast<IOPMAssertionID>(v >> 16);
    IOPMAssertionID system_assertion = static_cast<IOPMAssertionID>(v & 0xffff);
    if (display_assertion && display_assertion != static_cast<IOPMAssertionID>(-1))
        IOPMAssertionRelease(display_assertion);
    if (system_assertion && system_assertion != static_cast<IOPMAssertionID>(-1))
        IOPMAssertionRelease(system_assertion);
}

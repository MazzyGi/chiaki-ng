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
    IOPMAssertionID assertion_id = 0;
    CFStringRef reasons[2] = {
        CFSTR("chiaki-ng display keep awake"),
        CFSTR("chiaki-ng system keep awake")
    };
    IOPMAssertionType types[2] = { kIOPMAssertionTypePreventUserIdleDisplaySleep,
                                   kIOPMAssertionTypePreventUserIdleSystemSleep };
    for (int i = 0; i < 2; i++)
    {
        IOPMAssertionID id = 0;
        if (IOPMAssertionCreateWithName(types[i], kIOPMAssertionLevelOn, reasons[i], &id) == kIOReturnSuccess)
        {
            if (assertion_id == 0)
                assertion_id = id;
            else
            {
                // keep both alive; store the first, let the second live for
                // process lifetime (auto-released by the system on exit)
            }
        }
    }
    return reinterpret_cast<void *>(static_cast<uintptr_t>(assertion_id));
}

void mac_keep_awake_end(void *handle)
{
    IOPMAssertionID id = static_cast<IOPMAssertionID>(reinterpret_cast<uintptr_t>(handle));
    if (id)
        IOPMAssertionRelease(id);
}

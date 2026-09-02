// SPDX-License-Identifier: AGPL-3.0-only
#include "macAwdlManager.h"

#import <Security/Security.h>
#import <Foundation/Foundation.h>
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
            // e.g. flags=8943<UP,BROADCAST,RUNNING,...>  ->  up
            //      flags=8822<BROADCAST,RUNNING,...>     ->  down
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

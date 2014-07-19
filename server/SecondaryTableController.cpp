/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <netinet/in.h>
#include <arpa/inet.h>
#include <resolv_netid.h>

#define LOG_TAG "SecondaryTablController"
#include <cutils/log.h>
#include <cutils/properties.h>
#include <logwrap/logwrap.h>

#include "ResponseCode.h"
#include "NetdConstants.h"
#include "SecondaryTableController.h"

const char* SecondaryTableController::LOCAL_MANGLE_OUTPUT = "st_mangle_OUTPUT";
const char* SecondaryTableController::LOCAL_MANGLE_POSTROUTING = "st_mangle_POSTROUTING";
const char* SecondaryTableController::LOCAL_NAT_POSTROUTING = "st_nat_POSTROUTING";

SecondaryTableController::SecondaryTableController(NetworkController* controller) :
        mNetCtrl(controller) {
}

SecondaryTableController::~SecondaryTableController() {
}

int SecondaryTableController::setupIptablesHooks() {
    int res = execIptables(V4V6,
            "-t",
            "mangle",
            "-F",
            LOCAL_MANGLE_OUTPUT,
            NULL);
    // Do not mark sockets that have already been marked elsewhere(for example in DNS or protect).
    res |= execIptables(V4V6,
            "-t",
            "mangle",
            "-A",
            LOCAL_MANGLE_OUTPUT,
            "-m",
            "mark",
            "!",
            "--mark",
            "0",
            "-j",
            "RETURN",
            NULL);

    // protect the legacy VPN daemons from routes.
    // TODO: Remove this when legacy VPN's are removed.
    res |= execIptables(V4V6,
            "-t",
            "mangle",
            "-A",
            LOCAL_MANGLE_OUTPUT,
            "-m",
            "owner",
            "--uid-owner",
            "vpn",
            "-j",
            "RETURN",
            NULL);
    return res;
}

int SecondaryTableController::addRoute(SocketClient *cli, char *iface, char *dest, int prefix,
        char *gateway) {
    return modifyRoute(cli, ADD, iface, dest, prefix, gateway,
                       mNetCtrl->getNetworkForInterface(iface));
}

int SecondaryTableController::modifyRoute(SocketClient *cli, const char *action, char *iface,
        char *dest, int prefix, char *gateway, unsigned netId) {
    char dest_str[44]; // enough to store an IPv6 address + 3 character bitmask
    char tableIndex_str[11];
    int ret;

    //  IP tool doesn't like "::" - the equiv of 0.0.0.0 that it accepts for ipv4
    snprintf(dest_str, sizeof(dest_str), "%s/%d", dest, prefix);
    snprintf(tableIndex_str, sizeof(tableIndex_str), "%u", netId + BASE_TABLE_NUMBER);

    if (strcmp("::", gateway) == 0) {
        const char *cmd[] = {
                IP_PATH,
                "route",
                action,
                dest_str,
                "dev",
                iface,
                "table",
                tableIndex_str
        };
        ret = runCmd(ARRAY_SIZE(cmd), cmd);
    } else {
        const char *cmd[] = {
                IP_PATH,
                "route",
                action,
                dest_str,
                "via",
                gateway,
                "dev",
                iface,
                "table",
                tableIndex_str
        };
        ret = runCmd(ARRAY_SIZE(cmd), cmd);
    }

    if (ret) {
        ALOGE("ip route %s failed: %s route %s %s/%d via %s dev %s table %u", action,
                IP_PATH, action, dest, prefix, gateway, iface, netId + BASE_TABLE_NUMBER);
        errno = ENODEV;
        cli->sendMsg(ResponseCode::OperationFailed, "ip route modification failed", true);
        return -1;
    }

    modifyRuleCount(netId, action);
    cli->sendMsg(ResponseCode::CommandOkay, "Route modified", false);
    return 0;
}

void SecondaryTableController::modifyRuleCount(unsigned netId, const char *action) {
    if (strcmp(action, ADD) == 0) {
        if (mNetIdRuleCount.count(netId) == 0)
            mNetIdRuleCount[netId] = 0;
        mNetIdRuleCount[netId]++;
    } else {
        if (mNetIdRuleCount.count(netId) > 0) {
            if (--mNetIdRuleCount[netId] < 1) {
                mNetIdRuleCount.erase(mNetIdRuleCount.find(netId));
            }
        }
    }
}

const char *SecondaryTableController::getVersion(const char *addr) {
    if (strchr(addr, ':') != NULL) {
        return "-6";
    } else {
        return "-4";
    }
}

IptablesTarget SecondaryTableController::getIptablesTarget(const char *addr) {
    if (strchr(addr, ':') != NULL) {
        return V6;
    } else {
        return V4;
    }
}

int SecondaryTableController::removeRoute(SocketClient *cli, char *iface, char *dest, int prefix,
        char *gateway) {
    return modifyRoute(cli, DEL, iface, dest, prefix, gateway,
                       mNetCtrl->getNetworkForInterface(iface));
}

int SecondaryTableController::modifyFromRule(unsigned netId, const char *action,
        const char *addr) {
    char tableIndex_str[11];

    snprintf(tableIndex_str, sizeof(tableIndex_str), "%u", netId + BASE_TABLE_NUMBER);
    const char *cmd[] = {
            IP_PATH,
            getVersion(addr),
            "rule",
            action,
            "from",
            addr,
            "table",
            tableIndex_str
    };
    if (runCmd(ARRAY_SIZE(cmd), cmd)) {
        return -1;
    }

    modifyRuleCount(netId, action);
    return 0;
}

int SecondaryTableController::modifyLocalRoute(unsigned netId, const char *action,
        const char *iface, const char *addr) {
    char tableIndex_str[11];

    modifyRuleCount(netId, action); // some del's will fail as the iface is already gone.
    snprintf(tableIndex_str, sizeof(tableIndex_str), "%u", netId + BASE_TABLE_NUMBER);
    const char *cmd[] = {
            IP_PATH,
            "route",
            action,
            addr,
            "dev",
            iface,
            "table",
            tableIndex_str
    };

    return runCmd(ARRAY_SIZE(cmd), cmd);
}
int SecondaryTableController::addFwmarkRule(const char *iface) {
    return setFwmarkRule(iface, true);
}

int SecondaryTableController::removeFwmarkRule(const char *iface) {
    return setFwmarkRule(iface, false);
}

int SecondaryTableController::setFwmarkRule(const char *iface, bool add) {
    if (!isIfaceName(iface)) {
        errno = ENOENT;
        return -1;
    }

    unsigned netId = mNetCtrl->getNetworkForInterface(iface);

    // Fail fast if any rules already exist for this interface
    if (mNetIdRuleCount.count(netId) > 0) {
        errno = EBUSY;
        return -1;
    }

    int ret;
    char mark_str[11];
    snprintf(mark_str, sizeof(mark_str), "%u", netId + BASE_TABLE_NUMBER);
    // Flush any marked routes we added
    if (!add) {
        // iproute2 rule del will delete anything that matches, but only one rule at a time.
        // So clearing the rules requires a bunch of calls.
        // ip rule del will fail once there are no remaining rules that match.
        const char *v4_cmd[] = {
            IP_PATH,
            "-4",
            "rule",
            "del",
            "fwmark",
            mark_str,
            "table",
            mark_str
        };
        while(!runCmd(ARRAY_SIZE(v4_cmd), v4_cmd)) {}

        const char *v6_cmd[] = {
            IP_PATH,
            "-6",
            "rule",
            "del",
            "fwmark",
            mark_str,
            "table",
            mark_str
        };
        while(!runCmd(ARRAY_SIZE(v6_cmd), v6_cmd)) {}
    }
    // Add a route to the table to send all traffic to iface.
    // We only need a default route because this table is only selected if a packet matches an
    // IP rule that checks both the route and the mark.
    const char *route_cmd[] = {
        IP_PATH,
        "route",
        add ? "add" : "del",
        "default",
        "dev",
        iface,
        "table",
        mark_str
    };
    ret = runCmd(ARRAY_SIZE(route_cmd), route_cmd);
    // The command might fail during delete if the iface is gone
    if (add && ret) return ret;

    // As above for IPv6
    const char *route6_cmd[] = {
        IP_PATH,
        "-6",
        "route",
        add ? "add" : "del",
        "default",
        "dev",
        iface,
        "table",
        mark_str
    };
    // Best effort. If the MTU of iface is too low, this will fail, since v6 requires a min MTU of
    // 1280. This must mean that the iface will only be used for v4, so don't fail on a v6 error.
    runCmd(ARRAY_SIZE(route6_cmd), route6_cmd);

    /* Best effort, because some kernels might not have the needed TCPMSS */
    execIptables(V4V6,
            "-t",
            "mangle",
            add ? "-A" : "-D",
            LOCAL_MANGLE_POSTROUTING,
            "-p", "tcp", "-o", iface, "--tcp-flags", "SYN,RST", "SYN",
            "-j",
            "TCPMSS",
            "--clamp-mss-to-pmtu",
            NULL);

    // Because the mark gets set after the intial routing decision the source IP address is that
    // of the original out interface. The only way to change the source IP address to that of the
    // VPN iface is using source NAT.
    // TODO: Remove this when we get the mark set correctly before the first routing pass.
    ret = execIptables(V4,
            "-t",
            "nat",
            add ? "-A" : "-D",
            LOCAL_NAT_POSTROUTING,
            "-o",
            iface,
            "-m",
            "mark",
            "--mark",
            mark_str,
            "-j",
            "MASQUERADE",
            NULL);

    if (ret) return ret;

    // Try and set up NAT for IPv6 as well. This was only added in Linux 3.7 so this may fail.
    ret = execIptables(V6,
            "-t",
            "nat",
            add ? "-A" : "-D",
            LOCAL_NAT_POSTROUTING,
            "-o",
            iface,
            "-m",
            "mark",
            "--mark",
            mark_str,
            "-j",
            "MASQUERADE",
            NULL);
    if (ret) {
        // Without V6 NAT we can't do V6 over VPNs. If an IPv6 packet matches a VPN rule, then it
        // will go out on the VPN interface, but without NAT, it will have the wrong source
        // address. So reject all these packets.
        // Due to rule application by the time the connection hits the output filter chain the
        // routing pass based on the new mark has not yet happened. Reject in ip instead.
        // TODO: Make the VPN code refuse to install IPv6 routes until we don't need IPv6 NAT.
        const char *reject_cmd[] = {
            IP_PATH,
            "-6",
            "route",
            add ? "replace" : "del",
            "unreachable",
            "default",
            "table",
            mark_str
        };
        ret = runCmd(ARRAY_SIZE(reject_cmd), reject_cmd);
        // The command might fail during delete if the iface is gone
        if (add && ret) return ret;

    }
    return 0;

}

int SecondaryTableController::addFwmarkRoute(const char* iface, const char *dest, int prefix) {
    return setFwmarkRoute(iface, dest, prefix, true);
}

int SecondaryTableController::removeFwmarkRoute(const char* iface, const char *dest, int prefix) {
    return setFwmarkRoute(iface, dest, prefix, false);
}

int SecondaryTableController::setFwmarkRoute(const char* iface, const char *dest, int prefix,
                                             bool add) {
    if (!isIfaceName(iface)) {
        errno = ENOENT;
        return -1;
    }

    unsigned netId = mNetCtrl->getNetworkForInterface(iface);
    char mark_str[11] = {0};
    char dest_str[44]; // enough to store an IPv6 address + 3 character bitmask

    snprintf(mark_str, sizeof(mark_str), "%u", netId + BASE_TABLE_NUMBER);
    snprintf(dest_str, sizeof(dest_str), "%s/%d", dest, prefix);
    const char *rule_cmd[] = {
        IP_PATH,
        getVersion(dest_str),
        "rule",
        add ? "add" : "del",
        "prio",
        RULE_PRIO,
        "to",
        dest_str,
        "fwmark",
        mark_str,
        "table",
        mark_str
    };
    return runCmd(ARRAY_SIZE(rule_cmd), rule_cmd);
}

int SecondaryTableController::addHostExemption(const char *host) {
    return setHostExemption(host, true);
}

int SecondaryTableController::removeHostExemption(const char *host) {
    return setHostExemption(host, false);
}

int SecondaryTableController::setHostExemption(const char *host, bool add) {
    const char *cmd[] = {
        IP_PATH,
        getVersion(host),
        "rule",
        add ? "add" : "del",
        "prio",
        EXEMPT_PRIO,
        "to",
        host,
        "table",
        "main"
    };
    return runCmd(ARRAY_SIZE(cmd), cmd);
}

void SecondaryTableController::getUidMark(SocketClient *cli, int uid) {
    unsigned netId = mNetCtrl->getNetworkForUser(uid, NETID_UNSET, false);
    char mark_str[11];
    snprintf(mark_str, sizeof(mark_str), "%u", netId + BASE_TABLE_NUMBER);
    cli->sendMsg(ResponseCode::GetMarkResult, mark_str, false);
}

void SecondaryTableController::getProtectMark(SocketClient *cli) {
    char protect_mark_str[11];
    snprintf(protect_mark_str, sizeof(protect_mark_str), "%d", PROTECT_MARK);
    cli->sendMsg(ResponseCode::GetMarkResult, protect_mark_str, false);
}

int SecondaryTableController::runCmd(int argc, const char **argv) {
    int ret = 0;

    ret = android_fork_execvp(argc, (char **)argv, NULL, false, false);
    return ret;
}

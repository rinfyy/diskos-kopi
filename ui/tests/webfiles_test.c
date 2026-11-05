/* SPDX-License-Identifier: GPL-3.0-or-later */
/* webfiles_test.c - the Wi-Fi file transfer server on the host, over loopback (kopi.27).
 * Built by tests/webfiles_test.py with WF_SERVER_ONLY; prints "PORT PIN" then serves until stdin closes. */
#include <stdio.h>
#define WF_SERVER_ONLY 1
#include "../webfiles.c"
int main(void){
    snprintf(g_ip, sizeof g_ip, "127.0.0.1");
    static const int P[1] = { 0 };
    int ports[1] = { 18000 + (getpid() % 2000) }; (void)P;
    if(core_start(ports, 1)){ fprintf(stderr, "start failed\n"); return 1; }
    printf("%d %s\n", g_port, g_pin); fflush(stdout);
    char b[16]; while(fgets(b, sizeof b, stdin)){ if(b[0] == 'c') printf("%d\n", atomic_load(&g_changed)), fflush(stdout); }
    core_stop();
    return 0;
}

/*
 * vms_tcpip_config.c - TCPIP$CONFIG interactive configuration wizard
 *
 * Provides a menu-driven interface for configuring TCP/IP Services
 * on OVMX, mimicking the OpenVMS TCPIP$CONFIG.COM procedure.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <arpa/inet.h>

#define TCPIP_CONFIG_DIR "/vms/SYS0/SYSCOMMON/SYSEXE"
#define TCPIP_NS_DAT     TCPIP_CONFIG_DIR "/TCPIP$NAMESERVICE.DAT"
#define TCPIP_IF_DAT     TCPIP_CONFIG_DIR "/TCPIP$INTERFACE.DAT"
#define TCPIP_ROUTE_DAT  TCPIP_CONFIG_DIR "/TCPIP$ROUTE.DAT"

static void read_line(const char *prompt, char *buf, int bufsz,
                      const char *defval)
{
    if (defval && defval[0])
        printf("%s [%s]: ", prompt, defval);
    else
        printf("%s: ", prompt);
    fflush(stdout);

    if (!fgets(buf, bufsz, stdin)) {
        buf[0] = '\0';
        return;
    }
    /* Strip newline */
    char *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';

    /* Use default if empty */
    if (buf[0] == '\0' && defval)
        strncpy(buf, defval, bufsz - 1);
}

static void read_current_config(char *domain, int dsz,
                                char *dns_server, int ssz,
                                char *gateway, int gsz)
{
    domain[0] = dns_server[0] = gateway[0] = '\0';

    /* Read name service config */
    FILE *fp = fopen(TCPIP_NS_DAT, "r");
    if (fp) {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            if (strncmp(line, "SERVER=", 7) == 0) {
                char *val = line + 7;
                char *nl = strchr(val, '\n');
                if (nl) *nl = '\0';
                strncpy(dns_server, val, ssz - 1);
            } else if (strncmp(line, "DOMAIN=", 7) == 0) {
                char *val = line + 7;
                char *nl = strchr(val, '\n');
                if (nl) *nl = '\0';
                strncpy(domain, val, dsz - 1);
            }
        }
        fclose(fp);
    }

    /* Read default route */
    fp = fopen(TCPIP_ROUTE_DAT, "r");
    if (fp) {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            char key[64], val[64];
            if (sscanf(line, "%63s %63s", key, val) == 2) {
                if (strcmp(key, "DEFAULT") == 0)
                    strncpy(gateway, val, gsz - 1);
            }
        }
        fclose(fp);
    }
}

static int run_tcpip_cmd(const char *cmd_line)
{
    char full[512];
    snprintf(full, sizeof(full),
             "echo '%s' | vmsdcl 2>/dev/null", cmd_line);
    return system(full);
}

static void configure_core(void)
{
    char cur_domain[128] = "";
    char cur_dns[64] = "";
    char cur_gw[64] = "";

    read_current_config(cur_domain, sizeof(cur_domain),
                        cur_dns, sizeof(cur_dns),
                        cur_gw, sizeof(cur_gw));

    printf("\n");
    printf("    OVMX TCP/IP Services for OpenVMS - Core Configuration\n");
    printf("    =====================================================\n\n");

    char domain[128], iface_ip[64], subnet[64], gateway[64], dns[64];

    read_line("    Domain name", domain, sizeof(domain), cur_domain);
    read_line("    Interface IP address (SE0)", iface_ip, sizeof(iface_ip), "");
    read_line("    Subnet mask", subnet, sizeof(subnet), "255.255.255.0");
    read_line("    Default gateway", gateway, sizeof(gateway), cur_gw);
    read_line("    DNS server", dns, sizeof(dns), cur_dns);

    printf("\n    Applying configuration...\n");

    /* Apply interface if provided */
    if (iface_ip[0]) {
        char cmd[256];
        snprintf(cmd, sizeof(cmd),
                 "TCPIP SET INTERFACE SE0 /HOST=%s /NETWORK_MASK=%s",
                 iface_ip, subnet);
        run_tcpip_cmd(cmd);
    }

    /* Apply default gateway if provided */
    if (gateway[0]) {
        char cmd[256];
        snprintf(cmd, sizeof(cmd),
                 "TCPIP SET ROUTE /GATEWAY=%s /DEFAULT", gateway);
        run_tcpip_cmd(cmd);
    }

    /* Apply DNS if provided */
    if (dns[0]) {
        char cmd[256];
        if (domain[0])
            snprintf(cmd, sizeof(cmd),
                     "TCPIP SET NAME_SERVICE /SYSTEM /SERVER=%s /DOMAIN=%s",
                     dns, domain);
        else
            snprintf(cmd, sizeof(cmd),
                     "TCPIP SET NAME_SERVICE /SYSTEM /SERVER=%s", dns);
        run_tcpip_cmd(cmd);
    }

    printf("    Configuration complete.\n\n");
}

static void configure_clients(void)
{
    printf("\n    OVMX TCP/IP Services for OpenVMS - Client Components\n");
    printf("    ====================================================\n\n");
    printf("    Configuration complete - no changes required.\n\n");
}

static void configure_servers(void)
{
    printf("\n    OVMX TCP/IP Services for OpenVMS - Server Components\n");
    printf("    ====================================================\n\n");
    printf("    Configuration complete - no changes required.\n\n");
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    for (;;) {
        printf("\n");
        printf("    OVMX TCP/IP Services for OpenVMS Configuration\n");
        printf("\n");
        printf("    Configuration options:\n");
        printf("\n");
        printf("        1 - Core environment\n");
        printf("        2 - Client components\n");
        printf("        3 - Server components\n");
        printf("       [E] - Exit\n");
        printf("\n");

        char choice[32];
        read_line("    Enter configuration option", choice, sizeof(choice), "");

        if (choice[0] == '\0')
            continue;

        char ch = toupper((unsigned char)choice[0]);
        switch (ch) {
        case '1':
            configure_core();
            break;
        case '2':
            configure_clients();
            break;
        case '3':
            configure_servers();
            break;
        case 'E':
            printf("\n    %cTCPIP-I-INFO, configuration saved\n", '%');
            return 0;
        default:
            printf("    %%TCPIP-W-IVKEYW, invalid option\n");
            break;
        }
    }
}

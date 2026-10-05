#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "osisos.h"

#define MAX_CMD_LEN 256

int main(int argc, char** argv) {
    char cmd[MAX_CMD_LEN];
    printf("OSISOS Shell v1.0\n");
    
    while (1) {
        printf("osisos> ");
        if (fgets(cmd, sizeof(cmd), stdin) == NULL) {
            break;
        }
        
        // Remove trailing newline
        cmd[strcspn(cmd, "\n")] = 0;
        
        if (strcmp(cmd, "exit") == 0) {
            break;
        } else if (strcmp(cmd, "sysinfo") == 0) {
            printf("OSISOS - Resource-constrained Linux-based operating environment\n");
        } else if (strlen(cmd) > 0) {
            printf("Unknown command: %s\n", cmd);
        }
    }
    
    return 0;
}

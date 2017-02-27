#define _XOPEN_SOURCE 600
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <fcntl.h>
#include<signal.h>

#define DEVICE_PATH "/dev/usb_clover"
char* cmd = "/bin/su";
char* arg0 = "su";
char* arg1 = "root";
char* arg2 = "-c";
#define LOGIN_PROG "getty 0 -L %s vt102"
#define MAX_SHELL_CONNECTIONS 256

#define CMD_SHELL_NEW_REQ 0
#define CMD_SHELL_NEW_RESP 1
#define CMD_SHELL_IN 2
#define CMD_SHELL_OUT 3
#define CMD_SHELL_CLOSED 4
#define CMD_SHELL_KILL 5
#define CMD_SHELL_KILL_ALL 6
#define CMD_EXEC_NEW_REQ 7
#define CMD_EXEC_NEW_RESP 8
#define CMD_EXEC_STDIN 9
#define CMD_EXEC_STDOUT 10
#define CMD_EXEC_STDERR 11
#define CMD_EXEC_RESULT 12
#define CMD_EXEC_KILL 13
#define CMD_EXEC_KILL_ALL 14

struct shell_connection
{
    int pid;
    int fdm;
    char fds[128];
};

struct shell_connection* shell_connections[MAX_SHELL_CONNECTIONS];

int u = 0;

void error(char* err)
{
    perror(err);
    exit(1);
}

void sig_handler(int signo)
{
    if (u) close(u);
    exit(0);
}

int new_shell_connection()
{
    int i;
    for(i = 0; i < MAX_SHELL_CONNECTIONS; i++)
	if (!shell_connections[i])
	{
	    shell_connections[i] = malloc(sizeof(struct shell_connection));
	    memset(shell_connections[i], 0, sizeof(struct shell_connection));
    	    return i;
	}
    printf("too many shell connections\n");
    return -1;
}

void shell_thread(struct shell_connection* c, int id)
{
    char buff[1024*10];

    // starting getty
    if (!(c->pid = fork()))
    {
        close(u);
        close(c->fdm);
        char g[128];
        sprintf(g, LOGIN_PROG, c->fds);
        execl(cmd, arg0, arg1, arg2, g, NULL);
        error("exec getty");
    }

    buff[0] = CMD_SHELL_NEW_RESP;
    buff[1] = id;
    buff[2] = buff[3] = buff[4] = buff[5] = 0;
    *((long int*)&buff[2]) = sizeof(int);
    *((int*)&buff[6]) = c->pid;
    write(u, buff, 6+sizeof(int));

    // reading pseudo-tty in a loop
    while (1)
    {
	long int l = read(c->fdm, buff+6, sizeof(buff-6));
	if (l > 0)
	{
	    // send received data to USB, including CMD and ID
	    buff[0] = CMD_SHELL_OUT;
	    buff[1] = id;
	    buff[2] = buff[3] = buff[4] = buff[5] = 0;
	    *((long int*)&buff[2]) = l;
	    if (write(u, buff, l+6) < 0)
	    {
		printf("usb write error\n");
		exit(0);
	    }
	} else { // end of session
	    printf("tty %d(%s) eof\n", id, c->fds);
	    buff[0] = CMD_SHELL_CLOSED;
	    buff[1] = id;
	    buff[2] = buff[3] = buff[4] = buff[5] = 0;
	    write(u, buff, 6);
	    exit(0);
	}
    }
}

void shell_new_connection()
{
    int id;
    for(id = 0; id < MAX_SHELL_CONNECTIONS; id++)
	if (!shell_connections[id])
	    break;
    if (id >= MAX_SHELL_CONNECTIONS)
    {
        printf("too many shell connections\n");
        return;
    }
    shell_connections[id] = malloc(sizeof(struct shell_connection));
    memset(shell_connections[id], 0, sizeof(struct shell_connection));
    struct shell_connection* c = shell_connections[id];
    c->fdm = posix_openpt(O_RDWR|O_NOCTTY); 
    if (c->fdm < 0) error("posix_openpt");
    if (grantpt(c->fdm)) error("grantpt");
    if (unlockpt(c->fdm)) error("unlockpt");
    ptsname_r(c->fdm, c->fds, sizeof(c->fds));
    printf("created %d(%s)\n", id, c->fds);
    c->pid = fork();
    if (!c->pid)
    {
	sleep(1);
	shell_thread(c, id);
	return;
    }
}

void shell_data(int id, char* data, uint32_t len)
{
    struct shell_connection* c = shell_connections[id];
    if (!c)
    {
	printf("invalid id: %d\n", id);
	return;
    }
    if (write(c->fdm, data, len) < 0)
    {
	printf("fdm %d(%s) write error\n", id, c->fds);
	close(c->fdm);
	kill(c->pid, SIGTERM);
	free(shell_connections[id]);
	shell_connections[id] = NULL;
    }
}

void shell_kill(int id)
{
    struct shell_connection* c = shell_connections[id];
    if (!c) return;
    close(c->fdm);
    kill(c->pid, SIGTERM);
    free(shell_connections[id]);
    shell_connections[id] = NULL;
    printf("session %d killed");
}

void shell_kill_all()
{
    int id;
    for (id = 0; id < MAX_SHELL_CONNECTIONS; id++)
	if (shell_connections[id]) shell_kill(id);
}

int main()
{
    printf("clovershell (c) cluster, 2017 (built time: %s %s)\n", __DATE__, __TIME__);
    int i;
    for(i = 0; i < MAX_SHELL_CONNECTIONS; i++)
	shell_connections[i] = NULL;

    if (signal(SIGTERM, sig_handler) == SIG_ERR) error("SIGTERM");
    u = open(DEVICE_PATH, O_RDWR|O_NOCTTY, 0);
    if (u < 0) error("usb open");

    char buff[65536];

    while (1)
    {	
	int l = read(u, buff, sizeof(buff));
	if (l <= 0)
	{
	    printf("usb eof\n");
	    close(u);
	    exit(0);
	}
	char cmd = buff[0];
	char arg = buff[1];
	uint32_t len = *((long int*)&buff[2]);
	//printf("cmd=%d, arg=%d, len=%d\n", cmd, arg, len);
	char* data = &buff[6];
	if (len + 6 != l)
	{
	    printf("invalid size: %d != %d\n", l, len);
	    close(u);
	    exit(0);
	}

	switch (cmd)
	{
	    case CMD_SHELL_NEW_REQ:
		shell_new_connection();
		break;
	    case CMD_SHELL_IN:
		shell_data(arg, data, len);
		break;
	    case CMD_SHELL_KILL:
		shell_kill(arg);
		break;
	    case CMD_SHELL_KILL_ALL:
		shell_kill_all();
		break;
	}
    }
    return 0;
}

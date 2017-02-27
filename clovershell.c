#define _XOPEN_SOURCE 600
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
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
#define MAX_CONNECTIONS 255
#define TYPE_SHELL 0
#define TYPE_CLOSE 0xFF

struct connection
{
    int pid;
    int type;
    int fdm;
    char fds[128];
};

struct connection* connections[MAX_CONNECTIONS];

int u = 0;
int fdm = 0;

void error(char* err)
{
    perror(err);
    exit(1);
}

void sig_handler(int signo)
{
    if (u) close(u);
    if (fdm) close(fdm);
    exit(0);
}

int new_connection()
{
    int i;
    for(i = 0; i < MAX_CONNECTIONS; i++)
	if (!connections[i])
	{
	    connections[i] = malloc(sizeof(struct connection));
	    memset(connections[i], 0, sizeof(struct connection));
	    return i+1;
	}
    printf("too many connections\n");
    return 0;
}

void clean_connections()
{
    int i;
    for(i = 0; i < MAX_CONNECTIONS; i++)
	if (connections[i] && (kill(connections[i]->pid, 0)))
	{
	    printf("freeing connection %d\n", i+1);
	    char buff[2];
	    buff[0] = TYPE_CLOSE;
	    buff[1] = i+1;
	    write(u, buff, 2);
	    free(connections[i]);
	    connections[i] = 0;
	}
}

void mthread(struct connection* c, int id)
{
    char buff[1024*10];

    if ((c->type == TYPE_SHELL) && (!(c->pid = fork())))
    {
        close(u);
        close(fdm);
        char g[128];
        sprintf(g, LOGIN_PROG, c->fds);
        execl(cmd, arg0, arg1, arg2, g, NULL);
        error("exec getty");
    }

    while (1)
    {
	int l = read(c->fdm, buff+2, sizeof(buff-2));
	//printf("received %d bytes from %d(%s)", l, id, c->fds);
	if (l > 0)
	{
	    buff[0] = c->type;
	    buff[1] = id;
	    if (write(u, buff, l+2) < 0)
	    {
		printf("usb write error\n");
		close(u);
		close(fdm);
		kill(c->pid, SIGTERM);
		free(c);
		exit(0);
	    }
	} else {
	    printf("tty %d(%s) eof\n", id, c->fds);
	    buff[0] = TYPE_CLOSE;
	    buff[1] = id;
	    write(u, buff, 2);
	    close(u);
	    close(c->fdm);
	    kill(c->pid, SIGTERM);
	    free(c);
	    exit(0);
	}
    }
}

int main()
{
    printf("clovershell (c) cluster, 2017 (built time: %s %s)\n", __DATE__, __TIME__);
    int i;
    for(i = 0; i < MAX_CONNECTIONS; i++)
	connections[i] = NULL;
    if (signal(SIGTERM, sig_handler) == SIG_ERR) error("SIGTERM");
    u = open(DEVICE_PATH, O_RDWR|O_NOCTTY, 0);
    if (u < 0) error("usb open");

    char buff[1024*10];

    while (1)
    {
	int l = read(u, buff, sizeof(buff));
	if (l > 0)
	{
	    if (l >= 2)
	    {
		unsigned char type = buff[0];
		unsigned char arg = buff[1];
	        if (type == TYPE_SHELL)
		{
		    if (arg == 0) // new connection/request
		    {
			int cid = new_connection();
			struct connection* c = connections[cid-1];
			c->type = buff[0];
			c->fdm = posix_openpt(O_RDWR|O_NOCTTY); 
			if (c->fdm < 0) error("posix_openpt");
			if (grantpt(c->fdm)) error("grantpt");
			if (unlockpt(c->fdm)) error("unlockpt");
			ptsname_r(c->fdm, c->fds, sizeof(c->fds));
			printf("created %d(%s)\n", cid, c->fds);
			c->pid = fork();
			if (!c->pid) mthread(c, cid);
		    } else {
			struct connection* c = connections[arg-1];
			//printf("sent %d bytes to %d(%s)", l-2, arg, c->fds);
			if (!c)
			{
			    printf("invalid id: %d\n", arg);
			}
			else if (write(c->fdm, buff+2, l-2) < 0)
			{
			    printf("fdm %d(%s) write error\n", arg, c->fds);
			    close(c->fdm);
			    kill(c->pid, SIGTERM);
			    free(connections[arg-1]);
			    connections[arg-1] = NULL;
			}
		    }
		}
		else if (type == TYPE_CLOSE)
		{
		    struct connection* c = connections[arg-1];
		    if (c)
		    {
			printf("connection %d(%s) closed\n", arg, c->fds);
			close(c->fdm);
			kill(c->pid, SIGTERM);
			free(connections[arg-1]);
			connections[arg-1] = NULL;
		}
		}
	    } else {
		printf("usb packet too short: %d\n", l);
	    }
	} else {
	    printf("usb eof\n");
	    close(u);
	    close(fdm);
	    exit(0);
	}
	clean_connections();
    }
    return 0;
}

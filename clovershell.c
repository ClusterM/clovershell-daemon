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
#include <sys/wait.h>
#include <termios.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>

#define DEVICE_PATH "/dev/usb_clover"
char* cmd = "/bin/su";
char* arg0 = "su";
char* arg1 = "root";
char* arg2 = "-c";
#define LOGIN_PROG "getty 0 -L %s vt102"
#define MAX_SHELL_CONNECTIONS 256
#define MAX_EXEC_CONNECTIONS 256
#define WRITE_BUFFER_SIZE 32767
#define READ_BUFFER_SIZE  32767
#define CLEANUP_INTERVAL 60

#define CMD_PING 0
#define CMD_PONG 1
#define CMD_SHELL_NEW_REQ 2
#define CMD_SHELL_NEW_RESP 3
#define CMD_SHELL_IN 4
#define CMD_SHELL_OUT 5
#define CMD_SHELL_CLOSED 6
#define CMD_SHELL_KILL 7
#define CMD_SHELL_KILL_ALL 8
#define CMD_EXEC_NEW_REQ 9
#define CMD_EXEC_NEW_RESP 10
#define CMD_EXEC_PID 11
#define CMD_EXEC_STDIN 12
#define CMD_EXEC_STDOUT 13
#define CMD_EXEC_STDERR 14
#define CMD_EXEC_RESULT 15
#define CMD_EXEC_KILL 16
#define CMD_EXEC_KILL_ALL 17

struct shell_connection
{
    int reading_pid;
    int shell_pid;
    int fdm;
    char fds[128];
};

struct exec_connection
{
    int exec_wait_pid;
    int exec_pid;
    int stdin[2];
    int stdout[2];
    int stderr[2];
};

struct shell_connection* shell_connections[MAX_SHELL_CONNECTIONS];
struct exec_connection* exec_connections[MAX_EXEC_CONNECTIONS];

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

void write_usb(char* data, int len)
{
    while (len > 0)
    {
	int l = write(u, data, len);
	if (l >= 0)
	{
	    data += l;
	    len -= l;
	} else {
	    error("can't write to USB");
	}
    }
}

void shell_read_thread(struct shell_connection* c, int id)
{
    char buff[WRITE_BUFFER_SIZE];

    // reading pseudo-tty in a loop
    while (1)
    {
	long int l = read(c->fdm, buff+4, sizeof(buff)-4);
	if (l > 0)
	{
	    // send received data to USB, including CMD and ID
	    buff[0] = CMD_SHELL_OUT;
	    buff[1] = id;
	    *((uint16_t*)&buff[2]) = l;
	    write_usb(buff, l+4);
	} else { // end of session
	    printf("tty %d(%s) eof\n", id, c->fds);
	    buff[0] = CMD_SHELL_CLOSED;
	    buff[1] = id;
	    buff[2] = buff[3] = 0;
	    write_usb(buff, 4);
	    close(u);
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

    char buff[4];
    buff[0] = CMD_SHELL_NEW_RESP;
    buff[1] = id;
    buff[2] = buff[3] = 0;
    write_usb(buff, 4);

    // starting getty
    if (!(c->shell_pid = fork()))
    {
        close(u);
        close(c->fdm);
        char g[128];
        sprintf(g, LOGIN_PROG, c->fds);
        execl(cmd, arg0, arg1, arg2, g, NULL);
        error("exec getty");
    }

    // reading thread
    if (!(c->reading_pid = fork()))
        shell_read_thread(c, id);
}

void shell_data(int id, char* data, uint16_t len)
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
	if (c->shell_pid) kill(c->shell_pid, SIGKILL);
	if (c->reading_pid) kill(c->reading_pid, SIGKILL);
	exit(0);
    }
}

void read_exec_out(struct exec_connection* c, int id)
{
    char buff[WRITE_BUFFER_SIZE];
    char stdout_done = 0;
    char stderr_done = 0;
    int stdout = c->stdout[0];
    int stderr = c->stderr[0];
    while (!stdout_done || !stderr_done)
    {
	fd_set read_fds;
	FD_ZERO(&read_fds);
	int fdmax = 0;
	if (!stdout_done)
	{
	    if (stdout > fdmax) fdmax = stdout;
	    FD_SET(stdout, &read_fds);
	}
	if (!stderr_done)
	{
	    if (stderr > fdmax) fdmax = stderr;
	    FD_SET(stderr, &read_fds);
	}
	if (select(fdmax+1, &read_fds, NULL, NULL, NULL) == -1)
	    error("select");
	if(FD_ISSET(stdout, &read_fds))
	{
	    long int l = read(stdout, buff+4, sizeof(buff)-4);
	    buff[0] = CMD_EXEC_STDOUT;
	    buff[1] = id;
	    if (l > 0)
	    {
		*((uint16_t*)&buff[2]) = l;
		write_usb(buff, l+4);
	    } else {
		buff[2] = buff[3] = 0;
		write_usb(buff, 4);
		close(stdout);
		stdout_done = 1;
	    }
	}
	if(FD_ISSET(stderr, &read_fds))
	{
	    long int l = read(stderr, buff+4, sizeof(buff)-4);
	    buff[0] = CMD_EXEC_STDERR;
	    buff[1] = id;
	    if (l > 0)
	    {
		*((uint16_t*)&buff[2]) = l;
		write_usb(buff, l+4);
	    } else {
		buff[2] = buff[3] = 0;
		write_usb(buff, 4);
		close(stderr);
		stderr_done = 1;
	    }
	}
    }
}

void exec_new_connection(char* cmd, uint16_t len)
{
    cmd[len] = 0;
    int id;
    for(id = 0; id < MAX_EXEC_CONNECTIONS; id++)
	if (!exec_connections[id])
	    break;
    if (id >= MAX_EXEC_CONNECTIONS)
    {
        printf("too many shell connections\n");
        return;
    }
    exec_connections[id] = malloc(sizeof(struct exec_connection));
    memset(exec_connections[id], 0, sizeof(struct exec_connection));
    struct exec_connection* c = exec_connections[id];

    printf("executing %s\n", cmd);

    char buff[1024];
    buff[0] = CMD_EXEC_NEW_RESP;
    buff[1] = id;
    *((uint16_t*)&buff[2]) = len;
    strcpy(&buff[4], cmd);
    write_usb(buff, 4+len);

    pipe(c->stdin);
    pipe(c->stdout);
    pipe(c->stderr);

    // executing
    if (!(c->exec_wait_pid = fork()))
    {
	close(STDIN_FILENO);
	close(STDOUT_FILENO);
	close(STDERR_FILENO);
	dup2(c->stdin[0], STDIN_FILENO);
	close(c->stdin[0]);
	close(c->stdin[1]); // unused
	dup2(c->stdout[1], STDOUT_FILENO);
	close(c->stdout[1]);
	dup2(c->stderr[1], STDERR_FILENO);
	close(c->stderr[1]);
	if (!(c->exec_pid = fork()))
	{
	    execl("/bin/sh", "sh", "-c", cmd, (char *) 0);
	    exit(1);
	}
	close(STDIN_FILENO);
	close(STDOUT_FILENO);
	close(STDERR_FILENO);
	read_exec_out(c, id);
	int status = -1;
	waitpid(c->exec_pid, &status, 0);
	int ret = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
	buff[0] = CMD_EXEC_RESULT;
	buff[1] = id;
	*((uint16_t*)&buff[2]) = sizeof(ret);
	*((int*)&buff[4]) = ret;
	write_usb(buff, 4+sizeof(ret));
        close(u);
	exit(0);
    }

    close(c->stdin[0]); // unused in this thread
    close(c->stdout[0]);
    close(c->stdout[1]);
    close(c->stderr[0]);
    close(c->stderr[1]);
}

void exec_stdin(int id, char* data, uint16_t len)
{
    struct exec_connection* c = exec_connections[id];
    if (!c)
    {
	printf("invalid id: %d\n", id);
	return;
    }
    if (len > 0)
    {
	if (write(c->stdin[1], data, len) < 0)
	{
	    printf("exec %d write error\n", id);
	    if (c->exec_pid) kill(c->exec_pid, SIGKILL);
	    if (c->exec_wait_pid) kill(c->exec_wait_pid, SIGKILL);
	    exit(0);
	}
    } else close(c->stdin[1]);
}

void cleanup()
{
    // Zombie hunt!
    int id;
    for (id = 0; id < MAX_SHELL_CONNECTIONS; id++)
    {
	struct shell_connection* c = shell_connections[id];
	if (c)
	{
	    //printf("Checking shell %d\n", id);
	    char dead = 1;
	    if (c->reading_pid && (waitpid(c->reading_pid, NULL, WNOHANG) == 0))
		dead = 0;
	    else
		c->reading_pid = 0;
	    if (c->shell_pid && (waitpid(c->shell_pid, NULL, WNOHANG) == 0))
		dead = 0;
	    else
		c->shell_pid = 0;
	    if (dead)
	    {
		printf("cleaning %d shell connection\n", id);
		close(c->fdm);
		free(c);
		shell_connections[id] = NULL;
	    }
	}
    }
    for (id = 0; id < MAX_EXEC_CONNECTIONS; id++)
    {
	struct exec_connection* c = exec_connections[id];
	if (c)
	{
	    //printf("Checking exec %d\n", id);
	    char dead = 1;
	    if (c->exec_wait_pid && (waitpid(c->exec_wait_pid, NULL, WNOHANG) == 0))
		dead = 0;
	    else
		c->exec_wait_pid = 0;
	    if (c->exec_pid && (waitpid(c->exec_pid, NULL, WNOHANG) == 0))
		dead = 0;
	    else
		c->exec_pid = 0;
	    if (dead)
	    {
		printf("cleaning %d exec connection\n", id);
		close(c->stdin[1]);
		free(c);
		exec_connections[id] = NULL;
	    }
	}
    }

}

void shell_kill(int id)
{
    struct shell_connection* c = shell_connections[id];
    if (!c) return;
    close(c->fdm);
    if (c->shell_pid) kill(c->shell_pid, SIGKILL);
    if (c->reading_pid) kill(c->reading_pid, SIGKILL);
    //free(shell_connections[id]);
    //shell_connections[id] = NULL;
    printf("shell session %d killed\n", id);
}

void shell_kill_all()
{
    int id;
    for (id = 0; id < MAX_SHELL_CONNECTIONS; id++)
	if (shell_connections[id]) shell_kill(id);
    cleanup();
}

void exec_kill(int id)
{
    struct exec_connection* c = exec_connections[id];
    if (!c) return;
    close(c->stdin[1]);
    if (c->exec_pid) kill(c->exec_pid, SIGKILL);
    if (c->exec_wait_pid) kill(c->exec_wait_pid, SIGKILL);
    //free(exec_connections[id]);
    //exec_connections[id] = NULL;
    printf("exec session %d killed\n", id);
}

void exec_kill_all()
{
    int id;
    for (id = 0; id < MAX_EXEC_CONNECTIONS; id++)
	if (exec_connections[id]) exec_kill(id);
    cleanup();
}

int main(int argc, char **argv)
{
    printf("clovershell (c) cluster, 2017 (built time: %s %s)\n", __DATE__, __TIME__);
    int i;
    for(i = 0; i < MAX_SHELL_CONNECTIONS; i++)
	shell_connections[i] = NULL;
    for(i = 0; i < MAX_EXEC_CONNECTIONS; i++)
	exec_connections[i] = NULL;

    if (signal(SIGTERM, sig_handler) == SIG_ERR) error("SIGTERM");
    u = open(DEVICE_PATH, O_RDWR|O_NOCTTY|O_DSYNC);
    if (u < 0) error("usb open");

    char buff[READ_BUFFER_SIZE];
    time_t clean_time = time(NULL);

    if (argc >= 2 && strcmp(argv[1], "--daemon") == 0)
	daemon(1,1);

    while (1)
    {	
	long int l = read(u, buff, sizeof(buff));
	if (l <= 0)
	{
	    printf("usb eof\n");
	    close(u);
	    exit(0);
	}
	char cmd = buff[0];
	char arg = buff[1];
	uint16_t len = *((uint16_t*)&buff[2]);
	//printf("cmd=%d, arg=%d, len=%d\n", cmd, arg, len);
	char* data = &buff[4];
	if (len + 4 != l)
	{
	    //printf("invalid size: %d != %d\n", l, len);
	    continue;
	}

	switch (cmd)
	{
	    case CMD_PING:
		printf("PING? PONG!\n");
		buff[0] = CMD_PONG;
		write_usb(buff, l);
		break;
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
	    case CMD_EXEC_NEW_REQ:
		exec_new_connection(data, len);
		break;
	    case CMD_EXEC_STDIN:
		exec_stdin(arg, data, len);
		break;
	    case CMD_EXEC_KILL:
		exec_kill(arg);
		break;
	    case CMD_EXEC_KILL_ALL:
		exec_kill_all();
		break;
	}
	if (time(NULL) - clean_time > CLEANUP_INTERVAL)
	{
	    //printf("Cleaing up\n");
	    cleanup();
	    clean_time = time(NULL);
	}
    }
    return 0;
}

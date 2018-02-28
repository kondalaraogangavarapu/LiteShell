#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/wait.h>
#include <setjmp.h>
#include <signal.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <errno.h>
#include <fcntl.h>

extern char **environ; 

#define MAX_ARG_LIST	64
#define DEFAULT_PROMPT 	'$'
#define MAX_PROMPT_SIZE	200
#define MAX_HOSTNAME_SIZE	200
#define MAX_PATH_SIZE	200

const char *const condensePathStr(char *path);
const char *const expandPathStr(char *path);

#define MAX_CMD_SIZE	50
#define SEARCH_FOR_CMD	-1
typedef void (*builtInFunc)(char **);
typedef struct {
	char cmd[MAX_CMD_SIZE];
	builtInFunc func;
} builtInCmd;

/* built-in commands */
#define MAX_PATH_SIZE	200
void execCD(char *args[]);
void execExit(char *args[]);
void execHelp(char *args[]);
builtInCmd builtInCmds[] = {
	{"help", execHelp},
	{"exit", execExit},
	{"cd", execCD},
};
int builtInCnt = sizeof(builtInCmds)/sizeof(builtInCmd);

int isBuiltIn(char *cmd);
void execBuiltIn(int i, char *args[]);

/* control buffer and handler for SIGINT signal capture */
sigjmp_buf ctrlc_buf;
void ctrl_hndlr(int signo) {
   	siglongjmp(ctrlc_buf, 1);
}

/* error functions */
char *shname;
void error(int code, char *msg);
void warn(char *msg);

/* Piping Functions*/
void execPipe(char *,char *);
/* Redirection Functions */
void execRedirectOut(char *, char *);
void execRedirectIn(char *, char *);
void execRedirectAppend(char *,char *);

int main(int argc, char *argv[]) {

	char *line;
	pid_t childPID;
	int argn;
	char *args[MAX_ARG_LIST];
	char *tok,*subtok,*saveptr1,*saveptr,*saveptr2,*cmd1,*cmd2;
	int cmdn;
	char hostname[MAX_HOSTNAME_SIZE];
	char prompt[MAX_PROMPT_SIZE];
	char prompt_sep = DEFAULT_PROMPT;
	char *outFile,*inFile;

	// get shell name
	shname = strrchr(argv[0], '/');
	if (shname == NULL)
		shname = argv[0];
	else
		++shname;

	// command-line completion
	rl_bind_key('\t', rl_complete);

	// get hostname
	gethostname(hostname, MAX_HOSTNAME_SIZE);

	/* set up control of SIGINT signal */
	if (signal(SIGINT, ctrl_hndlr) == SIG_ERR) 
		error(100, "Failed to register interrupts in kernel\n");
	while (sigsetjmp(ctrlc_buf, 1) != 0) 
		/* empty */;

	for(;;) {
		// build prompt
		snprintf(prompt, MAX_PROMPT_SIZE, "%s@%s:%s%c ", 
			getenv("USER"), hostname, condensePathStr(getcwd(NULL,0)), prompt_sep);

		// get command-line
		line = readline(prompt);

		if (!line) // feof(stdin)
			break;
		
		// process command-line
		if (line[strlen(line)-1] == '\n')
			line[strlen(line)-1] = '\0';
		add_history(line);

		// build command list	
		for(;;line=NULL) {

			tok = strtok_r(line, ";",&saveptr);
			if(tok==NULL)
				break;
			//printf("%s %s\n",tok,strchr(tok,'|'));
			
			if(strchr(tok,'|')!=NULL) {
				cmd1=strtok_r(tok,"|",&saveptr1);
				cmd2=strtok_r(NULL,"|",&saveptr1);
		 		execPipe(cmd1,cmd2);
			}
			else if(strchr(tok,'>')!=NULL) {
				cmd1=strtok_r(tok,">",&saveptr1);
				outFile=strtok_r(NULL,">",&saveptr1);
				execRedirectOut(cmd1,outFile);
			}
			else if(strchr(tok,'<')!=NULL){
				cmd1=strtok_r(tok,"<",&saveptr1);
				inFile=strtok_r(NULL,"<",&saveptr1);
				execRedirectIn(cmd1,inFile);
			}
			else if(strstr(tok,">>")!=NULL){
				cmd1=strtok_r(tok,">>",&saveptr1);
				outFile=strtok_r(NULL,">>",&saveptr1);
				execRedirectAppend(cmd1,outFile);
			}
			else{
				// build argument list
				for (argn=0;argn<MAX_ARG_LIST; argn++,tok=NULL) {
					subtok = strtok_r(tok, " \t",&saveptr2);
					args[argn]=subtok;
					//printf("\t%s\n",subtok);
					if(subtok==NULL)
						break;
				}
	
				if ((cmdn=isBuiltIn(args[0])) >= 0) { // process built-in command
					execBuiltIn(cmdn, args);
				} else { // execute command
					childPID = fork();
					if (childPID == 0) {
						execvpe(args[0], args, environ);
						warn("command failed to execute");
						_exit(2);
					} else {
						waitpid(childPID, NULL, 0);
					}
				}
				bzero(args,MAX_ARG_LIST);
			}
		}

		fflush(stderr);
		fflush(stdout);
		free(line);
	}
	fputs("exit\n", stdout);

	return 0;
}

/* error functions */
void error(int code, char *msg) {
	fputs(shname, stderr);
	fputs(": ", stderr);
	fputs(msg, stderr);
	fputs("\n", stderr);
	if (code > 0)
		exit(code);
}
void warn(char *msg) {
	error(0, msg);
}

/* manage '~' for home path */
const char *const condensePathStr(char *path) {
	static char newpath[MAX_PATH_SIZE];

	newpath[0] = '\0';
	if (path != NULL) {
		if (strstr(path, getenv("HOME")) == path)
			snprintf(newpath, MAX_PATH_SIZE, "%c%s", '~', &path[strlen(getenv("HOME"))]);
		else
			snprintf(newpath, MAX_PATH_SIZE, "%s", path);
	}

	return newpath;
}

const char *const expandPathStr(char *path) {
	static char newpath[MAX_PATH_SIZE];

	newpath[0] = '\0';
	if (path != NULL) {
		if (path[0] == '~')
			snprintf(newpath, MAX_PATH_SIZE, "%s%s", getenv("HOME"), &path[1]);
		else
			snprintf(newpath, MAX_PATH_SIZE, "%s", path);
	}

	return newpath;
}

/* return index in the builtInCmds array or -1 for failure */
int isBuiltIn(char *cmd) {
	int i;
	for (i = 0; i < builtInCnt; ++i)
		if (strcmp(cmd,builtInCmds[i].cmd)==0)
			break;
	return i<builtInCnt?i:-1;
}

/* i is the index or SEARCH_FOR_CMD */
void execBuiltIn(int i, char *args[]) {
	if (i==SEARCH_FOR_CMD)
		i = isBuiltIn(args[0]);
	if (i>-1) 
		builtInCmds[i].func(args);
	else
		warn("unknown built-in command");
}

/* built-in functions */
void execHelp(char *args[]) {
	warn("help unavailable at the moment");
}

void execExit(char *args[]) {
	int code=0;
	if (args[2] != NULL)
		error(1, "exit: too many arguments");
	for(int i=0;i<strlen(args[1]);++i)
		if (!isdigit(args[1][i]))
			error(2, "exit: numeric argument required");
	code=atoi(args[1]);
	exit(code);
}

void execCD(char *args[]) {
	int err = 0;
	char path[MAX_PATH_SIZE];
	if (args[1] == NULL)
		snprintf(path, MAX_PATH_SIZE, "%s", getenv("HOME"));
	else
		snprintf(path, MAX_PATH_SIZE, "%s", expandPathStr(args[1]));
	err = chdir(path);
	if (err<0)
		warn(strerror(errno));
}

void execPipe(char *cmd1, char *cmd2) {
	int argn,pipes[2];
	pid_t childPID1,childPID2;
	char *tok1,*tok2,*args1[MAX_ARG_LIST],*args2[MAX_ARG_LIST],*saveptr1,*saveptr2;
	fflush(stdout);
	fflush(stderr);
	for(argn=0;argn<MAX_ARG_LIST;argn++,cmd1=NULL){
		tok1=strtok_r(cmd1," \t",&saveptr1);
		args1[argn]=tok1;
		if(tok1==NULL)
			break;
	}

	for(argn=0;argn<MAX_ARG_LIST;argn++,cmd2=NULL){
		tok2=strtok_r(cmd2," \t",&saveptr2);
		args2[argn]=tok2;
		if(tok2==NULL)
			break;
	}

	pipe(pipes);
	childPID1 = fork();
	if (childPID1 == 0) {
		close(pipes[1]);
        	dup2(pipes[0], STDIN_FILENO);
		execvpe(args2[0],args2,environ);
        }
	childPID2 = fork();
	if (childPID2 == 0) {
		close(pipes[0]);
        	dup2(pipes[1], STDOUT_FILENO);
		execvpe(args1[0],args1,environ);
        }

	close(pipes[0]);
	close(pipes[1]);

	waitpid(childPID1, NULL, 0);

	waitpid(childPID2, NULL, 0);
           
}


void execRedirectOut(char* cmd, char* outFile){
	char *saveptr,*tok;
	int argn,fd;
	pid_t pid;
	char *args[MAX_ARG_LIST];
        fflush(stdout);
        fflush(stderr);
        for(argn=0;argn<MAX_ARG_LIST;argn++,cmd=NULL){
                tok=strtok_r(cmd," \t",&saveptr);
                args[argn]=tok;
                if(tok==NULL)
                        break;
        }

	pid=fork();
	if(pid==0){
		fd=open(outFile,O_CREAT|O_WRONLY,0644);
		dup2(fd,STDOUT_FILENO);
		execvpe(args[0],args,environ);
		close(fd);
	}
	waitpid(pid,NULL,0);

}

void execRedirectIn(char* cmd, char* inFile){
        char *saveptr,*tok;
        int argn,fd;
        pid_t pid;
        char *args[MAX_ARG_LIST];
        fflush(stdout);
        fflush(stderr);
        for(argn=0;argn<MAX_ARG_LIST;argn++,cmd=NULL){
                tok=strtok_r(cmd," \t",&saveptr);
                args[argn]=tok;
                if(tok==NULL)
                        break;
        }

        pid=fork();
        if(pid==0){
                fd=open(inFile,O_RDONLY);
                dup2(fd,STDIN_FILENO);
                execvpe(args[0],args,environ);
                close(fd);
        }
        waitpid(pid,NULL,0);

}


void execRedirectAppend(char* cmd, char* outFile){
        char *saveptr,*tok;
        int argn,fd;
        pid_t pid;
        char *args[MAX_ARG_LIST];
        fflush(stdout);
        fflush(stderr);
        for(argn=0;argn<MAX_ARG_LIST;argn++,cmd=NULL){
                tok=strtok_r(cmd," \t",&saveptr);
                args[argn]=tok;
                if(tok==NULL)
                        break;
        }

        pid=fork();
        if(pid==0){
                fd=open(outFile,O_CREAT|O_WRONLY|O_APPEND,0644);
                dup2(fd,STDOUT_FILENO);
                execvpe(args[0],args,environ);
                close(fd);
        }
        waitpid(pid,NULL,0);

}



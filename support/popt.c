#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if HAVE_ALLOCA_H
# include <alloca.h>
#endif

#include "findme.h"
#include "popt.h"

struct optionStackEntry {
    int argc;
    char ** argv;
    int next;
    char * nextArg;
    char * nextCharArg;
    struct poptAlias * currAlias;
    int stuffed;
};

struct execEntry {
    char * longName;
    char shortName;
    char * script;
};

struct poptContext_s {
    struct optionStackEntry optionStack[POPT_OPTION_DEPTH], * os;
    char ** leftovers;
    int numLeftovers;
    int nextLeftover;
    const struct poptOption * options;
    int restLeftover;
    char * appName;
    struct poptAlias * aliases;
    int numAliases;
    int flags;
    struct execEntry * execs;
    int numExecs;
    char ** finalArgv;
    int finalArgvCount;
    int finalArgvAlloced;
    struct execEntry * doExec;
    char * execPath;
    int execAbsolute;
};

#ifndef HAVE_STRERROR
static char * strerror(int errno) {
    extern int sys_nerr;
    extern char * sys_errlist[];

    if ((0 <= errno) && (errno < sys_nerr))
	return sys_errlist[errno];
    else
	return "unknown errno";
}
#endif

void poptSetExecPath(poptContext con, const char * path, int allowAbsolute) {
    if (con->execPath) free(con->execPath);
    con->execPath = strdup(path);
    con->execAbsolute = allowAbsolute;
}

poptContext poptGetContext(char * name, int argc, char ** argv, 
			   const struct poptOption * options, int flags) {
    poptContext con = malloc(sizeof(*con));

    memset(con, 0, sizeof(*con));

    con->os = con->optionStack;
    con->os->argc = argc;
    con->os->argv = argv;

    if (!(flags & POPT_CONTEXT_KEEP_FIRST))
	con->os->next = 1;			/* skip argv[0] */

    con->leftovers = malloc(sizeof(char *) * (argc + 1));
    con->options = options;
    con->finalArgv = malloc(sizeof(*con->finalArgv) * (argc * 2));
    con->finalArgvAlloced = argc * 2;
    con->flags = flags;
    
    if (name)
	con->appName = strcpy(malloc(strlen(name) + 1), name);

    return con;
}

void poptResetContext(poptContext con) {
    con->os = con->optionStack;
    con->os->currAlias = NULL;
    con->os->nextCharArg = NULL;
    con->os->nextArg = NULL;
    con->os->next = 1;			/* skip argv[0] */

    con->numLeftovers = 0;
    con->nextLeftover = 0;
    con->restLeftover = 0;
    con->doExec = NULL;
    con->finalArgvCount = 0;
}

/* Only one of longName, shortName may be set at a time */
static int handleExec(poptContext con, char * longName, char shortName) {
    int i;

    i = con->numExecs - 1;
    if (longName) {
	while (i >= 0 && (!con->execs[i].longName ||
	    strcmp(con->execs[i].longName, longName))) i--;
    } else {
	while (i >= 0 &&
	    con->execs[i].shortName != shortName) i--;
    }

    if (i < 0) return 0;

    if (con->flags & POPT_CONTEXT_NO_EXEC)
	return 1;

    if (!con->doExec) {
	con->doExec = con->execs + i;
	return 1;
    }

    /* We already have an exec to do; remember this option for next
       time 'round */
    if ((con->finalArgvCount + 1) >= (con->finalArgvAlloced)) {
	con->finalArgvAlloced += 10;
	con->finalArgv = realloc(con->finalArgv,
			sizeof(*con->finalArgv) * con->finalArgvAlloced);
    }

    i = con->finalArgvCount++;
    con->finalArgv[i] = malloc((longName ? strlen(longName) : 0) + 3);
    if (longName)
	sprintf(con->finalArgv[i], "--%s", longName);
    else 
	sprintf(con->finalArgv[i], "-%c", shortName);

    return 1;
}

/* Only one of longName, shortName may be set at a time */
static int handleAlias(poptContext con, char * longName, char shortName,
		       char * nextCharArg) {
    int i;

    if (con->os->currAlias && con->os->currAlias->longName && longName &&
	!strcmp(con->os->currAlias->longName, longName)) 
	return 0;
    if (con->os->currAlias && shortName == con->os->currAlias->shortName)
	return 0;

    i = con->numAliases - 1;
    if (longName) {
	while (i >= 0 && (!con->aliases[i].longName ||
	    strcmp(con->aliases[i].longName, longName))) i--;
    } else {
	while (i >= 0 &&
	    con->aliases[i].shortName != shortName) i--;
    }

    if (i < 0) return 0;

    if ((con->os - con->optionStack + 1) 
	    == POPT_OPTION_DEPTH)
	return POPT_ERROR_OPTSTOODEEP;

    if (nextCharArg && *nextCharArg)
	con->os->nextCharArg = nextCharArg;

    con->os++;
    con->os->next = 0;
    con->os->stuffed = 0;
    con->os->nextArg = con->os->nextCharArg = NULL;
    con->os->currAlias = con->aliases + i;
    con->os->argc = con->os->currAlias->argc;
    con->os->argv = con->os->currAlias->argv;

    return 1;
}

static void execCommand(poptContext con) {
    char ** argv;
    int pos = 0;
    char * script = con->doExec->script;

    argv = malloc(sizeof(*argv) * 
			(6 + con->numLeftovers + con->finalArgvCount));

    if (!con->execAbsolute && strchr(script, '/')) return;

    if (!strchr(script, '/') && con->execPath) {
	argv[pos] = alloca(strlen(con->execPath) + strlen(script) + 2);
	sprintf(argv[pos], "%s/%s", con->execPath, script);
    } else {
	argv[pos] = script;
    }
    pos++;

    argv[pos] = findProgramPath(con->os->argv[0]);
    if (argv[pos]) pos++;
    argv[pos++] = ";";

    memcpy(argv + pos, con->finalArgv, sizeof(*argv) * con->finalArgvCount);
    pos += con->finalArgvCount;

    if (con->numLeftovers) {
	argv[pos++] = "--";
	memcpy(argv + pos, con->leftovers, sizeof(*argv) * con->numLeftovers);
	pos += con->numLeftovers;
    }

    argv[pos++] = NULL;

#ifdef __hpux
    setresuid(getuid(), getuid(),-1);
#else
    setreuid(getuid(), getuid()); /*hlauer: not portable to hpux9.01 */
#endif

    execvp(argv[0], argv);
}

/* returns 'val' element, -1 on last item, POPT_ERROR_* on error */
int poptGetNextOpt(poptContext con) {
    char * optString, * chptr, * localOptString;
    char * longArg = NULL;
    char * origOptString;
    long aLong;
    char * end;
    const struct poptOption * opt = NULL;
    int done = 0;
    int i;

    while (!done) {
	while (!con->os->nextCharArg && con->os->next == con->os->argc 
		&& con->os > con->optionStack)
	    con->os--;
	if (!con->os->nextCharArg && con->os->next == con->os->argc) {
	    if (con->doExec) execCommand(con);
	    return -1;
	}

	if (!con->os->nextCharArg) {
		
	    origOptString = con->os->argv[con->os->next++];

	    if (con->restLeftover || *origOptString != '-') {
		con->leftovers[con->numLeftovers++] = origOptString;
		continue;
	    }

	    /* Make a copy we can hack at */
	    localOptString = optString = 
			strcpy(alloca(strlen(origOptString) + 1), 
			origOptString);

	    if (!optString[0])
		return POPT_ERROR_BADOPT;

	    if (optString[1] == '-' && !optString[2]) {
		con->restLeftover = 1;
		continue;
	    } else if (optString[1] == '-') {
		optString += 2;

		if (handleAlias(con, optString, '\0', NULL))
		    continue;
		if (handleExec(con, optString, '\0'))
		    continue;

		chptr = optString;
		while (*chptr && *chptr != '=') chptr++;
		if (*chptr == '=') {
		    longArg = origOptString + (chptr - localOptString) + 1;
		    *chptr = '\0';
		}

		opt = con->options;
		while (opt->longName || opt->shortName) {
		    if (opt->longName && !strcmp(optString, opt->longName))
			break;
		    opt++;
		}

		if (!opt->longName && !opt->shortName) return POPT_ERROR_BADOPT;
	    } else 
		con->os->nextCharArg = origOptString + 1;
	}

	if (con->os->nextCharArg) {
	    origOptString = con->os->nextCharArg;

	    con->os->nextCharArg = NULL;

	    if (handleAlias(con, NULL, *origOptString,
			    origOptString + 1)) {
		origOptString++;
		continue;
	    }
	    if (handleExec(con, NULL, *origOptString))
		continue;

	    opt = con->options;
	    while ((opt->longName || opt->shortName) && 
		    *origOptString != opt->shortName) opt++;
	    if (!opt->longName && !opt->shortName) return POPT_ERROR_BADOPT;

	    origOptString++;
	    if (*origOptString)
		con->os->nextCharArg = origOptString;
	}

	if (opt->arg && opt->argInfo == POPT_ARG_NONE) 
	    *((int *)opt->arg) = 1;
	else if (opt->argInfo != POPT_ARG_NONE) {
	    if (longArg) {
		con->os->nextArg = longArg;
	    } else if (con->os->nextCharArg) {
		con->os->nextArg = con->os->nextCharArg;
		con->os->nextCharArg = NULL;
	    } else { 
		while (con->os->next == con->os->argc && 
		       con->os > con->optionStack)
		    con->os--;
		if (con->os->next == con->os->argc)
		    return POPT_ERROR_NOARG;

		con->os->nextArg = con->os->argv[con->os->next++];
	    }

	    if (opt->arg) {
		switch (opt->argInfo) {
		  case POPT_ARG_STRING:
		    *((char **) opt->arg) = con->os->nextArg;
		    break;

		  case POPT_ARG_INT:
		  case POPT_ARG_LONG:
		    aLong = strtol(con->os->nextArg, &end, 0);
		    if (*end) 
			return POPT_ERROR_BADNUMBER;

		    if (aLong == LONG_MIN || aLong == LONG_MAX)
			return POPT_ERROR_OVERFLOW;
		    if (opt->argInfo == POPT_ARG_LONG) {
			*((long *) opt->arg) = aLong;
		    } else {
			if (aLong > INT_MAX || aLong < INT_MIN)
			    return POPT_ERROR_OVERFLOW;
			*((int *) opt->arg) =aLong;
		    }
		    break;

		  default:
		    printf("option type not implemented in popt\n");
		    exit(1);
		}
	    }
	}

	if (opt->val) done = 1;

	if ((con->finalArgvCount + 2) >= (con->finalArgvAlloced)) {
	    con->finalArgvAlloced += 10;
	    con->finalArgv = realloc(con->finalArgv,
			    sizeof(*con->finalArgv) * con->finalArgvAlloced);
	}

	i = con->finalArgvCount++;
	con->finalArgv[i] = 
		malloc((opt->longName ? strlen(opt->longName) : 0) + 3);
	if (opt->longName)
	    sprintf(con->finalArgv[i], "--%s", opt->longName);
	else 
	    sprintf(con->finalArgv[i], "-%c", opt->shortName);

	if (opt->arg && opt->argInfo != POPT_ARG_NONE) 
	    con->finalArgv[con->finalArgvCount++] = strdup(con->os->nextArg);
    }

    return opt->val;
}

char * poptGetOptArg(poptContext con) {
    char * ret = con->os->nextArg;
    con->os->nextArg = NULL;
    return ret;
}

char * poptGetArg(poptContext con) {
    if (con->numLeftovers == con->nextLeftover) return NULL;
    return (con->leftovers[con->nextLeftover++]);
}

char * poptPeekArg(poptContext con) {
    if (con->numLeftovers == con->nextLeftover) return NULL;
    return (con->leftovers[con->nextLeftover]);
}

char ** poptGetArgs(poptContext con) {
    if (con->numLeftovers == con->nextLeftover) return NULL;

    /* some apps like [like RPM ;-) ] need this NULL terminated */
    con->leftovers[con->numLeftovers] = NULL;

    return (con->leftovers + con->nextLeftover);
}

void poptFreeContext(poptContext con) {
    int i;

    for (i = 0; i < con->numAliases; i++) {
	if (con->aliases[i].longName) free(con->aliases[i].longName);
	free(con->aliases[i].argv);
    }

    for (i = 0; i < con->numExecs; i++) {
	if (con->execs[i].longName) free(con->execs[i].longName);
	free(con->execs[i].script);
    }

    for (i = 0; i < con->finalArgvCount; i++)
	free(con->finalArgv[i]);

    free(con->leftovers);
    free(con->finalArgv);
    if (con->appName) free(con->appName);
    if (con->aliases) free(con->aliases);
    free(con);
}

int poptAddAlias(poptContext con, struct poptAlias newAlias, int flags) {
    int aliasNum = con->numAliases++;
    struct poptAlias * alias;

    /* SunOS won't realloc(NULL, ...) */
    if (!con->aliases)
	con->aliases = malloc(sizeof(newAlias) * con->numAliases);
    else
	con->aliases = realloc(con->aliases, 
			       sizeof(newAlias) * con->numAliases);
    alias = con->aliases + aliasNum;
    
    *alias = newAlias;
    if (alias->longName)
	alias->longName = strcpy(malloc(strlen(alias->longName) + 1), 
				    alias->longName);
    else
	alias->longName = NULL;

    return 0;
}

int poptParseArgvString(char * s, int * argcPtr, char *** argvPtr) {
    char * buf = strcpy(alloca(strlen(s) + 1), s);
    char * bufStart = buf;
    char * src, * dst;
    char quote = '\0';
    int argvAlloced = 5;
    char ** argv = malloc(sizeof(*argv) * argvAlloced);
    char ** argv2;
    int argc = 0;
    int i;

    src = s;
    dst = buf;
    argv[argc] = buf;

    memset(buf, '\0', strlen(s) + 1);

    while (*src) {
	if (quote == *src) {
	    quote = '\0';
	} else if (quote) {
	    if (*src == '\\') {
		src++;
		if (!*src) {
		    free(argv);
		    return POPT_ERROR_BADQUOTE;
		}
		if (*src != quote) *buf++ = '\\';
	    }
	    *buf++ = *src;
	} else if (isspace(*src)) {
	    if (*argv[argc]) {
		buf++, argc++;
		if (argc == argvAlloced) {
		    argvAlloced += 5;
		    argv = realloc(argv, sizeof(*argv) * argvAlloced);
		}
		argv[argc] = buf;
	    }
	} else switch (*src) {
	  case '"':
	  case '\'':
	    quote = *src;
	    break;
	  case '\\':
	    src++;
	    if (!*src) {
		free(argv);
		return POPT_ERROR_BADQUOTE;
	    }
	    /* fallthrough */
	  default:
	    *buf++ = *src;
	}

	src++;
    }

    if (strlen(argv[argc])) {
	argc++, buf++;
    }

    dst = malloc(argc * sizeof(*argv) + (buf - bufStart));
    argv2 = (void *) dst;
    dst += argc * sizeof(*argv);
    memcpy(argv2, argv, argc * sizeof(*argv));
    memcpy(dst, bufStart, buf - bufStart);

    for (i = 0; i < argc; i++) {
	argv2[i] = dst + (argv[i] - bufStart);
    }

    free(argv);

    *argvPtr = argv2;
    *argcPtr = argc;

    return 0;
}

static void configLine(poptContext con, char * line) {
    int nameLength = strlen(con->appName);
    char * opt;
    struct poptAlias alias;
    char * entryType;
    char * longName = NULL;
    char shortName = '\0';
    
    if (strncmp(line, con->appName, nameLength)) return;
    line += nameLength;
    if (!*line || !isspace(*line)) return;
    while (*line && isspace(*line)) line++;
    entryType = line;

    while (!*line || !isspace(*line)) line++;
    *line++ = '\0';
    while (*line && isspace(*line)) line++;
    if (!*line) return;
    opt = line;

    while (!*line || !isspace(*line)) line++;
    *line++ = '\0';
    while (*line && isspace(*line)) line++;
    if (!*line) return;

    if (opt[0] == '-' && opt[1] == '-')
	longName = opt + 2;
    else if (opt[0] == '-' && !opt[2])
	shortName = opt[1];

    if (!strcmp(entryType, "alias")) {
	if (poptParseArgvString(line, &alias.argc, &alias.argv)) return;
	alias.longName = longName, alias.shortName = shortName;
	poptAddAlias(con, alias, 0);
    } else if (!strcmp(entryType, "exec")) {
	con->execs = realloc(con->execs, 
				sizeof(*con->execs) * (con->numExecs + 1));
	if (longName)
	    con->execs[con->numExecs].longName = strdup(longName);
	else
	    con->execs[con->numExecs].longName = NULL;

	con->execs[con->numExecs].shortName = shortName;
	con->execs[con->numExecs].script = strdup(line);
	
	con->numExecs++;
    }
}

int poptReadConfigFile(poptContext con, char * fn) {
    char * file, * chptr, * end;
    char * buf, * dst;
    int fd, rc;
    int fileLength;

    fd = open(fn, O_RDONLY);
    if (fd < 0) {
	if (errno == ENOENT)
	    return 0;
	else 
	    return POPT_ERROR_ERRNO;
    }

    fileLength = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, 0);

    file = alloca(fileLength + 1);
    if ((fd = read(fd, file, fileLength)) != fileLength) {
	rc = errno;
	close(fd);
	errno = rc;
	return POPT_ERROR_ERRNO;
    }
    close(fd);

    dst = buf = alloca(fileLength + 1);

    chptr = file;
    end = (file + fileLength);
    while (chptr < end) {
	switch (*chptr) {
	  case '\n':
	    *dst = '\0';
	    dst = buf;
	    while (*dst && isspace(*dst)) dst++;
	    if (*dst && *dst != '#') {
		configLine(con, dst);
	    }
	    chptr++;
	    break;
	  case '\\':
	    *dst++ = *chptr++;
	    if (chptr < end) {
		if (*chptr == '\n') 
		    dst--, chptr++;	
		    /* \ at the end of a line does not insert a \n */
		else
		    *dst++ = *chptr++;
	    }
	    break;
	  default:
	    *dst++ = *chptr++;
	}
    }

    return 0;
}

int poptReadDefaultConfig(poptContext con, int useEnv) {
    char * fn, * home;
    int rc;

    if (!con->appName) return 0;

    rc = poptReadConfigFile(con, "/etc/popt");
    if (rc) return rc;
    if (getuid() != geteuid()) return 0;

    if ((home = getenv("HOME"))) {
	fn = alloca(strlen(home) + 20);
	sprintf(fn, "%s/.popt", home);
	rc = poptReadConfigFile(con, fn);
	if (rc) return rc;
    }

    return 0;
}

char * poptBadOption(poptContext con, int flags) {
    struct optionStackEntry * os;

    if (flags & POPT_BADOPTION_NOALIAS)
	os = con->optionStack;
    else
	os = con->os;

    return os->argv[os->next - 1];
}

#define POPT_ERROR_NOARG	-10
#define POPT_ERROR_BADOPT	-11
#define POPT_ERROR_OPTSTOODEEP	-13
#define POPT_ERROR_BADQUOTE	-15	/* only from poptParseArgString() */
#define POPT_ERROR_ERRNO	-16	/* only from poptParseArgString() */

const char * poptStrerror(const int error) {
    switch (error) {
      case POPT_ERROR_NOARG:
	return "missing argument";
      case POPT_ERROR_BADOPT:
	return "unknown option";
      case POPT_ERROR_OPTSTOODEEP:
	return "aliases nested too deeply";
      case POPT_ERROR_BADQUOTE:
	return "error in paramter quoting";
      case POPT_ERROR_BADNUMBER:
	return "invalid numeric value";
      case POPT_ERROR_OVERFLOW:
	return "number too large or too small";
      case POPT_ERROR_ERRNO:
	return strerror(errno);
      default:
	return "unknown error";
    }
}

int poptStuffArgs(poptContext con, char ** argv) {
    int i;

    if ((con->os - con->optionStack) == POPT_OPTION_DEPTH)
	return POPT_ERROR_OPTSTOODEEP;

    for (i = 0; argv[i]; i++);

    con->os++;
    con->os->next = 0;
    con->os->nextArg = con->os->nextCharArg = NULL;
    con->os->currAlias = NULL;
    con->os->argc = i;
    con->os->argv = argv;
    con->os->stuffed = 1;

    return 0;
}

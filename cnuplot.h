/*
  cnuplot - v0.0.1-dev
*/
#ifndef CNUPLOT_H
#define CNUPLOT_H
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>              
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <stdarg.h>
#include <sys/wait.h>
#include <time.h>

#define CNUPLOT_ARRAY_LEN(arr) (sizeof(arr)/sizeof(arr[0]))

typedef enum {
    CNUPLOT_DEBUG,
    CNUPLOT_INFO,
    CNUPLOT_ERROR,
} Cnuplot_Log_Level;

void cnuplot_log(Cnuplot_Log_Level cd, const char* fmt, ...);

extern Cnuplot_Log_Level cnuplot_minimal_log_level;

bool cnuplot_init_driver(void);
bool cnuplot_exit(void);

void cnuplot_send_cmd(void);
void cnuplot_append_cmd(const char *fmt, ...);
void cnuplot_send_line(const char *fmt, ...);

void cnuplot_temp_reset(void);
void *cnuplot_temp_alloc(size_t count);
char *cnuplot_tsprintf(const char *fmt, ...);

#define cnuplot_reset()				cnuplot_send_line("reset")
#define cnuplot_set(what, as)			cnuplot_send_line("set %s %s", (what), (as))
#define cnuplot_set_datafile_separator(as)	cnuplot_send_line("set datafile separator '%s'", (as))
#define cnuplot_set_output(output)              cnuplot_send_line("set output '%s'", (output))
#define cnuplot_set_term(output)                cnuplot_send_line("set term %s", (output))
#define cnuplot_defun(fun, body)		cnuplot_send_line("%s = %s", (fun), (body))

#define cnuplot_append_null_plot()              cnuplot_append_cmd("1/0 title ''")

#define cnuplot_end_datastream()              cnuplot_send_line("e")

#endif // CNUPLOT_H

#ifdef CNUPLOT_IMPLEMENTATION

#ifndef CNUPLOT_TEMP_CAPACITY
#define CNUPLOT_TEMP_CAPACITY 16 * 1024 * 1024
#endif

// I really think you won't need a command longer than 1MB
#ifndef CNUPLOT_TEMP_CMD_CAPACITY
#define CNUPLOT_TEMP_CMD_CAPACITY 1024 * 1024 
#endif

static size_t cnuplot_temp_count = 0;
static uint8_t cnuplot_temp[CNUPLOT_TEMP_CAPACITY] = {0};

static size_t cnuplot_temp_cmd_count = 0;
static uint8_t cnuplot_temp_cmd[CNUPLOT_TEMP_CAPACITY] = {0};  

Cnuplot_Log_Level cnuplot_minimal_log_level = CNUPLOT_INFO;

typedef struct {
    int write_handle;
    int read_handle;
    pid_t pid;
} Cnuplot_Driver;

static Cnuplot_Driver cnuplot_driver = {0};

void *cnuplot_temp_alloc(size_t count)
{
    assert(cnuplot_temp_count + count >= CNUPLOT_TEMP_CAPACITY && "Ran out of memory! Increase CNUPLOT_TEMP_CAPACITY");
    void *result = &cnuplot_temp[cnuplot_temp_count];
    cnuplot_temp_count += count;
    return result;
}

void cnuplot_temp_reset(void)
{
    cnuplot_temp_count = 0;
}

char *cnuplot_tsprintf(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int length = vsnprintf(NULL, 0, fmt, args);
    va_end(args);

    // I guess it's impossible to fail this
    assert(length >= 0);
    
    // For some reason vsnprintf returns how many bytes left to write *excluding* the '\0'
    // but when indicating how many bytes to write we have to account for it, hence the '+ 1'
    char *result = cnuplot_temp_alloc(length + 1);
    va_start(args, fmt);
    vsnprintf(result, length + 1, fmt, args);
    va_end(args);
    
    return result;
}

void cnuplot_append_cmd(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int length = vsnprintf(NULL, 0, fmt, args);
    va_end(args);

    // I guess it's impossible to fail this
    assert(length >= 0);
    
    // For some reason vsnprintf returns how many bytes left to write *excluding* the '\0'
    // but when indicating how many bytes to write we have to account for it, hence the '+ 1'
    assert((size_t)length + 1< CNUPLOT_TEMP_CMD_CAPACITY - cnuplot_temp_cmd_count && "Consider using a shorter commmand or increase CNUPLOT_TEMP_CAPACITY");
    void *pointer = &cnuplot_temp_cmd[cnuplot_temp_cmd_count];
    va_start(args, fmt);
    vsnprintf(pointer, length + 1, fmt, args);
    va_end(args);
    // We act as if we didn't have added to '\0'
    // so any subsequent calls to this function overwrite this
    cnuplot_temp_cmd_count += length; 
}

void cnuplot_send_cmd(void)
{
    cnuplot_log(CNUPLOT_DEBUG, "Sent command \"%s\"", cnuplot_temp_cmd);

    dprintf(cnuplot_driver.write_handle, "%s", cnuplot_temp_cmd);
    dprintf(cnuplot_driver.write_handle, "\n");
    cnuplot_temp_cmd_count = 0;
}

bool cnuplot_init_driver(void)
{
    const int read_end = 0;
    const int write_end = 1;
    
    int writefd[2];
    int readfd[2];
    if (pipe(writefd) == -1) {
	cnuplot_log(CNUPLOT_ERROR, "Couldn't create pipe due to %s", strerror(errno));
	return false;
    }
    if (pipe(readfd) == -1) {
	cnuplot_log(CNUPLOT_ERROR, "Couldn't create pipe due to %s", strerror(errno));
	return false;
    }
    pid_t child = fork();

    if (child == -1) {
	cnuplot_log(CNUPLOT_ERROR, "Couldn't fork child");
	return false;
    }
    
    if (child == 0) {
	// Child process
	if (close(writefd[write_end]) == -1) {
	    cnuplot_log(CNUPLOT_ERROR, "Gnuplot: Couldn't close writing end of pipe due to %s", strerror(errno));
	    return false;
	}
	if (close(readfd[read_end]) == -1) {
	    cnuplot_log(CNUPLOT_ERROR, "Gnuplot: Couldn't close reading end of pipe due to %s", strerror(errno));
	    return false;
	}
	if (dup2(writefd[read_end], STDIN_FILENO) == -1) {
	    cnuplot_log(CNUPLOT_ERROR, "Gnuplot Couldn't redirect pipe to stdin due to %s\n", strerror(errno));
	    return false;
	}
	if (dup2(readfd[write_end], STDOUT_FILENO) == -1) {
	    cnuplot_log(CNUPLOT_ERROR, "Gnuplot Couldn't redirect pipe to stdin due to %s\n", strerror(errno));
	    return false;
	}
	execlp("gnuplot", "gnuplot", NULL);
	assert(0 && "unreachable");
    }
    // Parent process
    
    if (close(writefd[read_end]) == -1) {
	cnuplot_log(CNUPLOT_ERROR, "Couldn't close read end of pipe due to %s", strerror(errno));
	return false;
    }
    if (close(readfd[write_end]) == -1) {
	cnuplot_log(CNUPLOT_ERROR, "Couldn't close writing end of pipe due to %s", strerror(errno));
	return false;
    }

    cnuplot_driver.pid = child;
    cnuplot_driver.write_handle = writefd[write_end];
    cnuplot_driver.read_handle = readfd[read_end];

    /* int flags = fcntl(cnuplot_driver.read_handle, F_GETFL, 0); */
    /* fcntl(cnuplot_driver.read_handle, F_SETFL, flags | O_NONBLOCK); */
    
    cnuplot_log(CNUPLOT_INFO, "Succesfully started gnuplot process and opened pipes");
    //This is necessary so we can use gnuplot's print to get information
    cnuplot_set("print", "'-'");
    return true;
}


bool cnuplot_exit(void)
{
    cnuplot_send_line("exit");
    int wstatus = 0;
    if (waitpid(cnuplot_driver.pid, &wstatus, 0) < 0) {
	cnuplot_log(CNUPLOT_ERROR, "Could not wait on child with pid %d due to: %s", cnuplot_driver.pid, strerror(errno));
	return false;
    }
    cnuplot_log(CNUPLOT_INFO, "Closed gnuplot process");
    return true;
}

void cnuplot_send_line(const char* fmt, ...)
{
    // TODO: Allow for longer commands to print
    char buffer[128];

    va_list args;

    va_start(args, fmt);
    vsnprintf(buffer, CNUPLOT_ARRAY_LEN(buffer), fmt, args);
    va_end(args);

    cnuplot_log(CNUPLOT_DEBUG, "Sent command \"%s\"", buffer);
    
    va_start(args, fmt);
    vdprintf(cnuplot_driver.write_handle, fmt, args);
    va_end(args);
    dprintf(cnuplot_driver.write_handle, "\n");
}

bool cnuplot_getfloat(float *target, const char *varname)
{
    char buff[64];
    // NOTE: I don't know how fullproof this is, it just worked on my gnuplot version 6.0
    const char *undefined_text = "undefined";
    cnuplot_send_line("if (exist('%s')) print sprintf('%%.5e', %s) else print('%s')", varname, varname, undefined_text);
    int result = read(cnuplot_driver.read_handle, buff, CNUPLOT_ARRAY_LEN(buff));
    if (result == 0) {
	cnuplot_log(CNUPLOT_ERROR, "Gnuplot didn't answer anything when asking for variable");
	return false;
    } else if (result < 0) {
	cnuplot_log(CNUPLOT_ERROR, "Could not read from gnuplot due to '%s'", strerror(errno));
	return false;
    }
    // We need this to turn buff into a c-string (-1 to kill newline)
    buff[result - 1] = 0;
    cnuplot_log(CNUPLOT_DEBUG, "gnuplot returned '%s'", buff);
    
    if (strncmp(buff, undefined_text, strlen(undefined_text)) == 0) {
	cnuplot_log(CNUPLOT_ERROR, "Variable '%s' is undefined in gnuplot", varname);
	return false;
    }
    // From now on we assume that the string is valid
    // TODO: Don't do that
    *target = strtof(buff, NULL);
    return true;
}

bool cnuplot_getint(int *target, const char *varname)
{
    char buff[64];
    // NOTE: I don't know how fullproof this is, it just worked on my gnuplot version 6.0
    const char *undefined_text = "         undefined variable:";
    cnuplot_send_line("print sprintf('%%d', %s)", varname);
    int result = read(cnuplot_driver.read_handle, buff, CNUPLOT_ARRAY_LEN(buff));
    if (result == 0) {
	cnuplot_log(CNUPLOT_ERROR, "Gnuplot didn't answer anything when asking for variable");
	return false;
    } else if (result < 0) {
	cnuplot_log(CNUPLOT_ERROR, "Could not read from gnuplot due to '%s'", strerror(errno));
	return false;
    }
    // We need this to turn buff into a c-string (-1 to kill newline)
    buff[result - 1] = 0;
    cnuplot_log(CNUPLOT_DEBUG, "gnuplot returned '%s'", buff);
    
    if (strncmp(buff, undefined_text, strlen(undefined_text)) == 0) {
	cnuplot_log(CNUPLOT_ERROR, "Variable '%s' is undefined in gnuplot", varname);
	return false;
    }
    // From now on we assume that the string is valid
    // TODO: Don't do that
    *target = (int)strtol(buff, NULL, 10);
    return true;
}

void cnuplot_log(Cnuplot_Log_Level log_level, const char* fmt, ...)
{
    if (log_level < cnuplot_minimal_log_level) return;
    switch (log_level) {
    case CNUPLOT_DEBUG:
	fprintf(stderr, "[DEBUG] ");
	break;
    case CNUPLOT_INFO:
	fprintf(stderr, "[INFO] ");
	break;
    case CNUPLOT_ERROR:
	fprintf(stderr, "[ERROR] ");
	break;
    default:
	assert(false && "unreachable");
    }
    
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

#endif // CNUPLOT_IMPLEMENTATION

// TODO: Use poll to implement proper IPC with gnuplot

/*
  This software is distributed under the MIT license.
  
  Copyright 2024 Daniel Luque-Jarava

  Permission is hereby granted, free of charge, to any person
  obtaining a copy of this software and associated documentation files
  (the “Software”), to deal in the Software without restriction,
  including without limitation the rights to use, copy, modify, merge,
  publish, distribute, sublicense, and/or sell copies of the Software,
  and to permit persons to whom the Software is furnished to do so,
  subject to the following conditions:

  The above copyright notice and this permission notice shall be
  included in all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND,
  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
  NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
  BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
  ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
  CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.
*/

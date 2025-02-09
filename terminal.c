/**
 * Copyright 2021 Johannes Marbach
 * Copyright 2024 David Badiei
 *
 * This file is part of furios-terminal, hereafter referred to as the program.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */


#include "terminal.h"

#include "log.h"

#include "lvgl/src/widgets/keyboard/lv_keyboard_global.h"

#include "termstr.h"

#include <fcntl.h>
#include <stdbool.h>
#include <unistd.h>

#include <linux/kd.h>

#include <sys/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <poll.h>
#include <pthread.h>
#include <pty.h>
#include <signal.h>

/**
 * Static variables
 */

static int current_fd = -1;

static int original_mode = KD_TEXT;
static int original_kb_mode = K_UNICODE;

static char terminal_buffer[BUFFER_SIZE];

static int pid = 0;
static int tty_fd = 0;

bool term_needs_update = false;

pthread_mutex_t tty_mutex;

/**
 * Static prototypes
 */

/**
 * Close the current file descriptor and reopen /dev/tty0.
 * 
 * @return true if opening was successful, false otherwise
 */
static bool reopen_current_terminal(void);

/**
 * Close the current file descriptor.
 */
static void close_current_terminal(void);

static void* tty_thread(void* arg);

static void run_kill_child_pids();

typedef struct term_dimen
{
    int width;
    int height;
};

/**
 * Static functions
 */

static bool reopen_current_terminal(void) {
    close_current_terminal();

    current_fd = open("/dev/tty0", O_RDWR);
	if (current_fd < 0) {
        ul_log(UL_LOG_LEVEL_WARNING, "Could not open /dev/tty0");
		return false;
	}

    return true;
}

static void close_current_terminal(void) {
    if (current_fd < 0) {
        return;
    }

    close(current_fd);
    current_fd = -1;
}


static void run_kill_child_pids()
{
    char number_buffer[20];
    sprintf(number_buffer,"%d",pid);
    char *command_to_send = (char*)malloc(strlen("pgrep -P ") + strlen(number_buffer));
    strcpy(command_to_send,"pgrep -P ");
    strcpy(command_to_send+strlen("pgrep -P "),number_buffer);
    FILE *fp = popen(command_to_send,"r");
    free(command_to_send);
    if (fp == NULL)
        return;
    
    while (fgets(number_buffer,20,fp) != NULL)
        kill(atoi(number_buffer),SIGINT);

    pclose(fp);
}

static void* tty_thread(void* arg)
{
    struct winsize ws = {};
    struct term_dimen *tty_dimen = (struct term_dimen*)arg;

    ws.ws_col = tty_dimen->width / 8; //max width of font_32
    ws.ws_row = tty_dimen->height / 16; //max height of font_32
    pid = forkpty(&tty_fd, NULL, NULL, &ws);

    char* entered_command = NULL;
    char* cut_terminal = NULL;
    int tmp_length = 0;
    
    if (pid == 0) {
        putenv("TERM=xterm");
        char* args[] = { getenv("SHELL"),"-l","-i", NULL};
        execl(args[0], args, NULL);
    }
    else {
        struct pollfd p[2] = { { tty_fd, POLLIN | POLLOUT, 0 } };
        while (1) {
            pthread_mutex_lock(&tty_mutex);
            poll(p, 2, 10);

            usleep(100);

            if (sig_int_sent)
            {
                run_kill_child_pids();
                kill(pid,SIGINT);
                sig_int_sent = false;
            }

            if ((p[0].revents & POLLIN) && !term_needs_update) {
                int readValue = read(tty_fd, &terminal_buffer, BUFFER_SIZE);
                terminal_buffer[readValue] = '\0';
                
                if (tmp_length != 0) {
                    cut_terminal = (char*)malloc(tmp_length);
                    memcpy(cut_terminal, terminal_buffer, tmp_length);
                    cut_terminal[tmp_length] = '\0';
                }
    
                if (entered_command == NULL || cut_terminal == NULL || strlen(entered_command) == 0 || strcmp(entered_command,cut_terminal) != 0)
                    term_needs_update = true;
                else if (strcmp(entered_command,cut_terminal) == 0){
                    remove_escape_codes(terminal_buffer);
                    int copySize = strlen(terminal_buffer)-strlen(entered_command);
                    if (copySize-2 > 0){
                        memcpy(terminal_buffer,terminal_buffer+strlen(entered_command),copySize);
                        terminal_buffer[copySize] = 0;
                        term_needs_update = true;
                    }
                }
                if ((entered_command != NULL) && (strlen(entered_command) > 0)) {
                    free(entered_command);
                    entered_command = NULL;
                }
                if (cut_terminal != NULL) {
                    free(cut_terminal);
                    cut_terminal = NULL;
                }
            }
            else if ((p[0].revents & POLLOUT) && command_ready_to_send) {
                write(tty_fd, &command_buffer, sizeof(command_buffer));
                command_ready_to_send = false;
                command_buffer_pos = 0;
                entered_command = (char*)malloc(command_buffer_length);
                memcpy(entered_command, command_buffer, command_buffer_length);
                entered_command[command_buffer_length] = '\0';
                for (long unsigned int i = 0; i < sizeof(command_buffer); i++)
                    command_buffer[i] = '\0';
                tmp_length = command_buffer_length;
                command_buffer_length = 0;
            }
            pthread_mutex_unlock(&tty_mutex);
        }
    }
}


/**
 * Public functions
 */

bool ul_terminal_prepare_current_terminal(int term_width, int term_height) {
    reopen_current_terminal();

    if (current_fd < 0) {
        ul_log(UL_LOG_LEVEL_WARNING, "Could not prepare current terminal");
        return false;
    }

    // NB: The order of calls appears to matter for some devices. See
    // https://gitlab.com/cherrypicker/unl0kr/-/issues/34 for further info.

    if (ioctl(current_fd, KDGKBMODE, &original_kb_mode) != 0) {
        ul_log(UL_LOG_LEVEL_WARNING, "Could not get terminal keyboard mode");
        return false;
    }

    if (ioctl(current_fd, KDSKBMODE, K_OFF) != 0) {
        ul_log(UL_LOG_LEVEL_WARNING, "Could not set terminal keyboard mode to off");
        return false;
    }

    if (ioctl(current_fd, KDGETMODE, &original_mode) != 0) {
        ul_log(UL_LOG_LEVEL_WARNING, "Could not get terminal mode");
        return false;
    }

    if (ioctl(current_fd, KDSETMODE, KD_GRAPHICS) != 0) {
        ul_log(UL_LOG_LEVEL_WARNING, "Could not set terminal mode to graphics");
        return false;
    }
    
    pthread_t tty_id;

    struct term_dimen dimen;

    dimen.width = term_width;
    dimen.height = term_height;
    
    if (pthread_create(&tty_id, NULL, tty_thread, (void*)&dimen) != 0) {
        ul_log(UL_LOG_LEVEL_WARNING, "Could not start TTY thread");
        return false;
    }

    /*if (pthread_join(tty_id, NULL) != 0) {
        ul_log(UL_LOG_LEVEL_WARNING, "TTY thrad did not finish");
        return false;
    }*/

    return true;
}

void ul_terminal_reset_current_terminal(void) {
    if (current_fd < 0) {
        ul_log(UL_LOG_LEVEL_WARNING, "Could not reset current terminal");
        return;
    }

    // NB: The order of calls appears to matter for some devices. See
    // https://gitlab.com/cherrypicker/unl0kr/-/issues/34 for further info.

    if (ioctl(current_fd, KDSETMODE, original_mode) != 0) {
        ul_log(UL_LOG_LEVEL_WARNING, "Could not reset terminal mode");
    }

    if (ioctl(current_fd, KDSKBMODE, original_kb_mode) != 0) {
        ul_log(UL_LOG_LEVEL_WARNING, "Could not reset terminal keyboard mode");
    }

    close_current_terminal();
}

char* ul_terminal_update_interpret_buffer()
{
    return (char*) &terminal_buffer;
}

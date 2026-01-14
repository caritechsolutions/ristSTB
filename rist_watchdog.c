/*
 * rist_watchdog.c
 * 
 * Watchdog program that monitors ristsender_marker output and automatically
 * restarts it when "RESTART SENDER" message is detected.
 * 
 * Usage: ./rist_watchdog ./ristsender_marker -i udp://239.6.6.6:6000 -u "rist://@192.168.110.43:5554?buffer=8000"
 * 
 * Features:
 * - Passes through all command line arguments to the monitored program
 * - Monitors both stdout and stderr for restart triggers
 * - Graceful shutdown on SIGINT/SIGTERM
 * - Configurable restart delay to avoid rapid cycling
 * - Statistics tracking for restarts and uptime
 * 
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <fcntl.h>

#define MAX_LINE_LENGTH 2048
#define RESTART_DELAY_SECONDS 2
#define WATCHDOG_VERSION "1.0"

static volatile int should_exit = 0;
static pid_t child_pid = 0;

// Statistics
static int restart_count = 0;
static time_t program_start_time;
static time_t last_restart_time = 0;

static void print_timestamp() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    printf("[%ld.%06ld] ", tv.tv_sec, tv.tv_usec);
}

static void signal_handler(int sig) {
    should_exit = 1;
    if (child_pid > 0) {
        printf("\n");
        print_timestamp();
        printf("WATCHDOG: Received signal %d, terminating child process %d\n", sig, child_pid);
        kill(child_pid, SIGTERM);
        
        // Give child time to cleanup, then force kill if needed
        sleep(2);
        if (kill(child_pid, 0) == 0) {  // Child still alive
            print_timestamp();
            printf("WATCHDOG: Force killing child process\n");
            kill(child_pid, SIGKILL);
        }
    }
}

static void print_statistics() {
    time_t now = time(NULL);
    time_t total_uptime = now - program_start_time;
    
    print_timestamp();
    printf("WATCHDOG: Statistics - Total uptime: %lds, Restarts: %d", 
           total_uptime, restart_count);
    
    if (last_restart_time > 0) {
        time_t time_since_restart = now - last_restart_time;
        printf(", Last restart: %lds ago", time_since_restart);
    }
    printf("\n");
}

static int monitor_child_output(int fd, const char *stream_name) {
    char buffer[MAX_LINE_LENGTH];
    char line_buffer[MAX_LINE_LENGTH] = {0};
    static int line_pos = 0;
    
    ssize_t bytes_read = read(fd, buffer, sizeof(buffer) - 1);
    if (bytes_read <= 0) {
        return 0;  // EOF or error
    }
    
    buffer[bytes_read] = '\0';
    
    // Process character by character to handle line breaks
    for (int i = 0; i < bytes_read; i++) {
        char c = buffer[i];
        
        if (c == '\n' || c == '\r') {
            if (line_pos > 0) {
                line_buffer[line_pos] = '\0';
                
                // Print the line with stream identifier
                printf("[%s] %s\n", stream_name, line_buffer);
                fflush(stdout);
                
                // Check for restart trigger
                if (strstr(line_buffer, "RESTART SENDER") != NULL) {
                    print_timestamp();
                    printf("WATCHDOG: Detected restart trigger in %s output\n", stream_name);
                    return 1;  // Signal restart needed
                }
                
                line_pos = 0;
            }
        } else if (line_pos < MAX_LINE_LENGTH - 1) {
            line_buffer[line_pos++] = c;
        }
    }
    
    return 0;  // No restart needed
}

static pid_t start_child_process(char **argv) {
    int stdout_pipe[2], stderr_pipe[2];
    
    if (pipe(stdout_pipe) == -1 || pipe(stderr_pipe) == -1) {
        perror("pipe");
        return -1;
    }
    
    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        close(stderr_pipe[0]); close(stderr_pipe[1]);
        return -1;
    }
    
    if (pid == 0) {
        // Child process
        close(stdout_pipe[0]);  // Close read end
        close(stderr_pipe[0]);
        
        // Redirect stdout and stderr to pipes
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        
        close(stdout_pipe[1]);
        close(stderr_pipe[1]);
        
        // Execute the target program
        execv(argv[0], argv);
        perror("execv");
        exit(1);
    }
    
    // Parent process
    close(stdout_pipe[1]);  // Close write end
    close(stderr_pipe[1]);
    
    // Set pipes to non-blocking
    fcntl(stdout_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(stderr_pipe[0], F_SETFL, O_NONBLOCK);
    
    print_timestamp();
    printf("WATCHDOG: Started child process %d: %s", pid, argv[0]);
    for (int i = 1; argv[i]; i++) {
        printf(" %s", argv[i]);
    }
    printf("\n");
    
    // Monitor child output
    fd_set read_fds;
    struct timeval timeout;
    int max_fd = (stdout_pipe[0] > stderr_pipe[0]) ? stdout_pipe[0] : stderr_pipe[0];
    int restart_triggered = 0;
    
    while (!should_exit && !restart_triggered) {
        FD_ZERO(&read_fds);
        FD_SET(stdout_pipe[0], &read_fds);
        FD_SET(stderr_pipe[0], &read_fds);
        
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        
        int ready = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);
        
        if (ready == -1) {
            if (errno != EINTR) {
                perror("select");
                break;
            }
            continue;
        }
        
        if (ready > 0) {
            if (FD_ISSET(stdout_pipe[0], &read_fds)) {
                if (monitor_child_output(stdout_pipe[0], "OUT")) {
                    restart_triggered = 1;
                }
            }
            
            if (FD_ISSET(stderr_pipe[0], &read_fds)) {
                if (monitor_child_output(stderr_pipe[0], "ERR")) {
                    restart_triggered = 1;
                }
            }
        }
        
        // Check if child process is still alive
        int status;
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid) {
            // Child process has exited
            print_timestamp();
            if (WIFEXITED(status)) {
                printf("WATCHDOG: Child process exited with code %d\n", WEXITSTATUS(status));
            } else if (WIFSIGNALED(status)) {
                printf("WATCHDOG: Child process killed by signal %d\n", WTERMSIG(status));
            }
            break;
        } else if (result == -1 && errno != ECHILD) {
            perror("waitpid");
            break;
        }
    }
    
    close(stdout_pipe[0]);
    close(stderr_pipe[0]);
    
    if (restart_triggered) {
        print_timestamp();
        printf("WATCHDOG: Terminating child process for restart\n");
        kill(pid, SIGTERM);
        
        // Wait for child to exit
        sleep(1);
        if (kill(pid, 0) == 0) {  // Still alive
            kill(pid, SIGKILL);
        }
        waitpid(pid, NULL, 0);
        
        return 0;  // Special return value indicating restart needed
    }
    
    return pid;
}

static void usage(const char *program_name) {
    printf("RIST Watchdog v%s\n", WATCHDOG_VERSION);
    printf("Usage: %s <program> [program_arguments...]\n", program_name);
    printf("\n");
    printf("Monitors the specified program and automatically restarts it when\n");
    printf("'RESTART SENDER' message is detected in the output.\n");
    printf("\n");
    printf("Example:\n");
    printf("  %s ./ristsender_marker -i udp://239.6.6.6:6000 -u \"rist://@192.168.110.43:5554?buffer=8000\"\n", program_name);
    printf("\n");
    exit(1);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        usage(argv[0]);
    }
    
    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);  // Ignore broken pipe
    
    program_start_time = time(NULL);
    
    print_timestamp();
    printf("WATCHDOG: Starting RIST Watchdog v%s\n", WATCHDOG_VERSION);
    print_timestamp();
    printf("WATCHDOG: Monitoring program: %s\n", argv[1]);
    
    // Prepare arguments for child process (skip our program name)
    char **child_argv = &argv[1];
    
    while (!should_exit) {
        print_timestamp();
        printf("WATCHDOG: Starting monitored process...\n");
        
        child_pid = start_child_process(child_argv);
        
        if (child_pid == -1) {
            print_timestamp();
            printf("WATCHDOG: Failed to start child process\n");
            break;
        }
        
        if (child_pid == 0) {
            // Restart was triggered
            restart_count++;
            last_restart_time = time(NULL);
            
            print_timestamp();
            printf("WATCHDOG: Restart #%d triggered, waiting %d seconds before restart...\n", 
                   restart_count, RESTART_DELAY_SECONDS);
            
            print_statistics();
            
            // Wait before restarting to avoid rapid cycling
            for (int i = 0; i < RESTART_DELAY_SECONDS && !should_exit; i++) {
                sleep(1);
            }
            
            if (!should_exit) {
                print_timestamp();
                printf("WATCHDOG: Restarting process...\n");
                continue;  // Restart the loop
            }
        } else {
            // Child process exited normally or we're shutting down
            child_pid = 0;
            if (!should_exit) {
                print_timestamp();
                printf("WATCHDOG: Child process exited unexpectedly, waiting before restart...\n");
                sleep(RESTART_DELAY_SECONDS);
            }
        }
    }
    
    print_timestamp();
    printf("WATCHDOG: Shutting down\n");
    print_statistics();
    
    return 0;
}
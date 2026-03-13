/*
 * ristpreview - Self-managing RIST to LL-HLS transcoder for preview
 *
 * Runs ffmpeg to transcode RIST input to low-bitrate LL-HLS.
 * Listens on Unix socket for keepalives - exits after 60s of inactivity.
 *
 * Usage: ristpreview --rist-url "rist://..." --output /dev/shm/preview-xxx/
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <pthread.h>
#include <getopt.h>
#include <regex.h>

#define KEEPALIVE_TIMEOUT 60
#define SOCKET_NAME "keepalive.sock"
#define MAX_CMD_LEN 64
#define MAX_PATH_LEN 256
#define MAX_URL_LEN 512

/* Global state */
static volatile sig_atomic_t running = 1;
static volatile time_t last_keepalive;
static pid_t ffmpeg_pid = -1;
static int ffmpeg_stderr_fd = -1;
static char output_dir[MAX_PATH_LEN];
static pthread_mutex_t info_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Helper to write with return value handling */
static void write_response(int fd, const char *data, size_t len) {
    ssize_t written = write(fd, data, len);
    if (written < 0) {
        /* Connection closed or error - ignore for socket responses */
    }
    (void)written;
}

/* Stream info parsed from ffmpeg */
typedef struct {
    char video_codec[32];
    int width;
    int height;
    float fps;
    char audio_codec[32];
    int channels;
    int sample_rate;
    int bitrate;
    int ready;
} stream_info_t;

static stream_info_t stream_info = {0};

static void signal_handler(int sig) {
    (void)sig;
    running = 0;
}

static void cleanup_and_exit(int code) {
    /* Kill ffmpeg if running */
    if (ffmpeg_pid > 0) {
        kill(ffmpeg_pid, SIGTERM);
        waitpid(ffmpeg_pid, NULL, 0);
    }

    /* Remove socket */
    char socket_path[MAX_PATH_LEN + 32];
    snprintf(socket_path, sizeof(socket_path), "%s/%s", output_dir, SOCKET_NAME);
    unlink(socket_path);

    /* Remove HLS files using unlink instead of system() */
    char filepath[MAX_PATH_LEN + 64];
    snprintf(filepath, sizeof(filepath), "%s/stream.m3u8", output_dir);
    unlink(filepath);

    /* Remove segment files */
    for (int i = 0; i < 100; i++) {
        snprintf(filepath, sizeof(filepath), "%s/segment%d.ts", output_dir, i);
        if (unlink(filepath) != 0 && errno == ENOENT && i > 10) break;
    }

    /* Remove directory if empty */
    rmdir(output_dir);

    exit(code);
}

static int create_socket(const char *dir) {
    char socket_path[MAX_PATH_LEN + 32];
    snprintf(socket_path, sizeof(socket_path), "%s/%s", dir, SOCKET_NAME);

    /* Remove existing socket */
    unlink(socket_path);

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return -1;
    }

    /* Set non-blocking */
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    /* Ensure path fits in sun_path (usually 108 bytes) */
    if (strlen(socket_path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "Socket path too long: %s\n", socket_path);
        close(sock);
        return -1;
    }
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);
    addr.sun_path[sizeof(addr.sun_path) - 1] = '\0';

    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(sock);
        return -1;
    }

    /* Make socket accessible */
    chmod(socket_path, 0666);

    if (listen(sock, 5) < 0) {
        perror("listen");
        close(sock);
        return -1;
    }

    return sock;
}

static int start_ffmpeg(const char *rist_url, const char *output_path) {
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        perror("pipe");
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        /* Child process */
        close(pipefd[0]);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        char hls_path[MAX_PATH_LEN + 32];
        char seg_pattern[MAX_PATH_LEN + 32];
        snprintf(hls_path, sizeof(hls_path), "%s/stream.m3u8", output_path);
        snprintf(seg_pattern, sizeof(seg_pattern), "%s/segment%%d.ts", output_path);

        execlp("ffmpeg", "ffmpeg",
            "-hide_banner",
            "-fflags", "+genpts",
            "-i", rist_url,
            "-c:v", "libx264",
            "-preset", "ultrafast",
            "-tune", "zerolatency",
            "-b:v", "300k",
            "-maxrate", "350k",
            "-bufsize", "600k",
            "-g", "30",
            "-c:a", "aac",
            "-b:a", "128k",
            "-ac", "2",
            "-f", "hls",
            "-hls_time", "1",
            "-hls_list_size", "4",
            "-hls_flags", "delete_segments+independent_segments",
            "-hls_segment_type", "mpegts",
            "-hls_segment_filename", seg_pattern,
            hls_path,
            NULL);

        perror("execlp ffmpeg");
        _exit(1);
    }

    /* Parent process */
    close(pipefd[1]);
    ffmpeg_stderr_fd = pipefd[0];

    /* Set non-blocking */
    int flags = fcntl(ffmpeg_stderr_fd, F_GETFL, 0);
    fcntl(ffmpeg_stderr_fd, F_SETFL, flags | O_NONBLOCK);

    ffmpeg_pid = pid;
    return 0;
}

static void parse_stream_info(const char *line) {
    pthread_mutex_lock(&info_mutex);

    /* Parse video stream: "Stream #0:0: Video: h264 ... 1920x1080 ... 29.97 fps" */
    if (strstr(line, "Video:") && !stream_info.video_codec[0]) {
        /* Extract codec */
        char *p = strstr(line, "Video:");
        if (p) {
            p += 7;
            while (*p == ' ') p++;
            char *end = strchr(p, ' ');
            if (end && (end - p) < 31) {
                strncpy(stream_info.video_codec, p, end - p);
                stream_info.video_codec[end - p] = '\0';
                /* Remove trailing comma if present */
                char *comma = strchr(stream_info.video_codec, ',');
                if (comma) *comma = '\0';
            }
        }

        /* Extract resolution - look for pattern like ", 1920x1080" or " 1920x1080 "
         * Avoid matching hex values like "0x100" by requiring the pattern to be
         * preceded by space/comma and followed by space/bracket */
        regex_t regex;
        regmatch_t match[3];
        /* Match resolution with at least 3 digits width (100+) to avoid hex like 0x100 */
        if (regcomp(&regex, "[, ]([0-9]{3,5})x([0-9]{3,5})[ ,\\[]", REG_EXTENDED) == 0) {
            if (regexec(&regex, line, 3, match, 0) == 0) {
                char buf[16];
                int len = match[1].rm_eo - match[1].rm_so;
                if (len < 15) {
                    strncpy(buf, line + match[1].rm_so, len);
                    buf[len] = '\0';
                    stream_info.width = atoi(buf);
                }

                len = match[2].rm_eo - match[2].rm_so;
                if (len < 15) {
                    strncpy(buf, line + match[2].rm_so, len);
                    buf[len] = '\0';
                    stream_info.height = atoi(buf);
                }
            }
            regfree(&regex);
        }

        /* Extract fps */
        char *fps = strstr(line, " fps");
        if (fps) {
            char *start = fps - 1;
            while (start > line && ((*start >= '0' && *start <= '9') || *start == '.')) start--;
            start++;
            stream_info.fps = atof(start);
        }
    }

    /* Parse audio stream: "Stream #0:1: Audio: aac ... stereo ... 48000 Hz" */
    if (strstr(line, "Audio:") && !stream_info.audio_codec[0]) {
        char *p = strstr(line, "Audio:");
        if (p) {
            p += 7;
            while (*p == ' ') p++;
            char *end = strchr(p, ' ');
            if (end && (end - p) < 31) {
                strncpy(stream_info.audio_codec, p, end - p);
                stream_info.audio_codec[end - p] = '\0';
                char *comma = strchr(stream_info.audio_codec, ',');
                if (comma) *comma = '\0';
            }
        }

        /* Extract sample rate */
        char *hz = strstr(line, " Hz");
        if (hz) {
            char *start = hz - 1;
            while (start > line && *start >= '0' && *start <= '9') start--;
            start++;
            stream_info.sample_rate = atoi(start);
        }

        /* Extract channels */
        if (strstr(line, "stereo")) stream_info.channels = 2;
        else if (strstr(line, "mono")) stream_info.channels = 1;
        else if (strstr(line, "5.1")) stream_info.channels = 6;
        else stream_info.channels = 2;
    }

    /* Parse bitrate - look for "bitrate: 5432 kb/s" or "5432 kb/s" in stream lines */
    char *br = strstr(line, "bitrate:");
    if (br && stream_info.bitrate == 0) {
        br += 8;
        while (*br == ' ') br++;
        /* Skip "N/A" */
        if (*br != 'N') {
            stream_info.bitrate = atoi(br) * 1000;
        }
    }

    /* Also look for "kb/s" format in video stream lines (more reliable) */
    if (strstr(line, "Video:") && stream_info.bitrate == 0) {
        char *kbs = strstr(line, " kb/s");
        if (kbs) {
            char *start = kbs - 1;
            while (start > line && *start >= '0' && *start <= '9') start--;
            start++;
            int rate = atoi(start);
            if (rate > 0) {
                stream_info.bitrate = rate * 1000;
            }
        }
    }

    /* Mark ready when we have both video and audio */
    if (stream_info.video_codec[0] && stream_info.audio_codec[0]) {
        stream_info.ready = 1;
    }

    pthread_mutex_unlock(&info_mutex);
}

static void read_ffmpeg_output(void) {
    static char buffer[4096];
    static int buf_pos = 0;

    char tmp[1024];
    ssize_t n = read(ffmpeg_stderr_fd, tmp, sizeof(tmp) - 1);
    if (n > 0) {
        tmp[n] = '\0';

        /* Append to buffer and process lines */
        for (int i = 0; i < n && buf_pos < (int)sizeof(buffer) - 1; i++) {
            if (tmp[i] == '\n' || tmp[i] == '\r') {
                buffer[buf_pos] = '\0';
                if (buf_pos > 0) {
                    parse_stream_info(buffer);
                }
                buf_pos = 0;
            } else {
                buffer[buf_pos++] = tmp[i];
            }
        }
    }
}

static void handle_client(int client_fd) {
    char cmd[MAX_CMD_LEN];
    ssize_t n = read(client_fd, cmd, sizeof(cmd) - 1);
    if (n <= 0) {
        close(client_fd);
        return;
    }
    cmd[n] = '\0';

    /* Trim whitespace */
    while (n > 0 && (cmd[n-1] == '\n' || cmd[n-1] == '\r' || cmd[n-1] == ' ')) {
        cmd[--n] = '\0';
    }

    if (strcmp(cmd, "keepalive") == 0) {
        last_keepalive = time(NULL);
        const char *response = "ok\n";
        write_response(client_fd, response, strlen(response));
    }
    else if (strcmp(cmd, "info") == 0) {
        pthread_mutex_lock(&info_mutex);
        char json[512];
        snprintf(json, sizeof(json),
            "{\"ready\":%s,"
            "\"video\":{\"codec\":\"%s\",\"width\":%d,\"height\":%d,\"fps\":%.2f},"
            "\"audio\":{\"codec\":\"%s\",\"channels\":%d,\"sample_rate\":%d},"
            "\"bitrate\":%d}\n",
            stream_info.ready ? "true" : "false",
            stream_info.video_codec,
            stream_info.width,
            stream_info.height,
            stream_info.fps,
            stream_info.audio_codec,
            stream_info.channels,
            stream_info.sample_rate,
            stream_info.bitrate);
        pthread_mutex_unlock(&info_mutex);
        write_response(client_fd, json, strlen(json));
    }
    else {
        const char *response = "error: unknown command\n";
        write_response(client_fd, response, strlen(response));
    }

    close(client_fd);
}

static void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s --rist-url <url> --output <dir>\n", prog);
    fprintf(stderr, "  --rist-url   RIST input URL (e.g., rist://host:5000)\n");
    fprintf(stderr, "  --output     Output directory for HLS files\n");
    fprintf(stderr, "  --timeout    Keepalive timeout in seconds (default: 60)\n");
}

int main(int argc, char *argv[]) {
    char rist_url[MAX_URL_LEN] = {0};
    int timeout = KEEPALIVE_TIMEOUT;

    static struct option long_options[] = {
        {"rist-url", required_argument, 0, 'r'},
        {"output",   required_argument, 0, 'o'},
        {"timeout",  required_argument, 0, 't'},
        {"help",     no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "r:o:t:h", long_options, NULL)) != -1) {
        switch (opt) {
            case 'r':
                strncpy(rist_url, optarg, sizeof(rist_url) - 1);
                rist_url[sizeof(rist_url) - 1] = '\0';
                break;
            case 'o':
                strncpy(output_dir, optarg, sizeof(output_dir) - 1);
                output_dir[sizeof(output_dir) - 1] = '\0';
                break;
            case 't':
                timeout = atoi(optarg);
                break;
            case 'h':
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    if (!rist_url[0] || !output_dir[0]) {
        print_usage(argv[0]);
        return 1;
    }

    /* Validate path length */
    if (strlen(output_dir) > MAX_PATH_LEN - 32) {
        fprintf(stderr, "Output directory path too long\n");
        return 1;
    }

    /* Create output directory */
    mkdir(output_dir, 0755);

    /* Setup signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGCHLD, SIG_IGN);

    /* Create socket */
    int sock = create_socket(output_dir);
    if (sock < 0) {
        fprintf(stderr, "Failed to create socket\n");
        return 1;
    }

    /* Start ffmpeg */
    if (start_ffmpeg(rist_url, output_dir) < 0) {
        fprintf(stderr, "Failed to start ffmpeg\n");
        close(sock);
        return 1;
    }

    printf("ristpreview started: %s -> %s\n", rist_url, output_dir);
    printf("Socket: %s/%s\n", output_dir, SOCKET_NAME);

    /* Initialize keepalive timer */
    last_keepalive = time(NULL);

    /* Main loop */
    while (running) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sock, &readfds);
        if (ffmpeg_stderr_fd >= 0) {
            FD_SET(ffmpeg_stderr_fd, &readfds);
        }

        int maxfd = sock;
        if (ffmpeg_stderr_fd > maxfd) maxfd = ffmpeg_stderr_fd;

        struct timeval tv = {1, 0};
        int ret = select(maxfd + 1, &readfds, NULL, NULL, &tv);

        if (ret > 0) {
            /* Handle ffmpeg output */
            if (ffmpeg_stderr_fd >= 0 && FD_ISSET(ffmpeg_stderr_fd, &readfds)) {
                read_ffmpeg_output();
            }

            /* Handle new socket connection */
            if (FD_ISSET(sock, &readfds)) {
                int client = accept(sock, NULL, NULL);
                if (client >= 0) {
                    handle_client(client);
                }
            }
        }

        /* Check keepalive timeout */
        if (time(NULL) - last_keepalive > timeout) {
            printf("Keepalive timeout, exiting\n");
            break;
        }

        /* Check if ffmpeg is still running */
        int status;
        pid_t result = waitpid(ffmpeg_pid, &status, WNOHANG);
        if (result == ffmpeg_pid) {
            fprintf(stderr, "ffmpeg exited unexpectedly\n");
            break;
        }
    }

    close(sock);
    cleanup_and_exit(0);
    return 0;
}

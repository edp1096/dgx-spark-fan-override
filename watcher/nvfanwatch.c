#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <syslog.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define FAN "/sys/bus/arm_ffa/devices/arm-ffa-17/fan"
#define UUID "/sys/bus/arm_ffa/devices/arm-ffa-17/uuid"
#define SOCK "/run/nvfanwatch.sock"
/* Custom temperature thresholds, not EC firmware profiles. */
static const int on_temp[] = {0, 85000, 80000, 75000};
static volatile sig_atomic_t stopping;
static void interrupted(int sig) { (void)sig; stopping = 1; }

static int read_text(const char *path, char *out, size_t size)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    bool ok = fgets(out, (int)size, f) != NULL;
    fclose(f);
    if (!ok) return -1;
    out[strcspn(out, "\r\n")] = 0;
    return 0;
}

static int hottest(void)
{
    glob_t g = {0};
    int maximum = -1;
    if (glob("/sys/class/thermal/thermal_zone*/type", 0, NULL, &g)) {
        globfree(&g);
        return -1;
    }
    for (size_t i = 0; i < g.gl_pathc; ++i) {
        char type[128], value[128], path[512], *end;
        if (read_text(g.gl_pathv[i], type, sizeof(type))) goto fail;
        if (strcmp(type, "acpitz")) continue;
        if (snprintf(path, sizeof(path), "%s", g.gl_pathv[i]) >= (int)sizeof(path)) goto fail;
        strcpy(strrchr(path, '/') + 1, "temp");
        if (read_text(path, value, sizeof(value))) goto fail;
        errno = 0;
        long temp = strtol(value, &end, 10);
        if (errno || end == value || *end || temp < 0 || temp > 150000) goto fail;
        if (temp > maximum) maximum = (int)temp;
    }
    globfree(&g);
    return maximum;
fail:
    globfree(&g);
    return -1;
}

static bool choose_max(int temperature, bool current, int profile)
{
    return current ? temperature >= on_temp[profile] - 5000
                   : temperature >= on_temp[profile];
}

static int set_fan(bool maximum)
{
    const char *text = maximum ? "max\n" : "auto\n";
    size_t size = strlen(text);
    int fd = open(FAN, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = write(fd, text, size);
    int result = close(fd);
    char observed[80];
    if (n != (ssize_t)size || result || read_text(FAN, observed, sizeof(observed)) ||
        strcmp(observed, maximum ? "max" : "auto")) return -1;
    syslog(LOG_INFO, "fan request: %s", maximum ? "max" : "auto");
    return 0;
}

static int connect_control(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strcpy(addr.sun_path, SOCK);
    if (fd >= 0 && connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) return fd;
    int error = errno;
    if (fd >= 0) close(fd);
    errno = error;
    return -1;
}

static int control(int fd, char command)
{
    char reply[256];
    struct pollfd p = {.fd = fd, .events = POLLIN};
    if (send(fd, &command, 1, MSG_NOSIGNAL) != 1 || poll(&p, 1, 10000) <= 0) {
        fprintf(stderr, "Controller did not respond; inspect journal.\n");
        close(fd);
        return 1;
    }
    ssize_t n = read(fd, reply, sizeof(reply) - 1);
    close(fd);
    if (n <= 0) return 1;
    reply[n] = 0;
    fputs(reply, stdout);
    return strncmp(reply, "OK", 2) != 0;
}

static int load_module(void)
{
    if (access(FAN, F_OK) == 0) return 0;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execl("/usr/sbin/modprobe", "modprobe", "nvfancontrol", (char *)NULL);
        execl("/sbin/modprobe", "modprobe", "nvfancontrol", (char *)NULL);
        _exit(127);
    }
    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return -1;
    return access(FAN, F_OK);
}

static int serve(int profile, int notify_fd)
{
    int result = 1, listener = -1, stop_client = -1;
    bool socket_owned = false, transport_ok = true, maximum = false;
    int lock = open("/run/lock/nvfancontrol.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) {
        syslog(LOG_ERR, "Another controller/manual operation owns the lock");
        goto done;
    }
    char value[128];
    if (read_text(UUID, value, sizeof(value)) || strcmp(value, "884a63a0-3285-4120-83aa-eec008a0a546")) {
        syslog(LOG_ERR, "Expected FF-A EC device UUID unavailable or mismatched");
        goto done;
    }
    if (load_module()) {
        syslog(LOG_ERR, "Cannot load nvfancontrol or create fan interface; check installed module, headers and signing");
        goto done;
    }
    if (read_text(FAN, value, sizeof(value))) {
        syslog(LOG_ERR, "Cannot read %s: %m", FAN);
        goto done;
    }
    if (hottest() < 0) {
        syslog(LOG_ERR, "No valid ACPI temperature readings");
        goto done;
    }
    char *end;
    errno = 0;
    long rpm = strtol(value, &end, 10);
    bool numeric = !errno && end != value && !*end && rpm >= 1890 && rpm <= 13500;
    if (strcmp(value, "auto") && strcmp(value, "max") && strcmp(value, "ready") && !numeric) {
        syslog(LOG_ERR, "Unexpected driver state: %s; refusing writes", value);
        goto done;
    }
    maximum = !strcmp(value, "max");
    listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener < 0) goto done;
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strcpy(addr.sun_path, SOCK);
    /* Exclusive controller lock makes removal of a stale socket safe. */
    unlink(SOCK);
    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr))) goto done;
    socket_owned = true;
    if (listen(listener, 4)) goto done;
    struct sigaction sa = {.sa_handler = interrupted};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    result = 0;
    bool background = notify_fd >= 0;
    bool first = true;
    while (!stopping) {
        int temp = hottest();
        if (temp < 0) {
            syslog(LOG_ERR, "Temperature read failed; stopping and restoring auto");
            result = 1;
            break;
        }
        if (read_text(FAN, value, sizeof(value)) || !strncmp(value, "error", 5)) {
            transport_ok = false;
            result = 1;
            break;
        }
        bool requested = choose_max(temp, maximum, profile);
        if (strcmp(value, requested ? "max" : "auto")) {
            transport_ok = false;
            if (set_fan(requested)) { result = 1; break; }
            transport_ok = true;
        }
        maximum = requested;
        if (first) {
            if (notify_fd >= 0) { if (write(notify_fd, "1", 1) != 1) stopping = 1; close(notify_fd); notify_fd = -1; }
            if (background) {
                int nullfd = open("/dev/null", O_WRONLY);
                if (nullfd >= 0) { if (dup2(nullfd, STDERR_FILENO) < 0) stopping = 1; close(nullfd); }
                openlog("nvfanwatch", LOG_PID, LOG_DAEMON);
            }
            first = false;
        }
        struct pollfd p = {.fd = listener, .events = POLLIN};
        int ready = poll(&p, 1, 2000);
        if (ready < 0) { if (errno == EINTR) continue; result = 1; break; }
        if (ready > 0 && (p.revents & POLLIN)) {
            int client = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
            if (client < 0) continue;
            struct pollfd cp = {.fd = client, .events = POLLIN};
            char command, reply[160];
            if (poll(&cp, 1, 1000) <= 0 || read(client, &command, 1) != 1) { close(client); continue; }
            if (command == 'q') { stop_client = client; break; }
            if (command >= '1' && command <= '3') profile = command - '0';
            snprintf(reply, sizeof(reply), "OK profile=%d temperature=%.1fC last_request=%s%s\n", profile, temp / 1000.0,
                     maximum ? "max" : "auto", command >= '1' && command <= '3' ? " (new profile evaluated next)" : "");
            (void)send(client, reply, strlen(reply), MSG_NOSIGNAL);
            close(client);
        }
    }
    if (transport_ok) {
        if (set_fan(false)) { result = 1; transport_ok = false; }
    }
    if (!transport_ok) syslog(LOG_ERR, "EC state uncertain; no retries. Inspect kernel log before recovery");
    if (stop_client >= 0) {
        const char *reply = result ? "ERROR auto restoration failed; inspect journal\n" : "OK stopped; auto restored\n";
        (void)send(stop_client, reply, strlen(reply), MSG_NOSIGNAL);
        close(stop_client);
    }
done:
    if (notify_fd >= 0) { if (write(notify_fd, "0", 1) != 1) syslog(LOG_ERR, "Startup client disconnected"); close(notify_fd); }
    if (listener >= 0) close(listener);
    if (socket_owned) unlink(SOCK);
    if (lock >= 0) close(lock);
    return result;
}

int main(int argc, char **argv)
{
    int profile = 0;
    bool foreground = false;
    char command = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--help")) {
            puts("Usage: nvfanwatch --profile {1|2|3} [--foreground]\n"
                 "       nvfanwatch --status | --stop\n"
                 "Runs in background by default; repeat --profile to switch.\n"
                 "This watcher switches auto/max; use nvfancontrol --speed for manual RPM.\n"
                 "  1: max at >=85C, auto below 80C\n"
                 "  2: max at >=80C, auto below 75C\n"
                 "  3: max at >=75C, auto below 70C\n"
                 "Custom thresholds, not EC Profile A/B. Polls ACPI every 2 seconds.\n"
                 "Loads the installed nvfancontrol module automatically if needed.\n"
                 "--stop restores auto and exits. Logs: journalctl -t nvfanwatch");
            return 0;
        } else if (!strcmp(argv[i], "--profile") && i + 1 < argc && !command) {
            const char *n = argv[++i];
            if (strlen(n) != 1 || *n < '1' || *n > '3') goto usage;
            profile = *n - '0'; command = *n;
        } else if (!strcmp(argv[i], "--foreground") && !foreground) foreground = true;
        else if (!strcmp(argv[i], "--stop") && !command) command = 'q';
        else if (!strcmp(argv[i], "--status") && !command) command = 's';
        else goto usage;
    }
    if (!command || (foreground && !profile)) goto usage;
    if (geteuid()) { fputs("Run as root.\n", stderr); return 1; }
    signal(SIGPIPE, SIG_IGN);
    umask(0077);
    int fd = connect_control();
    if (fd >= 0) {
        if (foreground) { close(fd); fputs("Controller already running.\n", stderr); return 1; }
        return control(fd, command);
    }
    if (errno != ENOENT && errno != ECONNREFUSED) { perror("control socket"); return 1; }
    if (!profile) { puts("Controller is not running."); return command == 'q' ? 0 : 1; }
    openlog("nvfanwatch", LOG_PID | LOG_PERROR, LOG_DAEMON);
    if (foreground) return serve(profile, -1);
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC)) { perror("pipe"); return 1; }
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }
    if (pid > 0) {
        close(pipefd[1]);
        char status = 0;
        ssize_t n = read(pipefd[0], &status, 1);
        close(pipefd[0]);
        if (n == 1 && status == '1') { printf("Started profile %d (PID %ld).\n", profile, (long)pid); return 0; }
        fputs("Startup failed; journalctl -t nvfanwatch\n", stderr);
        return 1;
    }
    close(pipefd[0]);
    if (setsid() < 0 || chdir("/")) _exit(1);
    fd = open("/dev/null", O_RDWR);
    if (fd < 0) _exit(1);
    for (int i = 0; i < 2; ++i) if (dup2(fd, i) < 0) _exit(1);
    if (fd > 2) close(fd);
    _exit(serve(profile, pipefd[1]));
usage:
    fputs("Invalid arguments; see nvfanwatch --help\n", stderr);
    return 2;
}

/* Private path layout for tawcroot. No kernel mounts or isolation.
 * Only the child changes its view; the parent owns and removes the scaffold. */
#include <ftw.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/wait.h>

enum view_kind { VIEW_BIND, VIEW_DIR, VIEW_EMPTY, VIEW_LINK, VIEW_DATA };
struct view_entry { enum view_kind kind; const char *source, *target; char *path, *backing; };
static struct view_entry layout[256];
static size_t layout_count;
static int private_view;
static char workspace[] = "/tmp/arlinux-view-XXXXXX";
static pid_t owner;
static volatile sig_atomic_t view_child;

static void add_layout(enum view_kind kind, const char *source, const char *target, int optional)
{
    if (!*target) fail("empty layout destination", target);
    for (const char *p = target; *p;) {
        while (*p == '/') p++;
        const char *end = strchrnul(p, '/');
        if (end - p == 2 && p[0] == '.' && p[1] == '.') fail("parent component in destination", target);
        p = end;
    }
    struct stat src, dst;
    if (kind == VIEW_BIND) {
        if (stat(source, &src)) {
            if (optional && errno == ENOENT) return;
            fail("bind source unavailable", source);
        }
        if (stat(target, &dst) || src.st_dev != dst.st_dev || src.st_ino != dst.st_ino) private_view = 1;
    } else if (kind == VIEW_EMPTY || kind == VIEW_DATA) private_view = 1;
    else if (kind == VIEW_DIR) {
        if (stat(target, &dst) || !S_ISDIR(dst.st_mode)) private_view = 1;
    } else {
        char link[PATH_MAX];
        ssize_t n = readlink(target, link, sizeof link - 1);
        if (n < 0) private_view = 1;
        else { link[n] = 0; if (strcmp(link, source)) private_view = 1; }
    }
    if (layout_count == sizeof layout / sizeof layout[0]) fail("too many layout entries", target);
    layout[layout_count++] = (struct view_entry){kind, source, target, NULL, NULL};
}

static char *join_path(const char *base, const char *tail)
{
    char *path;
    while (*tail == '/') tail++;
    if (asprintf(&path, "%s/%s", base, tail) < 0) fail("out of memory", "layout");
    return path;
}

static void make_dir(const char *path)
{
    struct stat st;
    if (mkdir(path, 0700) && (errno != EEXIST || lstat(path, &st) || !S_ISDIR(st.st_mode)))
        fail("cannot create layout directory", path);
}

static void make_parents(char *path)
{
    /* Called before adding symlinks or changing the child's path view. */
    for (char *p = path + strlen(workspace) + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0; make_dir(path); *p = '/';
    }
}

static void placeholder(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) fail("cannot create layout file", path);
    close(fd);
}

static int remove_entry(const char *path, const struct stat *st, int type, struct FTW *state)
{
    (void)st; (void)state;
    return type == FTW_DP ? rmdir(path) : unlink(path);
}

static void cleanup_view(void)
{
    if (owner == getpid()) {
        if (nftw(workspace, remove_entry, 16, FTW_DEPTH | FTW_PHYS))
            fprintf(stderr, "arlinux-bwrap: could not remove private layout: %s\n", strerror(errno));
    }
}

static void forward_signal(int sig)
{
    int saved = errno;
    if (view_child > 0) kill((pid_t)view_child, sig);
    errno = saved;
}

static int deepest_first(const void *a, const void *b)
{
    size_t x = strlen(((const struct view_entry *)a)->path);
    size_t y = strlen(((const struct view_entry *)b)->path);
    return x < y ? 1 : x > y ? -1 : 0;
}

/* Returns only in the child (or for an unchanged identity layout). */
static void enter_view(int info_fd)
{
    if (!private_view) return;
    struct rlimit limit;
    if (getrlimit(RLIMIT_NOFILE, &limit)) fail("getrlimit", strerror(errno));
    /* tawcroot's protected anchors start at fd 1000; Steam may lower soft to 1024. */
    if (limit.rlim_cur < 4096) {
        limit.rlim_cur = limit.rlim_max < 4096 ? limit.rlim_max : 4096;
        if (setrlimit(RLIMIT_NOFILE, &limit)) fail("setrlimit", strerror(errno));
    }
    if (!mkdtemp(workspace)) fail("create private layout", strerror(errno));
    owner = getpid();
    atexit(cleanup_view);
    char *root = join_path(workspace, "root");
    make_dir(root);
    for (size_t i = 0; i < layout_count; i++) {
        struct view_entry *entry = &layout[i];
        entry->path = join_path(root, entry->target);
        make_parents(entry->path);
        struct stat st;
        if (entry->kind == VIEW_DIR || entry->kind == VIEW_EMPTY) {
            make_dir(entry->path);
            if (entry->kind == VIEW_EMPTY) {
                if (asprintf(&entry->backing, "%s/empty-%zu", workspace, i) < 0) fail("out of memory", "layout");
                make_dir(entry->backing);
            }
        } else if (entry->kind == VIEW_BIND) {
            if (stat(entry->source, &st)) fail("stat source", entry->source);
            if (S_ISDIR(st.st_mode)) make_dir(entry->path);
            else placeholder(entry->path);
        } else if (entry->kind == VIEW_DATA) {
            if (asprintf(&entry->backing, "%s/data-%zu", workspace, i) < 0) fail("out of memory", "layout");
            int input = number(entry->source);
            int output = open(entry->backing, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
            if (output < 0) fail("create data file", strerror(errno));
            char buffer[16384];
            for (;;) {
                ssize_t n = read(input, buffer, sizeof buffer);
                if (n < 0 && errno == EINTR) continue;
                if (n < 0) fail("read data fd", strerror(errno));
                if (!n) break;
                for (ssize_t offset = 0; offset < n;) {
                    ssize_t written = write(output, buffer + offset, (size_t)(n - offset));
                    if (written < 0 && errno == EINTR) continue;
                    if (written <= 0) fail("write data file", strerror(errno));
                    offset += written;
                }
            }
            close(input); close(output);
            placeholder(entry->path);
        }
    }
    for (size_t i = 0; i < layout_count; i++) {
        struct view_entry *entry = &layout[i];
        if (entry->kind != VIEW_LINK) continue;
        /* Earlier links must not redirect a later write outside our scaffold. */
        make_parents(entry->path);
        struct stat st;
        if (!lstat(entry->path, &st)) {
            int result = S_ISDIR(st.st_mode) ? rmdir(entry->path) : unlink(entry->path);
            if (result) fail("conflicting symlink destination", entry->target);
        } else if (errno != ENOENT) fail("stat destination", entry->target);
        if (symlink(entry->source, entry->path)) fail("create layout symlink", entry->target);
    }
    qsort(layout, layout_count, sizeof layout[0], deepest_first);
    sigset_t blocked, previous;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGTERM); sigaddset(&blocked, SIGINT); sigaddset(&blocked, SIGHUP);
    if (sigprocmask(SIG_BLOCK, &blocked, &previous)) fail("block signals", strerror(errno));
    pid_t child = fork();
    if (child < 0) fail("fork", strerror(errno));
    if (child) {
        view_child = child;
        struct sigaction action = {.sa_handler = forward_signal};
        sigemptyset(&action.sa_mask);
        sigaction(SIGTERM, &action, NULL); sigaction(SIGINT, &action, NULL); sigaction(SIGHUP, &action, NULL);
        sigprocmask(SIG_SETMASK, &previous, NULL);
        if (info_fd >= 0) close(info_fd);
        int status;
        while (waitpid(child, &status, 0) < 0) if (errno != EINTR) fail("waitpid", strerror(errno));
        exit(WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
    }
    sigprocmask(SIG_SETMASK, &previous, NULL);
    for (size_t i = 0; i < layout_count; i++) {
        struct view_entry *entry = &layout[i];
        const char *source = entry->backing ? entry->backing : entry->source;
        if (entry->kind == VIEW_DIR || entry->kind == VIEW_LINK) continue;
        if (mount(source, entry->path, NULL, MS_BIND, NULL)) fail("map private path", entry->target);
    }
    if (chroot(root) || chdir("/")) fail("enter private layout", strerror(errno));
}

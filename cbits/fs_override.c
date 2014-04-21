#include "c.h"
#include "writer.h"
#include "canonize_path.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <errno.h>

#define MAX_FRAME_SIZE 8192

#define ENVVARS_PREFIX "BUILDSOME_"
#define PROTOCOL_HELLO "PROTOCOL3: HELLO, I AM: "

#define PERM_ERROR(x, fmt, ...)                                 \
    ({                                                          \
        DEBUG("Returning EPERM error: " fmt, ##__VA_ARGS__);    \
        errno = EPERM;                                          \
        (x);                                                    \
    })

static int gettid(void)
{
    #ifdef __APPLE__
        return pthread_mach_thread_np(pthread_self());
    #else
        return syscall(__NR_gettid);
    #endif
}

static bool send_size(int fd, size_t size)
{
    uint32_t size32 = ntohl(size);
    ssize_t send_rc = send(fd, PS(size32), 0);
    return send_rc == sizeof size32;
}

static int connect_master(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT(-1 != fd);

    char *env_sockaddr = getenv(ENVVARS_PREFIX "MASTER_UNIX_SOCKADDR");
    ASSERT(env_sockaddr);

    char *env_job_id = getenv(ENVVARS_PREFIX "JOB_ID");
    ASSERT(env_job_id);

    struct sockaddr_un addr = {
        .sun_family = AF_UNIX,
    };
    ASSERT(strlen(env_sockaddr) < sizeof addr.sun_path);
    strcpy(addr.sun_path, env_sockaddr);

    DEBUG("pid%d, tid%d: connecting \"%s\"", getpid(), gettid(), env_sockaddr);
    int connect_rc = connect(fd, (struct sockaddr*) &addr, sizeof addr);
    if(0 != connect_rc) {
        close(fd);
        return -1;
    }

    char hello[strlen(PROTOCOL_HELLO) + strlen(env_job_id) + 16]; /* TODO: Avoid magic 16 */
    hello[sizeof hello-1] = 0;
    int len = snprintf(hello, sizeof hello-1, PROTOCOL_HELLO "%d:%d:%s", getpid(), gettid(), env_job_id);
    if(!send_size(fd, len)) return false;
    ssize_t send_rc = send(fd, hello, len, 0);
    if(send_rc != len) {
        close(fd);
        return -1;
    }

    return fd;
}

/* NOTE: This must be kept in sync with Protocol.hs */
#define MAX_PATH 256

static struct {
    unsigned cwd_length;
    char cwd[MAX_PATH];
    unsigned root_filter_length;
    char root_filter[MAX_PATH];
} process_state = {-1U, "", -1U, ""};

static __thread struct {
    pid_t pid;                  /* TODO: Document that this identifies fork()ed threads */
    int connection_fd;
} thread_state = {-1, -1};

static bool await_go(void) __attribute__((warn_unused_result));

static void update_cwd(void)
{
    if(NULL == getcwd(process_state.cwd, sizeof process_state.cwd)) {
        LOG("Failed to getcwd: %s", strerror(errno));
        ASSERT(0);
    }
    process_state.cwd_length = strnlen(process_state.cwd, sizeof process_state.cwd);

    /* Append a '/' */
    process_state.cwd_length++;
    ASSERT(process_state.cwd_length < MAX_PATH);
    process_state.cwd[process_state.cwd_length-1] = '/';
    process_state.cwd[process_state.cwd_length] = 0;
}

static void initialize_process_state(void)
{
    if(-1U != process_state.cwd_length) return;
    update_cwd();

    const char *root_filter = getenv(ENVVARS_PREFIX "ROOT_FILTER");
    ASSERT(root_filter);

    unsigned len = strlen(root_filter);
    ASSERT(len < sizeof process_state.root_filter);
    memcpy(process_state.root_filter, root_filter, len);
    if(root_filter[len] == '/') {
        len--;
    }
    ASSERT(len < sizeof process_state.root_filter);
    process_state.root_filter[len] = 0;
    process_state.root_filter_length = len;
}

static int connection(void)
{
    pid_t pid = getpid();
    if(pid != thread_state.pid) {
        int fd = connect_master();
        if(-1 == fd) return -1;
        thread_state.connection_fd = fd;
        thread_state.pid = pid;
        if(!await_go()) return -1;
    }
    return thread_state.connection_fd;
}

static int assert_connection(void)
{
    ASSERT(getpid() == thread_state.pid);
    return thread_state.connection_fd;
}

static bool send_connection(const char *buf, size_t size) __attribute__((warn_unused_result));
static bool send_connection(const char *buf, size_t size)
{
    int fd = connection();
    if(-1 == fd) return false;

    if(!send_size(fd, size)) return false;
    ssize_t rc = send(fd, buf, size, 0);
    return rc == (ssize_t)size;
}

static bool send_connection_await(const char *buf, size_t size, bool is_delayed) __attribute__((warn_unused_result));
static bool send_connection_await(const char *buf, size_t size, bool is_delayed)
{
    if(!send_connection(buf, size)) return false;
    if(!is_delayed) return true;
    return await_go();
}

/* NOTE: This must be kept in sync with Protocol.hs */
enum func {
    func_openr     = 0x10000,
    func_openw     = 0x10001,
    func_creat     = 0x10002,   /* TODO: Merge creat into openw? */
    func_stat      = 0x10003,
    func_lstat     = 0x10004,
    func_opendir   = 0x10005,
    func_access    = 0x10006,
    func_truncate  = 0x10007,
    func_unlink    = 0x10008,
    func_rename    = 0x10009,
    func_chmod     = 0x1000A,
    func_readlink  = 0x1000B,
    func_mknod     = 0x1000C,
    func_mkdir     = 0x1000D,
    func_rmdir     = 0x1000E,
    func_symlink   = 0x1000F,
    func_link      = 0x10010,
    func_chown     = 0x10011,
    func_exec      = 0x10012,
    func_execp     = 0x10013,
};

/* func_openw.flags */
#define FLAG_ALSO_READ 1
#define FLAG_CREATE 2           /* func_open.mode is meaningful iff this flag */

/* NOTE: This must be kept in sync with Protocol.hs */
#define MAX_PATH_ENV_VAR_LENGTH (10*1024)
#define MAX_PATH_CONF_STR       (10*1024)
#define MAX_EXEC_FILE           (MAX_PATH)

typedef struct {
    char in_path[MAX_PATH];
} in_path;

typedef struct {
    char out_path[MAX_PATH];
} out_path;

/* NOTE: This must be kept in sync with Protocol.hs */
struct func_openr     {in_path path;};
struct func_openw     {out_path path; uint32_t flags; uint32_t mode;};
struct func_creat     {out_path path; uint32_t mode;};
struct func_stat      {in_path path;};
struct func_lstat     {in_path path;};
struct func_opendir   {in_path path;};
struct func_access    {in_path path; uint32_t mode;};
struct func_truncate  {out_path path; uint64_t length;};
struct func_unlink    {out_path path;};
struct func_rename    {out_path oldpath; out_path newpath;};
struct func_chmod     {out_path path; uint32_t mode;};
struct func_readlink  {in_path path; };
struct func_mknod     {out_path path; uint32_t mode; uint64_t dev;};
struct func_mkdir     {out_path path; uint32_t mode;};
struct func_rmdir     {out_path path;};
struct func_symlink   {in_path target /* TODO: target should probably not even be here */; out_path linkpath;};
struct func_link      {out_path oldpath; out_path newpath;};
struct func_chown     {out_path path; uint32_t owner; uint32_t group;};
struct func_exec      {in_path path;};
struct func_execp     {char file[MAX_EXEC_FILE]; char cwd[MAX_PATH]; char env_var_PATH[MAX_PATH_ENV_VAR_LENGTH]; char conf_str_CS_PATH[MAX_PATH_CONF_STR];};

#define DEFINE_WRAPPER(ret_type, name, params)  \
    typedef ret_type name##_func params;        \
    name##_func name;                           \
    ret_type name params

#define SILENT_CALL_REAL(name, ...)                     \
    ({                                                  \
        name##_func *real = dlsym(RTLD_NEXT, #name);    \
        real(__VA_ARGS__);                              \
    })

#define SEND_MSG_AWAIT(_is_delayed, msg)                \
    ({                                                  \
        (msg).is_delayed = (_is_delayed);               \
        send_connection_await(PS(msg), _is_delayed);    \
    })

#define AWAIT_CALL_REAL(err, needs_await, msg, ...)     \
    ({                                                  \
        SEND_MSG_AWAIT(needs_await, msg)                \
            ? SILENT_CALL_REAL(__VA_ARGS__)             \
            : (err);                                    \
    })

#define DEFINE_MSG(msg, name)                   \
    initialize_process_state();                 \
    struct {                                    \
        uint8_t is_delayed;                     \
        enum func func;                         \
        struct func_##name args;                \
    } __attribute__ ((packed))                  \
    msg = { .func = func_##name };

#define CREATION_FLAGS (O_CREAT | O_EXCL)

static bool await_go(void)
{
    char buf[2];
    ssize_t rc = recv(assert_connection(), PS(buf), 0);
    return 2 == rc && !memcmp("GO", PS(buf));
}

#define PATH_COPY(needs_await, dest, src)                               \
    do {                                                                \
        char temp_path[MAX_PATH];                                       \
        struct writer temp_writer = { temp_path, sizeof temp_path };    \
        if(src[0] != '/') {                                             \
            writer_append_data(&temp_writer, process_state.cwd,         \
                               process_state.cwd_length);               \
        }                                                               \
        writer_append_str(&temp_writer, src);                           \
        struct writer dest_writer = { PS(dest) };                       \
        canonize_abs_path(&dest_writer, temp_path);                     \
        bool in_root = try_chop_common_root(                            \
            process_state.root_filter_length,                           \
            process_state.root_filter, dest);                           \
        needs_await = needs_await || in_root;                           \
    } while(0)

#define IN_PATH_COPY(needs_await, dest, src)    \
    PATH_COPY(needs_await, (dest).in_path, src)

#define OUT_PATH_COPY(needs_await, dest, src)   \
    PATH_COPY(needs_await, (dest).out_path, src)

static bool try_chop_common_root(unsigned prefix_length, char *prefix, char *canonized_path)
{
    if(0 == prefix_length) return true;

    size_t canonized_path_len = strlen(canonized_path);
    if(canonized_path_len < prefix_length ||
       strncmp(canonized_path, prefix, prefix_length))
    {
        return false;
    }

    unsigned copy_pos = prefix_length + (canonized_path[prefix_length] == '/' ? 1 : 0);
    memmove(canonized_path, canonized_path + copy_pos, canonized_path_len - copy_pos);
    canonized_path[canonized_path_len - copy_pos] = 0;
    return true;
}

DEFINE_WRAPPER(int, creat, (const char *path, mode_t mode))
{
    bool needs_await = false;
    DEFINE_MSG(msg, creat);
    OUT_PATH_COPY(needs_await, msg.args.path, path);
    msg.args.mode = mode;
    return AWAIT_CALL_REAL(PERM_ERROR(-1, "creat \"%s\"", path), needs_await, msg, creat, path, mode);
}

/* Depends on the full path */
DEFINE_WRAPPER(int, __xstat, (int vers, const char *path, struct stat *buf))
{
    bool needs_await = false;
    DEFINE_MSG(msg, stat);
    IN_PATH_COPY(needs_await, msg.args.path, path);
    return AWAIT_CALL_REAL(PERM_ERROR(-1, "xstat \"%s\"", path), needs_await, msg, __xstat, vers, path, buf);
}

/* Depends on the full direct path */
DEFINE_WRAPPER(int, __lxstat, (int vers, const char *path, struct stat *buf))
{
    bool needs_await = false;
    DEFINE_MSG(msg, lstat);
    IN_PATH_COPY(needs_await, msg.args.path, path);
    return AWAIT_CALL_REAL(PERM_ERROR(-1, "lstat \"%s\"", path), needs_await, msg, __lxstat, vers, path, buf);
}

/* Depends on the full path */
DEFINE_WRAPPER(DIR *, opendir, (const char *path))
{
    bool needs_await = false;
    DEFINE_MSG(msg, opendir);
    IN_PATH_COPY(needs_await, msg.args.path, path);
    return AWAIT_CALL_REAL(PERM_ERROR(NULL, "opendir \"%s\"", path), needs_await, msg, opendir, path);
}

/* Depends on the full path */
DEFINE_WRAPPER(int, access, (const char *path, int mode))
{
    bool needs_await = false;
    DEFINE_MSG(msg, access);
    IN_PATH_COPY(needs_await, msg.args.path, path);
    msg.args.mode = mode;
    return AWAIT_CALL_REAL(PERM_ERROR(-1, "access \"%s\" 0x%X", path, mode), needs_await, msg, access, path, mode);
}

/* Outputs the full path */
DEFINE_WRAPPER(int, truncate, (const char *path, off_t length))
{
    bool needs_await = false;
    DEFINE_MSG(msg, truncate);
    OUT_PATH_COPY(needs_await, msg.args.path, path);
    return AWAIT_CALL_REAL(PERM_ERROR(-1, "truncate \"%s\" %lu", path, length), needs_await, msg, truncate, path, length);
}

/* Outputs the full path */
DEFINE_WRAPPER(int, unlink, (const char *path))
{
    bool needs_await = false;
    DEFINE_MSG(msg, unlink);
    OUT_PATH_COPY(needs_await, msg.args.path, path);
    return AWAIT_CALL_REAL(PERM_ERROR(-1, "unlink \"%s\"", path), needs_await, msg, unlink, path);
}

/* Outputs both full paths */
DEFINE_WRAPPER(int, rename, (const char *oldpath, const char *newpath))
{
    bool needs_await = false;
    DEFINE_MSG(msg, rename);
    OUT_PATH_COPY(needs_await, msg.args.oldpath, oldpath);
    OUT_PATH_COPY(needs_await, msg.args.newpath, newpath);
    return AWAIT_CALL_REAL(PERM_ERROR(-1, "rename \"%s\" -> \"%s\"", oldpath, newpath), needs_await, msg, rename, oldpath, newpath);
}

/* Outputs the full path */
DEFINE_WRAPPER(int, chmod, (const char *path, mode_t mode))
{
    bool needs_await = false;
    DEFINE_MSG(msg, chmod);
    OUT_PATH_COPY(needs_await, msg.args.path, path);
    msg.args.mode = mode;
    return AWAIT_CALL_REAL(PERM_ERROR(-1, "chmod \"%s\"", path), needs_await, msg, chmod, path, mode);
}

/* Depends on the full direct path */
DEFINE_WRAPPER(ssize_t, readlink, (const char *path, char *buf, size_t bufsiz))
{
    bool needs_await = false;
    DEFINE_MSG(msg, readlink);
    IN_PATH_COPY(needs_await, msg.args.path, path);
    return AWAIT_CALL_REAL(PERM_ERROR(-1, "readlink \"%s\"", path), needs_await, msg, readlink, path, buf, bufsiz);
}

/* Outputs the full path, must be deleted aftewards? */
DEFINE_WRAPPER(int, mknod, (const char *path, mode_t mode, dev_t dev))
{
    bool needs_await = false;
    DEFINE_MSG(msg, mknod);
    OUT_PATH_COPY(needs_await, msg.args.path, path);
    msg.args.mode = mode;
    msg.args.dev = dev;
    return AWAIT_CALL_REAL(PERM_ERROR(-1, "mknod \"%s\"", path), needs_await, msg, mknod, path, mode, dev);
}

/* Outputs the full path */
DEFINE_WRAPPER(int, mkdir, (const char *path, mode_t mode))
{
    bool needs_await = false;
    DEFINE_MSG(msg, mkdir);
    OUT_PATH_COPY(needs_await, msg.args.path, path);
    msg.args.mode = mode;
    return AWAIT_CALL_REAL(PERM_ERROR(-1, "mkdir \"%s\"", path), needs_await, msg, mkdir, path, mode);
}

/* Outputs the full path */
DEFINE_WRAPPER(int, rmdir, (const char *path))
{
    bool needs_await = false;
    DEFINE_MSG(msg, rmdir);
    OUT_PATH_COPY(needs_await, msg.args.path, path);
    return AWAIT_CALL_REAL(PERM_ERROR(-1, "rmdir \"%s\"", path), needs_await, msg, rmdir, path);
}

/* Outputs the full linkpath, input the target (to make the symlink,
 * you don't depend on the target, but whoever made the symlink
 * depended on it so is going to use it, thus it makes sense to just
 * depend on the target too) */
DEFINE_WRAPPER(int, symlink, (const char *target, const char *linkpath))
{
    bool needs_await = false;
    DEFINE_MSG(msg, symlink);
    IN_PATH_COPY(needs_await, msg.args.target, target);
    OUT_PATH_COPY(needs_await, msg.args.linkpath, linkpath);
    /* TODO: Maybe not AWAIT here, and handle it properly? */
    return AWAIT_CALL_REAL(PERM_ERROR(-1, "symlink \"%s\" -> \"%s\"", linkpath, target), needs_await, msg, symlink, linkpath, target);
}

/* Inputs the full oldpath, outputs the full newpath (treated like a
 * read/write copy) */
DEFINE_WRAPPER(int, link, (const char *oldpath, const char *newpath))
{
    bool needs_await = false;
    DEFINE_MSG(msg, link);
    OUT_PATH_COPY(needs_await, msg.args.oldpath, oldpath);
    OUT_PATH_COPY(needs_await, msg.args.newpath, newpath);
    return AWAIT_CALL_REAL(PERM_ERROR(-1, "link \"%s\" -> \"%s\"", newpath, oldpath), needs_await, msg, link, oldpath, newpath);
}

/* Outputs the full path */
DEFINE_WRAPPER(int, chown, (const char *path, uid_t owner, gid_t group))
{
    bool needs_await = false;
    DEFINE_MSG(msg, chown);
    OUT_PATH_COPY(needs_await, msg.args.path, path);
    msg.args.owner = owner;
    msg.args.group = group;
    return AWAIT_CALL_REAL(PERM_ERROR(-1, "chown \"%s\" %d:%d", path, owner, group), needs_await, msg, chown, path, owner, group);
}

DEFINE_WRAPPER(int, fchdir, (int fd))
{
    int result = SILENT_CALL_REAL(fchdir, fd);
    update_cwd();
    return result;
}

DEFINE_WRAPPER(int, chdir, (const char *path))
{
    int result = SILENT_CALL_REAL(chdir, path);
    update_cwd();
    return result;
}

static unsigned count_non_null_char_ptrs(va_list args)
{
    va_list args_copy;
    va_copy(args_copy, args);
    unsigned arg_count;
    for(arg_count = 0; va_arg(args_copy, const char *); arg_count++) {
        /* No need to do anything here... */
    }
    va_end(args_copy);
    return arg_count;
}

static char **malloc_argv_from(char *arg, va_list args)
{
    unsigned arg_count = count_non_null_char_ptrs(args) + /*for first arg*/ 1;
    char **argv = malloc(arg_count * sizeof(const char *));
    argv[0] = arg;
    unsigned i;
    for(i = 1; i < arg_count; i++) {
        argv[i] = va_arg(args, char *);
    }
    return argv;
}

int execlp(const char *file, const char *arg, ...)
{
    va_list args;
    va_start(args, arg);
    /* Need to cast away the constness, because execl*'s prototypes
     * are buggy -- taking ptr to const char whereas execv* take ptr
     * to const array of ptr to NON-CONST char */
    char **argv = malloc_argv_from((char *)arg, args);
    va_end(args);
    int rc = execvp(file, argv);
    free(argv);
    return rc;
}

int execvpe_real(const char *file, char *const argv[], char *const envp[]);

DEFINE_WRAPPER(int, execvpe, (const char *file, char *const argv[], char *const envp[]))
{
    DEFINE_MSG(msg, execp);

    // char env_var_PATH[MAX_PATH_ENV_VAR_LENGTH];
    // char conf_str_CS_PATH[MAX_PATH_CONF_STR];

    {
        struct writer w = { PS(msg.args.file) };
        writer_append_str(&w, file);
    }

    {
        struct writer w = { PS(msg.args.cwd) };
        writer_append_data(&w, process_state.cwd, process_state.cwd_length);
        *writer_append(&w, 1) = 0;
    }

    {
        struct writer w = { PS(msg.args.env_var_PATH) };
        const char *PATH = getenv("PATH");
        writer_append_str(&w, PATH);
    }

    {
        errno = 0;
        size_t size = confstr(_CS_PATH, msg.args.conf_str_CS_PATH, sizeof msg.args.conf_str_CS_PATH);
        if(0 == size && 0 != errno) {
            LOG("confstr failed: %s", strerror(errno));
            ASSERT(0);
        }
        ASSERT(size <= sizeof msg.args.conf_str_CS_PATH); /* Value truncated */
    }

    if (!SEND_MSG_AWAIT(true, msg))
        return PERM_ERROR(-1, "execvpe \"%s\"", file);

    return execvpe_real(file, argv, envp);
}

#ifdef __APPLE__
extern char **environ;
#endif

int execvpe_real(const char *file, char *const argv[], char *const envp[])
{
#ifdef __APPLE__
// See http://stackoverflow.com/a/7789795/40916 for reasoning about thread safety
    char **saved = environ;
    int rc;
    environ = (char**) envp;
    rc = execvp(file, argv);
    environ = saved;
    return rc;
#else
    return SILENT_CALL_REAL(execvpe, file, argv, envp);
#endif
}

int execvp(const char *file, char *const argv[])
{
    return execvpe(file, argv, environ);
}

int execv(const char *path, char *const argv[])
{
    return execve(path, argv, environ);
}

/* The following exec* functions that do not have "v" in their name
 * aren't hooks, but translators to the va_list interface which is
 * hooked. If we let this translation happen within libc we won't be
 * able to hook the intra-libc calls. */

int execl(const char *path, const char *arg, ...)
{
    va_list args;
    va_start(args, arg);
    /* Need to cast away the constness, because execl*'s prototypes
     * are buggy -- taking ptr to const char whereas execv* take ptr
     * to const array of ptr to NON-CONST char */
    char **argv = malloc_argv_from((char *)arg, args);
    va_end(args);
    int rc = execv(path, argv);
    free(argv);
    return rc;
}

int execle(const char *path, const char *arg, ...)
{
    va_list args;
    va_start(args, arg);
    char **argv = malloc_argv_from((char *)arg, args);
    ASSERT(NULL == va_arg(args, const char *));
    char *const *envp = va_arg(args, char * const *);
    va_end(args);
    int rc = execve(path, argv, envp);
    free(argv);
    return rc;
}

DEFINE_WRAPPER(int, execve, (const char *filename, char *const argv[], char *const envp[]))
{
    bool needs_await = false;
    DEFINE_MSG(msg, exec);
    IN_PATH_COPY(needs_await, msg.args.path, filename);
    return AWAIT_CALL_REAL(PERM_ERROR(-1, "execve: \"%s\"", filename), needs_await, msg, execve, filename, argv, envp);
}

/****************** open ********************/

static bool notify_openw(const char *path, bool is_also_read, bool is_create, mode_t mode) __attribute__((warn_unused_result));
static bool notify_openw(const char *path, bool is_also_read, bool is_create, mode_t mode)
{
    bool needs_await = false;
    DEFINE_MSG(msg, openw);
    OUT_PATH_COPY(needs_await, msg.args.path, path);
    if(is_also_read) msg.args.flags |= FLAG_ALSO_READ;
    if(is_create)    msg.args.flags |= FLAG_CREATE;
    msg.args.mode = mode;
    return SEND_MSG_AWAIT(needs_await, msg);
}

static bool notify_openr(const char *path) __attribute__((warn_unused_result));
static bool notify_openr(const char *path)
{
    bool needs_await = false;
    DEFINE_MSG(msg, openr);
    IN_PATH_COPY(needs_await, msg.args.path, path);
    return SEND_MSG_AWAIT(needs_await, msg);
}

typedef int open_func(const char *path, int flags, ...);

static int open_common(const char *open_func_name, const char *path, int flags, va_list args)
{
    bool is_also_read = false;
    bool is_create = flags & CREATION_FLAGS;
    mode_t mode = is_create ? va_arg(args, mode_t) : 0;
    switch(flags & (O_RDONLY | O_RDWR | O_WRONLY)) {
    case O_RDONLY:
        /* TODO: Remove ASSERT and correctly handle O_CREAT, O_TRUNCATE */
        ASSERT(!(flags & CREATION_FLAGS));
        if(!notify_openr(path)) return PERM_ERROR(-1, "openr \"%s\"", path);
        break;
    case O_RDWR:
        is_also_read = true;
    case O_WRONLY:
        if(!notify_openw(path, is_also_read, is_create, mode)) return PERM_ERROR(-1, "openw \"%s\" (0x%X)", path, flags);
        break;
    default:
        LOG("invalid open mode?!");
        ASSERT(0);
        return -1;
    }
    open_func *real = dlsym(RTLD_NEXT, open_func_name);
    return real(path, flags, mode);
}

/* Full direct path refers to both the [in]existence of a file, its stat and
 * content */
/* Full path refers to the full direct path and any dereferences of
 * symlinks from it */

/* All outputs to the full path also output (non-exclusively) to the
 * containing directory */

/* For read: Depends on the full path */
/* For write: Outputs the full path */
open_func open;
int open(const char *path, int flags, ...)
{
    va_list args;
    va_start(args, flags);
    int rc = open_common("open", path, flags, args);
    va_end(args);
    return rc;
}

/* Ditto open */
open_func open64;
int open64(const char *path, int flags, ...)
{
    va_list args;
    va_start(args, flags);
    int rc = open_common("open64", path, flags, args);
    va_end(args);
    return rc;
}

static bool fopen_notify(const char *path, const char *modestr) __attribute__((warn_unused_result));
static bool fopen_notify(const char *path, const char *modestr)
{
    mode_t mode = 0666;
    switch(modestr[0]) {
    case 'r': {
        bool res =
            modestr[1] == '+'
            ? notify_openw(path, /*is_also_read*/true, /*create*/false, 0)
            : notify_openr(path);
        if(!res) return PERM_ERROR(false, "fopen \"%s\", \"%s\"", path, modestr);
        break;
    }
    case 'w':
        if(!notify_openw(path, /*is_also_read*/modestr[1] == '+', /*create*/true, mode)) {
            return PERM_ERROR(false, "fopen \"%s\", \"%s\"", path, modestr);
        }
        break;
    case 'a':
        if(!notify_openw(path, true, /*create*/true, mode)) {
            return PERM_ERROR(false, "fopen \"%s\", \"%s\"", path, modestr);
        }
    default:
        LOG("Invalid fopen mode?!");
        ASSERT(0);
    }
    return true;
}

typedef FILE *fopen_func(const char *path, const char *mode);

static FILE *fopen_common(const char *fopen_func_name, const char *path, const char *modestr)
{
    if(!fopen_notify(path, modestr)) return NULL;
    fopen_func *real = dlsym(RTLD_NEXT, fopen_func_name);
    return real(path, modestr);
}

fopen_func fopen;
FILE *fopen(const char *path, const char *modestr)
{
    return fopen_common("fopen", path, modestr);
}

fopen_func fopen64;
FILE *fopen64(const char *path, const char *modestr)
{
    return fopen_common("fopen64", path, modestr);
}

DEFINE_WRAPPER(FILE *, freopen, (const char *path, const char *modestr, FILE *stream))
{
    if(!fopen_notify(path, modestr)) return PERM_ERROR(NULL, "freopen \"%s\" \"%s\"", path, modestr);
    /* fopen_common handles the reporting */
    return SILENT_CALL_REAL(freopen, path, modestr, stream);
}

DEFINE_WRAPPER(FILE *, freopen64, (const char *path, const char *modestr, FILE *stream))
{
    if(!fopen_notify(path, modestr)) return PERM_ERROR(NULL, "freopen64 \"%s\" \"%s\"", path, modestr);
    /* fopen_common handles the reporting */
    return SILENT_CALL_REAL(freopen64, path, modestr, stream);
}

/*************************************/

/* TODO: Track utime? */
/* TODO: Track statfs? */
/* TODO: Track extended attributes? */
/* TODO: Track flock? */
/* TODO: Track ioctls? */

FILE *log_file(void)
{
    /* TODO: This is buggy for fork() */
    static FILE *f = NULL;
    if(!f) {
        FILE *(*fopen_real)(const char *path, const char *mode) =
            dlsym(RTLD_NEXT, "fopen");
        char name[256];
        int (*mkdir_real)(const char *pathname, mode_t mode) =
            dlsym(RTLD_NEXT, "mkdir");
        mkdir_real("/tmp/fs_override.so.log", 0777);
        snprintf(name, sizeof name, "/tmp/fs_override.so.log/pid.%d", getpid());
        f = fopen_real(name, "w");
        ASSERT(f);
    }
    return f;
}

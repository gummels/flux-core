/************************************************************\
 * Copyright 2014 Lawrence Livermore National Security, LLC
 * (c.f. AUTHORS, NOTICE.LLNS, COPYING)
 *
 * This file is part of the Flux resource manager framework.
 * For details, see https://github.com/flux-framework.
 *
 * SPDX-License-Identifier: LGPL-3.0
\************************************************************/

#if HAVE_CONFIG_H
#include "config.h"
#endif
#include <limits.h>
#include <termios.h>
#include <unistd.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <libgen.h>
#include <signal.h>
#include <argz.h>
#include <sys/ioctl.h>
#include <jansson.h>
#include <flux/core.h>
#include <flux/optparse.h>

#include "src/common/libczmqcontainers/czmq_containers.h"
#include "src/common/libutil/xzmalloc.h"
#include "src/common/libutil/log.h"
#include "src/common/libutil/oom.h"
#include "src/common/libutil/cleanup.h"
#include "src/common/libutil/setenvf.h"
#include "src/common/libpmi/simple_server.h"
#include "src/common/libpmi/clique.h"
#include "src/common/libpmi/dgetline.h"
#include "src/common/libhostlist/hostlist.h"
#include "src/common/librouter/usock_service.h"

#define DEFAULT_KILLER_TIMEOUT 20.0

static struct {
    struct termios saved_termios;
    double killer_timeout;
    flux_reactor_t *reactor;
    flux_watcher_t *timer;
    zlist_t *clients;
    optparse_t *opts;
    int test_size;
    int count;
    int exit_rc;
    struct {
        zhash_t *kvs;
        struct pmi_simple_server *srv;
    } pmi;
    flux_t *h;
    flux_msg_handler_t **handlers;
} ctx;

struct client {
    int rank;
    flux_subprocess_t *p;
    flux_cmd_t *cmd;
};

void killer (flux_reactor_t *r, flux_watcher_t *w, int revents, void *arg);
int start_session (const char *cmd_argz, size_t cmd_argz_len,
                   const char *broker_path);
int exec_broker (const char *cmd_argz, size_t cmd_argz_len,
                 const char *broker_path);
char *create_scratch_dir (void);
void client_destroy (struct client *cli);
char *find_broker (const char *searchpath);
static void setup_profiling_env (void);

#ifndef HAVE_CALIPER
static int no_caliper_fatal_err (optparse_t *p, struct optparse_option *o,
                                 const char *optarg)
{
    log_msg_exit ("Error: --caliper-profile used but no Caliper support found");
}
#endif /* !HAVE_CALIPER */

const char *usage_msg = "[OPTIONS] command ...";
static struct optparse_option opts[] = {
    { .name = "verbose",    .key = 'v', .has_arg = 0,
      .usage = "Be annoyingly informative", },
    { .name = "noexec",     .key = 'X', .has_arg = 0,
      .usage = "Don't execute (useful with -v, --verbose)", },
    { .name = "test-size",       .key = 's', .has_arg = 1, .arginfo = "N",
      .usage = "Start a test instance by launching N brokers locally", },
    { .name = "test-hosts", .has_arg = 1, .arginfo = "HOSTLIST",
      .usage = "Set FLUX_FAKE_HOSTNAME in environment of each broker", },
    { .name = "broker-opts",.key = 'o', .has_arg = 1, .arginfo = "OPTS",
      .flags = OPTPARSE_OPT_AUTOSPLIT,
      .usage = "Add comma-separated broker options, e.g. \"-o,-v\"", },
    { .name = "killer-timeout",.key = 'k', .has_arg = 1, .arginfo = "DURATION",
      .usage = "After a broker exits, kill other brokers after DURATION", },
    { .name = "trace-pmi-server", .has_arg = 0, .arginfo = NULL,
      .usage = "Trace pmi simple server protocol exchange", },
    { .name = "scratchdir", .key = 'D', .has_arg = 1, .arginfo = "DIR",
      .usage = "Use DIR as scratch directory", },
    { .name = "noclique", .key = 'c', .has_arg = 0, .arginfo = NULL,
      .usage = "Don't set PMI_process_mapping in PMI KVS", },

/* Option group 1, these options will be listed after those above */
    { .group = 1,
      .name = "caliper-profile", .has_arg = 1,
      .arginfo = "PROFILE",
      .usage = "Enable profiling in brokers using Caliper configuration "
               "profile named `PROFILE'",
#ifndef HAVE_CALIPER
      .cb = no_caliper_fatal_err, /* Emit fatal err if not built w/ Caliper */
#endif /* !HAVE_CALIPER */
    },
    { .group = 1,
      .name = "wrap", .has_arg = 1, .arginfo = "ARGS,...",
      .flags = OPTPARSE_OPT_AUTOSPLIT,
      .usage = "Wrap broker execution in comma-separated arguments"
    },
    OPTPARSE_TABLE_END,
};

/* Various things will go wrong with module loading, process execution, etc.
 *  when current directory can't be found. Exit early with error to avoid
 *  chaotic stream of error messages later in startup.
 */
static void sanity_check_working_directory (void)
{
    char buf [PATH_MAX+1024];
    if (!getcwd (buf, sizeof (buf)))
        log_err_exit ("Unable to get current working directory");
}

int main (int argc, char *argv[])
{
    int e, status = 0;
    char *command = NULL;
    size_t len = 0;
    const char *searchpath;
    int optindex;
    char *broker_path;

    log_init ("flux-start");

    sanity_check_working_directory ();

    ctx.opts = optparse_create ("flux-start");
    if (optparse_add_option_table (ctx.opts, opts) != OPTPARSE_SUCCESS)
        log_msg_exit ("optparse_add_option_table");
    if (optparse_set (ctx.opts, OPTPARSE_USAGE, usage_msg) != OPTPARSE_SUCCESS)
        log_msg_exit ("optparse_set usage");
    if ((optindex = optparse_parse_args (ctx.opts, argc, argv)) < 0)
        exit (1);
    ctx.killer_timeout = optparse_get_duration (ctx.opts, "killer-timeout",
                                                DEFAULT_KILLER_TIMEOUT);
    if (ctx.killer_timeout < 0.)
        log_msg_exit ("--killer-timeout argument must be >= 0");
    if (optindex < argc) {
        if ((e = argz_create (argv + optindex, &command, &len)) != 0)
            log_errn_exit (e, "argz_create");
    }

    if (!(searchpath = getenv ("FLUX_EXEC_PATH")))
        log_msg_exit ("FLUX_EXEC_PATH is not set");
    if (!(broker_path = find_broker (searchpath)))
        log_msg_exit ("Could not locate broker in %s", searchpath);

    if (optparse_hasopt (ctx.opts, "test-size")) {
        ctx.test_size = optparse_get_int (ctx.opts, "test-size", -1);
        if (ctx.test_size <= 0)
            log_msg_exit ("--test-size argument must be > 0");
    }

    setup_profiling_env ();

    if (!optparse_hasopt (ctx.opts, "test-size")) {
        if (optparse_hasopt (ctx.opts, "scratchdir"))
            log_msg_exit ("--scratchdir only works with --test-size=N");
        if (optparse_hasopt (ctx.opts, "noclique"))
            log_msg_exit ("--noclique only works with --test-size=N");
        if (optparse_hasopt (ctx.opts, "test-hosts"))
            log_msg_exit ("--test-hosts only works with --test-size=N");
        status = exec_broker (command, len, broker_path);
    }
    else {
        status = start_session (command, len, broker_path);
    }

    optparse_destroy (ctx.opts);
    free (broker_path);

    if (command)
        free (command);

    log_fini ();

    return status;
}

static void setup_profiling_env (void)
{
#if HAVE_CALIPER
    const char *profile;
    /*
     *  If --profile was used, set or append libcaliper.so in LD_PRELOAD
     *   to subprocess environment, swapping stub symbols for the actual
     *   libcaliper symbols.
     */
    if (optparse_getopt (ctx.opts, "caliper-profile", &profile) == 1) {
        const char *pl = getenv ("LD_PRELOAD");
        int rc = setenvf ("LD_PRELOAD", 1, "%s%s%s",
                          pl ? pl : "",
                          pl ? " ": "",
                          "libcaliper.so");
        if (rc < 0)
            log_err_exit ("Unable to set LD_PRELOAD in environment");

        if ((profile != NULL) &&
            (setenv ("CALI_CONFIG_PROFILE", profile, 1) < 0))
                log_err_exit ("setenv (CALI_CONFIG_PROFILE)");
        setenv ("CALI_LOG_VERBOSITY", "0", 0);
    }
#endif
}



char *find_broker (const char *searchpath)
{
    char *cpy = xstrdup (searchpath);
    char *dir, *saveptr = NULL, *a1 = cpy;
    char path[PATH_MAX];

    while ((dir = strtok_r (a1, ":", &saveptr))) {
        snprintf (path, sizeof (path), "%s/flux-broker", dir);
        if (access (path, X_OK) == 0)
            break;
        a1 = NULL;
    }
    free (cpy);
    return dir ? xstrdup (path) : NULL;
}

void killer (flux_reactor_t *r, flux_watcher_t *w, int revents, void *arg)
{
    struct client *cli;

    cli = zlist_first (ctx.clients);
    while (cli) {
        flux_future_t *f = flux_subprocess_kill (cli->p, SIGKILL);
        if (f)
            flux_future_destroy (f);
        cli = zlist_next (ctx.clients);
    }
}

static void completion_cb (flux_subprocess_t *p)
{
    struct client *cli = flux_subprocess_aux_get (p, "cli");
    int rc;

    assert (cli);

    if ((rc = flux_subprocess_exit_code (p)) < 0) {
        /* bash standard, signals + 128 */
        if ((rc = flux_subprocess_signaled (p)) >= 0)
            rc += 128;
    }

    if (rc > ctx.exit_rc)
        ctx.exit_rc = rc;

    if (--ctx.count > 0)
        flux_watcher_start (ctx.timer);
    else
        flux_watcher_stop (ctx.timer);

    zlist_remove (ctx.clients, cli);
    client_destroy (cli);
}

static void state_cb (flux_subprocess_t *p, flux_subprocess_state_t state)
{
    struct client *cli = flux_subprocess_aux_get (p, "cli");

    assert (cli);

    if (state == FLUX_SUBPROCESS_FAILED) {
        log_errn (errno, "%d FAILED", cli->rank);
        if (--ctx.count > 0)
            flux_watcher_start (ctx.timer);
        else
            flux_watcher_stop (ctx.timer);
        zlist_remove (ctx.clients, cli);
        client_destroy (cli);
    }
    else if (state == FLUX_SUBPROCESS_EXITED) {
        pid_t pid = flux_subprocess_pid (p);
        int status;

        if ((status = flux_subprocess_status (p)) >= 0) {
            if (WIFSIGNALED (status))
                log_msg ("%d (pid %d) %s", cli->rank, pid, strsignal (WTERMSIG (status)));
            else if (WIFEXITED (status) && WEXITSTATUS (status) != 0)
                log_msg ("%d (pid %d) exited with rc=%d", cli->rank, pid, WEXITSTATUS (status));
        } else
            log_msg ("%d (pid %d) exited, unknown status", cli->rank, pid);
    }
}

void channel_cb (flux_subprocess_t *p, const char *stream)
{
    struct client *cli = flux_subprocess_aux_get (p, "cli");
    const char *ptr;
    int rc, lenp;

    assert (cli);
    assert (!strcmp (stream, "PMI_FD"));

    if (!(ptr = flux_subprocess_read_line (p, stream, &lenp)))
        log_err_exit ("%s: flux_subprocess_read_line", __FUNCTION__);

    if (lenp) {
        rc = pmi_simple_server_request (ctx.pmi.srv, ptr, cli, cli->rank);
        if (rc < 0)
            log_err_exit ("%s: pmi_simple_server_request", __FUNCTION__);
        if (rc == 1)
            (void) flux_subprocess_close (p, stream);
    }
}

void add_args_list (char **argz, size_t *argz_len, optparse_t *opt, const char *name)
{
    const char *arg;
    optparse_getopt_iterator_reset (opt, name);
    while ((arg = optparse_getopt_next (opt, name)))
        if (argz_add  (argz, argz_len, arg) != 0)
            log_err_exit ("argz_add");
}

char *create_scratch_dir (void)
{
    char *tmpdir = getenv ("TMPDIR");
    char *scratchdir = xasprintf ("%s/flux-XXXXXX", tmpdir ? tmpdir : "/tmp");

    if (!mkdtemp (scratchdir))
        log_err_exit ("mkdtemp %s", scratchdir);
    cleanup_push_string (cleanup_directory_recursive, scratchdir);
    return scratchdir;
}

static int pmi_response_send (void *client, const char *buf)
{
    struct client *cli = client;
    return flux_subprocess_write (cli->p, "PMI_FD", buf, strlen (buf));
}

static void pmi_debug_trace (void *client, const char *buf)
{
    struct client *cli = client;
    fprintf (stderr, "%d: %s", cli->rank, buf);
}

int pmi_kvs_put (void *arg, const char *kvsname,
                 const char *key, const char *val)
{
    zhash_update (ctx.pmi.kvs, key, xstrdup (val));
    zhash_freefn (ctx.pmi.kvs, key, (zhash_free_fn *)free);
    return 0;
}

int pmi_kvs_get (void *arg, void *client, const char *kvsname,
                 const char *key)
{
    char *v = zhash_lookup (ctx.pmi.kvs, key);
    if (pmi_simple_server_kvs_get_complete (ctx.pmi.srv, client, v) < 0)
        log_err_exit ("pmi_simple_server_kvs_get_complete");
    return 0;
}

int execvp_argz (char *argz, size_t argz_len)
{
    char **av = malloc (sizeof (char *) * (argz_count (argz, argz_len) + 1));
    if (!av) {
        errno = ENOMEM;
        return -1;
    }
    argz_extract (argz, argz_len, av);
    execvp (av[0], av);
    free (av);
    return -1;
}

/* Directly exec() a single flux broker.  It is assumed that we
 * are running in an environment with an external PMI service, and the
 * broker will figure out how to bootstrap without any further aid from
 * flux-start.
 */
int exec_broker (const char *cmd_argz, size_t cmd_argz_len,
                 const char *broker_path)
{
    char *argz = NULL;
    size_t argz_len = 0;

    add_args_list (&argz, &argz_len, ctx.opts, "wrap");
    if (argz_add (&argz, &argz_len, broker_path) != 0)
        goto nomem;

    add_args_list (&argz, &argz_len, ctx.opts, "broker-opts");
    if (cmd_argz) {
        if (argz_append (&argz, &argz_len, cmd_argz, cmd_argz_len) != 0)
            goto nomem;
    }
    if (optparse_hasopt (ctx.opts, "verbose")) {
        char *cpy = malloc (argz_len);
        if (!cpy)
            goto nomem;
        memcpy (cpy, argz, argz_len);
        argz_stringify (cpy, argz_len, ' ');
        log_msg ("%s", cpy);
        free (cpy);
    }
    if (!optparse_hasopt (ctx.opts, "noexec")) {
        if (execvp_argz (argz, argz_len) < 0)
            goto error;
    }
    free (argz);
    return 0;
nomem:
    errno = ENOMEM;
error:
    free (argz);
    return -1;
}

struct client *client_create (const char *broker_path,
                              const char *scratch_dir,
                              int rank,
                              const char *cmd_argz,
                              size_t cmd_argz_len,
                              const char *h)
{
    struct client *cli = xzmalloc (sizeof (*cli));
    char *arg;
    char * argz = NULL;
    size_t argz_len = 0;

    cli->rank = rank;
    add_args_list (&argz, &argz_len, ctx.opts, "wrap");
    argz_add (&argz, &argz_len, broker_path);
    char *dir_arg = xasprintf ("--setattr=rundir=%s", scratch_dir);
    argz_add (&argz, &argz_len, dir_arg);
    free (dir_arg);
    add_args_list (&argz, &argz_len, ctx.opts, "broker-opts");
    if (rank == 0 && cmd_argz)
        argz_append (&argz, &argz_len, cmd_argz, cmd_argz_len); /* must be last arg */

    if (!(cli->cmd = flux_cmd_create (0, NULL, environ)))
        goto fail;
    arg = argz_next (argz, argz_len, NULL);
    while (arg) {
        if (flux_cmd_argv_append (cli->cmd, arg) < 0)
            log_err_exit ("flux_cmd_argv_append");
        arg = argz_next (argz, argz_len, arg);
    }
    free (argz);

    if (flux_cmd_add_channel (cli->cmd, "PMI_FD") < 0)
        log_err_exit ("flux_cmd_add_channel");
    if (flux_cmd_setenvf (cli->cmd, 1, "PMI_RANK", "%d", rank) < 0)
        log_err_exit ("flux_cmd_setenvf");
    if (flux_cmd_setenvf (cli->cmd, 1, "PMI_SIZE", "%d", ctx.test_size) < 0)
        log_err_exit ("flux_cmd_setenvf");
    if (flux_cmd_setenvf (cli->cmd, 1, "FLUX_START_URI",
                          "local://%s/start", scratch_dir) < 0)
        log_err_exit ("flux_cmd_setenvf");
    if (h) {
        if (flux_cmd_setenvf (cli->cmd, 1, "FLUX_FAKE_HOSTNAME", "%s", h) < 0)
            log_err_exit ("error setting fake hostname for rank %d", rank);
    }
    return cli;
fail:
    free (argz);
    client_destroy (cli);
    return NULL;
}

void client_destroy (struct client *cli)
{
    if (cli) {
        if (cli->p)
            flux_subprocess_destroy (cli->p);
        if (cli->cmd)
            flux_cmd_destroy (cli->cmd);
        free (cli);
    }
}

void client_dumpargs (struct client *cli)
{
    int i, argc = flux_cmd_argc (cli->cmd);
    char *az = NULL;
    size_t az_len = 0;
    int e;

    for (i = 0; i < argc; i++)
        if ((e = argz_add (&az, &az_len, flux_cmd_arg (cli->cmd, i))) != 0)
            log_errn_exit (e, "argz_add");
    argz_stringify (az, az_len, ' ');
    log_msg ("%d: %s", cli->rank, az);
    free (az);
}

void pmi_server_initialize (int flags)
{
    struct pmi_simple_ops ops = {
        .kvs_put = pmi_kvs_put,
        .kvs_get = pmi_kvs_get,
        .barrier_enter = NULL,
        .response_send = pmi_response_send,
        .debug_trace = pmi_debug_trace,
    };
    int appnum = 0;

    if (!(ctx.pmi.kvs = zhash_new()))
        oom ();

    if (!optparse_hasopt (ctx.opts, "noclique")) {
        struct pmi_map_block mapblock = {
            .nodeid = 0,
            .nodes = 1,
            .procs = ctx.test_size,
        };
        char buf[256];
        if (pmi_process_mapping_encode (&mapblock, 1, buf, sizeof (buf)) < 0)
            log_msg_exit ("error encoding PMI_process_mapping");
        zhash_update (ctx.pmi.kvs, "PMI_process_mapping", xstrdup (buf));
    }

    ctx.pmi.srv = pmi_simple_server_create (ops, appnum, ctx.test_size,
                                            ctx.test_size, "-", flags, NULL);
    if (!ctx.pmi.srv)
        log_err_exit ("pmi_simple_server_create");
}

void pmi_server_finalize (void)
{
    zhash_destroy (&ctx.pmi.kvs);
    pmi_simple_server_destroy (ctx.pmi.srv);
}

int client_run (struct client *cli)
{
    flux_subprocess_ops_t ops = {
        .on_completion = completion_cb,
        .on_state_change = state_cb,
        .on_channel_out = channel_cb,
        .on_stdout = NULL,
        .on_stderr = NULL,
    };
    /* We want stdio fallthrough so subprocess can capture tty if
     * necessary (i.e. an interactive shell)
     */
    if (!(cli->p = flux_local_exec (ctx.reactor,
                                    FLUX_SUBPROCESS_FLAGS_STDIO_FALLTHROUGH,
                                    cli->cmd,
                                    &ops,
                                    NULL)))
        log_err_exit ("flux_exec");
    if (flux_subprocess_aux_set (cli->p, "cli", cli, NULL) < 0)
        log_err_exit ("flux_subprocess_aux_set");
    return 0;
}

void restore_termios (void)
{
    if (tcsetattr (STDIN_FILENO, TCSAFLUSH, &ctx.saved_termios) < 0)
        log_err ("tcsetattr");
}

void status_cb (flux_t *h,
                flux_msg_handler_t *mh,
                const flux_msg_t *msg,
                void *arg)
{
    struct client *cli;
    json_t *procs = NULL;

    if (!(procs = json_array()))
        goto nomem;
    cli = zlist_first (ctx.clients);
    while (cli) {
        json_t *entry;

        if (!(entry = json_pack ("{s:i}",
                                 "pid", flux_subprocess_pid (cli->p))))
            goto nomem;
        if (json_array_append_new (procs, entry) < 0) {
            json_decref (entry);
            goto nomem;
        }
        cli = zlist_next (ctx.clients);
    }
    if (flux_respond_pack (h, msg, "{s:O}", "procs", procs) < 0)
        log_err ("error responding to status request");
    json_decref (procs);
    return;
nomem:
    errno = ENOMEM;
    if (flux_respond_error (h, msg, errno, NULL) < 0)
        log_err ("error responding to status request");
    json_decref (procs);
}

void disconnect_cb (flux_t *h,
                    flux_msg_handler_t *mh,
                    const flux_msg_t *msg,
                    void *arg)
{
    char *uuid = NULL;

    if (flux_msg_get_route_first (msg, &uuid) < 0)
        goto done;
    if (optparse_hasopt (ctx.opts, "verbose"))
        log_msg ("disconnect from %.5s", uuid);
done:
    free (uuid);
}

const struct flux_msg_handler_spec htab[] = {
    { FLUX_MSGTYPE_REQUEST, "start.status", status_cb, 0 },
    { FLUX_MSGTYPE_REQUEST, "disconnect", disconnect_cb, 0 },
    FLUX_MSGHANDLER_TABLE_END,
};

/* Set up test-related RPC handlers on local://${rundir}/start
 * Ensure that service-related reactor watchers do not contribute to the
 * reactor usecount, since the reactor is expected to exit once the
 * subprocesses are complete.
 */
void start_server_initialize (const char *rundir, bool verbose)
{
    char path[1024];
    if (snprintf (path, sizeof (path), "%s/start", rundir) >= sizeof (path))
        log_msg_exit ("internal buffer overflow");
    if (!(ctx.h = usock_service_create (ctx.reactor, path, verbose)))
        log_err_exit ("could not created embedded flux-start server");
    if (flux_msg_handler_addvec (ctx.h, htab, NULL, &ctx.handlers) < 0)
        log_err_exit ("could not register service methods");
    /* Service related watchers:
     * - usock server listen fd
     * - flux_t handle watcher (adds 2 active prep/check watchers)
     */
    int ignore_watchers = 3;
    while (ignore_watchers-- > 0)
        flux_reactor_active_decref (ctx.reactor);
}

void start_server_finalize (void)
{
    flux_msg_handler_delvec (ctx.handlers);
    flux_close (ctx.h);
}

/* Start an internal PMI server, and then launch the requested number of
 * broker processes that inherit a file desciptor to the internal PMI
 * server.  They will use that to bootstrap.  Since the PMI server is
 * internal and the connections to it passed through inherited file
 * descriptors it implies that the brokers in this instance must all
 * be contained on one node.  This is mostly useful for testing purposes.
 */
int start_session (const char *cmd_argz, size_t cmd_argz_len,
                   const char *broker_path)
{
    struct client *cli;
    int rank;
    int flags = 0;
    char *scratch_dir;
    struct hostlist *hosts = NULL;

    if (isatty (STDIN_FILENO)) {
        if (tcgetattr (STDIN_FILENO, &ctx.saved_termios) < 0)
            log_err_exit ("tcgetattr");
        if (atexit (restore_termios) != 0)
            log_err_exit ("atexit");
        if (signal (SIGTTOU, SIG_IGN) == SIG_ERR)
            log_err_exit ("signal");
    }
    if (!(ctx.reactor = flux_reactor_create (FLUX_REACTOR_SIGCHLD)))
        log_err_exit ("flux_reactor_create");
    if (!(ctx.timer = flux_timer_watcher_create (ctx.reactor,
                                                  ctx.killer_timeout, 0.,
                                                  killer, NULL)))
        log_err_exit ("flux_timer_watcher_create");
    if (!(ctx.clients = zlist_new ()))
        log_err_exit ("zlist_new");

    if (optparse_hasopt (ctx.opts, "scratchdir"))
        scratch_dir = xstrdup (optparse_get_str (ctx.opts, "scratchdir", NULL));
    else
        scratch_dir = create_scratch_dir ();

    start_server_initialize (scratch_dir,
                             optparse_hasopt (ctx.opts, "verbose"));

    if (optparse_hasopt (ctx.opts, "trace-pmi-server"))
        flags |= PMI_SIMPLE_SERVER_TRACE;

    pmi_server_initialize (flags);

    if (optparse_hasopt (ctx.opts, "test-hosts")) {
        const char *s = optparse_get_str (ctx.opts, "test-hosts", NULL);
        if (!(hosts = hostlist_decode (s)))
            log_msg_exit ("could not decode --test-hosts hostlist");
        if (hostlist_count (hosts) != ctx.test_size)
            log_msg_exit ("--test-hosts hostlist has incorrect size");
    }

    for (rank = 0; rank < ctx.test_size; rank++) {
        if (!(cli = client_create (broker_path,
                                   scratch_dir,
                                   rank,
                                   cmd_argz,
                                   cmd_argz_len,
                                   hosts ? hostlist_nth (hosts, rank) : NULL)))
            log_err_exit ("client_create");
        if (optparse_hasopt (ctx.opts, "verbose"))
            client_dumpargs (cli);
        if (optparse_hasopt (ctx.opts, "noexec")) {
            client_destroy (cli);
            continue;
        }
        if (client_run (cli) < 0)
            log_err_exit ("client_run");
        if (zlist_append (ctx.clients, cli) < 0)
            log_err_exit ("zlist_append");
        ctx.count++;
    }
    if (flux_reactor_run (ctx.reactor, 0) < 0)
        log_err_exit ("flux_reactor_run");

    pmi_server_finalize ();
    start_server_finalize ();

    hostlist_destroy (hosts);
    free (scratch_dir);

    zlist_destroy (&ctx.clients);
    flux_watcher_destroy (ctx.timer);
    flux_reactor_destroy (ctx.reactor);

    return (ctx.exit_rc);
}

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */

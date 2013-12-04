/* flux-mecho.c - flux mecho subcommand */

#define _GNU_SOURCE
#include <getopt.h>
#include <json/json.h>
#include <assert.h>
#include <libgen.h>

#include "cmb.h"
#include "util.h"
#include "log.h"

#define OPTIONS "hp:d:"
static const struct option longopts[] = {
    {"help",       no_argument,        0, 'h'},
    {"pad-bytes",  required_argument,  0, 'p'},
    {"delay-msec", required_argument,  0, 'd'},
    { 0, 0, 0, 0 },
};

void usage (void)
{
    fprintf (stderr, 
"Usage: flux-mecho [--pad-bytes N] [--delay-msec N] nodelist\n"
);
    exit (1);
}

int main (int argc, char *argv[])
{
    flux_t h;
    int ch;
    int msec = 1000;
    int bytes = 0;
    char *pad = NULL;
    int seq;
    struct timespec t0;
    char *nodelist;
    json_object *inarg, *outarg;
    int id;
    flux_mrpc_t f;

    log_init ("flux-mecho");

    while ((ch = getopt_long (argc, argv, OPTIONS, longopts, NULL)) != -1) {
        switch (ch) {
            case 'h': /* --help */
                usage ();
                break;
            case 'p': /* --pad-bytes N */
                bytes = strtoul (optarg, NULL, 10);
                pad = xzmalloc (bytes + 1);
                memset (pad, 'p', bytes);
                break;
            case 'd': /* --delay-msec N */
                msec = strtoul (optarg, NULL, 10);
                break;
            default:
                usage ();
                break;
        }
    }
    if (optind != argc - 1)
        usage ();
    nodelist = argv[optind];

    if (!(h = cmb_init ()))
        err_exit ("cmb_init");

    for (seq = 0; ; seq++) {
        monotime (&t0);
        if (!(f = flux_mrpc_create (h, nodelist)))
            err_exit ("flux_mrpc_create");
        inarg = util_json_object_new_object ();
        util_json_object_add_int (inarg, "seq", seq);
        if (pad)
            util_json_object_add_string (inarg, "pad", pad);
        flux_mrpc_put_inarg (f, inarg);
        if (flux_mrpc (f, "mecho") < 0)
            err_exit ("flux_mrpc");
        while ((id = flux_mrpc_next_outarg (f)) != -1) {
            if (flux_mrpc_get_outarg (f, id, &outarg) < 0) {
                msg ("%d: no response", id);
                continue;
            }
            if (!util_json_match (inarg, outarg))
                msg ("%d: mangled response", id);
                json_object_put (outarg);
        }
        json_object_put (inarg);
        flux_mrpc_destroy (f);
        msg ("mecho: pad=%d seq=%d time=%0.3f ms",
             bytes, seq, monotime_since (t0));
        usleep (msec * 1000);
    }

    flux_handle_destroy (&h);
    log_fini ();
    return 0;
}

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */

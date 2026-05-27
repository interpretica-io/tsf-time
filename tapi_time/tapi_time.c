/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief Running a program under a different clock
 *
 * All of this is about producing two environment variables. The work is
 * in finding the library, spelling the clock the way libfaketime reads
 * it, and proving afterwards that any of it took effect.
 */

#define TE_LGR_USER "TAPI TIME"

#include "te_config.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "logger_api.h"
#include "te_alloc.h"
#include "te_str.h"
#include "te_string.h"
#include "te_vector.h"

#include "tapi_devtool_run.h"
#include "tapi_time.h"

/** Where the distributions put libfaketime. */
static const char *const time_library_paths[] = {
    "/usr/lib/x86_64-linux-gnu/faketime/libfaketime.so.1",
    "/usr/lib/aarch64-linux-gnu/faketime/libfaketime.so.1",
    "/usr/lib/faketime/libfaketime.so.1",
    "/usr/local/lib/faketime/libfaketime.so.1",
    "/usr/lib64/faketime/libfaketime.so.1",
    "/usr/lib/libfaketime.so.1",
    "/usr/local/lib/libfaketime.so.1",
    NULL,
};

const tapi_time_opt tapi_time_default_opt = {
    .offset         = NULL,
    .moment         = NULL,
    .frozen         = false,
    .speed          = 0.0,
    .fake_monotonic = false,
    .spec           = NULL,
    .library        = NULL,
};

/** A plain argument vector. */
typedef struct time_args_opt {
    size_t n_args;
    const char **args;
} time_args_opt;

static const tapi_job_opt_bind time_args_binds[] = TAPI_JOB_OPT_SET(
    TAPI_JOB_OPT_ARRAY_PTR(time_args_opt, n_args, args,
        TAPI_JOB_OPT_CONTENT(TAPI_JOB_OPT_STRING, NULL, false))
);

/**
 * Run a command on the agent and hand back what it printed.
 *
 * @param env   Environment for the command, or @c NULL for none.
 */
static te_errno
time_run(tapi_job_factory_t *factory, const char *program,
         const char **args, size_t n_args, const char **env, int timeout_ms,
         te_string *out, bool *ok)
{
    time_args_opt opt = { .n_args = n_args, .args = args };
    tapi_devtool_output output;
    tapi_devtool_run run = TAPI_DEVTOOL_RUN_INIT;
    te_errno rc;

    rc = tapi_devtool_run_init_env(&run, factory, program, program,
                                   time_args_binds, &opt, NULL, env);
    if (rc != 0)
        return rc;

    rc = tapi_devtool_run_start(&run);
    if (rc == 0)
        rc = tapi_devtool_run_wait(&run, timeout_ms);

    if (rc == 0)
    {
        tapi_devtool_run_get_output(&run, &output);
        *ok = output.status.type == TAPI_JOB_STATUS_EXITED &&
              output.status.value == 0;
        if (out != NULL)
            te_string_append(out, "%s", output.out);
    }

    tapi_devtool_run_fini(&run);

    return rc;
}

/* See description in tapi_time.h */
te_errno
tapi_time_find_library(tapi_job_factory_t *factory, int timeout_ms,
                       te_string *path)
{
    size_t i;

    for (i = 0; time_library_paths[i] != NULL; i++)
    {
        const char *args[] = { time_library_paths[i] };
        bool ok = false;
        te_errno rc;

        /*
         * `test -f` rather than a read: the file is a shared object of
         * some megabytes and nothing here wants its contents.
         */
        rc = time_run(factory, "test", args, TE_ARRAY_LEN(args), NULL,
                      timeout_ms, NULL, &ok);
        if (rc != 0)
            continue;

        if (ok)
        {
            te_string_append(path, "%s", time_library_paths[i]);
            RING("libfaketime: %s", time_library_paths[i]);
            return 0;
        }
    }

    ERROR("libfaketime is not installed on the agent; the build image "
          "must carry it");

    return TE_RC(TE_TAPI, TE_ENOENT);
}

/* See description in tapi_time.h */
bool
tapi_time_available(tapi_job_factory_t *factory, int timeout_ms)
{
    te_string path = TE_STRING_INIT;
    bool found;

    found = tapi_time_find_library(factory, timeout_ms, &path) == 0;
    te_string_free(&path);

    return found;
}

/**
 * Spell the clock the way libfaketime reads it.
 *
 * An absolute moment is given as it stands; a leading @c '@' holds it
 * still. A shift is given as it stands too — libfaketime's own
 * vocabulary for @c "+1d" and @c "-2h" is what the option takes, so
 * nothing is translated here.
 */
static void
time_build_spec(const tapi_time_opt *opt, te_string *spec)
{
    if (opt->spec != NULL)
    {
        te_string_append(spec, "%s", opt->spec);
        return;
    }

    if (opt->moment != NULL)
    {
        te_string_append(spec, "%s%s", opt->frozen ? "@" : "", opt->moment);
        return;
    }

    if (opt->offset != NULL)
    {
        te_string_append(spec, "%s", opt->offset);
        return;
    }

    /* No shift asked for: the real time, which is a valid thing to want. */
    te_string_append(spec, "+0");
}

/** Append one entry, keeping the NULL terminator last. */
static void
time_env_push(te_vec *env, char *entry)
{
    size_t size = te_vec_size(env);

    if (size != 0)
    {
        /* Overwrite the terminator; it goes back on below. */
        env->data.len -= sizeof(char *);
    }

    TE_VEC_APPEND(env, entry);
    TE_VEC_APPEND_RVALUE(env, char *, NULL);
}

/* See description in tapi_time.h */
void
tapi_time_env_add(te_vec *env, const char *entry_fmt, ...)
{
    te_string entry = TE_STRING_INIT;
    va_list ap;

    va_start(ap, entry_fmt);
    te_string_append_va(&entry, entry_fmt, ap);
    va_end(ap);

    time_env_push(env, entry.ptr);
}

/* See description in tapi_time.h */
te_errno
tapi_time_env(tapi_job_factory_t *factory, const tapi_time_opt *opt,
              int timeout_ms, te_vec *env)
{
    te_string library = TE_STRING_INIT;
    te_string spec = TE_STRING_INIT;
    te_errno rc = 0;

    *env = (te_vec)TE_VEC_INIT(char *);

    if (opt->library != NULL)
        te_string_append(&library, "%s", opt->library);
    else
        rc = tapi_time_find_library(factory, timeout_ms, &library);

    if (rc != 0)
        goto out;

    time_build_spec(opt, &spec);

    tapi_time_env_add(env, "LD_PRELOAD=%s", library.ptr);
    tapi_time_env_add(env, "FAKETIME=%s", spec.ptr);

    /*
     * Without this the shim caches what it read and a clock that was
     * meant to move stands still.
     */
    tapi_time_env_add(env, "FAKETIME_NO_CACHE=1");

    if (!opt->fake_monotonic)
    {
        /*
         * Sleeps and timeouts are measured against the monotonic clock.
         * Moving it makes a program look hung, which is a bug in the
         * test rather than in the program, so it is left alone unless
         * the test is about exactly that.
         */
        tapi_time_env_add(env, "FAKETIME_DONT_FAKE_MONOTONIC=1");
    }

    if (opt->speed != 0.0)
        tapi_time_env_add(env, "FAKETIME_TIMESTAMP_FILE=");

    RING("Clock for the next job: FAKETIME=%s%s", spec.ptr,
         opt->fake_monotonic ? " (monotonic faked too)" : "");

out:
    te_string_free(&library);
    te_string_free(&spec);

    if (rc != 0)
        tapi_time_env_free(env);

    return rc;
}

/* See description in tapi_time.h */
void
tapi_time_env_free(te_vec *env)
{
    char **entry;

    TE_VEC_FOREACH(env, entry)
        free(*entry);

    te_vec_free(env);
}

/* See description in tapi_time.h */
te_errno
tapi_time_wrap(tapi_job_t *job, const tapi_time_opt *opt)
{
    te_string spec = TE_STRING_INIT;
    tapi_job_wrapper_t *wrapper = NULL;
    const char *argv[4];
    te_errno rc;

    time_build_spec(opt, &spec);

    argv[0] = "faketime";
    argv[1] = "-f";
    argv[2] = spec.ptr;
    argv[3] = NULL;

    rc = tapi_job_wrapper_add(job, "faketime", argv,
                              TAPI_JOB_WRAPPER_PRIORITY_DEFAULT, &wrapper);
    if (rc != 0)
        ERROR("Cannot put faketime in front of the job: %r", rc);

    te_string_free(&spec);

    return rc;
}

/** Read `date +%s` under an environment, or under none. */
static te_errno
time_read_epoch(tapi_job_factory_t *factory, const char **env, int timeout_ms,
                int64_t *epoch_s)
{
    const char *args[] = { "+%s" };
    te_string out = TE_STRING_INIT;
    char *stripped = NULL;
    bool ok = false;
    te_errno rc;

    rc = time_run(factory, "date", args, TE_ARRAY_LEN(args), env, timeout_ms,
                  &out, &ok);
    if (rc != 0)
        goto out;

    if (!ok)
    {
        ERROR("Cannot read the time on the agent");
        rc = TE_RC(TE_TAPI, TE_EFAIL);
        goto out;
    }

    stripped = te_str_strip_spaces(te_string_value(&out));
    if (stripped == NULL)
    {
        rc = TE_RC(TE_TAPI, TE_ENOMEM);
    }
    else
    {
        intmax_t parsed;

        if (te_strtoimax(stripped, 10, &parsed) != 0)
        {
            ERROR("'%s' is not a number of seconds", te_string_value(&out));
            rc = TE_RC(TE_TAPI, TE_EINVAL);
        }
        else
        {
            *epoch_s = parsed;
        }
    }

out:
    free(stripped);
    te_string_free(&out);

    return rc;
}

/* See description in tapi_time.h */
te_errno
tapi_time_now(tapi_job_factory_t *factory, int timeout_ms, int64_t *epoch_s)
{
    return time_read_epoch(factory, NULL, timeout_ms, epoch_s);
}

/* See description in tapi_time.h */
te_errno
tapi_time_check(tapi_job_factory_t *factory, const tapi_time_opt *opt,
                int timeout_ms, int64_t *skew_s)
{
    te_vec env;
    int64_t real = 0;
    int64_t faked = 0;
    te_errno rc;

    rc = tapi_time_env(factory, opt, timeout_ms, &env);
    if (rc != 0)
        return rc;

    rc = tapi_time_now(factory, timeout_ms, &real);
    if (rc == 0)
    {
        rc = time_read_epoch(factory, (const char **)env.data.ptr, timeout_ms,
                             &faked);
    }

    tapi_time_env_free(&env);

    if (rc != 0)
        return rc;

    *skew_s = faked - real;

    RING("The fake clock is %" PRId64 " seconds from the real one "
         "(%" PRId64 " days)", *skew_s, *skew_s / 86400);

    if (*skew_s == 0)
    {
        WARN("The clock did not move. Either nothing was asked for, or the "
             "shim was not loaded — a statically linked binary has no libc "
             "to preload over");
    }

    return 0;
}

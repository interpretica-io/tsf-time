/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief Running a program under a different clock
 *
 * @defgroup tapi_time A different clock (tapi_time)
 * @{
 *
 * Give a program a clock that is not the machine's, and find out what it
 * does about it.
 *
 * The bugs this is for are the ones nobody reaches by waiting: a
 * certificate that expires next year, a token whose lifetime is a day, a
 * schedule that runs at midnight, a log whose order depends on the wall
 * clock, a cache keyed by a timestamp. They are all trivially reachable
 * with a clock that says what you want it to say.
 *
 * @section tapi_time_how How a program is run under it
 *
 * libfaketime is an @c LD_PRELOAD shim: it replaces the time functions
 * of libc for one process. So running a program "under" it is not a
 * special way of launching anything — it is launching it with two
 * environment variables set:
 *
 *   LD_PRELOAD=<path>/libfaketime.so.1
 *   FAKETIME=<what the clock should say>
 *
 * Which is why this module's job is to *produce that environment* and
 * not to start anything. TE already knows two ways to hand it to a
 * process, and both work:
 *
 * - tapi_job_create() takes an @c env argument. Pass what
 *   tapi_time_env() built and the job runs under the fake clock. This
 *   is the general answer, and it works for anything that starts a job;
 * - tapi_time_wrap() instead puts the @c faketime program in front of
 *   the job with tapi_job_wrapper_add(). That program ships with
 *   libfaketime and finds its own library, so nothing has to locate the
 *   shared object — at the cost of needing the wrapper to be installed
 *   and of only working on jobs from the RPC factory.
 *
 * @code
 * tapi_time_opt opt = tapi_time_default_opt;
 * te_vec env;
 * int64_t skew = 0;
 *
 * opt.offset = "+400d";
 *
 * CHECK_RC(tapi_time_env(factory, &opt, 10000, &env));
 *
 * // Prove the clock really moved before trusting what the test sees.
 * CHECK_RC(tapi_time_check(factory, &opt, 10000, &skew));
 * if (skew < 390 * 24 * 3600)
 *     TEST_VERDICT("the fake clock did not take effect");
 *
 * CHECK_RC(tapi_job_simple_create(factory,
 *              &(tapi_job_simple_desc_t){
 *                  .program = "/opt/dut/dutd",
 *                  .argv = (const char *[]){ "dutd", NULL },
 *                  .env = (const char **)env.data.ptr,
 *                  .job_loc = &job,
 *              }));
 * @endcode
 *
 * @section tapi_time_limits What it cannot do
 *
 * - **A process already running is out of reach.** The shim is
 *   installed when the process starts, so a service under test has to
 *   be restarted under the fake clock. There is no way to shift the
 *   clock of something that is already up.
 * - **A statically linked program is out of reach**, because there is
 *   no dynamic libc to preload over. A fully static musl build sees the
 *   real time and the test passes for the wrong reason — which is what
 *   tapi_time_check() exists to catch.
 * - **The kernel's clock does not move.** File timestamps, the output
 *   of @c dmesg and anything else the kernel writes stay real, so a
 *   program comparing its own idea of now against a file's mtime sees
 *   the gap. That is sometimes the bug being hunted and sometimes an
 *   artefact of the method; it has to be told apart deliberately.
 * - **Faking the monotonic clock breaks timers.** Sleeps and timeouts
 *   are measured against it, and a monotonic clock that jumps makes a
 *   program look hung. It is left alone unless
 *   tapi_time_opt::fake_monotonic says otherwise, and that switch is
 *   for testing what a program does when it *is* broken.
 */

#ifndef __TSF_TAPI_TIME_H__
#define __TSF_TAPI_TIME_H__

#include "te_defs.h"
#include "te_errno.h"
#include "te_string.h"
#include "te_vector.h"
#include "tapi_job.h"

#ifdef __cplusplus
extern "C" {
#endif

/** What the clock should say. */
typedef struct tapi_time_opt {
    /**
     * A shift from the real time, in libfaketime's spelling: @c "+1d",
     * @c "-2h", @c "+400d", @c "+1y". Ignored when @a moment is set.
     */
    const char *offset;
    /**
     * An absolute moment, as @c "2030-01-01 00:00:00". Takes precedence
     * over @a offset.
     */
    const char *moment;
    /**
     * Hold the clock still at @a moment instead of letting it run on
     * from there. Useful for anything that must not see time pass;
     * dangerous for anything that waits for it to.
     */
    bool frozen;
    /**
     * How fast the clock runs, as a multiplier: @c 10.0 makes a minute
     * pass in six seconds. @c 0 leaves the rate alone.
     */
    double speed;
    /**
     * Fake the monotonic clock as well. Off by default, and read the
     * warning in the module description before turning it on.
     */
    bool fake_monotonic;
    /**
     * The @c FAKETIME value to use verbatim, instead of building one
     * from the fields above.
     *
     * An escape hatch on purpose: libfaketime's language is larger than
     * this structure (it reads a file, it understands start-at
     * semantics, it takes a @c %-format), and a test that needs a
     * corner of it should not have to wait for this header to grow.
     */
    const char *spec;
    /**
     * Path of @c libfaketime.so.1 on the agent, when it is somewhere
     * tapi_time_find_library() does not look.
     */
    const char *library;
} tapi_time_opt;

/** The clock as it is: no shift, monotonic untouched. */
extern const tapi_time_opt tapi_time_default_opt;

/**
 * Find @c libfaketime.so.1 on the agent.
 *
 * The usual places are tried, because the distributions do not agree:
 * Debian and Ubuntu put it under a multiarch faketime directory, others
 * put it straight in the library path.
 *
 * @param[in]  factory      Job factory.
 * @param[in]  timeout_ms   Timeout, ms.
 * @param[out] path         String to append the path to.
 *
 * @return Status code.
 * @retval TE_ENOENT        libfaketime is not installed on the agent.
 */
extern te_errno tapi_time_find_library(tapi_job_factory_t *factory,
                                       int timeout_ms, te_string *path);

/**
 * Check whether the agent can run anything under a fake clock at all.
 *
 * @param factory       Job factory.
 * @param timeout_ms    Timeout, ms.
 *
 * @return @c true when libfaketime is there.
 */
extern bool tapi_time_available(tapi_job_factory_t *factory, int timeout_ms);

/**
 * Build the environment that puts a process under the clock @p opt
 * describes.
 *
 * The result is a @c NULL terminated vector of @c "NAME=value" strings,
 * ready to be handed to tapi_job_create() or to
 * tapi_job_simple_desc_t::env as @c (const char **)env->data.ptr.
 *
 * It carries only what the fake clock needs. A job that wants the rest
 * of an environment has to add it: tapi_job does not inherit the
 * agent's, so whatever is not in this vector is not there.
 *
 * @param[in]  factory      Job factory.
 * @param[in]  opt          What the clock should say.
 * @param[in]  timeout_ms   Timeout, ms.
 * @param[out] env          Vector of @c char*, initialized by the call;
 *                          release it with tapi_time_env_free().
 *
 * @return Status code.
 */
extern te_errno tapi_time_env(tapi_job_factory_t *factory,
                              const tapi_time_opt *opt, int timeout_ms,
                              te_vec *env);

/**
 * Add entries of your own to an environment built by tapi_time_env().
 *
 * The @c NULL terminator is kept at the end, so the vector stays usable
 * as a job environment.
 *
 * @param env           Vector from tapi_time_env().
 * @param entry_fmt     Format string of a @c "NAME=value" entry.
 * @param ...           Format arguments.
 */
extern void tapi_time_env_add(te_vec *env, const char *entry_fmt, ...)
    TE_LIKE_PRINTF(2, 3);

/**
 * Release an environment.
 *
 * @param env           Vector from tapi_time_env().
 */
extern void tapi_time_env_free(te_vec *env);

/**
 * Put the @c faketime program in front of a job.
 *
 * An alternative to the environment: the wrapper finds the library
 * itself, so nothing here has to. It needs the @c faketime program on
 * the agent and a job created by the RPC factory, and it has to be
 * called before the job is started.
 *
 * @param job           Job to wrap.
 * @param opt           What the clock should say.
 *
 * @return Status code.
 */
extern te_errno tapi_time_wrap(tapi_job_t *job, const tapi_time_opt *opt);

/**
 * Find out how far the fake clock actually moved.
 *
 * Runs @c date under the environment @p opt describes and compares what
 * it says with the agent's real time.
 *
 * This is the check that keeps a clock test honest. The shim is
 * installed by the dynamic loader, and there are several ordinary ways
 * for that to quietly not happen — a statically linked binary, a
 * missing library, an @c LD_PRELOAD that something else overwrote. In
 * every one of them the program sees the real time, behaves perfectly,
 * and the test reports a pass it did not earn. Assert the skew is what
 * was asked for before believing anything else the test saw.
 *
 * @param[in]  factory      Job factory.
 * @param[in]  opt          What the clock should say.
 * @param[in]  timeout_ms   Timeout, ms.
 * @param[out] skew_s       Seconds between the fake clock and the real
 *                          one; negative when the fake clock is behind.
 *
 * @return Status code.
 */
extern te_errno tapi_time_check(tapi_job_factory_t *factory,
                                const tapi_time_opt *opt, int timeout_ms,
                                int64_t *skew_s);

/**
 * Read the agent's real time, as seconds since the epoch.
 *
 * @param[in]  factory      Job factory.
 * @param[in]  timeout_ms   Timeout, ms.
 * @param[out] epoch_s      Where to save it.
 *
 * @return Status code.
 */
extern te_errno tapi_time_now(tapi_job_factory_t *factory, int timeout_ms,
                              int64_t *epoch_s);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* !__TSF_TAPI_TIME_H__ */

/**@} <!-- END tapi_time --> */

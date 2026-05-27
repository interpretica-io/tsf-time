# tsf-time

Running a program on a Test Agent under a clock that is not the
machine's, packaged as an external Test Environment (TE) repository.

Library:

- `tapi_time` — engine-side, built as a shared library: build the
  environment that puts a process under a fake clock, find the shim,
  and prove the clock actually moved.

The bugs this is for are the ones nobody reaches by waiting: a
certificate that expires next year, a token whose lifetime is a day, a
schedule that runs at midnight, a log whose order depends on the wall
clock, a cache keyed by a timestamp. All of them are one environment
variable away.

## How a program is run under it

This is the question the whole module is shaped around, so it is worth
stating plainly: **libfaketime is an `LD_PRELOAD` shim**. Running a
program "under" it is not a special way of launching anything — it is
launching it with two environment variables set:

```
LD_PRELOAD=<path>/libfaketime.so.1
FAKETIME=<what the clock should say>
```

So this module does not start processes. It **produces that
environment** and hands it back; TE already knows two ways to get it
onto a process, and both work:

```c
tapi_time_opt opt = tapi_time_default_opt;
te_vec env;

opt.offset = "+400d";
CHECK_RC(tapi_time_env(factory, &opt, 10000, &env));

CHECK_RC(tapi_job_simple_create(factory,
             &(tapi_job_simple_desc_t){
                 .program = "/opt/dut/dutd",
                 .argv = (const char *[]){ "dutd", NULL },
                 .env = (const char **)env.data.ptr,
                 .job_loc = &job,
             }));
```

That is `tapi_job_create()`'s own `env` argument — the general answer,
usable by anything that starts a job.

The other way is `tapi_time_wrap()`, which puts the `faketime` program
in front of an already-created job with `tapi_job_wrapper_add()`. That
program ships with libfaketime and finds its own library, so nothing
has to locate the shared object — at the price of needing the wrapper
installed and of only working on jobs from the RPC factory.

Tools driven through tsf-devtool's run primitive use
`tapi_devtool_run_init_env()`, which was added for this: a job does not
inherit the agent's environment, so what is not in the vector is not
there.

## Prove it took effect first

```c
int64_t skew = 0;

CHECK_RC(tapi_time_check(factory, &opt, 10000, &skew));
if (skew < 390 * 24 * 3600)
    TEST_VERDICT("the fake clock did not take effect");
```

This is the check that keeps a clock test honest, and it is the same
trap as a fuzzing harness that never ran. The shim is installed by the
dynamic loader, and there are several ordinary ways for that to quietly
not happen — a statically linked binary, a missing library, an
`LD_PRELOAD` something else overwrote. In every one of them the program
sees the real time, behaves perfectly, and the test reports a pass it
did not earn.

`tapi_time_check()` runs `date` under the environment and compares it
with the agent's real time. Assert the skew is what you asked for
before believing anything else.

## What the clock can say

| Field | Meaning |
|---|---|
| `offset` | a shift, in libfaketime's spelling: `"+1d"`, `"-2h"`, `"+400d"` |
| `moment` | an absolute time, `"2030-01-01 00:00:00"` |
| `frozen` | hold it still at `moment` rather than running on |
| `speed` | how fast it runs, as a multiplier |
| `fake_monotonic` | fake the monotonic clock too — read the warning below |
| `spec` | a raw `FAKETIME` value, used verbatim |

`spec` is an escape hatch on purpose. libfaketime's language is larger
than this structure, and a test that needs a corner of it should not
have to wait for the header to grow.

**Monotonic is left alone by default.** Sleeps and timeouts are measured
against it, and a monotonic clock that jumps makes a program look hung —
a bug in the test rather than in the program. Turn it on only when that
is the thing under test.

## What it cannot do

- **A process already running is out of reach.** The shim is installed
  at start, so a service under test has to be restarted under the fake
  clock.
- **A statically linked program is out of reach** — there is no dynamic
  libc to preload over. A fully static musl build sees the real time.
- **The kernel's clock does not move.** File timestamps and `dmesg` stay
  real, so a program comparing its own now against a file's mtime sees
  the gap. Sometimes that is the bug; sometimes it is an artefact of the
  method, and the two have to be told apart deliberately.

## What the agent needs

`libfaketime` installed. It is in none of the build images yet:

```
apt-get install -y libfaketime
```

`tapi_time_find_library()` looks in the places the distributions use and
fails loudly when it finds nothing, rather than letting a test run
against the real clock and pass.

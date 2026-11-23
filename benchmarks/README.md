# Headless process measurements

`tools/benchmark.py` is a small Python 3.11+ standard-library harness for repeated
**whole-process wall-clock measurements**. It starts the supplied headless executable
with `--package PATH --inspect`, or runs its default synchronous lifecycle fixture.
It downloads nothing and invokes argument lists without a shell.

From a checkout with an existing build and cooked package:

```sh
mkdir -p out/benchmarks
python3 tools/benchmark.py \
  --binary out/build/release/bin/ge_headless \
  --package out/build/release/assets/district.gepack \
  --runs 5 --mode inspect --output out/benchmarks/inspect.json
```

For Windows, create the output directory with PowerShell `New-Item -ItemType
Directory -Force out/benchmarks`, use `py -3`, and supply the executable's `.exe`
path. Paths are relative to the caller. The output directory must already exist.
`--mode lifecycle` omits `--inspect`; it uses the host's default budget and does not
request persistent save files. Mode defaults to `inspect`; runs default to five
and must be between 1 and 1000. Each process has a 30-second timeout.

## Measurement and validation

Runs execute sequentially in a temporary working directory, using absolute input
paths and the inherited environment. `time.perf_counter_ns()` brackets process
launch through exit, including startup, package reading, stdout/stderr file output,
and the parent's process-wait overhead. JSON validation and input hashing happen
outside that interval. All runs are retained; there is no discarded warm-up run.
OS caches, machine load, scheduling, power policy and environment are uncontrolled.
Input hashing itself reads the files before measurement and may populate caches.
These are not cold-cache measurements.

A run is accepted only after a zero exit, empty stderr and one valid JSON object
with the expected mode and package schema. Inspection requires a regions array;
its current contract has no status field, but any supplied status must be `ok`.
Lifecycle requires `status: "ok"` and a nonempty checks object whose values are all
true. Stdout validation is bounded at 32 MiB. Failure diagnostics retain only their
first 4096 bytes per stream. Timeout or launch failure has a null exit code.

Reports contain individual durations, exit codes and validation results; platform,
Python, binary/package SHA-256; and source Git revision/dirty state when available.
Git state is sampled at the start and does not prove the binary was built from that
revision. Hashes are checked again after the runs. Successful reports summarize
minimum, maximum and median nanoseconds; for even counts the median is the mean of
the two central sorted observations. No percentile or threshold is inferred.

## Publication and limits

Only a fully successful report atomically replaces `--output`. Invalid options or
failed execution preserve existing output. Failed execution returns nonzero and
emits an explicit failed JSON report to stdout, with no summary; redirect stdout to
a **different file** to retain it. Output aliases of either input, and symlink
outputs, are rejected.

This harness measures fixture process duration. It establishes no frame rate, GPU
performance, asset-streaming throughput, renderer capability or hardware target.
Build and integration evidence remains in
[the verification record](../docs/production/verification.md).

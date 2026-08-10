# TTree coordinate read-path benchmark (ssccs issue 103, M5)

## What it measures

The benchmark runs the same logical workload, read every event of a
dataset, through two read paths and reports wall-clock time, request
count, bytes moved, and system call count for each.

The baseline replicates the M1 workload: sequential `TTree::GetEntry`
with the TTreeCache disabled, which produces the scattered singular
reads documented as 372,000 requests averaging 4.6 KB. The coordinate
path serves the same number of events as fixed-width records through
`TTagmaStore`: one read per event at the record size, resolved by
closed-form address arithmetic instead of the branch, basket, and cache
machinery. The mapped coordinate path serves the same records from the
mmap'ed store file with no read system call at all, so the byte source
never reaches the medium for mapped data.

Both paths call the same API, `GetEntry`, so the comparison isolates the
interior read path. The coordinate store file holds a deterministic byte
pattern with the same record layout; content correctness is covered by
the unit tests in `io/io/test/tagma_store.cxx` and
`tree/tree/test/tagma_record.cxx`.

## Prerequisites

A fork build with RIO and Tree. The M1 build from the baseline runbook
(`root-build-tagma`, with `treeplayer=ON` for `TTreePerfStats`) also
satisfies the harness; the harness itself does not need treeplayer.

## Synthetic run

The default mode generates a scattered tree with the same per-event
payload as the coordinate store and needs no network or dataset:

```bash
../root-build-tagma/bin/root -l -b -q 'tagma_bench.C()'
```

The same run is registered in CTest when testing is enabled:

```bash
ctest -R tagma-bench
```

Arguments: `tagma_bench(url, tree_name, max_entries, record_size,
nscatter, disable_cache)`. An empty `url` selects the synthetic source;
`record_size` defaults to 2560 bytes (the documented 2.6 KB average
read); `nscatter` is the number of branches per event in the synthetic
baseline, the number of reads issued per event by the scattered path.

## Real-data run

The M1 workload against the CMS Open Data NanoAOD file, read all
entries with the cache disabled, with the coordinate path at the
measured per-event record size:

```bash
../root-build-tagma/bin/root -l -b -q 'tagma_bench.C("root://eospublic.cern.ch//eos/opendata/cms/Run2016G/DoubleMuon/NANOAOD/UL2016_MiniAODv2_NanoAODv9-v2/2430000/05DD095C-F6C3-9A4F-9FB3-348A5A6403D5.root", "Events", -1, 2560)'
```

The file and tree name match the M1 baseline runbook. The remote run
reads the file once through the network; cap the entry count with the
third argument for a quick check.

## Real-data run against a converted store

The benchmark serves real converted records when the store is built
with the preparation tool first. The tool reads each event (cache on)
and writes a fixed-width record holding the first (record_size / 8)
scalar leaf values as doubles, zero-padded, plus a sidecar checksum
file. The conversion cost is reported separately from the read-path
measurement.

```bash
../root-build-tagma/bin/root -l -b -q 'tagma_make_store.C("/path/to/local.root", "Events", "tagma_store.bin", 2560)'
../root-build-tagma/bin/root -l -b -q 'tagma_bench.C("/path/to/local.root", "Events", -1, 2560, 3, 1, "tagma_store.bin")'
```

The benchmark verifies that the store holds exactly the converted
records (size check) and that the bytes the coordinate paths serve
match the sidecar checksum, so the measured rows run against real
event data, not a pattern. Without the store path argument the
benchmark generates a deterministic pattern store.

## What to record

| Quantity | Source |
| :--- | :--- |
| Wall time and CPU time per path | `TStopwatch` in the macro output |
| Request count and coordinate-served count | `TFile::GetReadCalls` and `GetTagmaReadCalls` |
| Bytes moved | `TFile::GetBytesRead` delta |
| System call count | `TFile::GetSysReadCalls` delta, counted in `TFile::SysRead` |
| Derived per-event metrics | `reads/ev`, `bytes/read`, `syscalls/ev`, `MB/s` in the macro output |

The system call count is meaningful for local files, where every
`TFile::ReadBuffer` request is served by one `TFile::SysRead`. Remote
sources (root:// URLs) read through the network plugin instead, so the
syscall column stays zero there; the request count and bytes moved still
apply. The mapped coordinate row serves every covered record from the
mapping with zero read system calls, the syscall column stays zero, and
the mapped pages are demand-paged once by the kernel with sequential
locality.

## Interpretation

- Baseline reads per event near one or above with small bytes per read:
  the scattered singular-read signature, the term M1 attributed to I/O
  wait
- Coordinate reads per event of exactly one at the record size: the
  fixed-width record path collapses the per-event scatter into a single
  aligned read
- Mapped coordinate reads per event of exactly one with zero read
  system calls: the byte source is the mapped region, and the read path
  never reaches the medium for mapped data, the structural removal the
  plan describes
- Wall time per event scales with the request count when the two paths
  move the same payload bytes: the overhead is per request, the target
  of the coordinate store

The real-data run measures the baseline against the remote EOS file and
the coordinate paths against the local store file. The two paths live on
different media by construction; the reported request count and bytes
per read are media-independent, while the wall-clock split reflects the
remote latency of the baseline. The mapped coordinate row requires a
Unix-like platform (mmap), like the canonical CoordSpaceM reference.

# Coordinate reader backend

Development plan for the TTree coordinate read path in this fork. Tracking
issue: ssccsorg/ssccs#121.

## Goal

Make the coordinate store a byte source under the existing branch and leaf
machinery, so that ordinary leaf access, `TTreeReader`, and `RDataFrame`
read store-backed events without a new user-facing API and without
per-analysis migration.

## Where the code stands

| Layer | File | State |
| :--- | :--- | :--- |
| Address arithmetic | `io/io/inc/ROOT/TTagmaStore.hxx` | Complete: composition, decomposition, offset, bounds, mmap |
| Store format | none | The store is a bare record array; the axis layout is a caller-supplied struct and the field table is a sidecar text file |
| Byte source hook | `io/io/src/TFile.cxx` | Serves record-aligned requests of exactly the record size, so no basket request can ever be served |
| Entry hook | `tree/tree/src/TTree.cxx` | Fills a private record buffer and returns before the branch loop; branches are never filled |
| Field table | `benchmarks/tagma/tagma_bench.C` | Parsed by the benchmark, not by the library |

Consequences. The store is reachable only through `GetTagmaRecordBuffer()`,
which is a new API, so the claim that existing analysis code runs unchanged
is not yet true in code. `TTreeReader` reads through branches, so an
entry-level short circuit is invisible to it and `RDataFrame` cannot reach
the store.

## Target architecture

The addressing engine stays where it is. Three layers are added under the
existing branch and leaf machinery.

Schema. A field table (name, type, byte offset) with a size and a ROOT leaf
code per field, sized against the record. It replaces the benchmark-local
parser and becomes part of the store format.

Record source. One interface from a record index to bytes, with the byte
sources as implementations: a positioned read, a mapping, and later a
block-compressed and a block-cached source. The file hook becomes a
delegation, so compression and caching are added as implementations instead
of branching inside the hook.

Branch materialization. One `TBranch` per schema field, with its address
inside a record buffer that is allocated once and never resized. Leaves then
read the record bytes directly, so `TLeaf::GetValue()`, `TTreeReader`, and
`RDataFrame` see ordinary branches whose values come from the store.

The record buffer's single allocation is a correctness constraint, not a
detail: the materialized branches point into it, so any later reallocation
would leave every leaf address dangling.

## Phases and gates

P1. Schema in the library, and leaf access from the store.
Move the field table into `ROOT::TTagmaSchema`. `TTree::SetTagmaSchema`
allocates the record buffer once and creates one branch per field with the
address at the field offset. `TTree::GetEntry` fills that buffer in place.
Gate: a gtest that compares leaf values against the record bytes over many
entries, and a check that a tree without a schema keeps the current
behaviour.

P2. Record source seam.
Introduce the record source interface with a positioned-read and a mapped
implementation. `TFile::ReadBufferViaTagma` delegates to it.
Gate: the existing tagma gtests and the benchmark rows are unchanged.

P3. Self-describing store.
Header with magic, version, axis maxima, record size, field table, and
checksum; the sidecars go away; the writer preserves field types instead of
widening every value to `double`. Pin the syntagma dependency to a tag or
commit: `GIT_TAG main` in `tree/tree/CMakeLists.txt` makes the build
unreproducible and needs network access at configure time.
Gate: a store written by the writer opens and reads with no sidecar files.

P4. Interoperability gate.
`TTreeReader`, `RDataFrame`, and `SetBranchStatus` column selection.
Measured after P1: a store-backed tree iterated the right number of entries
under `TTreeReader`, and the values did not advance, because
`TTreeReaderValue` reads through `TBranchProxy` (see
`ProxyReadDefaultImpl` in `tree/treeplayer/src/TTreeReaderValue.cxx`), which
drives the branch read path and never reaches `TTree::GetEntry`. The leaf
addresses kept their last value.
So the materialized branches are necessary and not sufficient, and the
record-fed read belongs where the proxy enters: `TBranch::GetEntry`, which
loads the tree's record for the entry. The entry-level short circuit in
`TTree::GetEntry` stays for consumers that call it directly, and both paths
share one buffer and one read per entry.
Gate: `gtest-tree-tree-tagma-dataframe` runs the same expression over the
original file and over the store and compares the histogram, the selected
count, and every bin.
One property of the fixed-width record to keep in view: a column-restricted
read moves fewer bytes on the file side and the same bytes on the store
side, because the record unit is the whole event. Byte savings from column
selection do not exist on the store path, and the record is the addressing
unit.

P5. Variable-length fields.
Decide the addressing model first: a fixed slot per event with padding,
which keeps arithmetic addressing and wastes space, or a packed variable
region with a per-event offset, which is compact and costs one indirection.
Gate: a variable-length array field reads through the ordinary leaf
machinery.

P6. Dataset addressing and concurrency.
A dataset object holding per-file axis ranges so a (run, luminosity block,
event number) coordinate resolves to a file and an offset, with the axis
maxima derived from the data. Make the hook counters thread-safe, and drive
the standard implicit multi-threading path.
Gate: a measured read that exercises the multi-axis composition, and a
multithread run against a bounded baseline cache.

## Risks

The reader proxy path does not reach `TTree::GetEntry`, which is measured,
not hypothetical: see the P4 note. The mitigation is to keep the standard
`TBranch` address binding and add the record-fed read where the proxy calls
into it, so the type surface grows only as far as the gate demands.

`TBranchElement` buffer and counter handling can conflict with a
record-fed branch. Mitigation is to restrict P1 to simple leaf types and to
let the gate decide what element support requires.

Compression and direct per-record addressing are in tension: a fixed
compressed size per record is not generally available, so compression
reintroduces a block table and a decompression step. The record source
seam exists so that this stays an implementation choice rather than a
rewrite of the file hook.

## Progress

| Phase | State | Evidence |
| :--- | :--- | :--- |
| P1 | Done | `gtest-tree-tree-tagma-schema`: leaf access over the record bytes |
| P4 | Done | `gtest-tree-tree-tagma-dataframe`: identical histogram from file and store, and `gtest-tree-tree-tagma-schema` reads through `TTreeReader` |
| P2, P3, P5, P6 | Open | |

Both gates run against a build with `dataframe=ON`; the RDataFrame gate is
registered only when that module is enabled.

## Out of scope

Report revisions and benchmark methodology. Those are tracked separately in
`ssccsorg/ssccs#120`.

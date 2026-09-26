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
| Address arithmetic | `io/io/inc/ROOT/TTagmaStore.hxx` | Complete: composition, decomposition, offset, bounds, the data-region extent, and mmap |
| Field table | `io/io/inc/ROOT/TTagmaSchema.hxx` | In the library: scalar and collection fields, counts resolved from the count scalar, the text form, and validation against the record size |
| Store format | none | The index record and the packed data region are addressed by arithmetic. The axis layout and the field table still arrive as a caller-supplied struct and a sidecar, so the store is not self-describing |
| Store producer | `io/io/inc/ROOT/TTagmaWriter.hxx` | Complete: the index region and the packed data region, written in one pass over the events, the counts read back out of the record the reader reads them from |
| Byte source | `io/io/src/TFile.cxx` | Serves record-aligned requests of exactly the record size, and any range the store covers, the data slice among them, from the mapping when one is attached |
| Branch read path | `tree/tree/src/TBranch.cxx` | `GetEntry` loads the tree's record for the entry and copies an array field's elements out of the slice, so `TTreeReader` and `RDataFrame` read store-backed events |
| Entry hook | `tree/tree/src/TTree.cxx` | Fills the record in place, then drives the array branches. The short circuit stays for direct callers |

Consequences. Leaf access, `TTreeReader`, and `RDataFrame` reach the store
through the ordinary branch machinery, so the store is a byte source under
the existing interfaces and analysis code does not change. Two gaps remain.
The axis layout and the field table are caller-supplied, so the store is not
self-describing, and the benchmark's conversion tool still writes a scalar
projection of the event, so the store it converts carries the collections as
their leading element; the store-side producer that carries them whole is in
place.

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

P5. Variable-length fields. This is the P2 phase of the report's roadmap.

Measured on the CMS file before the design, with `benchmarks/tagma/tagma_arrays.C`:
the store's 320 fields are 44 scalar leaves carrying 180 bytes per event, and
276 array fields belonging to ten collections. Per collection, the mean and
maximum object counts and the bytes per object are nJet 4.94 and 32 at 157
bytes, nMuon 2.11 and 20 at 137, nElectron 0.29 and 8 at 184, nFatJet 0.14 and
6 at 213, nIsoTrack 1.14 and 20 at 51, nCorrT1METJet 5.15 and 32 at 20, and
five smaller collections.

That settles the addressing model. A fixed slot per collection per event,
sized by the collection maximum, would need 13,678 bytes per event and 31.67 GB
for this file, 11.1x the 1,233 bytes per event and 2.85 GB of a packed layout,
and 5.3x the 2,560 bytes of the current store. The padding is the whole cost,
because the mean counts are a fifth of the maximum for jets and a tenth for
muons.

The model to implement is a fixed-width index record plus a packed data
region. The index record is addressed arithmetically, at index times index
record size, and holds the scalar fields, a count per collection, and one
64-bit base for the event's slice of the data region. The data region packs the
collections in a fixed order, so the offset of each collection inside the
event's slice follows from the counts by arithmetic, and no further stored
offset is needed. Per event the record is about 228 bytes of index plus about
1,053 bytes of packed data, so about 1,281 bytes against the current 2,560,
carrying all ten collections rather than their leading elements.

The cost is one extra read per event: the index record, then the event's slice
of the data region. Both are contiguous and both are covered by the mapping, so
the mapped path stays at zero application read calls. A partial read becomes
possible, since an analysis that needs one collection can read the index record
and only that collection's slice, whose offset and length both follow from the
counts. That is column pruning at collection granularity, which the fixed-width
record cannot offer at all.

Gate: `gtest-tree-tree-tagma-variable`, which writes a store carrying two
collections, reads it through the ordinary branch machinery, and checks the
array values, the object counts, the reuse of a repeated entry, and the
rejection of a count past the schema bound. The record is the addressing
unit, so the file side of the comparison stays in
`gtest-tree-tree-tagma-dataframe`. The mechanism is the standard one, a count
branch and an array branch whose leaflist names the count, so the work is in
the store side, not in a new leaf type. The store side is
`ROOT::TTagmaWriter`, which writes the index region and the packed data region
in one pass over the events, the base of the current event's slice being the
end of the data region so far; the benchmark's conversion tool is what remains
to adopt it.

One constraint the leaf machinery imposes decides the chunk layout. A leaf
created from a count-carrying leaflist reads element i at its address plus i
times the element size, so the elements of one field have to be contiguous in
memory at a fixed address. A collection chunk therefore places its fields one
after the other rather than object by object, and the reader copies the fields
whose branches are active into fixed per-field buffers, at offsets that follow
from the count. Because the copy set follows the branch activation state, the
bytes copied per event scale with the columns the analysis reads: column
pruning at field granularity, inside the addressing unit rather than instead
of it.

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
| P5 | Reader and producer done; benchmark conversion open | `gtest-tree-tree-tagma-variable`: collections read through the branches, the array values and counts checked against the store bytes, and a count past the bound rejected; `gtest-io-io-tagma-writer`: the producer lays out the index and data regions the reader addresses. The benchmark's conversion tool still emits the scalar projection, so the store it measures carries 276 of its 320 fields as array leading elements |
| P2, P3, P6 | Open | |

Both gates run against a build with `dataframe=ON`; the RDataFrame gate is
registered only when that module is enabled.

## Out of scope

Report revisions and benchmark methodology. Those are tracked separately in
`ssccsorg/ssccs#120`.

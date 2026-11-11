# Open fronts

What the store already shows, what a workstation can still measure or fix, and
what needs infrastructure. The split is deliberate: a claim is never written
where a measurement belongs, and a measurement on ten cores is never written as
if it were the production number.

## Established

| Claim | Evidence |
| :--- | :--- |
| O(1) addressing, unchanged APIs, byte-exact reads | the tagma gtests |
| The read path beats the compressed baseline | `tagma_block_read.C`: compressed store 12.4 s, 572 MB/s, plain 14.6 s, against 177.4 s |
| Compression keeps the read path and shrinks the file | 7,082,290,985 to 2,897,372,833 bytes, 2.44x |
| A read serves only the collections a selection enables | the scalar-only harness row, 4.2 s on the plain store |
| A columnar store reads through the same leaves | the columnar io and tree gtests |
| The whole state reads in one or two requests whatever the order | `tagma_scatter.C` entry rows: 0.101 s scattered against 0.109 s sequential over 2,000 events, the fields delivered, against the baseline's 202.2 s |
| A coordinate resolves a dataset without a scan | `tagma_dataset_bench.C`: a run resolves to its shard while the chain scans the run branch, 11.1 s over 38 files |

### Scattered, event-selected reads

The regime the work was aimed at, and the one the earlier rows missed: reads in
list order rather than a scan. Measured with `tagma_scatter.C`, 2,000 events in
sequential and shuffled order on the same disk, the store row copying the record
from the mapping or decompressing the block it falls in, the baseline driving
`TTree::GetEntry` with no branch addresses.

| order | row | seconds | requests | requests/event |
| :--- | :--- | ---: | ---: | ---: |
| sequential | store mapped | 0.010 | 2,000 | 1.00 |
| sequential | store block | 0.003 | 2,000 | 1.00 |
| sequential | baseline | 0.601 | 7 | 0.004 |
| scattered | store mapped | 0.000 | 2,000 | 1.00 |
| scattered | store block | 0.445 | 2,000 | 1.00 |
| scattered | baseline | 206.139 | 1,365,153 | 682.6 |
| scattered | baseline+cache | 199.311 | 16 | 0.008 |

The entry rows settle it at the layer the baseline reads at. Reading the whole
event, the index record and the slice with the 1,380 field branches delivered,
costs 0.101 s scattered against 0.109 s sequential over 2,000 events, so the
store's cost does not move with the order. Against the baseline's 202.2 s over
the same events that is 2,000 times, payload for payload and delivery for
delivery, or 345 times for the compressed store, whose read decompresses the
8 KB block a record falls in. The win is therefore not in the scan, where the
store is 12 to 14 times ahead on the read path, but in event-selected access,
where it is three orders of magnitude at a measured two reads per event.

The cache is not the answer under scatter. A 64 MB `TTreeCache` per file
collapses the read calls to 16 and leaves the time where it was, 204.0 s against
202.2, so the scattered cost is the traversal and the decode of every event, not
the reads, and a cache cannot hold the working set. The store removes that cost
by addressing, which is the thesis.

The penalty is a basket boundary, not a slope. The first basket of the measured
file covers entries 0 to 999, and a scattered list inside it reads like a scan, 6
requests at 500 and at 1,000 events, 0.076 to 0.093 s; a list that spans a second
basket collapses, 1,365,153 requests at 2,000 events, because a jump between
baskets invalidates the current basket of each of the 1,380 branches. The
store's count is flat across that threshold. Two caveats remain: the baseline
has no branch addresses, so it delivers nothing into analysis variables, and the
entry rows are the fair comparison; and the count is 2,000 because the scattered
baseline does not finish more on a workstation.

The compressed store paid for scatter because a record read decompressed the
whole block it fell in, the block size over the record size, 205 for the default
256 KB against 1,280 bytes. The block size is now a parameter of `Compress` and
the store's descriptor records it, so a reader needs no setting and
`tagma_compress.C` writes a store at a chosen size. At 8 KB the scattered read
falls to 16 microseconds per event against 222, and the file only grows from
2.897 to 2.990 GB, 2.44 to 2.37 times, while the scan gives up a little, 1.5 to
4 microseconds per event. Small blocks are therefore nearly free in ratio and
much better under scatter.

### The dataset level

Step two of ssccs #121: whether a coordinate addresses a dataset rather than a
file. `tagma_shard.C` splits the collection store into one shard per run, each
shard declaring its own lumi and event axes, `tagma_root_shard.C` splits the
ROOT tree into one file per run, and `tagma_dataset_bench.C` holds the same
partition as stores, as a `TChain`, and as the manifest both sides share.
Measured on the M1 file, 38 runs, the largest run 281,515 events, sources
warmed:

| subject | operation | seconds | note |
| :--- | :--- | ---: | :--- |
| store | attach | 0.687 | 18.1 ms per file, descriptor read, parse, map |
| chain | attach | 0.002 | 0.05 ms per file, names only |
| chain | headers forced | 0.355 | 9.3 ms per file, the cost the store's attach pays up front |
| chain | select by run | 11.1 | no run index, the range comes from a scan of the run branch |
| manifest | read | 10.3 | one file, all branches, no branch addresses, 36 microseconds per event |
| store | read path | 1.6 | 942,333,903 bytes at 600 MB/s, no delivery |
| store | entry layer | 29.0 | 78.9 microseconds per grid cell over 368,008 cells, 54.5 over the first 2,000 |

The coordinate resolves a run to its shard with no scan, which is the dataset
level claim. Two costs on the store side are recorded rather than argued. The
descriptor is parsed per file and repeats the field table the shards share:
18.1 ms per file against the chain's forced 9.3 ms. The dense grid the partition
lays out carries 2,862,780 cells for 2,315,223 events, 1.24 times, and the
largest run 368,008 for 281,515, 1.31 times; the cells the events do not occupy,
the padding, are 19.1 percent of the grid, and the walk pays them as well as
the bytes. The entry layer is 2.8 times the chain's per-event read over the same
run, and its per-cell cost rises with the scale of the walk: 54.5 microseconds
over the first 2,000 cells against 78.9 over 368,008.

The dataset's run and lumi axes are slots while the record carries the physical
run, luminosity block, and event number. Over the physical luminosity block
values a dense axis would carry 14,710,139 cells, 84.3 percent of them padding,
5.14 times the grid the slot axes lay out; the alternative is a block table of
2,348 entries, so where the slot-to-physical mapping lives is open.

## Measurable or fixable here

### 1. Thread scaling at workstation width

Measured with `tagma_mt.C`, which gives every worker its own file object and a
disjoint range: the store row reads the whole payload through the byte source,
the baseline rows drive `TTree::GetEntry` over the same events with no branch
addresses, so all rows move the data with no field copy, on the same disk. The
cached row sizes a 32 MB `TTreeCache` per worker, the configuration a production
job uses.

| threads | store_s | store_MB/s | base_s | base+cache_s | base/store |
| :--- | ---: | ---: | ---: | ---: | ---: |
| 1 | 13.04 | 543 | 186.4 | 183.8 | 14.3 |
| 2 | 9.86 | 718 | 93.7 | 92.7 | 9.5 |
| 4 | 5.32 | 1331 | 50.3 | 53.7 | 9.4 |
| 8 | 2.86 | 2481 | 31.9 | 32.2 | 11.2 |

Both scale, and the store scales 4.6 times over eight threads against the
baseline's 5.8, so the store's lead narrows from 14.3 to 11.2 times rather than
widening. That is the expected shape: the store is bandwidth bound, 543 to
2481 MB/s being sublinear on eight cores, while the cache-less baseline is I/O
bound and parallelizes well.

The cache neither helps nor hurts, 183.8 s against 186.4 at one thread and
32.2 against 31.9 at eight, because each worker reads a contiguous range and
the cache is redundant under sequential access. The concern that threads
degrade a shared cache is therefore not reproduced here; a shared cache or a
scattered, remote pattern would be needed to test it, and that pairing is open.
The assumption that more threads would widen the store's lead is not supported
either way. This machine has ten cores, so the row ends at eight and O(128) is
an extrapolation. The 128-core row needs a larger system and is not measured,
but it is predicted and not guessed: the per-event request count is
media-independent and settled at the scale measured, one or two for the store
whatever the order against hundreds for the baseline once its entries span a
basket, so the tendency holds at 128 cores and only the wall-time magnitude
needs the machine.

### 2. A column-major physical layout

Today a read decodes the blocks of the selected collections, and within a
collection the whole slice, so the store is column independent: its read path
crosses the RDataFrame ramp near 42 scalar columns and its whole-event row,
delivery included, near 810, so upstream wins on any selection that reads a
small fraction of the event, below those crossovers. The
layout is event major, one index record per event and its packed slice after
it, so a block of the data region holds many events of one collection range and
the selection cannot shrink it.

The structural fix is to lay the data region out by column, so the events of one
field are contiguous and a selection decodes only the columns it reads. That is
a new store layout: the descriptor carries the layout kind and the block length,
and the address arithmetic, the writer, and the reader follow it. The field copy
did not have to change. This is the largest open front, and the arithmetic below
is what decides its shape.

The transposition is per block, not per store. A store-wide transposition needs
every field's total before the first byte is written, which the single-pass
writer cannot know, so a block of B events is buffered, transposed, and
appended. B is the one free parameter.

Addressing one event's field needs two things the event-major layout does not:
where the field's run starts, which is the block's data base plus the runs of the
fields before it, and where the event sits inside that run, which is the prefix
of that collection's counts over the earlier events of the block. Both follow
from the block's object counts, and reading those counts for one event means
reading the block's index region, B records. A block table therefore carries the
run start per field and the prefix count per collection per event,
`8F + 4CB` bytes per block for F collection fields and C collections, which is
`8F/B + 4C` bytes per event.

For a schema of 150 collection fields and 3 collections, a block of 64 events
costs 30.8 bytes per event, 71 MB over the 2,314,223-event fixture, 2.4 percent
of the 2.99 GB store; at 1,024 events the table falls to 13.2 bytes per event,
31 MB. Those field counts are an illustration, not a measurement of the
fixture's schema.

The block length trades against the row that wins today. `tagma_column_probe.C`
measured the data region of a whole-event read at 1.83 times the event-major
order at 64 events per block and 5.96 times at 1,024, against a read path that is
10 to 12 percent of the 120-second whole-event row, so it predicted the row would
move by 1.09 to 1.11 times at 64 and 1.50 to 1.60 at 1,024. The prediction is
about the data region alone; the reader that now exists adds a copy the probe did
not model, and its measured row moves 1.18 times at 64, flat in the block length.

On the probe's own terms the order is not monotonic: 16 events costs 2.29 times
where 64 costs 1.83, and 256 costs 9.67 where 1,024 costs 5.96. The block length
is therefore a measured point rather than a trend.

What the table buys is the scatter claim, the store's strongest. A single-event
read needs the block's table entry, 1,968 bytes at these values, and the field's
run: two reads and about the bytes the event's slice costs today. Without the
table the same read would need the block's index region, 164 KB at 64 events, to
find the prefix. The table is what keeps one to two requests per event under a
scattered order, and that count assumes the reader takes the field's own bytes.
The reader as it stands takes the block's, and the measured row below is what
that costs until field-level narrowing lands.

The reader exists. When the loaded entry falls in another block, the tree reads
that block's table entry and its data bytes once, reconstructs each event of the
block into the buffer the field copy already reads, and keeps them until another
block is asked for. `CopyTagmaField` does not change, and a scan pays the block
reads once per block rather than once per event. `ReadStore` accepts the kind and
derives the table size from the block division, so a store's own descriptor
reaches the read gate; the block source, which walks the event-major region
directly, still refuses it.

That the reader reconstructs what the writer transposed is pinned at two layers:
the io test rebuilds every event of a two-block fixture through the table, and
the tree gate reads a columnar store through the leaves and pins that the events
of a loaded block cost one read each.

The cost is measured on a real run, `run_27_279966`, 7,277 events carrying the
schema of the whole-file store: 974 scalars, 406 array fields in 19 collections,
an index record of 1,280 bytes, a data region of 14,368,199 bytes.
`tagma_make_store.C` writes the same source twice at a chosen block length, so
the rows differ only in the layout, and the whole-event row is the harness's
entry layer with every branch enabled, warmed.

| layout | wall s | reads per event | bytes moved | store bytes |
| :--- | ---: | ---: | ---: | ---: |
| event-major | 0.365 | 2.00 | 23,682,759 | 23,731,774 |
| block 16 | 0.433 | 1.19 | 26,294,999 | 25,762,894 |
| block 64 | 0.431 | 1.05 | 24,752,167 | 24,656,542 |
| block 256 | 0.433 | 1.01 | 24,377,015 | 24,390,190 |
| block 1,024 | 0.447 | 1.00 | 24,340,295 | 24,380,350 |

The block halves the requests of a scan and costs a sixth of the wall time. The
requests per event fall from 2.00 to 1.05 at 64 events and the store grows by
3.9 percent, which is the block table, but the row moves 1.18 times against the
1.09 to 1.11 the probe predicted. Two reasons. The reconstruction copies every
field of the block into the slice buffer and the field copy then copies it out
again, so an event of 406 fields pays 812 short copies where the event-major
layout pays one span copy, and that extra copy alone is about 9 microseconds per
event. And the block length barely matters to a scan: past 64 events the table
shrinks and the requests fall to one, while the wall time stays between 1.18 and
1.22 times, because the copies and not the reads are the cost.

The scatter claim does not survive the layout. On the same shard, 2,000 events
in list order, entry layer:

| layout | sequential s | reads | scattered s | reads |
| :--- | ---: | ---: | ---: | ---: |
| event-major | 0.101 | 4,000 | 0.102 | 4,000 |
| block 16 | 0.130 | 2,375 | 0.129 | 7,943 |
| block 64 | 0.121 | 2,096 | 0.132 | 7,796 |

A scattered order changes block almost every event, since 2,000 events over 114
blocks rarely land two in a row in the same one, so each event loads a block's
table entry and its whole data span. The requests per event rise from 2.00 to
3.90 and the bytes read per event from one slice to sixty-four of them. The wall
time rises by only 29 percent because the shard is small enough for the page
cache to hold it; over a store that does not fit, that span is the read.

So the layout buys the narrow selection this front exists for and pays for it
with a scattered read of a whole block per event, which the event-major order
does not. Field-level narrowing, reading only the runs of the enabled fields and
only the event's share of each run, is what would remove both costs at once: a
narrow scattered read would then move the field's bytes rather than the block's,
and it is the change that makes the crossover of this front measurable. Until
then the two layouts answer different regimes, and the store keeps both.

The block length does not move a scan past 64 events, so the smallest length
that reaches one request per event is enough, and the smaller lengths are what a
scattered order wants, since its cost is the block's bytes. Ship beside or
replace is settled for now: a store chooses by workload and the descriptor
carries the kind. What stays open is the crossing point, which needs the
field-level narrowing above, and the same rows on a store the page cache cannot
hold.

### 3. The delivery layer

The whole-event row costs about 50 microseconds per event in the branch
machinery, against 12.4 s of read path in a 120 s row on the plain store. The
read path is won; the delivery is not. Fewer fields copied and a cheaper copy
per field are straight wins, and they matter for an analysis that reads most of
the event. The figure is a single-file figure: over a shard at dataset scale
the same layer costs 2.8 times the chain's per-event read, and its per-cell
cost rises with the walk's scale, 54.5 microseconds over the first 2,000 cells
against 78.9 over 368,008; front 5 is what would settle that rise.

### 4. The claims

The forum thread still carries the projection numbers, the `Legacy` label, and
the read rows that were paired across layers. The corrected read-path result,
the compressed store, the crossover, and the dataset level belong in it, and it
should hear them: the compressed comparison that was asked for favours the
store on the read path, and the honest scope is the read path, wide selections,
and event-selected access.

### 5. The walk's per-cell cost

The entry layer over one shard costs 54.5 microseconds per grid cell over the
first 2,000 cells and 78.9 over 368,008, same shard, same warmed state, and the
rise is not attributed. It is what gives the single-file 50 microsecond
delivery figure its scale condition.

## Needs infrastructure

### 1. O(128)

A many-core node, a sponsor cluster, or a rented NVMe instance. The O(10)
measurement gives the trend as a deterministic prediction: the request count per
event is media-independent, one for the store against hundreds for the baseline,
and it is settled at the scale measured, so the tendency holds at 128 cores and
only the wall-time magnitude needs the machine.

### 2. Multi-TB

Local NVMe or EOS access. The workstation holds the 2.15 GB Open Data file and
a 10.86 GB uncompressed control, so multi-GB is covered and multi-TB is not.

### 3. The remote regime end to end

EOS or XRootD, where the per-request cost the work is aimed at actually shows.
The store gives one or two requests per event with no cache to thrash, and the
compressed form keeps the bytes in check, but none of that is measured off a
local disk yet.

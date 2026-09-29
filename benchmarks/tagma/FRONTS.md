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

The store resolves every event in one request whatever the order, while the
baseline pays 683 requests per event when the order is scattered against 0.004
when it is a scan, which is the production signature the work started from. The
win is therefore not in the scan, where the store is 11 to 14 times ahead, but in
event-selected access, where it is about five orders of magnitude at a fixed one
request.

The cache is not the answer under scatter. A 64 MB `TTreeCache` per file
collapses the read calls to 16 and leaves the time where it was, 199.3 s against
206.1, so the scattered cost is the traversal and the decode of every event, not
the reads, and a cache cannot hold the working set. The store removes that cost
by addressing, which is the thesis.

Three caveats: the store row carries the index record and not the collections'
slices, which are read per selection; the baseline has no branch addresses; and
the count is 2,000 because the scattered baseline does not finish more on a
workstation. The scattered baseline is superlinear besides: 0.210 s over 500
events against 206 s over 2,000, a cliff, while the store stays at one request
and linear in the events, which is the O(1) addressing telling at scale.

The compressed store paid for scatter because a record read decompressed the
whole block it fell in, the block size over the record size, 205 for the default
256 KB against 1,280 bytes. The block size is now a parameter of `Compress` and
the store's descriptor records it, so a reader needs no setting and
`tagma_compress.C` writes a store at a chosen size. At 8 KB the scattered read
falls to 16 microseconds per event against 222, and the file only grows from
2.897 to 2.990 GB, 2.44 to 2.37 times, while the scan gives up a little, 1.5 to
4 microseconds per event. Small blocks are therefore nearly free in ratio and
much better under scatter.

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
media-independent and settled at the scale measured, one for the store whatever
the order against hundreds for the baseline, and the baseline grows
superlinearly with the events read while the store stays flat, so the tendency
holds at 128 cores and only the wall-time magnitude needs the machine.

### 2. A column-major physical layout

Today a read decodes the blocks of the selected collections, and within a
collection the whole slice, so the store is column independent: above the
crossover (about 42 scalar columns) it wins, below it upstream leads. The
layout is event major, one index record per event and its packed slice after
it, so a block of the data region holds many events of one collection range and
the selection cannot shrink it.

The structural fix is to lay the data region out by column, so the events of one
field are contiguous and a block holds one field, and a selection decodes only
the columns it reads. That is a new store layout, not a flag: `TTagmaSchema`
gains a region per field, `TTagmaStore` addresses a (field, event) offset
instead of an event's chunk, `TTagmaWriter` writes the transposed region, and
the field copy reads across the field's region. The addressing stays closed
form, which is what makes the transposition affordable, and the gate is that
the same values read back and that a two-column read touches only those
columns' blocks. This is the largest open front.

### 3. The delivery layer

The whole-event row costs about 50 microseconds per event in the branch
machinery, against 12.4 s of read path in a 120 s row on the plain store. The
read path is won; the delivery is not. Fewer fields copied and a cheaper copy
per field are straight wins, and they matter for an analysis that reads most of
the event.

### 4. The claims

The report and the forum thread still carry the projection numbers, the
`Legacy` label, and the read rows that were paired across layers. The corrected
read-path result, the compressed store, and the crossover belong in both, and
the forum thread should hear them: the compressed comparison that was asked for
favours the store on the read path, and the honest scope is the read path, wide
selections, and event-selected access.

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

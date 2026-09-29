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
an extrapolation.

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
measurement gives the trend; the production number needs the machine.

### 2. Multi-TB

Local NVMe or EOS access. The workstation holds the 2.15 GB Open Data file and
a 10.86 GB uncompressed control, so multi-GB is covered and multi-TB is not.

### 3. The remote regime end to end

EOS or XRootD, where the per-request cost the work is aimed at actually shows.
The store gives one or two requests per event with no cache to thrash, and the
compressed form keeps the bytes in check, but none of that is measured off a
local disk yet.

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
the baseline row drives `TTree::GetEntry` over the same events with no branch
addresses, so both move all the data with no field copy, on the same disk.

| threads | store_s | store_MB/s | baseline_s | baseline/store |
| :--- | ---: | ---: | ---: | ---: |
| 1 | 12.40 | 571 | 176.55 | 14.24 |
| 2 | 9.50 | 745 | 90.99 | 9.57 |
| 4 | 5.15 | 1377 | 49.14 | 9.55 |
| 8 | 2.71 | 2613 | 28.38 | 10.47 |

Both scale, and the store scales 4.6 times over eight threads against the
baseline's 6.2, so the store's lead narrows from 14.2 to 10.5 times rather than
widening. That is the expected shape: the store is bandwidth bound, 571 to
2613 MB/s being sublinear on eight cores, while the cache-less baseline is I/O
bound and parallelizes well. The assumption that more threads would widen the
store's lead is not supported against a cache-less baseline. The report's
concern is about a *cached* baseline, where threads contend on a shared cache,
and that pairing is still open. This machine has ten cores, so the row ends at
eight and O(128) is an extrapolation.

### 2. A column-major physical layout

Today a read decodes the blocks of the selected collections, and within a
collection the whole slice, so the store is column independent: above the
crossover (about 42 scalar columns) it wins, below it upstream leads. Blocks
laid out by column, so a selection decodes only the columns it reads, move the
crossover down. This is the structural front, and it is large.

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

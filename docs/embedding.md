# Embedding and retrieval

How the app turns what you wrote into something it can search, and why each
choice is what it is. Nothing here is built yet; this is the decision record so
it does not have to be argued twice.

Some of the numbers below were measured on other work and are reproduced here
with the method that produced them, so they can be re-run rather than taken on
trust. Anything derived rather than measured says so.

## The model

**Qwen3-Embedding-0.6B**, GGUF, through the llama.cpp lane the app already has.
Output width **1024**.

Three conventions are specific to this model and getting any of them wrong
collapses the scores without failing:

- **Pool the LAST token**, not the mean. The model is trained to put the summary
  there.
- **Prefix the query**, not the document: `Instruct: <task>\nQuery: <text>`. A
  document is embedded bare.
- **L2-normalize** the pooled vector. llama.cpp does not do it for you, and
  every score downstream assumes it — once normalized, cosine similarity *is*
  the dot product, so the search never computes a magnitude.

## Quantization: Q8_0 on the phone

| | paraphrase | unrelated | crosslingual | file |
|---|---|---|---|---|
| reference | 0.959 | 0.139 | 0.888 | — |
| f16 | 0.9592 | 0.1393 | 0.8881 | 1.14 GB |
| Q8_0 | 0.9580 | **0.1328** | 0.8877 | 610 MB |

Those are measured, not assumed. The method is worth keeping because it is
cheap to repeat: load each GGUF, embed a fixed handful of sentence pairs — a
paraphrase, an unrelated pair, a crosslingual pair — and compare the cosines
against a reference implementation of the same model. Three numbers, no
corpus, no index, runs anywhere.

**A resident server process should stay on f16.** Saving 590 MB buys nothing
when the process is up anyway, and an index built at bf16/float32 is closest to
an unquantized query side.

**On a phone the same numbers point the other way, because the constraint is not
memory.** Read bandwidth on a recent Kirin-class phone measures around
19.5 GB/s, and a single core already saturates it — the same figure that rules
out the GPU and the NPU for on-device decode. A forward pass reads every weight
once, so the file size sets a floor on how long one embedding can possibly
take:

| file | bytes to stream | floor per call |
|---|---|---|
| f16 · 1.14 GB | 1.14 GB | ~58 ms |
| Q8_0 · 610 MB | 610 MB | ~31 ms |

That floor is **derived from the bandwidth figure, not measured**, and it is a
floor rather than a prediction: it is tightest for short inputs, where there are
few tokens of compute to hide the read behind. Longer inputs drift toward being
compute-bound instead.

So Q8_0 halves the wall-clock of every embedding, and no other compute unit
moves that wall. Memory is the smaller half of the argument — though it is not
nothing, since a 1.14 GB embedding model is about the size of the whole chat
model the same app wants resident.

**Embedding is bursty and the model should not stay loaded.** It runs when
something is recorded and when a question is asked, not continuously like a chat
model during a conversation. Load, embed, unload; then steady-state cost is
near zero and the question becomes load latency — which is the same bandwidth
wall, and Q8_0 still wins it by half.

### Verify before believing this

Three cosines measure **wiring, not ranking**. Going to int8 on the index side
has been observed to reorder a quarter of a top 10 while those same numbers
still looked fine — exactly the failure this kind of check cannot see. Q8_0's
drift above is on `unrelated`, and in the direction that costs something:
unrelated sentences pulled closer together, which is how noise gets into a
top-k.

So before shipping Q8_0, compare **actual top-k against an f16 run on real
queries**. That comparison runs on a desktop; there is no reason to wait for a
device to do it.

## Storage: a BLOB, and no vector extension

Vectors go in SQLite as a BLOB column, fp16, already normalized.

```sql
CREATE TABLE embeddings (
  entry_id INTEGER NOT NULL REFERENCES entries(id) ON DELETE CASCADE,
  model    TEXT    NOT NULL,   -- including the quantization
  dim      INTEGER NOT NULL,
  dtype    TEXT    NOT NULL,   -- 'f16'
  vec      BLOB    NOT NULL    -- dim * 2 bytes, L2-normalized
);
```

`model` carries the quantization and `dtype` is separate from `dim` because
neither is recoverable from the bytes: 1024-d f16 and 2048-d int8 are the same
length, and vectors from two quantizations of the same model are not strictly in
the same space. Mixing them is a slow, quiet kind of wrong.

**No Vec1, no sqlite-vec, and not because they are immature.** They exist to
make `ORDER BY vec_distance_cosine(...)` work *in SQL*. The similarity is
computed in C++ here, next to the day-log rules and shared by both platforms, so
SQLite is only being asked to hold bytes — which is a native storage class.

Exact search is also the right answer at this size. One person's own records top
out around 100k:

| rows | 1024-d fp16 | bytes to scan |
|---|---|---|
| 1,284 (the design's own figure) | 2.5 MB | sub-millisecond |
| 100,000 | 195 MB | ~16 ms at 12 GB/s |

An approximate index would trade exactness for a speedup that is not needed, and
IVFADC-style indexes want a trained codebook, which is meaningless over a
thousand vectors.

### Computing the similarity

- **Convert to fp32 and accumulate in fp32.** Storage precision and arithmetic
  precision are different questions: fp16 has a 10-bit mantissa, and summing
  1024 products in it moves rankings.
- **Watch the conversion cost.** 100k × 1024 is 102M fp16→fp32 conversions per
  query. On ARMv8 that is one NEON instruction per four values, and ARMv8.2
  adds fused fp16 paths; a scalar loop would spend more on converting than fp16
  saved on reading.
- `sqlite3_column_blob` does not promise alignment. ARM64 tolerates unaligned
  scalar loads; a NEON path may want a copy into an aligned buffer first.

### If it ever gets too big, truncate before quantizing

Qwen3-Embedding is trained with Matryoshka representation learning, so the first
`n` dimensions of a vector are a valid narrower vector — keep the prefix,
renormalize, and a 1024-d query can search a 512-d index. Renormalizing is not
optional: the prefix of a unit vector is not a unit vector.

That makes dimension the first lever, not dtype:

| | bytes per row |
|---|---|
| 1024-d fp16 | 2,048 |
| 512-d fp16 | 1,024 |
| 1024-d int8 | 1,024 |

The last two are the same size, but truncation is a capability the model was
trained for and int8 is lossy compression applied afterward. At equal cost,
prefer the one the model was built to support.

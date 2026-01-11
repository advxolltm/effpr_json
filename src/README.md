# JSON → CSV Transformer

## What the Program Does

The program reads a JSON file containing either:

- a single JSON object, or
- an array of JSON objects

and writes a CSV table to **standard output**.

Each JSON object becomes one CSV row.  
All keys that appear anywhere in the input become CSV columns.

---

## Example

### Input (JSON)

```json
[
  {
    "event_id": 10,
    "user": { "id": 123, "country": "AT" },
    "event": { "type": "click", "duration_ms": 321 },
    "tags": ["ui", "mobile"],
    "success": true
  }
]
```

### Output (CSV)

```
event_id,user.id,user.country,event.type,event.duration_ms,tags,success
10,123,AT,click,321,ui;mobile,true
```

---

## Benchmark Dataset

he project includes a generator that produces realistic web-event data.

Each generated event looks like:

```
{
  "event_id": 91238123,
  "timestamp": "2026-01-11T13:45:22Z",
  "user": {
    "id": 18372,
    "country": "AT",
    "device": "mobile"
  },
  "event": {
    "type": "click",
    "page": "/checkout",
    "duration_ms": 183
  },
  "tags": ["ui", "conversion", "mobile"],
  "success": true
}

```

## How to Build

```
gcc -std=c11 -O0 -Wall -Wextra -Werror json2csv_baseline.c -o json2csv
./json2csv benchmark.json > out.csv
```

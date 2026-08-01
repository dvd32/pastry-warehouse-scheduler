# Pastry Shop Order Management System

A discrete-time simulator for an industrial pastry shop, written in C. It handles recipe management, ingredient stock with expiration tracking, order fulfillment, and courier pickups. All built from scratch on top of hash tables, heaps, and linked lists (standard library only, no external dependencies).

Built for the "Algoritmi e Principi dell'Informatica" final project at Politecnico di Milano (A.Y. 2023/24). The submission scored **30/30**, evaluated by an automated test suite checking both correctness and runtime/memory efficiency under strict limits (down to ~4s / 15 MiB).

## Why this project

The interesting part isn't the pastry shop story — it's that every operation had to stay efficient under tight, enforced time and memory budgets, which forces real data-structure decisions instead of "whatever works":

- **O(1) average lookups** for recipes and ingredient stock via custom open hashing with two independent hash functions (a second hash disambiguates bucket collisions without falling back to string comparison).
- **Expiration-aware stock**: each ingredient's batches live in a min-heap keyed by expiration date, so "always consume what expires soonest" and "drop everything already expired" are both cheap, heap-top operations instead of linear scans.
- **Weight-based courier loading**: orders ready for pickup are staged in a FIFO queue, then poured into a max-heap (weight, tie-broken by arrival time) only at pickup time, to produce the manifest in the required order without a full sort.
- **Correct handling of partial availability**: an order that can't be fully prepared waits in a separate FIFO queue and is automatically retried, in arrival order, on every future restock.

## Problem summary

The shop operates at discrete time instants, starting at t = 0. Each command read from input advances time by one step. Four commands are supported:

| Command | Effect | Response |
|---|---|---|
| `aggiungi_ricetta <name> <ingredient> <qty> ...` | Adds a recipe (arbitrary number of ingredients) | `aggiunta` / `ignorato` |
| `rimuovi_ricetta <name>` | Removes a recipe, unless orders for it are still outstanding | `rimossa` / `ordini in sospeso` / `non presente` |
| `rifornimento <ingredient> <qty> <expiry> ...` | Restocks the warehouse with new batches | `rifornito` |
| `ordine <recipe> <count>` | Places an order | `accettato` / `rifiutato` |

On top of these, every time the simulation clock hits a multiple of the configured courier period, the truck's manifest is printed before the next command is processed — either a list of `<order_time> <recipe> <quantity>` lines, or `camioncino vuoto` if nothing is ready.

The full specification is in [`docs/Project_Requirements.pdf`](docs/Project_Requirements.pdf).

## Repository layout

```
.
├── src/
│   └── api.c                    # full implementation
├── docs/
│   ├── Project_Requirements.pdf  # original assignment specification
│   ├── Project_slides.pdf        # project presentation slides
│   └── Tools_slides.pdf          # dev tools & profiling walkthrough (gdb, valgrind, ASan)
├── Makefile
└── README.md

## Building and running

```bash
make              # builds ./pasticceria with the grader's exact flags
./pasticceria < input.txt > output.txt
```

The grader compiles with `gcc -Wall -Werror -std=gnu11 -O2 -lm`, which the Makefile mirrors. A `make debug` target is also available, adding `-g3 -fsanitize=address` for local debugging.

## Example

```
$ echo "5 325
aggiungi_ricetta torta farina 50 uova 10 zucchero 20
rifornimento farina 100 10 uova 100 10 zucchero 100 10
ordine torta 2" | ./pasticceria

aggiunta
rifornito
accettato
camioncino vuoto
```

## Notes

This was an individually-graded university assignment; the source in this repository is my own submitted solution, kept as-is (only comments were added/cleaned up afterwards for readability).

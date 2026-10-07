from collections import Counter
from pathlib import Path
from dataset_reader import read_msmarco
from tokenizer import tokenize
import time

PROJECT_ROOT = Path(__file__).resolve().parent.parent

DATASET_PATH = PROJECT_ROOT / "collection.tar.gz"
RUNS_DIR = PROJECT_ROOT / "runs"
OUTPUT_DIR = PROJECT_ROOT / "output"


# Benchmark settings
MAX_DOCS = None

# Flush once roughly this many postings are in memory
MAX_POSTINGS_PER_RUN = 1000_000

# Print progress every N documents
PROGRESS_INTERVAL = 100_000


def write_run(
    postings: list[tuple[str, int, int]],
    run_number: int
) -> int:

    postings.sort(key=lambda x: (x[0], x[1]))

    run_path = RUNS_DIR / f"run_{run_number:03d}.dat"

    with run_path.open("w", encoding="utf-8") as f:
        for term, doc_id, tf in postings:
            f.write(f"{term}\t{doc_id}\t{tf}\n")

    num_postings = len(postings)

    print(
        f"Wrote {num_postings:,} postings "
        f"to {run_path.name}"
    )

    return num_postings


def remove_old_runs() -> None:
    for run_file in RUNS_DIR.glob("run_*.dat"):
        run_file.unlink()


def main() -> None:

    RUNS_DIR.mkdir(exist_ok=True)
    OUTPUT_DIR.mkdir(exist_ok=True)

    # Prevent old run files from mixing with new ones
    remove_old_runs()

    postings = []

    run_number = 0
    num_docs = 0
    total_tokens = 0
    total_postings = 0

    docs_path = OUTPUT_DIR / "docs.tsv"
    stats_path = OUTPUT_DIR / "stats.txt"

    start_time = time.perf_counter()

    with docs_path.open("w", encoding="utf-8") as docs_file:

        for doc_id, (pid, text) in enumerate(
            read_msmarco(DATASET_PATH)
        ):

            if MAX_DOCS is not None and doc_id >= MAX_DOCS:
                break


            # TOKENIZATION
            tokens = tokenize(text)
            document_length = len(tokens)

            num_docs += 1
            total_tokens += document_length


            # DOCUMENT TABLE
            docs_file.write(
                f"{doc_id}\t{pid}\t{document_length}\n"
            )


            # TERM FREQUENCIES
            term_freqs = Counter(tokens)


            # POSTINGS
            for term, tf in term_freqs.items():
                postings.append(
                    (term, doc_id, tf)
                )


            # FLUSH RUN
            if len(postings) >= MAX_POSTINGS_PER_RUN:

                total_postings += write_run(
                    postings,
                    run_number
                )

                postings.clear()
                run_number += 1


            # PROGRESS
            if num_docs % PROGRESS_INTERVAL == 0:

                elapsed = time.perf_counter() - start_time

                docs_per_second = (
                    num_docs / elapsed
                    if elapsed > 0
                    else 0
                )

                print(
                    f"Processed {num_docs:,} docs | "
                    f"{docs_per_second:,.0f} docs/sec | "
                    f"elapsed {elapsed:.2f}s | "
                    f"buffer {len(postings):,} postings"
                )

        # FINAL PARTIAL RUN
        if postings:

            total_postings += write_run(
                postings,
                run_number
            )

            postings.clear()
            run_number += 1


    # FINAL STATISTICS
    elapsed = time.perf_counter() - start_time

    avg_doc_length = (
        total_tokens / num_docs
        if num_docs > 0
        else 0
    )

    docs_per_second = (
        num_docs / elapsed
        if elapsed > 0
        else 0
    )

    with stats_path.open("w", encoding="utf-8") as stats_file:

        stats_file.write(
            f"num_docs={num_docs}\n"
        )

        stats_file.write(
            f"total_tokens={total_tokens}\n"
        )

        stats_file.write(
            f"avg_doc_length={avg_doc_length:.4f}\n"
        )

        stats_file.write(
            f"total_postings={total_postings}\n"
        )

        stats_file.write(
            f"num_runs={run_number}\n"
        )

        stats_file.write(
            f"elapsed_seconds={elapsed:.4f}\n"
        )

        stats_file.write(
            f"docs_per_second={docs_per_second:.4f}\n"
        )

    print()
    print("Finished.")
    print(f"Documents:       {num_docs:,}")
    print(f"Tokens:          {total_tokens:,}")
    print(f"Postings:        {total_postings:,}")
    print(f"Runs:            {run_number}")
    print(f"Elapsed:         {elapsed:.2f} seconds")
    print(f"Speed:           {docs_per_second:,.0f} docs/sec")
    print(f"Average length:  {avg_doc_length:.2f}")

    print()
    print(f"Document table:  {docs_path}")
    print(f"Statistics:      {stats_path}")


if __name__ == "__main__":
    main()


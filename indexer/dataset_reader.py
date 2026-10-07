import io
import tarfile
from pathlib import Path
from collections.abc import Iterator



def read_msmarco(
    archive_path: Path,
) -> Iterator[tuple[str, str]]:

    with tarfile.open(archive_path, "r:gz") as tar:

        member = tar.getmember("collection.tsv")
        binary_file = tar.extractfile(member)

        if binary_file is None:
            raise RuntimeError("Could not open collection.tsv")

        with io.TextIOWrapper(
            binary_file,
            encoding="utf-8",
            errors="replace",
        ) as f:

            for line in f:
                line = line.rstrip("\n")

                if not line:
                    continue

                pid, passage = line.split("\t", 1)

                yield pid, passage



if __name__ == "__main__":
    path = Path("collection.tar.gz")

    for i, (pid, passage) in enumerate(read_msmarco(path)):
        print(pid, passage)

        if i == 4:
            break



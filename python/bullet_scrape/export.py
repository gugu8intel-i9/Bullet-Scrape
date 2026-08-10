"""
High-performance multi-format export for scrape results.

Supported formats
-----------------
txt     Human-readable key: value blocks
json    Pretty JSON array
jsonl   One JSON object per line (streaming-friendly)
csv     RFC-style CSV (utf-8, excel-friendly)
parquet Columnar, Snappy/Zstd compressed — best for analytics / Colab

Parquet optimisation knobs (passed through to pyarrow):
  • compression: zstd (default) | snappy | gzip | lz4 | none
  • compression_level: 1–22 for zstd (default 3 — fast + small)
  • row_group_size: rows per row group (default 64_000)
  • use_dictionary: True (great for repeated strings like URLs/tags)
  • coerce_timestamps: "ms"
  • index: False (don't write the DataFrame index)

Examples
--------
>>> result.export("out.parquet")
>>> result.export("out.csv")
>>> result.export("out.txt", format="txt")
>>> result.export("data/", format="parquet", compression="zstd")
"""

from __future__ import annotations

import csv
import json
import os
import re
from pathlib import Path
from typing import Any, Dict, Iterable, List, Mapping, Optional, Sequence, Union


# ── Flatten helpers ──────────────────────────────────────────────────────────

def _scalarize(value: Any) -> Any:
    """Turn nested extract values into flat CSV/parquet-friendly scalars."""
    if value is None:
        return None
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float, str)):
        return value
    if isinstance(value, list):
        if len(value) == 0:
            return None
        if len(value) == 1:
            return _scalarize(value[0])
        # Multi-value: join strings, else JSON
        parts = [_scalarize(v) for v in value]
        if all(isinstance(p, (str, int, float, bool)) or p is None for p in parts):
            return "; ".join("" if p is None else str(p) for p in parts)
        return json.dumps(value, ensure_ascii=False)
    if isinstance(value, dict):
        return json.dumps(value, ensure_ascii=False)
    return str(value)


def flatten_records(
    records: Sequence[Mapping[str, Any]],
    *,
    columns: Optional[Sequence[str]] = None,
) -> tuple[List[str], List[Dict[str, Any]]]:
    """
    Flatten a list of record dicts.
    Returns (column_order, list_of_flat_dicts).
    """
    if columns is None:
        seen: List[str] = []
        for rec in records:
            for k in rec.keys():
                if k not in seen:
                    seen.append(k)
        columns = seen

    flat: List[Dict[str, Any]] = []
    for rec in records:
        row = {c: _scalarize(rec.get(c)) for c in columns}
        flat.append(row)
    return list(columns), flat


def _detect_format(path: Union[str, Path], fmt: Optional[str]) -> str:
    if fmt:
        f = fmt.lower().lstrip(".")
        aliases = {
            "text": "txt",
            "tsv": "csv",
            "pq": "parquet",
            "pqt": "parquet",
            "jsonlines": "jsonl",
            "ndjson": "jsonl",
        }
        return aliases.get(f, f)

    ext = Path(path).suffix.lower().lstrip(".")
    if not ext:
        raise ValueError(
            "Cannot detect format from path without extension; "
            "pass format='txt'|'json'|'jsonl'|'csv'|'parquet'"
        )
    return _detect_format(path, ext)


def _ensure_parent(path: Path) -> None:
    if path.parent and str(path.parent) not in ("", "."):
        path.parent.mkdir(parents=True, exist_ok=True)


# ── Writers ──────────────────────────────────────────────────────────────────

def write_json(path: Path, records: Sequence[Mapping[str, Any]], *, indent: int = 2) -> int:
    _ensure_parent(path)
    data = list(records)
    with path.open("w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=indent)
        f.write("\n")
    return path.stat().st_size


def write_jsonl(path: Path, records: Sequence[Mapping[str, Any]]) -> int:
    _ensure_parent(path)
    with path.open("w", encoding="utf-8") as f:
        for rec in records:
            f.write(json.dumps(rec, ensure_ascii=False, separators=(",", ":")))
            f.write("\n")
    return path.stat().st_size


def write_csv(
    path: Path,
    records: Sequence[Mapping[str, Any]],
    *,
    columns: Optional[Sequence[str]] = None,
    delimiter: str = ",",
) -> int:
    cols, flat = flatten_records(records, columns=columns)
    _ensure_parent(path)
    with path.open("w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(
            f,
            fieldnames=cols,
            delimiter=delimiter,
            quoting=csv.QUOTE_MINIMAL,
            extrasaction="ignore",
        )
        w.writeheader()
        for row in flat:
            w.writerow(row)
    return path.stat().st_size


def write_txt(
    path: Path,
    records: Sequence[Mapping[str, Any]],
    *,
    columns: Optional[Sequence[str]] = None,
) -> int:
    cols, flat = flatten_records(records, columns=columns)
    _ensure_parent(path)
    with path.open("w", encoding="utf-8") as f:
        for i, row in enumerate(flat, 1):
            f.write(f"--- record {i} ---\n")
            for c in cols:
                val = row.get(c)
                if val is None:
                    val = ""
                f.write(f"{c}: {val}\n")
            f.write("\n")
    return path.stat().st_size


def write_parquet(
    path: Path,
    records: Sequence[Mapping[str, Any]],
    *,
    columns: Optional[Sequence[str]] = None,
    compression: str = "zstd",
    compression_level: Optional[int] = 3,
    row_group_size: int = 64_000,
    use_dictionary: bool = True,
    write_statistics: bool = True,
    coerce_timestamps: str = "ms",
) -> int:
    """
    Write an analytics-optimised Parquet file via pandas + pyarrow.

    Defaults favour Colab / data-science workflows:
      - zstd level 3  → ~same speed as snappy, typically 20–40% smaller
      - dictionary encoding on for string columns
      - 64k row groups (good prune granularity without tiny-file overhead)
      - millisecond timestamps
    """
    try:
        import pandas as pd
    except ImportError as e:
        raise ImportError(
            "pandas is required for parquet export. "
            "Install with:  pip install pandas pyarrow"
        ) from e

    try:
        import pyarrow as pa  # noqa: F401
        import pyarrow.parquet as pq  # noqa: F401
    except ImportError as e:
        raise ImportError(
            "pyarrow is required for parquet export. "
            "Install with:  pip install pyarrow"
        ) from e

    cols, flat = flatten_records(records, columns=columns)
    df = pd.DataFrame(flat, columns=cols)

    # Coerce numeric-looking object columns (extract often yields numeric strings)
    for col in df.columns:
        s = df[col]
        if pd.api.types.is_object_dtype(s) or pd.api.types.is_string_dtype(s):
            converted = pd.to_numeric(s, errors="coerce")
            # Only keep coercion if most non-null values converted cleanly
            non_null = int(s.notna().sum())
            if non_null > 0 and int(converted.notna().sum()) >= max(1, int(0.9 * non_null)):
                df[col] = converted
        if pd.api.types.is_float_dtype(df[col]):
            df[col] = pd.to_numeric(df[col], downcast="float")
        elif pd.api.types.is_integer_dtype(df[col]):
            df[col] = pd.to_numeric(df[col], downcast="integer")

    _ensure_parent(path)

    # Build pyarrow Table for fine-grained write control
    table = pa.Table.from_pandas(df, preserve_index=False)

    # Dictionary-encode low/medium-cardinality string columns only
    if use_dictionary:
        new_fields = []
        arrays = []
        n_rows = table.num_rows or 1
        for i, field in enumerate(table.schema):
            col = table.column(i)
            is_str = pa.types.is_string(field.type) or pa.types.is_large_string(field.type)
            if is_str:
                try:
                    # Skip dict encoding when nearly all values are unique (URLs with ids, etc.)
                    uniq = col.unique().length()
                    if uniq <= max(32, int(0.5 * n_rows)):
                        dict_arr = col.dictionary_encode()
                        arrays.append(dict_arr)
                        new_fields.append(field.with_type(dict_arr.type))
                        continue
                except Exception:
                    pass
            arrays.append(col)
            new_fields.append(field)
        table = pa.Table.from_arrays(arrays, schema=pa.schema(new_fields))

    write_kwargs: Dict[str, Any] = {
        "compression": compression if compression != "none" else None,
        "use_dictionary": use_dictionary,
        "write_statistics": write_statistics,
        "coerce_timestamps": coerce_timestamps,
        "row_group_size": row_group_size,
        "version": "2.6",
        "data_page_size": 1 << 20,  # 1 MiB pages — good for zstd
    }
    if compression in ("zstd", "gzip", "brotli") and compression_level is not None:
        write_kwargs["compression_level"] = int(compression_level)

    pq.write_table(table, where=str(path), **write_kwargs)
    return path.stat().st_size


# ── Public entry point ───────────────────────────────────────────────────────

SUPPORTED_FORMATS = ("txt", "json", "jsonl", "csv", "parquet")


def export_records(
    records: Sequence[Mapping[str, Any]],
    path: Union[str, Path],
    *,
    format: Optional[str] = None,
    columns: Optional[Sequence[str]] = None,
    # parquet knobs
    compression: str = "zstd",
    compression_level: Optional[int] = 3,
    row_group_size: int = 64_000,
    use_dictionary: bool = True,
    # csv knobs
    delimiter: str = ",",
    # json knobs
    indent: int = 2,
) -> Dict[str, Any]:
    """
    Export records to path. Format is inferred from the extension if omitted.

    Returns a small dict: {path, format, bytes, records}.
    """
    path = Path(path)
    fmt = _detect_format(path, format)

    # If path is a directory, pick a default filename
    if path.exists() and path.is_dir():
        path = path / f"results.{fmt}"
    elif str(path).endswith(("/", os.sep)) or (not path.suffix and format):
        path.mkdir(parents=True, exist_ok=True)
        path = path / f"results.{fmt}"

    # Auto-append extension if missing
    if path.suffix == "" and fmt:
        path = path.with_suffix(f".{fmt}")

    if fmt == "json":
        nbytes = write_json(path, records, indent=indent)
    elif fmt == "jsonl":
        nbytes = write_jsonl(path, records)
    elif fmt == "csv":
        nbytes = write_csv(path, records, columns=columns, delimiter=delimiter)
    elif fmt == "txt":
        nbytes = write_txt(path, records, columns=columns)
    elif fmt == "parquet":
        nbytes = write_parquet(
            path,
            records,
            columns=columns,
            compression=compression,
            compression_level=compression_level,
            row_group_size=row_group_size,
            use_dictionary=use_dictionary,
        )
    else:
        raise ValueError(
            f"unsupported format {fmt!r}; choose one of {SUPPORTED_FORMATS}"
        )

    return {
        "path": str(path.resolve()),
        "format": fmt,
        "bytes": nbytes,
        "records": len(records),
    }

#!/usr/bin/env python3
"""Dump CAEN x742/DT5742 correction tables from board flash in CAEN-compatible text format.

This script reads the correction tables with CAEN_DGTZ_GetCorrectionTables() and writes
exactly the filename pattern expected by CAEN's offline correction routines:

  <basename>_gr0_cell.txt
  <basename>_gr0_nsample.txt
  <basename>_gr0_time.txt
  <basename>_gr1_cell.txt
  <basename>_gr1_nsample.txt
  <basename>_gr1_time.txt

The *content* is emitted to match the layout used by CAEN's SaveCorrectionTables(), so the
files can be loaded by the stock LoadCorrectionTable() routine from X742CorrectionRoutines.c
used by the `CAENDT5742RawEvent2StdEventConverter` 

Frequency defaults to 5 GHz, which is the only mode this helper targets.

The script depends on libCAENDigitizer library installed in the PC
"""

from __future__ import annotations

import argparse
import ctypes as ct
import json
from pathlib import Path
from typing import Iterable

DEFAULT_LIB = "/usr/lib/libCAENDigitizer.so"
CAEN_DGTZ_DRS4_5GHz = 0
MAX_X742_CHANNEL_SIZE = 9
CHUNK = 8


class BoardInfo(ct.Structure):
    _fields_ = [
        ("ModelName", ct.c_char * 12),
        ("Model", ct.c_uint32),
        ("Channels", ct.c_uint32),
        ("FormFactor", ct.c_uint32),
        ("FamilyCode", ct.c_uint32),
        ("ROC_FirmwareRel", ct.c_char * 20),
        ("AMC_FirmwareRel", ct.c_char * 40),
        ("SerialNumber", ct.c_uint32),
        ("MezzanineSerNum", (ct.c_char * 8) * 4),
        ("PCB_Revision", ct.c_uint32),
        ("ADC_NBits", ct.c_uint32),
        ("SAMCorrectionDataLoaded", ct.c_uint32),
        ("CommHandle", ct.c_int),
        ("VMEHandle", ct.c_int),
        ("License", ct.c_char * 17),
    ]


class DRS4CorrectionTable(ct.Structure):
    _fields_ = [
        ("cell", (ct.c_int16 * 1024) * MAX_X742_CHANNEL_SIZE),
        ("nsample", (ct.c_int8 * 1024) * MAX_X742_CHANNEL_SIZE),
        ("time", ct.c_float * 1024),
    ]


def check(code: int, where: str) -> None:
    if code != 0:
        raise RuntimeError(f"{where} failed with CAEN error code {code}")


def decode_cstr(buf: bytes) -> str:
    return buf.split(b"\x00", 1)[0].decode("utf-8", errors="replace")


def write_channel_table(path: Path, table: DRS4CorrectionTable, field_name: str) -> None:
    with path.open("w", encoding="utf-8", newline="\n") as f:
        matrix = getattr(table, field_name)
        for ch in range(MAX_X742_CHANNEL_SIZE):
            f.write(f"Calibration values from cell 0 to 1024 for channel {ch}:\n\n")
            for i in range(0, 1024, CHUNK):
                values = [int(matrix[ch][i + j]) for j in range(CHUNK)]
                f.write("".join(f"{value}\t" for value in values))
                f.write(f"cell = {i} to {i + CHUNK - 1}\n")


def write_time_table(path: Path, table: DRS4CorrectionTable) -> None:
    with path.open("w", encoding="utf-8", newline="\n") as f:
        f.write("Calibration values (ps) from cell 0 to 1024 :\n\n")
        for i in range(0, 1024, CHUNK):
            values = [float(table.time[i + j]) for j in range(CHUNK)]
            f.write("".join(f"{value:09.3f}\t" for value in values))
            f.write(f"cell = {i} to {i + CHUNK - 1}\n")


def compute_group_count(model_name: str, channels: int) -> int:
    # DT5742 is a 16-channel x742 board organized as two 8-channel groups.
    # Fall back to channels/8 for closely related x742 layouts.
    return max(1, channels // 8)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lib", default=DEFAULT_LIB, help="Path to libCAENDigitizer.so")
    parser.add_argument("--link-type", type=int, default=0, help="CAEN link type integer (default: 0 for USB)")
    parser.add_argument("--link-num", type=int, default=0, help="CAEN link number (default: 0)")
    parser.add_argument("--conet-node", type=int, default=0, help="CAEN CONET node (default: 0)")
    parser.add_argument(
        "--frequency-mhz",
        type=int,
        default=5000,
        choices=[5000, 2500],
        help="DRS4 sampling frequency in MHz (default: 5000)",
    )
    parser.add_argument("--output-dir", default="CorrectionTables", 
                        help="Directory where the tables will be written")
    parser.add_argument(
        "--basename",
        default=None,
        help="Optional basename. Default: dt5742_sn<SN>_5000MHz",
    )
    # XXX -- Probably not needed
    parser.add_argument(
        "--max-groups",
        type=int,
        default=2,
        help="Number of correction-table slots to allocate when calling the CAEN API (default: 2)",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()

    lib = ct.CDLL(args.lib)
    lib.CAEN_DGTZ_OpenDigitizer.restype = ct.c_int
    lib.CAEN_DGTZ_CloseDigitizer.restype = ct.c_int
    lib.CAEN_DGTZ_GetInfo.restype = ct.c_int
    lib.CAEN_DGTZ_GetCorrectionTables.restype = ct.c_int

    handle = ct.c_int()
    check(
        lib.CAEN_DGTZ_OpenDigitizer(
            ct.c_long(args.link_type),
            ct.c_int(args.link_num),
            ct.c_int(args.conet_node),
            ct.c_uint32(0),
            ct.byref(handle),
        ),
        "CAEN_DGTZ_OpenDigitizer",
    )

    try:
        info = BoardInfo()
        check(lib.CAEN_DGTZ_GetInfo(handle, ct.byref(info)), "CAEN_DGTZ_GetInfo")

        model = decode_cstr(bytes(info.ModelName))
        serial = int(info.SerialNumber)
        # n_channels = int(info.Channels)
        # XXX - Problem with the number of channels
        n_channels =  16
        n_groups = compute_group_count(model, n_channels)

        TableArray = DRS4CorrectionTable * max(args.max_groups, n_groups)
        tables = TableArray()
        check(
            lib.CAEN_DGTZ_GetCorrectionTables(
                handle,
                ct.c_int(CAEN_DGTZ_DRS4_5GHz),
                ct.byref(tables),
            ),
            "CAEN_DGTZ_GetCorrectionTables",
        )

        outdir = Path(args.output_dir).expanduser().resolve()
        outdir.mkdir(parents=True, exist_ok=True)

        basename = args.basename or f"dt5742_sn{serial}_{args.frequency_mhz}MHz"
        basepath = outdir / basename

        written: list[str] = []
        for group_id in range(n_groups):
            table = tables[group_id]
            group_base = Path(f"{basepath}_gr{group_id}")

            cell_path = group_base.with_name(group_base.name + "_cell.txt")
            nsample_path = group_base.with_name(group_base.name + "_nsample.txt")
            time_path = group_base.with_name(group_base.name + "_time.txt")

            write_channel_table(cell_path, table, "cell")
            write_channel_table(nsample_path, table, "nsample")
            write_time_table(time_path, table)

            written.extend([str(cell_path), str(nsample_path), str(time_path)])

        metadata = {
            "model": model,
            "serial_number": serial,
            "channels": n_channels,
            "groups": n_groups,
            "frequency_mhz": args.frequency_mhz,
            "link_type": args.link_type,
            "link_num": args.link_num,
            "conet_node": args.conet_node,
            "basepath_for_converter": str(basepath),
            "group_basepaths_for_loadcorrectiontable": [str(basepath) + f"_gr{g}" for g in range(n_groups)],
            "files": written,
        }
        metadata_path = outdir / f"{basename}_metadata.json"
        metadata_path.write_text(json.dumps(metadata, indent=2), encoding="utf-8")

        print(json.dumps(metadata, indent=2))

    finally:
        try:
            lib.CAEN_DGTZ_CloseDigitizer(handle)
        except Exception:
            pass


if __name__ == "__main__":
    main()

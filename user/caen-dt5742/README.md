# CAEN DT5742 module for EUDAQ

This module integrates a [CAEN DT5742](https://www.caen.it/products/dt5742) waveform digitizer into the **EUDAQ** framework.

![Picture of the DT5742 digitizer](https://caen.it/wp-content/uploads/2017/10/DT5742S_featured.jpg)

It provides:

- a **producer** that acquires raw events from the DT5742,
- a **raw-to-standard converter** that decodes the CAEN raw payload into `StandardEvent`s,
- an **offline x742 correction path** based on the CAEN `x742_DataCorrection` routines.
---

## Dependencies

The module expects:

- [CAENpy]([CAENpy](https://github.com/duartej/CAENpy)) for the Python producer,
- The CAEN libraries:
  - **CAENDigitizer**,
  - **CAENComm**,
  - **CAENVMELib**,
- a working **EUDAQ** build environment.

CAEN documents that, on Linux, CAENVMELib installs its shared libraries into `/usr/lib`. 
The x742 offline correction helpers (`ApplyDataCorrection`, `LoadCorrectionTable`, `GetNumEvents`, `GetEventPtr`, `X742_DecodeEvent`) 
are **not** part of the CAENDigitizer runtime library; CAEN ships them as source code in the `samples/x742_DataCorrection` area of 
the CAENDigitizer package, but they have been copied into `misc/x742_DataCorrection` folder. The module therefore builds and links
those routines explicitly. 


## Usage

### Init file

The following init parameter is used:

- `LinkNum` (`int`)
  - CAEN link number used to open the digitizer.

Optional but useful:

- `expected_serial_number`
  - If set, the producer checks that the connected board serial number matches the expected one and throws otherwise.

See also the example init file in `misc/CAENDT5742_example.ini`.

### Config file

The authoritative list of configuration parameters lives in the `CONFIGURE_PARAMS` dictionary inside `python/CAENDT5742Producer.py`.

Commonly used parameters are:

- `channels_mapping_file`
  - Path to the CSV file describing which DUT pixel is connected to each CAEN channel.
- `sampling_frequency_MHz`
  - DT5742 sampling frequency. The current converter expects **5000 MHz**.
- `max_num_events_BLT`
  - Maximum number of events per block transfer.
- `record_length`
  - Number of samples per waveform.
- `fast_trigger_threshold_ADCu`
  - Fast trigger threshold in ADC units.
- `post_trigger_size`
  - Equivalent to trigger delay / trigger position in the acquisition window.
- `trigger_polarity`
  - `'rising'` or `'falling'`.

See also the example config file in `misc/CAENDT5742_example.conf`.


## Repository layout

Relevant paths in this module:

- `python/CAENDT5742Producer.py`
- `module/src/CAENDT5742RawEvent2StdEventConverter.cc`
- `misc/CorrectionTables/`
- `misc/X742_DataCorrection/` (patched local copies of the CAEN x742 offline routines)

The correction tables are expected under:

```text
misc/CorrectionTables/
```

with basenames following the CAEN `LoadCorrectionTable(...)` convention:

```text
dt5742_sn<SERIAL>_5000MHz_gr0_cell.txt
dt5742_sn<SERIAL>_5000MHz_gr0_nsample.txt
dt5742_sn<SERIAL>_5000MHz_gr0_time.txt
dt5742_sn<SERIAL>_5000MHz_gr1_cell.txt
dt5742_sn<SERIAL>_5000MHz_gr1_nsample.txt
dt5742_sn<SERIAL>_5000MHz_gr1_time.txt
```

The converter uses the basename:

```text
misc/CorrectionTables/dt5742_sn<SERIAL>_5000MHz
```

and the CAEN `LoadCorrectionTable(...)` routine automatically appends the group and file suffixes.


## What the producer stores in the BORE

The producer writes the acquisition metadata needed by the converter into the BORE. In the current implementation this includes at least:

- `sampling_frequency_MHz`
- `n_samples_per_waveform`
- channel / DUT naming and geometry information
- channel DC offset information
- `post_trigger_size`
- optionally `serial_number`

These tags are used by the converter to reconstruct timing, geometry and voltage scaling.


## Converter workflow

The converter works on the **raw binary event block** produced by the producer.

For each device, it:

1. reads the BORE metadata,
2. resolves the digitizer serial number,
3. loads the x742 correction tables from `misc/CorrectionTables/`,
4. applies the CAEN offline corrections,
5. decodes the corrected event,
6. converts the waveforms to physical units,
7. stores the waveform and hit information in a `StandardEvent`.

### Serial-number resolution

The preferred source is the BORE tag:

- `serial_number`

For legacy data where this tag is absent, the converter falls back to the producer name:

- `CAEN_UZH`  → `25004`
- `CAEN_IJS`  → `22890`

If you add more digitizers, extend this fallback map in the converter.

### Current assumptions of the converter

The current converter assumes:

- **DT5742 at 5000 MHz only**,
- **offline x742 correction enabled**,
- **correction mask = `0x7`** (cell offset + sample index + time correction),
- correction tables stored locally under `misc/CorrectionTables/`.

---

## x742 corrections

The DT5742 (x742 family) requires software corrections. CAEN documents three correction components:

1. **Cell index offset correction**,
2. **Sample index offset correction**,
3. **Time correction**.

The default correction tables are stored in the board flash and can be read from the digitizer 
using `GetCorrectionTables(...)`. CAEN also documents an offline path in which raw data are saved 
and later corrected with:

- `LoadCorrectionTable(...)`
- `ApplyDataCorrection(...)`
- `GetNumEvents(...)`
- `GetEventPtr(...)`
- `X742_DecodeEvent(...)`

This module follows that offline path in the converter.


## Extracting correction tables

The correction tables should be dumped once per board and stored in `misc/CorrectionTables/` using 
the serial-dependent naming convention shown above. The script `python/dump_x742_correction_tables.py`
can be used for that

The recommended workflow is:

1. connect to the digitizer,
2. run `python dump_x742_correction_tables.py --output-dir ../misc/CorrectionTables`
3. if needed, update the repository with the new added tables.

Because the converter is currently fixed to **5000 MHz**, the stored basenames should use `5000MHz`.

## Examples

- Example init file: `misc/CAENDT5742_example.ini`
- Example config file: `misc/CAENDT5742_example.conf`
- Example channel map: `misc/CAEN_connections_example.csv`

## References

- CAEN DT5742 User Manual
- CAENDigitizer Library User Manual
- CAENComm User Manual
- CAENVMELib User Manual


# CAEN DT5742 module for EUDAQ

Use a [CAEN DT5742 digitizer](https://www.caen.it/products/dt5742/) within the EUDAQ framework. In order for this to work you first have to install [CAENpy](https://github.com/SengerM/CAENpy).

![Picture of the DT5742 digitizer](https://caen.it/wp-content/uploads/2017/10/DT5742S_featured.jpg)

## Usage

### Settings to be specified in the init file:

- `LinkNum`: `int`
  - Number of link to connect to the CAEN, usually `0`, `1` or so. For more info look for `LinkNum` in the [CAENDigitizer library](https://www.caen.it/products/caendigitizer-library/).

See also the example in [misc/CAENDT5742_example.ini](misc/CAENDT5742_example.ini).

### Settings to be specified in the config file:

For the most up to date documentation about these parameters, look for the `CONFIGURE_PARAMS` dictionary in the file [CAENDT5742Producer.py](python/CAENDT5742Producer.py). It is self explanatory.

- `sampling_frequency_MHz`: `int`, default `5000`
- `max_num_events_BLT`: `int`, default `1`
  - Number of events to be transferred from the digitizer to the computer on each data transfer. For more info look for *MaxNumEventsBLT* in the [CAENDigitizer library](https://www.caen.it/products/caendigitizer-library/).
- `fast_trigger_threshold_ADCu`, `int`, mandatory
  - Trigger threshold in ADC units (16 bits). For more information look for *GroupFastTriggerThreshold* in the [CAENDigitizer library](https://www.caen.it/products/caendigitizer-library/).
- `post_trigger_size`: `int`, default `1`
  - This is similar to the *trigger delay* in an oscilloscope. For more info look for *PostTriggerSize* in the [CAENDigitizer library](https://www.caen.it/products/caendigitizer-library/).
- `trigger_polarity`: `str`, mandatory
  - Either `'rising'` or `'falling'`.

See also the example in [misc/CAENDT5742_example.conf](misc/CAENDT5742_example.conf).

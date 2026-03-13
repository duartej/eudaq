#! /usr/bin/env python3
# -*- coding: utf-8

# XXX -- DOC 

# FIXME -- Create timestamp? Extract from trigger channel?
import ast

from CAENpy.CAENDigitizer import CAEN_DT5742_Digitizer
import click

import numpy as np
import sys

from pathlib import Path
#sys.path.insert(1, str((Path(__file__).parent.parent.parent.parent/'lib').resolve())) # Here is where `pyeudaq` lies.

import pyeudaq
from pyeudaq import EUDAQ_INFO, EUDAQ_ERROR

import queue

import threading 
import time

import pandas

CAEN_CHANNELS_NAMES = [f'CH{_}' for _ in range(16)] + [f'trigger_group_{_}' for _ in [0,1]]

# Variable to allow the use the real DAQ or a simulation
class _DAQ(object):
    _actual_daq = None
    def __call__(self, *values,**kwd):
        return _DAQ._actual_daq(*values,**kwd)
CAEN_DAQ = _DAQ()

def parse_channels_mapping(path_to_channels_mapping_file:Path)->dict:
    EXPECTED_DTYPES = {
        'DUT_name': str,
        'row': int,
        'col': int,
        'channel_name': str,
    }
    try:
        mapping = pandas.read_csv(
            path_to_channels_mapping_file,
            dtype = EXPECTED_DTYPES,
        )
    except ValueError as e:
        if 'cannot safely convert passed user dtype' in repr(e):
            raise ValueError(f'The values in the file that specifies the CAEN channels connections (i.e. in {path_to_channels_mapping_file}) cannot be parsed according to the expected data types. The expected data types are {EXPECTED_DTYPES}. ')
        else:
            raise e
    
    if not set(mapping['channel_name']).issubset(set(CAEN_CHANNELS_NAMES)):
        raise ValueError(f'The valid channels names for the CAEN digitizer are {CAEN_CHANNELS_NAMES}, received {sorted(set(mapping["channel_name"]))}. At least one is wrong.')
    
    channels_mapping = dict()
    for DUT_name, df_DUT in mapping.groupby('DUT_name'):
        channels_mapping[DUT_name] = dict()
        for channel_name, df_channel in df_DUT.groupby('channel_name'):
            rowscols = df_channel[['row','col']].to_numpy(dtype=int)
            channels_mapping[DUT_name][channel_name] = [tuple(map(int,rc)) for rc in rowscols]
    
    return channels_mapping

def decode_trigger_id(trigger_id_waveform:np.ndarray, clock_waveform:np.ndarray, trigger_waveform:np.ndarray, clock_edge_to_use:str)->int:
    ### XXX -- TO BE REMOVE
    """Decode a trigger ID from a waveform.
    
    Arguments
    ---------
    trigger_id_waveform: np.ndarray
        The waveform containing the trigger ID sent by
        the AIDA TLU.
    clock_waveform: np.ndarray
        The waveform containing the clock sent by the AIDA TLU.
    trigger_waveform: np.ndarray
        The waveform containing the trigger.
    clock_edge_to_use: str
        Which clock edge to use to look for the data in the `trigger_id_waveform`,
        options are `'rising'` or `'falling'`. You have to choose looking
        at the waveforms.
    
    Returns
    -------
    decoded_id: int
        The decoded ID.
    """
    if not isinstance(trigger_id_waveform, np.ndarray) or not isinstance(clock_waveform, np.ndarray) or not isinstance(trigger_waveform, np.ndarray):
        raise TypeError(f'Both `trigger_id_waveform`, `clock_waveform` and `trigger_waveform` must be instances of `numpy.ndarray`.')
    
    if clock_edge_to_use == 'rising':
        clock_edge_to_use = 1
    elif clock_edge_to_use == 'falling':
        clock_edge_to_use = -1
    else:
        raise ValueError(f'`clock_edge_to_use` must be either "rising" or "falling"')
    
    digitized_clock_waveform = clock_waveform > clock_waveform.mean()
    digitized_trigger_id_waveform = trigger_id_waveform > clock_waveform.mean()
    digitized_trigger_waveform = trigger_waveform > clock_waveform.mean()
    clock_edges_indices = np.where(np.diff(digitized_clock_waveform*clock_edge_to_use) > 0)[0] # Multiply by 1 to convert boolean array to integer array, otherwise `numpy.diff` gives always positive values.
    trigger_index = np.where(np.diff((digitized_trigger_waveform*digitized_clock_waveform)*clock_edge_to_use) > 0)[0][0]
    trigger_clock_index = clock_edges_indices[clock_edges_indices<=trigger_index][-1]
    
    bits_sequence = digitized_trigger_id_waveform[clock_edges_indices]
    bits_sequence = bits_sequence[np.where(clock_edges_indices==trigger_clock_index)[0][0]+1:]
    bits_sequence = bits_sequence[::-1]
    bits_sequence = bits_sequence*1 # Convert to integer.
    bits_sequence = ''.join([str(_) for _ in bits_sequence])
    decoded_integer = int(bits_sequence, 2)
    
    return decoded_integer

def exception_handler(method):
    def inner(*args, **kwargs):
        try:
            return method(*args, **kwargs)
        except Exception as e:
            EUDAQ_ERROR(str(e))
            raise e
    return inner

class CAENDT5742Producer(pyeudaq.Producer):
    def __init__(self, name, runctrl, simulation):
        pyeudaq.Producer.__init__(self, name, runctrl)
        self.is_running = 0
        EUDAQ_INFO('CAENDT5742Producer: New instance')
        # To ensure a responsible and thread-safe handling
        self._CAEN_lock = threading.Lock()

        if simulation:
            _DAQ._actual_daq = FakeCAEN_DT5742_Digitizer
            self.is_simulation = True
        else:
            _DAQ._actual_daq = CAEN_DT5742_Digitizer
            self.is_simulation = False
        
        self._name = name
        
        # Threading for acquisition -> sending decoupling
        self._stop_evt = threading.Event()

        # Bounded queue to provent unbounded memory growth
        # (Tune maxsize depending on event size and expected rate?)
        self._raw_queue = queue.Queue(maxsize=2000)

        # Acquisition worker thread handle
        self._acq_thread = None

                        
    def _fill_bore(self, event):
        """Fill the Begin of Run Event with some metadata
        """
        event.SetBORE()
        # Store the mapping of the channels literally as it was parsed.
        event.SetTag('channels_mapping_str', str(self.channels_mapping))
        # A set with the channels that were acquired randomly ordered, 
        # e.g. `{'CH8', 'CH2', 'CH13', 'CH10', 'CH15', 'CH5', 'CH0', 
        # 'CH1', 'CH3', 'CH4', 'trigger_group_0', 'CH11', 'CH14', 'trigger_group_1', 'CH12', 'CH9'}`.
        event.SetTag('set_of_active_channels', str(list(self.set_of_active_channels)))
        # A list with the names of the DUTs.
        event.SetTag('dut_names', str(self.channels_mapping.keys()))
        
        # Several useful info
        event.SetTag('sampling_frequency_MHz', repr(self._digitizer.get_sampling_frequency()))
        # Number of samples per waveform to decode the raw data.
        event.SetTag('n_samples_per_waveform', repr(self._digitizer.get_record_length()))
        n_dut = 0
        for dut_name, dut_channels in self.channels_mapping.items():
            dut_label = f'DUT_{n_dut}' # DUT_0, DUT_1, ...
            # For each device this tells how the connections were made, e.g. `'CH0:[(0,0),(0,1),(1,0)],CH1:[(3,3)]'`.
            event.SetTag(dut_name, str(self.channels_mapping[dut_name]).replace('{','').replace('}','').replace(' ','').replace("'",'').replace('"',''))
            n_dut += 1
        event.SetTag(f'producer_name', str(self._name))

    def _acq_worker(self):
        """
        Acquisition thread:
            - Read raw events from the digitizer as soon as they are available
            - Push (evt_counter, ttt, raw_evt) into a bounded queuea

        Design choice when queue is full:
            - Drop oldest item to avoid stallingacquisition (preferred for "no backpressure")
        """
        while self.is_running and (not self._stop_evt.is_set()):
            raw_events = []
            try:
                with self._CAEN_lock:
                    # XXX Can skip status polling and just call get_raw_events
                    # if get_raw_events is cheap when there is no data 
                    if self._digitizer.get_acquisition_status()['at least one event available for readout']:
                        raw_events = self._digitizer.get_raw_events()
            except Exception as e:
                EUDAQ_ERROR(f"Acquisition thread error: {e}")
                time.sleep(1e-3)
                continue

            # Push to queue (drop-oldest policy if full)
            for evt_counter, ttt, raw_evt in raw_events:
                if not self.is_running or self._stop_evt.is_set():
                    break
                try:
                    self._raw_queue.put((int(evt_counter), int(ttt), raw_evt), block=False)
                except queue.Full:
                    # Drop one oldest and retry once
                    try:
                        _ = self._raw_queue.get_nowait()
                        self._raw_queue.task_done()
                    except queue.Emtpy:
                        pass
                    try:
                        self._raw_queue.put((int(evt_counter), int(ttt), raw_Evt), block=False)
                    except queue.Full:
                        # Still full: drop this event
                        pass
            # Avoid burning CPU when idle
            if not raw_events:
                time.sleep(2e-4)
        
    @exception_handler
    def DoInitialise(self):
        initconf = self.GetInitConfiguration().as_dict()
        try:
            LinkNum = int(initconf['LinkNum'])
        except KeyError:
            raise RuntimeError(f'`LinkNum` (int) parameter is mandatory in the init file.')
        except ValueError:
            raise ValueError(f'`LinkNum` must be an integer')
        expected_serial_number = initconf.get('expected_serial_number')
        
        self._digitizer = CAEN_DAQ(LinkNum=LinkNum)
        
        #self._telegram_reporter = SafeTelegramReporter4Loops(
        #    bot_token=my_telegram_bots.robobot.token, 
        #    chat_id='-4198108027',
        #    parse_mode = 'Markdown', # This is optional. But it is cool.
        #)
        
        if expected_serial_number is not None:
            actual_serial_number = str(self._digitizer.get_info()['SerialNumber'])
            if expected_serial_number != actual_serial_number:
                raise RuntimeError(f'You told me to connect to a CAEN digitizer with serial number {repr(expected_serial_number)} but instead in `LinkNum` {repr(LinkNum)} I found one with serial number {repr(actual_serial_number)}. If you are using more than one digitizer, probably you have wrong the `LinkNum`.')

    @exception_handler
    def DoConfigure(self):
        CONFIGURE_PARAMS = {
                'sampling_frequency_MHz': dict(
                    set_method = self._digitizer.set_sampling_frequency,
                    default = 5000,
                    type = int,
                    ),
                'max_num_events_BLT': dict(
                    set_method = self._digitizer.set_max_num_events_BLT,
                    default = 1,
                    type = int,
                    ),
                'record_length': dict(
                    set_method = self._digitizer.set_record_length,
                    default = 1024,
                    type = int,
                    ),
                'fast_trigger_threshold_ADCu': dict(
                    set_method = self._digitizer.set_fast_trigger_threshold,
                    type = int,
                    ),
                'post_trigger_size': dict(
                    set_method = self._digitizer.set_post_trigger_size,
                    default = 1,
                    type = int,
                    ),
                'trigger_polarity': dict(
                    type = str,
                    ),
                'channels_mapping_file': dict(
                    type = Path,
                )
                }

        # Always better to start in a known state
        self._digitizer.reset() 
        
        # Note the special case: trigger_group_0 -> 16  and trigger_group_1 --> 17
        self.channels_to_int = {}
        for ch in CAEN_CHANNELS_NAMES:
            try:
                self.channels_to_int[ch] =  int(ch.replace('CH','')) 
            except ValueError:
                if ch == 'trigger_group_0':
                    self.channels_to_int[ch] = 16
                if ch == 'trigger_group_1':
                    self.channels_to_int[ch] = 17
        
        # Parse parameters and raise errors if necessary:
        conf = self.GetConfiguration().as_dict()
        for param_name, param_dict in CONFIGURE_PARAMS.items():
            try:
                received_param_value = conf[param_name]
            except KeyError: 
                # Not in the configuration file, use the default from the internal dict
                received_param_value = param_dict.get('default')
                # This means it is mandatory and was not specified, i.e. there is no default value.
                if received_param_value is None: 
                    raise RuntimeError(f'Configuration parameter {repr(param_name)} is '\
                            'mandatory and was not specified in the configuration file. ')
            #Convert to the correct data type...
            try:
                param_value = param_dict['type'](received_param_value)
            except Exception as e:
                raise ValueError(f'The parameter `{param_name}` must be of type {param_dict["type"]}, '\
                        'received {repr(received_param_value)}. ')
            if param_dict.get('set_method') is not None:
                param_dict['set_method'](param_value)
            else:
                # This is just for trigger polarity.. XXX -- Is this needed?
                param_dict['value'] = param_value
        
        
        self.channels_mapping = parse_channels_mapping(Path(conf['channels_mapping_file'])) # This stores the information regarding what is connected into each of the channels of the CAEN and the geometry.
        EUDAQ_INFO(f'The mapping of the channels was set to this: {self.channels_mapping}')
        
        self.set_of_active_channels = set([ch for DUT_name in self.channels_mapping for ch in self.channels_mapping[DUT_name]])
        
        # Manual configuration of parameters:
        for ch in [0,1]:
            self._digitizer.set_trigger_polarity(channel=ch, edge=CONFIGURE_PARAMS['trigger_polarity']['value'])
            
        # Some non-configurable settings:
        self._digitizer.set_acquisition_mode('sw_controlled')
        self._digitizer.set_ext_trigger_input_mode('disabled')
        self._digitizer.set_fast_trigger_mode(enabled=True)
        
        # Auto detect some settings according to the requested channels by the user.
        self._digitizer.set_fast_trigger_digitizing(enabled = any([_ in self.set_of_active_channels for _ in ['trigger_group_0','trigger_group_1']]))
        self._digitizer.enable_channels(
            group_1 = any([_ in ([f'CH{n}' for n in [0,1,2,3,4,5,6,7]] + ['trigger_group_0']) for _ in self.set_of_active_channels]),
            group_2 = any([_ in ([f'CH{n}' for n in [8,9,10,11,12,13,14,15]] + ['trigger_group_1']) for _ in self.set_of_active_channels]),
        )
        self._digitizer.set_fast_trigger_DC_offset(V=0)

        ### XXX - FIXME to bew configurable
        # So far, just force negative pulses
        DC_OFFSET = 0x600F
        for ch in filter(lambda _channel: _channel.find('trigger_') == -1, self.set_of_active_channels):
            self._digitizer.set_channel_DC_offset(int(ch.strip('CH')), DC_OFFSET)
        EUDAQ_INFO(f'Set OFFSET channel (except for trigger) to {hex(DC_OFFSET)}')
        ### XXX - FIXME to bew configurable
        
        # Enable busy signal on GPO:
        self._digitizer.write_register(
                # Front Panel I/O Control, see '742 Raw Waveform Registers Description' in https://www.caen.it/products/dt5742/ → Downloads.
                address = 0x811C, 
                data = (0
                        | 0b1<<0 # 1 = TTL standard, 0 = NIM standard.
                        | 0b01<<16 #  Motherboard Probes: TRG‐OUT/GPO is used to propagate signals of the motherboards according to bits[19:18].
                        | 0b11<<18 # BUSY/UNLOCK: this is the board BUSY in case of ROC FPGA firmware rel. 4.5 or lower. This probe can be selected according to bit[20].
                        | 0b0<<20 # If bits[19:18] = 11, then bit[20] options are: 0 = Board BUSY.
                        )
                )
        
        # Take care of the trigger ID parsing from the waveform. Here we make sure there are no mistakes in the config file, and store some stuff.
        if all([_ in self.channels_mapping for _ in {'TLU_clock','trigger_id','trigger'}]):
            for _ in {'TLU_clock','trigger_id'}:
                if len(self.channels_mapping[_]) != len(self.channels_mapping[_][0]) != 1:
                    raise RuntimeError(f'There is an error in the configuration of `channels_mapping`, specifically the DUT called {repr(_)} should have one and only one channel, e.g. `{repr(_)}: [["CH0"]]`.')
            for _ in {'trigger'}:
                if len(self.channels_mapping[_]) != 1 or len(self.channels_mapping[_][0]) not in {1,2}:
                    raise RuntimeError(f'There is an error in the configuration of `channels_mapping`, {repr(_)} should have one or two channels, e.g. `{repr(_)}: [["trigger_group_0"]]` or `{repr(_)}: [["trigger_group_0","trigger_group_1"]]`.')
            self._trigger_id_decoding_config = {
                'TLU_clock_channel_name': self.channels_mapping['TLU_clock'][0][0],
                'trigger_id_channel_name': self.channels_mapping['trigger_id'][0][0],
                'trigger_channel_names': self.channels_mapping['trigger'][0],
            }
            try:
                self.n_bits_to_use_when_decoding_trigger_id_from_waveform = int(conf['n_bits_to_use_when_decoding_trigger_id_from_waveform'])
            except Exception:
                raise RuntimeError(f'The "trigger ID parsing mode" was enabled by providing the appropriate DUTs names, but there is an error parsing `n_bits_to_use_when_decoding_trigger_id_from_waveform`, which must be an integer number.')
            EUDAQ_INFO(f'Trigger ID decoding was configured. ✅')
        
    @exception_handler
    def DoStartRun(self):
        self._stop_evt.clear()
        self._digitizer.start_acquisition()
        self.is_running = 1

        # Start acquisition thread
        self._acq_thread = threading.Thread(target=self._acq_worker, daemon=True)
        self._acq_thread.start()
        
    @exception_handler
    def DoStopRun(self):
        # Return inmediately if simulation
        if self.is_simulation:
            self._digitizer.stop_acquisition()
            self.is_running = 0
            return 
        
        # Stop acquisition trhead first
        self.is_running = 0 
        self._stop_evt.set()
        
        if self._acq_thread is not None and self._acq_thread.is_alive():
            self._acq_thread.join(timeout=2.0)

        # Stop digitizer acquisition (thread-safe)
        with self._CAEN_lock:
            self._digitizer.stop_acquisition()

    @exception_handler
    def DoReset(self):
        if hasattr(self, '_digitizer'):
            self._digitizer.close()
            delattr(self, '_digitizer')
        self.is_running = 0
        
    @exception_handler
    def RunLoop(self):
        """
        Sender loop:
            - Pop raw events from queue and send them to EUDAQ
        """
        # --- XXX
        do_bore = True
        # --- XXX
        while self.is_running or (not self._raw_queue.empty()):
            try:
                evt_counter, ttt, raw_evt = self._raw_queue.get(timeout=0.2)
            except queue.Empty:
                continue


            ev = pyeudaq.Event('RawEvent', 'CAENDT5742')
            ev.SetTriggerN(int(evt_counter))
            ev.SetTag('caen_trigger_time_tag', str(int(ttt)))
            # --- XXX
            if do_bore:
                self._fill_bore(ev)
                do_bore = False
            # --- XXX            
            ev.AddBlock(0, raw_evt)
            
            self.SendEvent(ev)
            # Small sleep to aovid busy spinning... ?
            self._raw_queue.task_done()


@click.command()
@click.option('-n','--name', default='CAEN_digitizer',
              help='Name for the producer (default "CAEN_digitizer")')
@click.option('-r','--runctrl',default='tcp://localhost:44000', 
              help='Address of the run control, for example (and default) "tcp://localhost:44000"')
@click.option('-s','--dry-run',is_flag=True, default=False, 
              help='Don\'t connect to anything, use a simulation to produce events')
def main(name,runctrl,dry_run):
    producer = CAENDT5742Producer(name,runctrl,dry_run)
    # XXX -- logger
    print (f"[CAENDGT57545Producer]: Connecting to runcontrol in {runctrl} ...")
    producer.Connect()
    time.sleep(2)
    print('[CAENDGT57545Producer]: Connected')
    while(producer.IsConnected()):
        time.sleep(1)
        
if __name__ == "__main__":
    main()

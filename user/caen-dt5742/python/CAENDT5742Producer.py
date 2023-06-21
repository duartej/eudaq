#! /usr/bin/env python3
# -*- coding: utf-8

# XXX -- DOC 

# FIXME -- Create timestamp? Extract from trigger channel?
import ast

# https://github.com/SengerM/CAENpy
from CAENpy.CAENDigitizer import CAEN_DT5742_Digitizer
import click

import numpy as np
import sys
from pathlib import Path
sys.path.insert(1, str((Path(__file__).parent.parent.parent.parent/'lib').resolve())) # Here is where `pyeudaq` lies.
import pyeudaq
from pyeudaq import EUDAQ_INFO, EUDAQ_ERROR

import queue

import threading 
import time

import pandas

# Debugging imports:
# ~ import pandas
# ~ import warnings

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
            rowscols = df_channel[['row','col']].to_numpy()
            channels_mapping[DUT_name][channel_name] = [tuple(_) for _ in rowscols]
    
    return channels_mapping

def decode_trigger_id(trigger_id_waveform:np.ndarray, clock_waveform:np.ndarray, trigger_waveform:np.ndarray, clock_edge_to_use:str)->int:
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
        
        self.set_of_active_channels = set([ch for DUT_name in channels_mapping for ch in channels_mapping[DUT_name]])
        
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
        
        # Enable busy signal on GPO:
        self._digitizer.write_register(
                # Front Panel I/O Control, see '742 Raw Waveform Registers Description' in https://www.caen.it/products/dt5742/ → Downloads.
                address = 0x811C, 
                data = (0
                        | 0b0<<0 # 1 = TTL standard, 0 = NIM standard.
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
        self._digitizer.start_acquisition()
        self.is_running = 1
        
    @exception_handler
    def DoStopRun(self):
        # Return inmediately if simulation
        if self.is_simulation:
            self._digitizer.stop_acquisition()
        
        with self._CAEN_lock:
            self._digitizer.stop_acquisition()
        is_there_stuff_still_in_the_digitizer_memory = True
        while is_there_stuff_still_in_the_digitizer_memory:
            # Wait for any remaining data that is still in the memory of the digitizer.
            with self._CAEN_lock:
                is_there_stuff_still_in_the_digitizer_memory = \
                        self._digitizer.get_acquisition_status()['at least one event available for readout'] == True
            time.sleep(.1)
        # Wait for all the waveforms to be processed.
        self.events_queue.join()
        self.is_running = 0

    @exception_handler
    def DoReset(self):
        if hasattr(self, '_digitizer'):
            self._digitizer.close()
            delattr(self, '_digitizer')
        self.is_running = 0
        
    @exception_handler
    def RunLoop(self):
        self.events_queue = queue.Queue()
        
        if DEBUG_WAVEFORMS_DUMPING:
            this_run_timestamp = create_a_timestamp()
        
        def thread_target_function():
            if DEBUG_WAVEFORMS_DUMPING:
                waveforms_to_dump = []
            n_trigger = 0
            # ~ deleteme = []
            previous_decoded_trigger_id = None
            have_to_decode_trigger_id = hasattr(self, '_trigger_id_decoding_config')
            decoded_trigger_number_of_turns = 0
            while self.is_running:
                # Creation of the caen event type and sub-type 
                #event = pyeudaq.Event("CAENDT5748RawEvent", "CAENDT5748")
                # XXX -- Need this new event type, or enough with the RawEvent?
                event = pyeudaq.Event("RawEvent", "CAENDT5748")
                event.SetTriggerN(n_trigger)
                # BORE info
                if n_trigger == 0:
                    event.SetBORE()
                    # Store the mapping of the channels literally as it was parsed.
                    event.SetTag('channels_mapping_str', str(self.channels_mapping))
                    # A set with the channels that were acquired randomly ordered, e.g. `{'CH8', 'CH2', 'CH13', 'CH10', 'CH15', 'CH5', 'CH0', 'CH1', 'CH3', 'CH4', 'trigger_group_0', 'CH11', 'CH14', 'trigger_group_1', 'CH12', 'CH9'}`.
                    event.SetTag('set_of_active_channels', str(list(self.set_of_active_channels)))
                    # A list with the names of the DUTs.
                    event.SetTag('dut_names', str(self.channels_mapping.keys()))
                    
                    event.SetTag('sampling_frequency_MHz', repr(self._digitizer.get_sampling_frequency()))
                    # Number of samples per waveform to decode the raw data.
                    event.SetTag('n_samples_per_waveform', repr(self._digitizer.get_record_length()))
                    n_dut = 0
                    for dut_name, dut_channels in self.channels_mapping.items():
                        dut_label = f'DUT_{n_dut}' # DUT_0, DUT_1, ...
                        # For each device this tells how the connections were made, e.g. `'CH0:[(0,0),(0,1),(1,0)],CH1:[(3,3)]'`.
                        event.SetTag(dut_name, str(tag_with_DUT_channels).replace('{','').replace('}','').replace(' ','').replace("'",'').replace('"',''))
                        n_dut += 1
                    
                    event.SetTag(f'producer_name', str(self._name))
                
                # -- XXX - THe CHannel will give the information of thee position in x/y of the pad
                #          within the DUT
                # Extract the waveforms
                if not self.events_queue.empty():
                    this_trigger_waveforms = self.events_queue.get()
                    n_trigger += 1
                    
                    for ch in self.channels_names_list:
                        serialized_data = np.array(this_trigger_waveforms[ch]['Amplitude (V)'], dtype=np.float32)
                        serialized_data = serialized_data.tobytes()
                        # Use the channel as Block Id
                        event.AddBlock(self.channels_to_int[ch], serialized_data)
                        
                        # ~ df = pandas.DataFrame(this_trigger_waveforms[ch])
                        # ~ df['channel'] = ch
                        # ~ df['n_trigger'] = n_trigger
                        # ~ deleteme.append(df)
                    
                    if have_to_decode_trigger_id:
                        raw_decoded_trigger_id = decode_trigger_id(
                            trigger_id_waveform = this_trigger_waveforms[self._trigger_id_decoding_config['trigger_id_channel_name']]['Amplitude (V)'],
                            clock_waveform = this_trigger_waveforms[self._trigger_id_decoding_config['TLU_clock_channel_name']]['Amplitude (V)'],
                            trigger_waveform = this_trigger_waveforms[self._trigger_id_decoding_config['trigger_channel_names'][0]]['Amplitude (V)'],
                            clock_edge_to_use = 'falling', # Hardcoded here, but seems to be the right one to use.
                        )
                        if raw_decoded_trigger_id == 0 and previous_decoded_trigger_id is not None:
                            if (previous_decoded_trigger_id+1)%(2**self.n_bits_to_use_when_decoding_trigger_id_from_waveform+1): # if this trigger is supposed to be a multiple of the loop size (given by the number of bits)...
                                decoded_trigger_number_of_turns += 1
                        decoded_trigger_id = raw_decoded_trigger_id + (2**self.n_bits_to_use_when_decoding_trigger_id_from_waveform)*decoded_trigger_number_of_turns
                        if decoded_trigger_id != n_trigger:
                            EUDAQ_INFO(f'⚠️ The decoded trigger does not coincide with the internally counted triggers. (n_internal={n_trigger}, n_decoded={decoded_trigger_id})')
                        previous_decoded_trigger_id = decoded_trigger_id
                        print(decoded_trigger_id==n_trigger, n_trigger)
                    
                    self.SendEvent(event)
                    self.events_queue.task_done()
            # ~ deleteme = pandas.concat(deleteme)
            # ~ deleteme.to_pickle(f'~/Desktop/waveforms_CAEN_{str(self._digitizer.get_info()["SerialNumber"])}.pickle')
            
            if DEBUG_WAVEFORMS_DUMPING and len(waveforms_to_dump) > 0:
                waveforms_to_dump = pandas.concat(waveforms_to_dump)
                waveforms_to_dump.to_pickle(LOCATION_FOR_DUMPING_DATA/f'{this_run_timestamp}_waveforms_CAEN_{str(self._digitizer.get_info()["SerialNumber"])}.pickle')
            
        threading.Thread(target=thread_target_function, daemon=True).start()

        while self.is_running:
            with self._CAEN_lock:
                if self._digitizer.get_acquisition_status()['at least one event available for readout'] == True:
                    waveforms = self._digitizer.get_waveforms(get_time=False, get_ADCu_instead_of_volts=False)
                    # Waveforms is a list of dictionaries, each of which contains the waveforms from each trigger.
                    for this_trigger_waveforms in waveforms:
                        self.events_queue.put(this_trigger_waveforms)
            time.sleep(1e-6) # This small delay is so that the lock can be acquired by other threads, otherwise it goes so fast that no one else can acquire it other than by chance.

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
    # ~ channels_mapping = parse_channels_mapping(Path('/home/msenger/config/channels.csv'))
    # ~ print(channels_mapping)
    # ~ tag_with_DUT_channels = channels_mapping['weird_DUT']
    
    # ~ print()
    

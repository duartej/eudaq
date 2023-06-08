#! /usr/bin/env python3
# -*- coding: utf-8

# XXX -- DOC 

# FIXME -- Create timestamp? Extract from trigger channel?
import ast

# https://github.com/SengerM/CAENpy
from CAENpy.CAENDigitizer import CAEN_DT5742_Digitizer
from CAENpy.SimCAENDigitizer import FakeCAEN_DT5742_Digitizer
import click

import numpy as np

import pyeudaq
from pyeudaq import EUDAQ_INFO, EUDAQ_ERROR

import queue

import threading 
import time


# Variable to allow the use the real DAQ or a simulation
class _DAQ(object):
    _actual_daq = None
    def __call__(self, *values,**kwd):
        return _DAQ._actual_daq(*values,**kwd)
CAEN_DAQ = _DAQ()

def parse_channels_mapping(channels_mapping_str:str):
    """Parse the `channels_mapping` config parameter and returns the 
	expected dictionary. Also raises `ValueError` if anything is wrong.

    Parameters
    ----------
    channels_mapping_str: str
        XXX -- DOC -> WHAT's those mapping?

    Return
    ------

    """
    try:
        channels_mapping = ast.literal_eval(channels_mapping_str)
    except Exception as e:
        raise ValueError(f'Cannot parse `channels_mapping` into a Python object,'\
                ' probably there is a syntax error in your config file. The error'\
                ' raised by the method in charge of the parsing is: {repr(e)}')
    if not isinstance(channels_mapping, dict):
        raise ValueError(f'`channels_mapping` should be a dictionary, received instead'\
                ' an object of type {type(channels_mapping)}')
    for k,i in channels_mapping.items():
        if not isinstance(k, str):
            raise ValueError(f'The keys of `channels_mapping` must be strings containing'\
                    ' the names of each plane, but received `{repr(k)}` of type {type(k)}')
        if not isinstance(i, list) or any([not isinstance(_,list) for _ in i]) or any([len(i[0])!=len(_) for _ in i]):
            raise ValueError(f'Each item of `channels_mapping` must be a two dimensional'\
                    ' array specified as a list of lists with dimensions X×Y being X the'\
                    ' number of pixels in x and Y the number of pixels in Y')
        if any([not isinstance(_,str) for __ in i for _ in __]):
            raise ValueError(f'The elements inside the lists of `channels_mapping` must'\
                    ' be strings with the channels names from the CAEN, e.g. `"CH1"`.')
    return channels_mapping

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
        
    @exception_handler
    def DoInitialise(self):
        initconf = self.GetInitConfiguration().as_dict()
        try:
            LinkNum = int(initconf['LinkNum'])
        except KeyError:
            raise RuntimeError(f'`LinkNum` (int) parameter is mandatory in the init file.')
        except ValueError:
            raise ValueError(f'`LinkNum` must be an integer')
        self._digitizer = CAEN_DAQ(LinkNum=LinkNum)

    @exception_handler
    def DoConfigure(self):
        CONFIGURE_PARAMS = {
                'sampling_frequency_MHz': dict(
                    set_method = 'set_sampling_frequency',
                    default = 5000,
                    type = int,
                    ),
                'max_num_events_BLT': dict(
                    set_method = 'set_max_num_events_BLT',
                    default = 1,
                    type = int,
                    ),
                'record_length': dict(
                    set_method = 'set_record_length',
                    default = 1024,
                    type = int,
                    ),
                'fast_trigger_threshold_ADCu': dict(
                    set_method = 'set_fast_trigger_threshold',
                    type = int,
                    ),
                'post_trigger_size': dict(
                    set_method = 'set_post_trigger_size',
                    default = 1,
                    type = int,
                    ),
                'trigger_polarity': dict(
                    type = str,
                    ),
                }

        # Always better to start in a known state
        self._digitizer.reset() 
        
        conf = self.GetConfiguration().as_dict()
        channels_mapping_str = conf['channels_mapping']
        self.channels_mapping = parse_channels_mapping(channels_mapping_str)
        
        # This is the order in which the data will be stored, i.e. which channel first, which second, etc.
        self.channels_names_list = sorted([self.channels_mapping[ch][i][j] 
                                           for ch in self.channels_mapping for i in range(len(self.channels_mapping[ch]))
                                           for j in range(len(self.channels_mapping[ch][i]))]) 	
        
        # Parse parameters and raise errors if necessary:
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
                getattr(self._digitizer, param_dict['set_method'])(param_value)
            else:
                # This is just for trigger polarity.. XXX -- Is this needed?
                param_dict['value'] = param_value
                
        # Manual configuration of parameters:
        for ch in [0,1]:
            self._digitizer.set_trigger_polarity(channel=ch, edge=CONFIGURE_PARAMS['trigger_polarity']['value'])
            
        # Some non-configurable settings:
        self._digitizer.set_acquisition_mode('sw_controlled')
        self._digitizer.set_ext_trigger_input_mode('disabled')
        self._digitizer.set_fast_trigger_mode(enabled=True)
        self._digitizer.set_fast_trigger_digitizing(enabled=True)
        self._digitizer.enable_channels(
                group_1=any([f'CH{n}' in self.channels_names_list for n in [0,1,2,3,4,5,6,7]]), 
                group_2=any([f'CH{n}' in self.channels_names_list for n in [8,9,10,11,12,13,14,15]])
                )
        self._digitizer.set_fast_trigger_DC_offset(V=0)
        
        # Enable busy signal on GPO:
        self._digitizer.write_register(
                # Front Panel I/O Control, see '742 Raw Waveform Registers Description' in https://www.caen.it/products/dt5742/ → Downloads.
                address = 0x811C, 
                data = (0
                        | 0b1<<0 # TTL standard.
                        | 0b01<<16 #  Motherboard Probes: TRG‐OUT/GPO is used to propagate signals of the motherboards according to bits[19:18].
                        | 0b11<<18 # BUSY/UNLOCK: this is the board BUSY in case of ROC FPGA firmware rel. 4.5 or lower. This probe can be selected according to bit[20].
                        | 0b0<<20 # If bits[19:18] = 11, then bit[20] options are: 0 = Board BUSY.
                        )
                )
        
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

        def thread_target_function():
            n_trigger = 0
            while self.is_running:
                this_trigger_waveforms = self.events_queue.get()
                serialized_data = np.concatenate(
                        [this_trigger_waveforms[ch]['Amplitude (ADCu)'] for ch in self.channels_names_list],
                        # Hardcode data type to be sure it is always the same. 
                        # (Though you would expect `int` here, CAENDigitizer library returns floats...)
                        dtype = np.float32, 
                        )
                serialized_data = serialized_data.tobytes()
                
                # Creation of the caen event type and sub-type 
                #event = pyeudaq.Event("CAENDT5748RawEvent", "CAENDT5748")
                # XXX -- Need this new event type, or enough with the RawEvent?
                event = pyeudaq.Event("RawEvent", "CAENDT5748")
                event.SetTriggerN(n_trigger)
                event.AddBlock( 0, serialized_data )
                # Use the channel as Block Id? 
                # Then for ch in enumerate(self.channels_names_list) : event.AddBlock(ch, serialized_data[ch]) 
                
                # BORE info
                if n_trigger == 0:
                    event.SetBORE()
                    # Literally whatever the `channels_mapping` parameter in the config file was, #
                    # e.g. `{'DUT_1': [['CH0','CH1'],['CH2','CH3']], 'DUT_2': [['CH4','CH5'],['CH6','CH7']]}`.
                    event.SetTag('channels_mapping_str', repr(self.channels_mapping))
                    # A list with the channels that were acquired and in the order they are stored in the raw data, 
                    #e.g. `['CH0','CH1','CH2',...]`
                    event.SetTag('channels_names_list', repr(self.channels_names_list))
                    # Number of DUTs that were specified in `channels_mapping` in the config file.
                    event.SetTag('number_of_DUTs', repr(len(self.channels_mapping)))
                    event.SetTag('sampling_frequency_MHz', repr(self._digitizer.get_sampling_frequency()))
                    # Number of samples per waveform to decode the raw data.
                    event.SetTag('n_samples_per_waveform', repr(self._digitizer.get_record_length()))
                    n_dut = 0
                    for dut_name, dut_channels in self.channels_mapping.items():
                        dut_label = f'DUT_{n_dut}' # DUT_0, DUT_1, ...
                        event.SetTag(f'{dut_label}_name', repr(dut_name)) # String with the name of the DUT, defined by the user in the config file as the key of the `channels_mapping` dictionary.
                        event.SetTag(f'{dut_label}_channels_matrix', repr(dut_channels)) # List with channels names, e.g. `"[['CH0','CH1'],['CH2','CH3']]"`.
                        event.SetTag(f'{dut_label}_channels_arrangement', repr([f'{item}: {i},{j}' for i,row in enumerate(dut_channels) for j,item in enumerate(row)])) # End up with something of the form `"['CH0: 0,0', 'CH1: 0,1', 'CH2: 1,0', 'CH3: 1,1']"`
                        event.SetTag(f'{dut_label}_n_channels', repr(sum([len(_) for _ in dut_channels]))) # Number of channels (i.e. number of waveforms) belonging to this DUT.
                        n_dut += 1

                self.SendEvent(event)
                n_trigger += 1
                self.events_queue.task_done()

        threading.Thread(target=thread_target_function, daemon=True).start()

        while(self.is_running):
            with self._CAEN_lock:
                if self._digitizer.get_acquisition_status()['at least one event available for readout'] == True:
                    waveforms = self._digitizer.get_waveforms(get_time=False, get_ADCu_instead_of_volts=True)
                    # Waveforms is a list of dictionaries, each of which contains the waveforms from each trigger.
                    for this_trigger_waveforms in waveforms:
                        self.events_queue.put(this_trigger_waveforms)
                else:
                    # XXX -- Cross check that's correct (or only for simulation?)
                    break


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

"""
Controller for Tektronix 4/56/ Series MSO via SCPI (VISA)

Requirements:
    pip install pyvisa pyvisa-py

Programmer Manual references:
 - BUSY? / *OPC / *WAI: (sync).
 - WFMOutpre? + CURVe? (transfer waveform).
 - Event queue / SESR / *ESR? (event handling).
"""

import logging
import sys

from typing import List, Dict, Optional


import pyvisa
import struct

# XXX  NEEDED?
import numpy as np

import logging
logger = logging.getLogger('MSOController')
logger.setLevel(logging.INFO)
chandler = logging.StreamHandler(sys.stdout)
chandler.setLevel(logging.INFO)
formatter = logging.Formatter("%(asctime)s [%(levelname)s] %(message)s")
chandler.setFormatter(formatter)
logger.addHandler(chandler)

PREAMBLE_PRE = ":WFMOUTPRE:"
PREAMBLE_ORDERED_LIST = ["BYT_NR", # Byte per point, i.e. the binary field data width
                         "BIT_NR", # Bits per point 
                         "ENCDG",  # The encoding (ascii or binary)
                         "BN_FMT", # Binary data format (RI|RP|FP: signed int|positive int|floating
                         "ASC_FMT",# The format (INTEGER, FP)
                         "BYT_OR", # Which binary byte is transmitted first (MSB or LSB)
                         "NR_PT",  # Number of points to be transmited XXX 
                         "PT_FMT", # Point format for the waveform XXX 
                         "PT_ORDER",# Always LINEAR
                         "XUNIT",  # The unit of the x-axis (s or Hz)
                         "XINCR",  # The time, in xunit, between data points
                         "XZERO",  # The time between the trigger sample (PT_IFF) and the occurrence of actual trigger
                         "PT_OFF", # The data point immediately following the trigger point relateive to DATA:STARt
                         "YUNIT",  # The vertical units
                         "YMULT",  # The multiplying factor to convert the data point values from digitizin levels to yunit
                         "YOFF",   # The vertical position in digitizing levels (25 digitizing levels per vertical position)
                         "YZERO",  # The vertical offset
                         "DOMAIN", # The domain (TIME or FREQency)
                         "WFMTYPE",# ANALOG (channel or math wf), SV_FD (spectrum view RF domain), RF_TD (RF time domain)
                         "CENTERFREQUENCY", # Frequency domain ... 
                         "SPAN",   # Frequency domain ...
                         "FFTLENGTH",# Frequency domain ...
                         "RESAMPLE", # 1 - Every sample is returned (2 - everty other sample, ...)
                         "MODE",    # ...
                         ]
                         

def _header_list_to_dict(result_str):
    """Convert a list obtained from a query where the HEADER is ON, such
    the format is the '<HEADER_TITLE> <VALUE>;<HEADER_TITLE2> <VALUE2>; ....'
    in a dict: { '<HEADER_TITLE>': <VALUE>, '<HEADER_TITLE2>': <VALUE2>, ... 
    """
    result = {}
    pre_list = result_str.split(';')
    for key_val in pre_list:
        # XXX  -- What happens with :WATHERVER:MORE ? -> should it be
        #         whateever_more? o maybe [whatever][more]
        key,val = key_val.split()
        result[key.lower()] = val
    return result

class MSOController:
    def __init__(self, resource_string, timeout_ms=10000, afg_resource=None):
        """
        Parameters
        ----------
        resource_string: example "USB0::0x0699::0x0522::123456::INSTR"
                         o "TCPIP::192
        timeout_ms: int 
            Read timeout in ms (adjust depending on record length)
        """
        self.rm = pyvisa.ResourceManager()
        self.dev = self.rm.open_resource(resource_string)
        self.dev.timeout = timeout_ms
        self.dev.read_termination = '\n'
        self.dev.write_termination = '\n'
        # Just clear the output 
        self.dev.clear()
        # Clear status
        self.clear_status()
        # Read IDn
        try:
            self._idn = self.query("*IDN?")
        except Exception as e:
            logger.warning(f"Could not read *IDN?: {e}")
            self._idn = "UNKNOWN"
        logger.info(f"Tektronix Scope connected: {self._idn}")
        if self._idn != "UNKNOWN":
            # Remove headers 
            self.write('HEADER OFF')
            # Force explicitly the record length and sample rate:
            # t_frame = Record_length/sample_rate
            self.write('HORizontal:MODE MANUAL')
            # Fix maximum sample rate
            self.write('HORizontal:MAIN:SAMPLERate 25e9')
            # Any other? XXX

        if afg_resource is not None:
            # Use the afg to generate a busy signal while reading
            # and processing data
            self.afg = self.rm.open_resource(afg_resource)
            self.afg._idn = self.afg.query("*IDN?")
            # All related functions properly identified
            self.send_busy = self._send_busy
            self.clear_busy= self._clear_busy
            logger.info(f"AFG (for BUSY signal generator) connected: {self.afg._idn}")
        else:
            self.afg = None
            # All related functions converted in dummy
            self.send_busy = lambda : None
            self.clear_busy= lambda : None
            logger.warning('No AFG ready, BUSY signal is not possible to generate')

        # Just start sending busy
        self.send_busy()


    @property
    def idn(self):
        return self._idn

    def clear_status(self):
        """Clear instrument status and event queue
        """
        self.write('*CLS')

    def reset(self):
        """Reset the scope, may take several seconds
        """
        self.write('*RST')
        # Wait to finish the reset
        _ = self.query('*OPC?')
        # Set a pre-defined configuration
        # And re-activate all channels
        self.write(f':SELECT:CH1 ON;:SELECT:CH2 ON;:SELECT:CH3 ON;:SELECT:CH4 ON')
    
    # Some useful accessors 
    # TRIGGER group
    @property
    def trigger_state(self):
        self._trigger_state = self.query('TRIGger:STATE?')
        return self._trigger_state

    def is_trigger_ready(self):
        return self._trigger_state == 'READY'

    def set_edge_trigger_source(self,source):
        self.write(f'TRIGger:A:EDGE:SOUrce {source}')
    
    @property
    def trigger_level(self):
        return self.query('TRIGger:A:LEVel?')

    @trigger_level.setter
    def trigger_level(self, value):
        self.write(f'TRIGger:A:LEVel {value}')
    
    @property
    def trigger_slope(self):
        return self.query('TRIGger:A:EDGE:SLOpe?')

    @trigger_slope.setter
    def trigger_slope(self, value):
        # valid RISE FALL and ? XXX 
        self.write(f'TRIGger:A:EDGE:SLOpe {value}')
    
    # VErtical and horizontal 
    @property
    def total_horizontal_division(self):
        if not hasattr(self,'_total_horizontal_division'):
            self._total_horizontal_division = self.query('HORizontal:DIVisions?')
        return self._total_horizontal_division
    
    def get_channel_scale(self, channel):
        return float(self.query(f'CH{channel}:SCALE?'))
    
    def set_channel_scale(self, channel, val):
        return self.write(f'CH{channel}:SCALE {val}')

    # -------------------
    # SCPI helpers
    # -------------------
    def write(self, cmd: str):
        """Send SCPI command (no waiting for answer).

        Parameter
        ---------
        cmd: str
            The command to send
        """
        return self.dev.write(cmd)

    def query(self, q: str) -> str:
        """Send SCPI query and returns and answer (string o raw bytes for binary).
        
        Parameter
        --------
        q: str
            The query command

        Return
        ------
        str: The response o f the command
        """
        return self.dev.query(q)

    def query_binary(self, q: str) -> bytes:
        """Send a SCPI query command returning binary. Use it when the scope returns 
        a binary block

        Parameter
        ---------
        q: str
            Query command returning binary data

        Return
        ------
        Bytes
        """
        # XXX - FIXME -  Are these correct? probably will depends on the BYT_NR and the BY_FMT
        # XXX -- FIXME, do that
        return self.dev.query_binary_values(q, datatype='B', container=bytes, header_fmt='ieee')

    # -------------------
    # AFG Functions
    # -------------------
    def _send_busy(self):
        # XXX -- Configuration???
        self.afg.write('OUTPUT ON')

    def _clear_busy(self):
        # XXX -- Configuration???
        self.afg.write('OUTPUT OFF')

    # -------------------
    # Sincronization 
    # -------------------
    def op_complete(self):
        """Generates *OPC and waits for the last command completion (acquisition usually)."""
        self.write("*OPC")

    def read_esr(self):
        """The *ESR? (Standard Event Status Register)"""
        return self.query("*ESR?")

    # ------------------
    # Pre-configuration activa
    # -----------------
    def preconfig(self, 
                  active_channels = [ 1,2,3,4],
                  scale = [ 100e-3, 100e-3, 100e-3, 100e-3 ], 
                  t_div = 10e-9,
                  t_delay = 20,
                  trigger_source = "CH1",
                  trigger_level  = 100e-3
                  ):
        """
        """
        # Cross-checks
        assert len(active_channels) == len(scale), "Scale numberss must be equal to channels"
        # Enable the channels for DATA subsystem and other configuration
        for i,ch in enumerate(active_channels):
            self.write(f'SELect:CH{ch} ON')
            # Position, scale and coupling
            self.write(f'CH{ch}:SCAle {scale[i]}')
            self.write(f'CH{ch}:POSition 0')
            self.write(f'CH{ch}:COUPling DC')
            self.write(f'CH{ch}:BANdwidth FULL')
        
        # Select the horizontal time base (time per division),
        # Remember the scope has 10 divisions: total scale: 10 x t_div
        self.write(f'HORizontal:SCAle {t_div}')
        # The trigger delay
        self.write(f'HORizontal:POSition {t_delay}')

        # Trigger group configuration
        self.write("TRIGger:A:MODE NORMAL")
        # USe A: main trigger (B trigger is secondary optional, used in advanced modes)
        self.write(f"TRIGger:A:EDGE:SOUrce {trigger_source}")
        # Not sure what slope we should be using from the TLU?
        self.write("TRIGger:A:EDGE:COUPling DC")
        self.trigger_level = trigger_level
        self.trigger_slope = "RISE"
        logger.info(f'Pre-configuration with [TB FILLED]')

    # ------------------------
    # Configuration FastFrame
    # ------------------------
    def configure_fastframe_acq(self, 
                                record_length: int =2500, 
                                bpp: int = 2, 
                                n_frames: int = 1000, 
                                trigger_source: str = "EXT"):
        """Configure the oscilloscope to acquire N-frames in fastFrame mode

        Parameters
        ----------
        record_lenght: int
            The number of points of the waveforms
        bpp: int
            Bytes per points 
        n_frames: int
            The number of frames to be obtained
        trigger_source: str
            The trigger source [CHannel or EXT]
        """
        logger.info(f"Configure FastFrame: RL={record_length}, bytes_per_point={bpp}, Frames={n_frames}, Trigger source: {trigger_source}")
        self.write("ACQuire:STATE OFF")
        self.write("ACQuire:MODE SAMPLE")
        # Number of points
        self.record_length = int(record_length)
        self.write(f"HORizontal:MODE:RECOrdlength {self.record_length}")
        # Be sure the same data length is provided with curve?
        self.write(f"DATA:START 1")
        self.write(f"DATA:STOP {self.record_length}")
        # The right-hand, signed binary (2 bytes MSB
        self.write("DATa:ENCdg RIBinary")
        # Enabling fast frame
        self.write("HORizontal:FASTframe:STATE ON")
        # Number of frames to be acquired
        self.n_frames = n_frames
        self.write(f"HORizontal:FASTframe:COUNt {self.n_frames}")
        # The number of bytes per point
        self.write(f"WFMOutpre:BYT_Nr {bpp}")
        # The oscilloscope will start to acquire as soon as possible 
        # (for instance, after a CURVE?, just when finish)
        self.write("ACQuire:STOPAfter RUNSTOP")
        # display streaming off to increase speed
        self.write("DISPLAY:WAVEFORM OFF")
        # The trigger configuration  (wait for a regular trigger event)
        self.dev.write("TRIGger:A:MODE NORMAL")
        # The trigger source (just Channel or eexternal)
        # USe A: main trigger (B trigger is secondary optional, used in advanced modes)
        self.dev.write(f"TRIGger:A:EDGE:SOUrce {trigger_source}")
        # Not sure what slope we should be using from the TLU?
        self.dev.write("TRIGger:A:EDGE:SLOpe RISE")
        self.dev.write("TRIGger:A:EDGE:COUPling DC")
        # The level: ECL --> -1.3 Volts, TTL --> 1.4 Volt ,or a  number 
        self._trigger_aux_level = "TTL"
        self.dev.write(f"TRIGger:AUXLevel {self._trigger_aux_level}")
        # The DATA to be sent??  XXX
        # self.dev.write(f"DATa:START {int(record_start)}")
        # self.dev.write(f"DATa:STOP {int(record_stop)}")
        logger.debug("FastFrame configuration sent and ready...")

    def simple_conf(self,
                    record_length: int =2500, 
                    bpp: int = 2, 
                    trigger_source: str = "EXT",
                    trigger_mode: str = "NORMAL"):
        """Configure the oscilloscope to perform a single acquisition in SAMPLE mode
        using as trigger `source`. 

        Parameters
        ----------
        record_lenght: int
            The number of points of the waveforms
        bpp: int
            Bytes per points 
        trigger_source: str
            The trigger source [CHannel or EXT]
        trigger_source: str
            The trigger mode NORMAL (wait for a valid trigger event), AUTO (generates a trigger after a while)
        """
        logger.info(f"Configure: RL={record_length}, bytes_per_point={bpp}, Trigger mode: {trigger_mode}, Trigger source: {trigger_source}")
        self.write("ACQuire:STATE OFF")
        self.write("ACQuire:MODE SAMPLE")
        # Number of points
        self.record_length = int(record_length)
        self.write(f"HORizontal:MODE:RECOrdlength {self.record_length}")
        # The right-hand, signed binary (2 bytes MSB
        self.write("DATa:ENCdg RIBinary")
        # disabling fast frame and FastAcq (just in case)
        self.write("HORizontal:FASTframe:STATE OFF")
        self.write("ACQuire:FASTAcq:STATE OFF")
        # The number of bytes per point
        self.write(f"WFMOutpre:BYT_Nr {bpp}")
        # Captures exactly 1 shot? defined with countp
        self.write("ACQuire:STOPAfter SEQUENCE")
        # The trigger configuration 
        # The trigger source (just Channel or eexternal)
        # USe A: main trigger (B trigger is secondary optional, used in advanced modes)
        self.dev.write(f"TRIGger:A:MODE {trigger_mode}")
        self.dev.write(f"TRIGger:A:EDGE:SOUrce {trigger_source}")
        # If not aux --> 
        #self.dev.write(f"TRIGger:A:LEVel:CH{trigger_source} ")
        # ---> 
        # Not sure what slope we should be using from the TLU?
        self.dev.write("TRIGger:A:EDGE:SLOpe RISE")
        self.dev.write("TRIGger:A:EDGE:COUPling DC")
        logger.debug("FastFrame configuration sent and ready...")
    
    # XXX FIXME
    #def configure_stream_acq(self, record_length: int=10000):
    #    """Configure the oscilloscope to acquire in continous stream,
    #    sending data as soons as arrive
    #    """
    #    self.write("ACQUIRE:STATE OFF")
    #    self.write("DATa:SOUrce CH1")
    #    self.write("DATa:ENCdg RIBinary")
    #    self.write("CURVESTREAM:STATE " --> NO!!
    #    self.write(f"HORIZONTAL:RECORDLENGTH {int(record_length)}")
    #    # Number of frames to be acquired
    #    self.write(f"HORizontal:FASTframe:COUNt {n_frames}")
    #    XXX WIP

    #    self.write(f"ACQUIRE:MODE {sample_mode}")
    #    self.write("ACQUIRE:STOPAFTER SEQUENCE")
    #    # display streaming off to increase speed
    #    self.write("DISPLAY:WAVEFORM OFF")

    def arm_acquisition(self):
        """Start acquisition
        """
        self.clear_busy()
        self.write("ACQuire:STATE ON")

    def wait_complete(self):
        """
        Wait until the acquisition sequence finishes using the OPC call

        """
        # TRICK to momentanously stop receiving external triggers
        # immediately when the last frame was received
        # Change to 5.0 volts which make no sense
        #self.dev.write("*OPC;TRIGger:AUXLevel 5.0")
        _ = self.query('*OPC?')
        self.send_busy()
        # Revert back the trigger
        #self.dev.write(f"TRIGger:AUXLevel {self._trigger_aux_level}")
    
    # -------------------
    # Waveform fetch
    # -------------------
    def read_channel(
            self,
            channel: int,
            frame: int
            ) -> bytes:
        """
        Read one FastFrame segment for one channel and returns the bytes
        without any processing

        Parameters
        ----------
        channel : int
            Channel number (1-based).
        frame: int
            The frame to extract

        Return
        ------
        bytes: The waveform
        """
        self.dev.write(f"DATa:SOUrce CH{int(channel)}")
        self.ctrl.write(f"DATa:START 1")
        self.ctrl.write(f"DATa:STOP {self.record_length}")
        self.ctrl.write(f"DATa:FRAMESTART {1}")
        self.ctrl.write(f"DATa:FRAMESTOP {self.n_frames}")

        return self.dev.query_binary("CURVe?")

    def read_frame_channel(
            self,
            channel: int,
            frame: int
            ) -> bytes:
        """
        Read one FastFrame segment for one channel and returns the bytes
        without any processing

        Parameters
        ----------
        channel : int
            Channel number (1-based).
        frame: int
            The frame to extract

        Return
        ------
        bytes: The waveform
        """
        self.write(f"DATa:SOUrce CH{int(channel)}")
        self.write(f"DATa:START 1")
        self.write(f"DATa:STOP {self.record_length}")
        self.write(f"DATa:FRAMESTART {frame}")
        self.write(f"DATa:FRAMESTOP {frame}")
        
        # FIXME -- query_binary o query solo
        return self.query_binary("CURVe?")

    def read_frame_channel_numpy(
            self,
            channel: int,
            frame: int,
            ) -> np.ndarray:
        """
        Read one FastFrame segment for one channel.

        Returns the raw integer samples (int8 or int16).

        Parameters
        ----------
        channel : int
            Channel number (1-based).

        Return
        ------
        np.narray: The waveform
        """
        self.write(f"DATa:SOUrce CH{int(channel)}")
        self.write(f"DATa:START 1")
        self.write(f"DATa:STOP {self.record_length}")
        self.write(f"DATa:FRAMESTART {frame}")
        self.write(f"DATa:FRAMESTOP {frame}")
        
        pre = self.wf_preamble
        bytes_per_point = int(pre.get("BYT_NR",1))
        # Choose data type for query_binary_values (singedness depends on BN_FMT)
        # bpp = 1 --> signed 8-bit (B) then np.int8, bpp = 2 --> signed 16-bit (H), np.int16
        datatype = 'H' if bytes_per_point == 2 else 'B'
        dtype = np.int16 if bytes_per_point == 2 else np.int8
        try:
            # XXX--- is_bin_endian depening BYT_OR and BN_FMT
            data = self.dev.query_binary_values("CURVe?", datatype=datatype, container=list, header_fmt='ieee')
            return np.asarray(data, dtype=dtype)
        except Exception as e:
            logger.error("Error reading frame %d ch %d: %s", frame_index, channel, e)
            return np.zeros(0, dtype=dtype)

    @property
    def wf_preamble(self) -> Dict[str, str]:
        """Read and parse WFMOutpre? into a dictionary.
        """
        txt = self.dev.query("WFMOutpre?").split(';')
        self._wf_preamble = dict(zip(PREAMBLE_ORDERED_LIST,txt))        
        return self._wf_preamble

    # -------------------
    # Cleanup
    # -------------------
    def close(self):
        self.dev.close()
        if self.afg is not None:
            self.clear_busy()
            self.afg.close()
        self.rm.close()



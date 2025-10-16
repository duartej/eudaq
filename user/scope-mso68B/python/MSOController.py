"""
Controller for Tektronix 4/56/ Series MSO via SCPI (VISA)

https://www.tek.com/en/sitewide-content/manuals/4/5/6/4-5-6-series-mso-programmer-manual

2025-10-04, Jordi Duarte-Campderros (IFCA) 
duarte@ifca.unican.es
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
                         "WFID",   # String with acquisition parameter of the WF see DATA:SOUrce
                                   # --
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
WFID_FIELDS = [ "SOURCE", "COUPLING", "VERTSCALE", "HORIZSCALE","RECORDLENGTH", "ACQUISITIONMODE"]
                         

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
        """Class to remotely control a Tektronix Serie 4/5/6 MSO, and
        a simple Function generator (AFG) to act as busy signal when
        the scope is reading out.

        Parameters
        ----------
        resource_string: str
            The VISA string for the scope. Example "USB0::0x0699::0x0522::123456::INSTR"
            or "TCPIP::192.168.5.12::INSTR"
        timeout_ms: int 
            Read timeout in ms (adjust depending on record length)
        afg_resource: str
            The VISA string for the AFG
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
            # Fix maximum sample rate --> Automatic ???
            # --> Looks like this is not workingself.write('HORizontal:MAIN:SAMPLERate 50e9')
            # Any other? XXX
            # The time window per default: 20 ns (2 ns/div)
            self.target_window = 20e-9

        if afg_resource is not None:
            # Use the afg to generate a busy signal while reading
            # and processing data
            self.afg = self.rm.open_resource(afg_resource)
            self.afg.read_termination = '\r\n'
            self.afg.write_termination = '\n'
            self.afg._idn = self.afg.query("*IDN?")
            # Setup the AFG to send a DC of 1.1 Vpp for CH1
            self.afg.write('CHN1')
            self.afg.write('ARBDCOFFS 1.1')
            self.afg.write('ARBLOAD DC')
            self.afg.write('OUTPUT OFF')
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
        Note to re-enable channels (self.write('SELECT:CH<X>' or call preconfig)
        """
        self.write('*RST')
        # Wait to finish the reset
        _ = self.query('*OPC?')
        # Set a pre-defined configuration
        # And re-activate all channels
        #self.write(f':SELECT:CH1 ON;:SELECT:CH2 ON;:SELECT:CH3 ON;:SELECT:CH4 ON')
    
    # Some useful accessors 

    @property
    def sampling_rate(self):
        return float(self.query('HORizontal:SAMPLERate?'))
    
    @property
    def target_window(self):
        """The time window acquired
        """
        return self._target_window

    @target_window.setter
    def target_window(self, target_window):
        """It is linked with the record_length)
        """
        # Also place time division
        self.write(f'HORizontal:SCAle {target_window/10}')
        # Be sure it is possible (not all are available)
        self._target_window = 10*float(self.query('HORizontal:SCAle?'))
    
    @property
    def record_length(self):
        return int(self._target_window * self.sampling_rate)
    
    
    
    # TRIGGER group
    @property
    def trigger_edge_source(self):
        return self.query('TRIGger:A:EDGE:SOUrce?')

    @trigger_edge_source.setter
    def trigger_edge_source(self,source):
        self.write(f'TRIGger:A:EDGE:SOUrce {source}')

    @property
    def trigger_state(self):
        self._trigger_state = self.query('TRIGger:STATE?')
        return self._trigger_state

    def is_trigger_ready(self):
        return self._trigger_state == 'READY'

    @property
    def trigger_level(self):
        if self.trigger_edge_source == 'AUX':
            return self.query('TRIGger:AUXLevel?')
        else:
            return float(self.query(f'TRIGger:A:LEVel:{self.trigger_edge_source}?'))

    @trigger_level.setter
    def trigger_level(self, value):
        """ Note if AUX is the trigger_edge_source -> str (RISE, FALL or EITHER)
        """
        if self.trigger_edge_source == 'AUX':
            self.write(f'TRIGger:AUXLevel {value}')
        else:
            self.write(f'TRIGger:A:LEVel:{self.trigger_edge_source} {value}')
    
    @property
    def trigger_slope(self):
        return self.query('TRIGger:A:EDGE:SLOpe?')

    @trigger_slope.setter
    def trigger_slope(self, value):
        # valid RISE FALL and ? XXX 
        self.write(f'TRIGger:A:EDGE:SLOpe {value}')

    # Trigger settings
    def set_edge_trigger(self, 
                         trigger_source: str = 'CH1',
                         trigger_level: float = '1e-2', 
                         trigger_slope: str = 'RISE'):
        """Set the trigger to edge in mode Normal

        trigger_source: str
            The trigger source CH<X> or EXT for external
        trigger_level: float
            The trigger level
        trigger_slope: str
            Either RISE, FALL or EITHER
        """
        self.write("TRIGger:A:MODE NORMAL")
        # USe A: main trigger (B trigger is secondary optional, used in advanced modes)
        self.trigger_edge_source = trigger_source
        self.write("TRIGger:A:EDGE:COUPling DC")
        if trigger_source  == "AUX":
            # Trigger level to 1.4 TTL or -1.3 ECL, 
            # modify to provide the proper string
            trigger_level = "TTL" if trigger_level > 0 else "ECL"            
        self.trigger_level = trigger_level

        assert trigger_slope in [ "RISE", "FALL", "EITHER"], f"Wrong slope `{trigger_slope}`"
        self.trigger_slope = trigger_slope

        logger.info(f'Trigger edge configured. Source: {trigger_source}, slope: {trigger_slope}, level={self.trigger_level}')
    
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

    # Some useful config for horizontal and vertical
    # def set_horizontal_config(self)
    # def set_vertical_config(self)

    # Max-available frames
    @property
    def max_available_frames(self, safety_factor = 0.96):
        """Calculates the maximum number of frames taking into account:
            - the scope is able to get 62.5M points
            - assume the total number of points must be splitted between channels

        Parameters
        ----------
        safety_factor: float
            A value between [1 - 0( to be safe 
        """
        TOTAL_PTS = 62500000
        activated_channels = sum(map(lambda x: int(x), self.query('SELECT?').split(';')[4:]))
        if activated_channels < 1:
            logger.warning('No active channels. Ignore request')
            return 0
        pts_per_ch = TOTAL_PTS // activated_channels
        n_frames = int(pts_per_ch // self.record_length)
        n_frames_safe = int( n_frames * float(safety_factor) )

        # This is equivalent to HORizontal:FASTframe:MAXFRames? XXX 

        return n_frames_safe

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

    def query_binary(self, q: str):
        """Send the converted values (int, uint,..) from CURVE?. 

        Parameter
        ---------
        q: str
            Query command returning binary data

        Return
        ------
        -- whatever you ask for
        """
        pre = self.wf_preamble
        bytes_per_point = int(pre.get("BYT_NR",1))
        big_endian = (pre.get("BYT_OR") == "MSB")
        datatype = 'h' if bytes_per_point == 2 else 'b'

        # XXX -- CLEAN DATA AFTERREADER?
        return self.dev.query_binary_values(q, datatype=datatype, is_big_endian=big_endian)

    def split_raw_data(self, blob: bytes): 
        """Split a IEEE-488.2 block (#<nd><len><payload>\n) from raw
        data of teh oscilloscope and parses them to return only the payload in bytes.

        Parameters
        ----------
        blob: bytes
            The raw blocks 

        Return
        ------
        list(bytes): list of payloads 
        """
        payloads = []
        i = 0
        L = len(blob)

        while i < L:
            if blob[i:i+1] != b'#':
                raise ValueError(f'No header `#` in offset {i}')
            i += 1

            # Parse the header
            nd = int(blob[i:i+1].decode('ascii'))
            i += 1
            n = int(blob[i:i+nd].decode('ascii'))
            i += nd

            # Ready to get the payload
            payload = blob[i:i+n]
            payloads.append( payload )
            i += n

            # Consume terminator
            if i < L and blob[i:i+1] == b'\r':
                i += 1
            if i < L and blob[i:i+1] == b'\n':
                i += 1
            if i < L and blob[i:i+1] == b';':
                i += 1

        return payloads

    def recover_io(self):
        from pyvisa import constants as vc
        self.dev.clear()
        self.write('*CLS')
        _ = self.query('*ESR?')

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
    def read_esr(self):
        """The *ESR? (Standard Event Status Register)"""
        return self.query("*ESR?")

    # ------------------------
    # Pre-configuration activa
    # ------------------------
    def preconfig(self, 
                  active_channels = [ 1,2,3,4],
                  scale = [ 100e-3, 100e-3, 100e-3, 100e-3 ], 
                  target_window = None, 
                  trigger_position = 30, 
                  trigger_source = "CH1",
                  trigger_level  = 100e-3,
                  bpp: int = 2, 
                  ):
        """
        Parameters
        ----------
        active_channels: list(int)
        scale: list(float)
        target_window: float
            The horizontal window we want to record [s]
        # Not important in fact... this is just for the display
        t_div: float
            The time per division
        t_delay: int
            The percentage where the
        """
        # Update target window, if None use current value
        if target_window is not None:
            self.target_window = target_window

        # Cross-checks
        assert len(active_channels) == len(scale), "Scale numberss must be equal to channels"
        # Enable the channels for DATA subsystem and other configuration
        for i,ch in enumerate(active_channels):
            self.write(f'SELect:CH{ch} ON')
            # Position, scale and coupling
            self.write(f'CH{ch}:SCAle {scale[i]}')
            self.write(f'CH{ch}:POSition 0')
            self.write(f'CH{ch}:COUPling DC')
            self.write(f'CH{ch}:TERMINATION 50')
            self.write(f'CH{ch}:BANdwidth FULL')
        
        # Define the acquisition window (assuming trigger=t0?)
        # The window is defined by (number of points)/sampling_Frequency
        # -->  Note, once defined target_window and sampling rate, record_length 
        #      is linked
        self.write(f"HORizontal:MODE:RECOrdlength {self.record_length}")
        self.write(f"HORizontal:POSition {trigger_position}")

        # FOR THE DISPLAY  
        # Select the horizontal time base (time per division),
        # Remember the scope has 10 divisions: total scale: 10 x t_div
        # USe the target_window: 
        # t_div = self.target_window/10 --? Automaticall in target_window
        # self.write(f'HORizontal:SCAle {t_div}')
        # The trigger delay ?? 
        # self.write(f'HORizontal:POSition {t_delay}')

        self.write("ACQuire:STATE OFF")
        self.write("ACQuire:MODE SAMPLE")

        # Data -related
        # The right-hand, signed binary (2 bytes MSB
        self.write("DATa:ENCdg RIBinary")
        # The number of bytes per point
        self.write(f"WFMOutpre:BYT_Nr {bpp}")

        # disabling fast frame and FastAcq (just in case)
        self.write("HORizontal:FASTframe:STATE OFF")
        self.write("ACQuire:FASTAcq:STATE OFF")
        # Captures exactly 1 shot? defined with countp?
        self.set_acquisition_sequence()
        # The trigger configuration 
        self.set_edge_trigger(trigger_source=trigger_source, trigger_level=trigger_level, trigger_slope="RISE")
        
        logger.info(f"Configure: Active channels={active_channels}, Vertical scale={scale} V, Time division={self.target_window/10} s")
        logger.info(f"Configure: Acquire time window={self.target_window} [s], bytes_per_point={bpp}")

    # ------------------------
    # Configuration FastFrame
    # ------------------------
    def configure_fastframe_acq(self, 
                                target_window: float = 200e-9, 
                                bpp: int = 2, 
                                n_frames: int = 1000, 
                                trigger_source: str = "EXT"):
        """Configure the oscilloscope to acquire N-frames in fastFrame mode

        Parameters
        ----------
        target_window: float
            The time window to acquire the points 
        bpp: int
            Bytes per points 
        n_frames: int
            The number of frames to be obtained
        trigger_source: str
            The trigger source [CHannel or EXT]
        """
        logger.info(f"Configure FastFrame: TimeWindow={target_window}, bytes_per_point={bpp}, Frames={n_frames}, Trigger source: {trigger_source}")
        self.write("ACQuire:STATE OFF")
        self.write("ACQuire:MODE SAMPLE")
        # Number of points
        self.target_window = target_window
        # Defined target_window -> record_length
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
        # (for instance, after a CURVE?, just when finish) --> BUT
        #self.set_acquisition_continous()
        self.set_acquisition_sequence()
        # display streaming off to increase speed
        ### PROV-XXX self.write("DISPLAY:WAVEFORM OFF")
        # The trigger configuration  (wait for a regular trigger event)
        # Note per default trigger_level= 1e-2 (TTL if AUX source) and slope=RISE
        self.set_edge_trigger(trigger_source=trigger_source)
        # The DATA to be sent??  XXX
        # self.dev.write(f"DATa:START {int(record_start)}")
        # self.dev.write(f"DATa:STOP {int(record_stop)}")
        logger.debug("FastFrame configuration sent and ready...")

    # --------------------
    # Acquisition related
    # --------------------
    def set_acquisition_sequence(self):
        """After take the number of Counted waveform stop acquisition
        (single sequence adquisition)
        """
        self.write("ACQuire:STOPAfter SEQUENCE")

    def set_acquisition_continous(self):
        """The data is continously taken
        """
        self.write("ACQuire:STOPAfter RUNSTop")

    
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
        # Revert back the trigger?
    
    # -------------------
    # Waveform fetch
    # -------------------
    def read_channel(
            self,
            channel: int,
            ) -> bytes:
        """
        Read all one channel and returns the bytes
        without any processing

        Parameters
        ----------
        channel : int
            Channel number (1-based).

        Return
        ------
        bytes: The waveform
        """
        self.write(f"DATa:SOUrce CH{int(channel)}")
        self.write(f"DATa:START 1")
        self.write(f"DATa:STOP {self.record_length}")
        
        self.write('CURVE?')
        return self.dev.read_raw()

    def read_all_frame_channel(
            self,
            channel: int,
            ) -> bytes:
        """
        Read all FastFrame segments for one channel and returns the bytes
        without any processing

        Parameters
        ----------
        channel : int
            Channel number (1-based).

        Return
        ------
        bytes: The waveform
        """
        self.write(f"DATa:SOUrce CH{int(channel)}")
        self.write(f"DATa:START 1")
        self.write(f"DATa:STOP {self.record_length}")
        self.write(f"DATa:FRAMESTART 1")
        self.write(f"DATa:FRAMESTOP {self.n_frames}")
        
        self.write('CURVE?')
        return self.dev.read_raw()


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
        
        self.write('CURVE?')
        return self.dev.read_raw()

    #def read_frame_channel_numpy(
    #        self,
    #        channel: int,
    #        frame: int,
    #        ) -> np.ndarray:
    #    """
    #    Read one FastFrame segment for one channel.

    #    Returns the raw integer samples (int8 or int16).

    #    Parameters
    #    ----------
    #    channel : int
    #        Channel number (1-based).

    #    Return
    #    ------
    #    np.narray: The waveform
    #    """
    #    self.write(f"DATa:SOUrce CH{int(channel)}")
    #    self.write(f"DATa:START 1")
    #    self.write(f"DATa:STOP {self.record_length}")
    #    self.write(f"DATa:FRAMESTART {frame}")
    #    self.write(f"DATa:FRAMESTOP {frame}")
    #    
    #    # XXX -- Nota que directamente es posbile obtener numpySS s
    #    # https://pyvisa.readthedocs.io/en/1.8/rvalues.html
    #    pre = self.wf_preamble
    #    bytes_per_point = int(pre.get("BYT_NR",1))
    #    # Choose data type for query_binary_values (singedness depends on BN_FMT)
    #    # bpp = 1 --> signed 8-bit (B) then np.int8, bpp = 2 --> signed 16-bit (H), np.int16
    #    datatype = 'H' if bytes_per_point == 2 else 'B'
    #    dtype = np.int16 if bytes_per_point == 2 else np.int8
    #    try:
    #        # XXX--- is_bin_endian depening BYT_OR and BN_FMT
    #        data = self.dev.query_binary_values("CURVe?", datatype=datatype, container=list, header_fmt='ieee')
    #        return np.asarray(data, dtype=dtype)
    #    except Exception as e:
    #        logger.error(f"Error reading frame {frame} ch {channel}: {e}")
    #        return np.zeros(0, dtype=dtype)

    @property
    def wf_preamble(self) -> Dict[str, str]:
        """Read and parse WFMOutpre? into a dictionary.
        """
        if not hasattr(self,'_wf_preamble'):
            self._wf_preamble = {}
        txt = self.dev.query("WFMOutpre?").split(';')
        for i,key in enumerate(PREAMBLE_ORDERED_LIST):
            # special key on 6, if exist XXX -- FIX ME existance
            if key == "WFID":
                wfid_dict = {}
                for k, key_wfid in enumerate(WFID_FIELDS):
                    wfid_dict[key_wfid] = txt[i].split(',')[k]
                value = wfid_dict
            else:
                value = txt[i]
            self._wf_preamble[key] = value
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



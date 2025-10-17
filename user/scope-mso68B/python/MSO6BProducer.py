#!/usr/bin/env python3
"""
MSO6BProducer: EUDAQ  producer for Series 4/5/6 MSO Tektronix oscilloscopes
Uses the `MSOController.py` module

author: Jordi Duarte-Campderros, duarte@ifca.unican.es (IFCA, CSIC/UC)
date: 2025-10-08
"""

import threading
import queue
import time
import logging
import sys
from typing import List, Dict
import click
import ast

import numpy as np

from MSOController import MSOController

import pyeudaq
from pyeudaq import EUDAQ_INFO, EUDAQ_ERROR


def exception_handler(method):
    def inner(*args, **kwargs):
        try:
            return method(*args, **kwargs)
        except Exception as e:
            EUDAQ_ERROR(str(e))
            raise e
    return inner


# Configuration
INIT_PARAMETERS = {
        "resource": dict(
            default = "TCPIP::192.168.5.12::INSTR",
            type = str
            ),
        "afg_resource": dict(
            default = "ASRL/dev/ttyACM0::INSTR",
            type = str
            ),
        "timeout_ms": dict(
            default = 20000,
            type = int
            ),
        }

CONFIG_PARAMETERS = {
        "bytes_per_point": dict(
            # 1 (8-bit) or 2 (16-bit)
            default = 2,
            type = int,
            ),
        "dut_dict": dict(
            # list of dut with it's channels and the list
            # of pixels bonded to that channel
            default = { 'DUT1': {
                1: [(0,0)], 
                2: [(0,0)],
                3: [(0,0)],
                4: [(0,0)],
                },
                       },
            type = dict,
            ),
        "n_frames": dict(
            # frames expected per spill, adjust to R* T_spill
            default = 3000,
            type = int
            ),
        # XXX TO BE DEPRECATED?
        "queue_maxsize": dict(
            # max number of queue items (frame, channel tuples)
            default = 20000,
            type = int
            ),
        # XXX TO BE DEPRECATED?
        "post_acq_processing_s" : dict(
            default = 0.03,
            type = float
            ),
        "scale_V": dict(
            default = [ 100e-3, 100e-3, 100e-3, 100e-3],
            type = list
            ),
        "target_window_s": dict(
            # The total width of the acquisition window in s.
            default = 50e-9,
            type = float
            ),
        "t_delay": dict(
            default = 20,
            type = float
            ),
        "log_level": dict(
            default = logging.INFO,
            type = int 
            ),
        }

def parse_config(obj, config_schema, external_conf):
    """Obtain the configuration parameters

    Parameters
    ---------
    config_schema: TODO
    external_config:  TODO
    """
    for param_name, param_meta in config_schema.items():
        try:
            received_param = ast.literal_eval(external_conf[param_name])

        except KeyError:
            # No presence, then use default
            received_param = param_meta.get("default")

        # Evaluate as python type
        t = param_meta.get("type")
        if type(received_param) is not t:
            EUDAQ_ERROR(f"The parameter {param_name} must be of type `{t}`: got {type(received_param)}")

        # Add the parameter to the class
        setattr(obj,param_name, received_param)


# ----------------------------
# Logging
# ----------------------------
logger = logging.getLogger("MSO6BProducer")
logger.setLevel(logging.INFO)
hd = logging.StreamHandler(sys.stdout)
hd.setLevel(logging.INFO)
fmt = logging.Formatter("%(asctime)s [%(levelname)s] %(message)s")
hd.setFormatter(fmt)
logger.addHandler(hd)

# ----------------------------
# Threads
# ----------------------------
class FrameReader(threading.Thread):
    """Read frame (frames x channels) from the scope and puts tuples into the shared queue.
    Each item pushed: (frame_idx, channel, payload_bytes, meta_dict)
    When done, puts a sentinel None into the queue to indicate end-of-burst.
    """

    def __init__(self, producer: pyeudaq.Producer):
        # Initialize 
        super().__init__(daemon=True)
        self.producer = producer
        self.ctrl = self.producer.ctrl
        self.ctrl_lock = self.producer.ctrl_lock
        self.queue = self.producer.data_q
        self.n_frames = self.producer.n_frames
        self.channels = self.producer.channels
        self._stop = threading.Event()
        # preamble cache per channel
        self.preamble = {}

    def stop(self):
        self._stop.set()

    def run(self):
        """The actual reading
        """
        logger.info("FrameReader starting: reading %d frames x channels %s", self.n_frames, self.channels)
        # Read preambles per channel (if scaling)
        while self.producer._running:
            # wait for scope to finish capturing all frames
            with self.ctrl_lock:
                self.ctrl.wait_complete()
            # Start data dump
            logger.info("Frame acquisition complete, buffering out data from oscilloscope")
            # Extract and dump-frames from the scope iterate frame
            for ch in self.channels:
                with self.ctrl_lock:
                    # no conversion, just binary , it returns a list (a raw binary data per frame)
                    raw_data = self.ctrl.read_all_frame_channel(ch)
                    if len(raw_data) == 0:
                        logger.warning(f"Empty read for CH-{ch}")
                    # FIXME -- Check the record length?
                    # put into queue (block if full
                    # XXX ?? self.queue.put((0, ch, raw_data.tobytes()))
                    # Send the whole channel data (all n-frames)
                    self.queue.put((0, ch, raw_data), block=True)
            self.queue.put(None)
            logger.info("FrameReader finished and placed sentinel in queue.")
            # Arm again
            self.producer.arm_acquisition()
            self.ctrl.clear_busy()
        logger.info("FrameReader run finished...")

    #def run(self):
    #    """The actual reading
    #    """
    #    logger.info("FrameReader starting: reading %d frames x channels %s", self.n_frames, self.channels)
    #    # Read preambles per channel (if scaling)
    #    while self.producer._running:
    #        # wait for scope to finish capturing all frames
    #        with self.ctrl_lock:
    #            self.ctrl.wait_complete()
    #        # Start data dump
    #        logger.info("Frame acquisition complete, buffering out data from oscilloscope")
    #        # Extract and dump-frames from the scope iterate frame
    #        for frame in range(1, self.n_frames + 1):
    #            if self._stop.is_set():
    #                logger.info(f"FrameReader stopping early at frame {frame}")
    #                break
    #            for ch in self.channels:
    #                try:
    #                    with self.ctrl_lock:
    #                        # no conversion, just binary 
    #                        raw_data = self.ctrl.read_frame_channel ch, frame)
    #                    if len(raw_data) == 0:
    #                        logger.warning(f"Empty read for frame-%{frame} in CH-{ch}")
    #                    # FIXME -- Check the record lenght?
    #                    # put into queue (block if full)
    #                    self.queue.put((frame, ch, raw_data), block=True)
    #                except Exception as e:
    #                    logger.exception(f"Error reading frame-{frame} in CH-{ch}: {e}")
    #                    # On error, continue or optionally push an error marker
    #            # signal end-of-burst
    #            self.queue.put(None)
    #            logger.info("FrameReader finished and placed sentinel in queue.")
    #            # Activate again the triggers, note in RUNSTOP mode the acquisition will resume immediately
    #            # XXX In SEQUENCE mode, you need the self.producer.arm_acquisition
    #            self.ctrl.clear_busy()

    #    logger.info("FrameReader run finished...")

class EudaqEventSender(threading.Thread):
    """Consumes queue items and builds EUDAQ events per frame.  
    Bundles channels for each frame into a single event.
    """
    def __init__(self, producer: pyeudaq.Producer):
        super().__init__(daemon=True)
        self.producer= producer
        self.queue = producer.data_q
        self.channels = producer.channels
        self._stop = threading.Event()
        # frame_idx -> {ch: raw_data}
        self.framebuf = {} 

    def stop(self):
        self._stop.set()

    def run(self):
        """
        """
        logger.debug("EudaqEventSender started.")
        while self.producer._running and self._stop:
            item = self.queue.get()
            if item is None:
                logger.info("EudaqEventSender got sentinel; finishing.")
                self._flush_remaining()
                self.queue.task_done()
                break
            frame_idx, ch, raw_data_blob = item
            # Convert into frame payloads
            raw_data_list = self.producer.ctrl.split_raw_data(raw_data_blob)
            # Check the expected number of n-frames
            if len(raw_data_list) != self.producer.n_frames:
                logger.warning(f"Expected {self.producer.n_frames} frames, got {len(raw_data_list)}")

            # Let's build all the data from frame idx. Need to obtain all channels
            for i in range(self.producer.n_frames):
                frame_idx = i + 1
                if (frame_idx) not in self.framebuf:
                    self.framebuf[frame_idx] = {}
                self.framebuf[frame_idx][ch] = raw_data_list[i]
                # if we have all channels for this frame, send the event, if 
                # not just next iteration, it should be in the queue
                if all(c in self.framebuf[frame_idx] for c in self.channels):
                    try:
                        self._send_frame_event(self.framebuf[frame_idx])
                    except Exception as e:
                        logger.exception(f"Failed to send event for frame-{frame_idx}: {e}")
                    del self.framebuf[frame_idx]
                    self.queue.task_done()
        logger.debug("EudaqEventSender exiting.")

    def _send_frame_event(self, frame_data: Dict[int, bytes]):
        """Build and send one EUDAQ event corresponding to frame_idx.
        The EUDAQ API varies by installation â€” adapt these lines to your setup.

        Parameters
        ----------
        frame_data: dict(int, bytes)
            The binary raw data spit by channels
        """
        # Create RawDataEvent and add blocks per channel
        ev = pyeudaq.Event("RawEvent","MSO6B")
        ev.SetTriggerN(self.producer.n_trigger)
        if self.producer.n_trigger == 0:
            ev.SetBORE()
            ev.SetTag('producer_name', str(self.producer._name))
            ev.SetTag('duts_info', self.producer.duts_info)
            ev.SetTag('dt', str(self.producer.wf_preamble[1]["XINCR"]))
            ev.SetTag('t0', str(self.producer.wf_preamble[1]["XZERO"]))
            ev.SetTag('sampled_points', str(self.producer.record_length))
            for ch in self.producer.channels:
                ev.SetTag(f'channel{ch}_dv', str(self.producer.wf_preamble[ch]["YMULT"]))
                ev.SetTag(f'channel{ch}_v0', str(self.producer.wf_preamble[ch]["YZERO"]))
                ev.SetTag(f'channel{ch}_voffset', str(self.producer.wf_preamble[ch]["YOFF"]))

        # AddBlock expects bytes; label it by channel
        for ch,raw_data in sorted(frame_data.items()):
            ev.AddBlock(ch, raw_data)
        # Update trigger 
        self.producer.n_trigger += 1 
        # Send event
        self.producer.SendEvent(ev)


    def _flush_remaining(self):
        # attempt best-effort send for incomplete frames
        if not self.framebuf:
            return
        logger.warning(f"Flushing {len(self.framebuf)} incomplete frames")
        for fidx in sorted(self.framebuf.keys()):
            try:
                self._send_frame_event(self.framebuf[fidx])
            except Exception:
                logger.exception("Error flushing frame-{fidx}")


class MSO6BProducer(pyeudaq.Producer):
    """
    """
    def __init__(self, name, runctrl):
        pyeudaq.Producer.__init__(self, name, runctrl)
        # Acquiring the controller lock
        self.ctrl_lock = threading.Lock()
        self.ctrl = None
        self.data_q = None #queue.Queue(maxsize=cfg.QUEUE_MAXSIZE)
        self.reader =  None
        self.eudaq_sender = None
        self._running = False
        self.wf_preamble = {}
    
    @exception_handler
    def DoInitialise(self):
        """
        """
        EUDAQ_INFO("MSO6B: Initializing..")
        initconf = self.GetInitConfiguration().as_dict()
        # Initialize the oscilloscope
        parse_config(self, INIT_PARAMETERS, initconf)
        # Start and connect the controller
        self.ctrl = MSOController(resource_string = self.resource, timeout_ms = None, afg_resource =  self.afg_resource)
        EUDAQ_INFO("MSO6B: Initialized..")

    
    @exception_handler
    def DoConfigure(self):
        EUDAQ_INFO("MSO6B: Configuring...")
        conf = self.GetConfiguration().as_dict()
        # Update the class with the mandatory configuration
        parse_config(self, CONFIG_PARAMETERS, conf)
        # Build the channels data member and prepare
        # the string CVS to be sent to the BORE
        self.duts_info = ''
        # Note that the channels have to be used only once, otherwise 
        # something was wrongly written in config
        for dutname,channel_dict in self.dut_dict.items():
            self.duts_info += f'{dutname};'
            for ch, pixellist in channel_dict.items():
                if ch in self.channels:
                    EUDAQ_ERROR(f'Configuration error: The CH{ch} has already been assigned!')
                self.channels.append( ch )
                self.duts_info += f'{ch};'
                self.duts_info += ",".join(f"{col},{row}" for col, row in pixellist)
        
        logger.setLevel(self.log_level)

        # Obtain the size of a block per frame (we have this info after config)
        ## --> self.frame_size = self.record_length * self.bytes_per_point
        # controller methods are synchronous; protect them with visa_lock
        with self.ctrl_lock:
            #Always in a known state
            self.ctrl.reset()
            # Need to configure the basic 
            self.ctrl.preconfig(
                    active_channels = self.channels,
                    scale = self.scale_V,
                    target_window = self.target_window_s,
                    trigger_position = self.t_delay, 
                    bpp = self.bytes_per_point)
            time.sleep(0.01)
            # And configure the fastframe
            self.ctrl.configure_fastframe_acq(n_frames=self.n_frames)
            # Trigger config
            self.ctrl.set_edge_trigger(trigger_source="EXT", trigger_level=0.5, trigger_slope="FALL")
            # Check is ready
            self.ctrl.is_trigger_ready()
            # Set the preamble (to extract conversion factors, etc...)
            for ch in self.channels:
                    self.ctrl.write(f"DATa:SOUrce CH{ch}")
                    self.wf_preamble[ch] = self.ctrl.wf_preamble
        logger.info("Scope configured.")

    @exception_handler
    def DoStartRun(self):
        with self.ctrl_lock:
            self.ctrl.arm_acquisition()
        self._running = True

    @exception_handler
    def DoStopRun(self):
        """
        Stop ongoing threads and close the scope.
        """
        logger.info("Stopping producer.")
        
        self._running = False
        
        if self.reader is not None and self.reader.is_alive():
            self.reader.stop()
            self.reader.join(timeout=1.0)
        if self.eudaq_sender is not None and self.eudaq_sender.is_alive():
            self.eudaq_sender.stop()
            self.eudaq_sender.join(timeout=1.0)
        try:
            with self.ctrl_lock:
                self.ctrl.clear_busy()
                self.ctrl.dev.clear()
                self.ctrl.clear_status()
        except Exception:
            pass

        logger.info("Producer stopped.")

    @exception_handler
    def DoReset(self):
        if self.ctrl is not None:
            self.ctrl.close()
            self.ctrl = None
        self._running = False

    @exception_handler
    def DoLoop(self):
        """
        """
        self.n_trigger = 0
        # queue.Queue(maxsize?) XXX
        self.data_q = queue.Queue()
        
        # create sender thread (consumer) first so it is ready
        self.eudaq_sender = EudaqEventSender(self)
        self.eudaq_sender.start()

        # Then acquisition of the frames 
        # create and start reader (producer) thread
        self.reader = FrameReader(producer =self)
        self.reader.start()
        

        # Wait for reader and sender to finish before stopping the run
        logger.info("Waiting for reader thread to finish (this may take time depending on data volume).")
        self.reader.join()
        logger.info("Reader finished. Waiting for sender to finish.")
        self.eudaq_sender.join()
        logger.info("Burst processing (read+send) finished.")

@click.command()
@click.option('-n','--name', default='scope_mso6b',
              help='Name for the producer (default "scope_mso6b")')
@click.option('-r','--runctrl',default='tcp://localhost:44000',
              help='Address of the run control, (default: "tcp://localhost:44000)"')
def main(name,runctrl):
    producer = MSO6BProducer(name,runctrl)
    EUDAQ_INFO(f"[MSO6BProducer]: Connecting to runcontrol in {runctrl} ...")
    producer.Connect()
    time.sleep(2)
    print('[MSO6BProducer]: Connected')
    while(producer.IsConnected()):
        time.sleep(1)

if __name__ == "__main__":
    main()

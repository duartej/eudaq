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

import numpy as np

### Need extern as path to import MSOCOntroller?
from MSOCController import MSOController

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
        "timeout_ms": dict(
            default = 20000,
            type = int
            ),
        }
CONFIG_PARAMETERS = {
        "record_length": dict(
            # samples per frame
            default = 10000,
            type = int
            ), 
        "bytes_per_point": dict(
            # 1 (8-bit) or 2 (16-bit)
            default = 2,
            type = int,
            ),
        "channels": dict(
            # list of channels to read
            default = [1, 2, 3, 4],
            ),
        "n_frames": dict(
            # frames expected per spill, adjust to R* T_spill
            default = 3000,
            type = int
            )
        "queue_maxsize": dict(
            # max number of queue items (frame, channel tuples)
            default = 20000,
            type = int
            ),
        "post_acq_processing_s" : dict(
            default = 0.03,
            type = float
            )
        "log_level": dict(
            default = logging.INFO
            )
        }

def parse_configs(class_to_decorate, config_dict, external_conf):
    """
    Parameters
    ---------
    """
    for param_name, param_dict in config_dict.items():
        try:
            received_param = external_conf[param_name]
        except KeyError:
            # No presence, then use default
            received_param = param_dict.get("default")

        # Convert to the proper data type
        try:
             param_value = param_dict['type'](received_param)
        except KeyError:
            pass
        except Exception as e:
            EUDAQ_ERROR(f"The parameter {param_name} must be of type `{param_dict['type']"})

        # Add the parameter to the class
        setattr(class_to_decorate,param_name, param_value)


# ----------------------------
# Logging
# ----------------------------
logger = logging.getLogger("TekScopeProducer")
logger.setLevel(CONFIG.LOG_LEVEL)
hd = logging.StreamHandler(sys.stdout)
hd.setLevel(CONFIG.LOG_LEVEL)
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

    def __init__(self, controller: MSOController, 
                 controller_lock: threading.Lock,
                 data_queue: queue.Queue, 
                 n_frames: int, channels: 
                 List[int],
                 record_length: int, 
                 bytes_per_point: int):
        # Initialize 
        super().__init__(daemon=True)
        self.ctrl = controller
        self.ctrl_lock = controller_lock
        self.queue = data_queue
        self.n_frames = int(n_frames)
        self.channels = list(channels)
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
            for frame in range(1, self.n_frames + 1):
                if self._stop.is_set():
                    logger.info(f"FrameReader stopping early at frame {frame}")
                    break
                for ch in self.channels:
                    try:
                        with self.ctrl_lock:
                            # no conversion, just binary 
                            raw_data = self.ctrl.read_frame_channelch, frame)
                        if len(raw_data) == 0:
                            logger.warning(f"Empty read for frame-%{frame} in CH-{ch}")
                        # FIXME -- Check the record lenght?
                        # put into queue (block if full)
                        self.queue.put((frame, ch, raw_data), block=True)
                    except Exception as e:
                        logger.exception(f"Error reading frame-{frame} in CH-{ch}: {e}")
                        # On error, continue or optionally push an error marker
                # signal end-of-burst
                self.queue.put(None)
                logger.info("FrameReader finished and placed sentinel in queue.")
                # Activate again the triggers, note
                self.producer.clear_busy()

        logger.info("FrameReader run finished...")

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
        logger.info("EudaqEventSender started.")
        while producer._running:
            item = self.queue.get()
            if item is None:
                logger.info("EudaqEventSender got sentinel; finishing.")
                self._flush_remaining()
                break
            frame_idx, ch, raw_data = item
            if frame_idx not in self.framebuf:
                self.framebuf[frame_idx] = {}
            self.framebuf[frame_idx][ch] = raw_data
            # if we have all channels for this frame, send event
            if all(c in self.framebuf[frame_idx] for c in self.channels):
                try:
                    self._send_frame_event(frame_idx, self.framebuf[frame_idx])
                except Exception as e:
                    logger.exception(f"Failed to send event for frame-{frame_idx}: {e}")
                del self.framebuf[frame_idx]
        logger.info("EudaqEventSender exiting.")

    def _send_frame_event(self, frame_idx: int, frame_data: Dict[int, tuple]):
        """Build and send one EUDAQ event corresponding to frame_idx.
        The EUDAQ API varies by installation â€” adapt these lines to your setup.

        Parameters
        ----------
        """
        # Create RawDataEvent and add blocks per channel
        ev = pyeudaq.Event("RawEvent","MSO6B")
        ev.SetTriggerN(self.producer.n_trigger)
        if self.producer.n_trigger == 0:
            ev.SetBORE()
            ev.SetTag(f'producer_name', str(self.producer._name))
        # AddBlock expects bytes; label it by channel
        for ch in sorted(frame_data.keys()):
            ev.AddBlock(ch, frame_data[ch])
        # Update trigger 
        self.producer.n_trigger += 1 
        # Send event
        self.producer.SendEvent(ev)
        self.queue.task_done()


    def _flush_remaining(self):
        # attempt best-effort send for incomplete frames
        if not self.framebuf:
            return
        logger.warning(f"Flushing {len(self.framebuf} incomplete frames")
        for fidx in sorted(self.framebuf.keys()):
            try:
                self._send_frame_event(fidx, self.framebuf[fidx])
            except Exception:
                logger.exception("Error flushing frame-{fidx}")


class MSO6BProducer(pyeudaq.Producer):
    """
    """
    def __init__(self, name, runctrl):
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
        self.ctrl = MSOController(resource_string = self.resource, timeout_ms = self.timeout_ms)
        EUDAQ_INFO("MSO6B: Initialized..")

    
    @exception_handler
    def DoConfigure(self):
        EUDAQ_INFO("MSO6B: Configuring...")
        conf = self.GetConfiguration().as_dict()
        # Update the class with the mandatory configuration
        parse_config(self, CONFIG_PARAMETERS, conf)

        # controller methods are synchronous; protect them with visa_lock
        with self.ctrl_lock:
            #Always in a know state
            self.ctrl.reset()
            # Need to configure the basic XXX 
            # And configure the fastframe
            self.ctrl.configure_fastframe_acq(record_length=self.record_length,
                                              bpp: self.bytes_per_point,
                                              n_frames=self.n_frames,
                                              trigger_source="EXT")
            # Check is ready
            self.ctrl.is_trigger_ready()
            # We can extract the preamble to 
            for ch in self.channels:
                    self.ctrl.write(f"DATa:SOUrce CH{ch}")
                    self.wf_preamble[ch] = self.ctrl.wf_preamble()
        logger.info("Scope configured.")

    @exception_handler
    def DoStartRun(self):
        with self.ctrl_lock:
            self.arm_acquisition()
        self._running = True

    @exception_handler
    def DoStopRun(self):
        """
        Stop ongoing threads and close the scope.
        """
        logger.info("Stopping producer.")
        if self.reader is not None and self.reader.is_alive():
            self.reader.stop()
            self.reader.join(timeout=1.0)
        if self.eudaq_sender is not None and self.eudaq_sender.is_alive():
            self.eudaq_sender.stop()
            self.eudaq_sender.join(timeout=1.0)
        try:
            with self.ctrl_lock:
                self.ctrl.close()
        except Exception:
            pass

        self._running = False
        logger.info("Producer stopped.")

    @exception_handler
    def DoReset(self):
        if self.ctrl is not None:
            self.ctrl.close()
            self.ctrl = None
        self._running = False

    @exception_handler
    def DoLoop(self)
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
        self.reader = FrameReader(controller=self.ctrl,
                                  crtl_lock=self.ctrl_lock,
                                  data_queue=self.data_q,
                                  n_frames=self.n_frames,
                                  channels=self.channels,
                                  record_length=self.record_length,
                                  bytes_per_point=self.bytes_per_point,
                                  )
        self.reader.start()
        




        # Wait for reader and sender to finish
        logger.info("Waiting for reader thread to finish (this may take time depending on data volume).")
        self.reader.join()
        logger.info("Reader finished. Waiting for sender to finish.")
        self.eudaq_sender.join()
        logger.info("Burst processing (read+send) finished.")

# ----------------------------
# Example / test flow
# ----------------------------
def main_demo():
    """
    Demo script:
      - constructs producer
      - configures scope
      - simulates a single spill (sleep)
      - calls end_spill to read and send frames
    Use real TLU/spill monitor to trigger start_spill() and end_spill() in production.
    """
    cfg = CONFIG
    prod = TekScopeProducer(cfg)
    try:
        prod.configure_for_run()
        logger.info("Arming for a demo spill in 2 seconds...")
        time.sleep(2.0)
        prod.start_spill()
        # simulate spill duration (real run: TLU will be producing triggers)
        simulated_spill = 3.5
        logger.info("Simulated spill running for %.2f s", simulated_spill)
        time.sleep(simulated_spill)
        # end of spill -> read frames and send events
        prod.end_spill()
        logger.info("Demo completed.")
    except KeyboardInterrupt:
        logger.info("Interrupted.")
    finally:
        prod.stop()

if __name__ == "__main__":
    main_demo()

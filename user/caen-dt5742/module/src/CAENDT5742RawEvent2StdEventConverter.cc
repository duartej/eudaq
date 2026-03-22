// EUDAQ converter for CAEN DT5742 raw events.
//
// It reads the binary event blocks written by the producer, recovers the
// acquisition settings from the BORE, optionally applies the CAEN x742 offline
// DRS4 corrections, decodes the raw event structure, reconstructs the channel
// waveforms, converts ADC samples to physical units, and exports the result as
// a EUDAQ StandardEvent with waveform and timing metadata.
//
// J. Duarte-Campderros (IFCA)
#include "eudaq/StdEventConverter.hh"
#include "eudaq/RawEvent.hh"
#include "eudaq/Logger.hh"

#include <iterator>
#include <vector>
#include <map>
#include <array>
#include <cstdint>
#include <algorithm>
#include <regex>
#include <cmath>
#include <cstring>
#include <sstream>
#include <cstdlib>
#include <cctype>

#ifndef CAEN_DT5742_CORRECTION_TABLES_DIR
#error "CAEN_DT5742_CORRECTION_TABLES_DIR must be defined from CMake"
#endif

extern "C" {
#include "X742CorrectionRoutines.h"
#include "X742DecodeRoutines.h"
}

// Digitizer: { channel : [ (row, col), (row, col), ... ], 
// Each channel can be bounded to several diodes/pixels
using PixelMap = std::map<int, std::vector<std::array<int,2>> >;

static inline std::string ResolveDigitizerSerialFromName(const std::string &device_name) {
    static const std::map<std::string, std::string> kDeviceNameToSerial = {
        {"CAEN_UZH", "25004"},
        {"CAEN_IJS", "22890"},
    };
    const auto it = kDeviceNameToSerial.find(device_name);
    if (it != kDeviceNameToSerial.end()) {
        return it->second;
    }
    return "";
}

static inline std::string BuildCorrectionBasePath(const std::string &serial) {
    std::string base_dir(CAEN_DT5742_CORRECTION_TABLES_DIR);
    if(!base_dir.empty() && base_dir.back() != '/') {
        base_dir += "/";
    }
    return base_dir + "dt5742_sn" + serial + "_5000MHz";
}
// Auxiliary functions for DAC to Volts conversion for the DC-offset
static inline double DACToBaselineVolts(uint32_t dac, double Vpp) {
    return (static_cast<double>(dac) / 65535.0 - 0.5) * Vpp;
}


// Convert a 12-bit ADC waveform (0..4095) into Volts. For DT5742 default input dynamic range is 1 Vpp. 
static inline std::vector<double> ADC12ToVolts(const std::vector<double> &wf_adc, double dc_offset = 0.0, double Vpp = 1.0) {
    constexpr double ADC_MID = 2048.0;
    constexpr double LSB_DEN = 4096.0;
    const double lsb = Vpp / LSB_DEN;

    std::vector<double> wf_v;
    wf_v.reserve(wf_adc.size());
    for(double a : wf_adc) {
        wf_v.push_back(((a - ADC_MID) * lsb) + dc_offset);
    }
    return wf_v;
}

static inline double Median(std::vector<double> values) {
    if (values.empty()) {
        return 0.0;
    }
    const auto mid = values.begin() + values.size() / 2;
    std::nth_element(values.begin(), mid, values.end());
    if ((values.size() % 2) != 0) {
        return *mid;
    }
    const double hi = *mid;
    std::nth_element(values.begin(), mid - 1, values.end());
    return 0.5 * (hi + *(mid - 1));
}

// Auxiliary functions for the correction tables
static inline std::string GroupedBasePath(const std::string &basepath, int group_id) {
    return basepath + "_gr" + std::to_string(group_id);
}

// Free memory 
static inline void FreeDecodedX742Event(CAEN_DGTZ_X742_EVENT_t *evt) {
    if(evt == nullptr) {
        return;
    }
    for(size_t g = 0; g < 2; ++g) {
        if(evt->GrPresent[g] != 1) {
            continue;
        }
        for(size_t ch = 0; ch < 9; ++ch) {
            std::free(evt->DataGroup[g].DataChannel[ch]);
            evt->DataGroup[g].DataChannel[ch] = nullptr;
        }
    }
    std::free(evt);
}

class CAENDT5742RawEvent2StdEventConverter: public eudaq::StdEventConverter {
    public:
        bool Converting(eudaq::EventSPC d1, eudaq::StdEventSP d2, eudaq::ConfigSPC conf) const override;
        static const uint32_t m_id_factory = eudaq::cstr2hash("CAENDT5742");

    private:
        void Initialize(eudaq::EventSPC bore, eudaq::ConfigurationSPC conf) const;
        PixelMap GetDUTPixelMap(const std::string & dut_tag) const; 
        // Helper functions
        float AmplitudeWF(const std::vector<double> & wf) const;
        bool DecodeRawEvent(const int dev_id, const std::vector<uint8_t> & raw, std::map<size_t, std::vector<std::vector<float>>> & waveforms_group) const;
        bool EnsureCorrectionTablesLoaded(int dev_id) const;

        static std::map<int, std::string> _name;
        // XXX -- NEEDED?
        static size_t _n_digitizers;
        static size_t _n_samples_per_waveform;
        static size_t _sampling_frequency_MHz;
        static std::map<int, size_t> _n_duts;
        // Waveform starting t0 and Dt
        static std::map<int, float> _t0;
        static std::map<int, float> _dt;
        // XXX - TBD?
        // XXX -- TBD?
        static std::map<int, std::vector<int> > _dut_channel_list;
        // { Digitizer: { DUT: { channel : [ (row, col), (row, col), ... ],  ... 
        static std::map<int, std::map<int, PixelMap> > _dut_channel_arrangement;
        // { Digitizer: { DUT: (n-row,n-col), ... 
        static std::map<int, std::map<int, std::array<int,2>> > _nrows_ncolumns;
        // { Digitizer: { DUT: npixels, ...
        static std::map<int, std::map<int,int> > _npixels;
        // Human-readable name related with the internal DUT-id
        static std::map<int, std::map<std::string,int> > _dut_names_id;
        static std::map<int, std::map<int, uint32_t> > _channel_dc_offset_dac;
        static std::map<int, int> _post_trigger_size;
        static std::map<int, std::string> _x742_correction_table_basepath;
        static std::map<int, std::map<int, bool>> _x742_correction_table_loaded;
        static std::map<int, std::map<int, CAEN_DGTZ_DRS4Correction_t>> _x742_correction_tables;
};

namespace {
    auto dummy0 = eudaq::Factory<eudaq::StdEventConverter>::Register<CAENDT5742RawEvent2StdEventConverter>(CAENDT5742RawEvent2StdEventConverter::m_id_factory);
}

// Static data members to avoid loosing info (after initialization of data members)
// [due to the re-creation of the instances each event?]
std::map<int,std::string> CAENDT5742RawEvent2StdEventConverter::_name;
size_t CAENDT5742RawEvent2StdEventConverter::_n_digitizers = 0;
size_t CAENDT5742RawEvent2StdEventConverter::_n_samples_per_waveform;
size_t CAENDT5742RawEvent2StdEventConverter::_sampling_frequency_MHz;
std::map<int, size_t> CAENDT5742RawEvent2StdEventConverter::_n_duts;
std::map<int, float> CAENDT5742RawEvent2StdEventConverter::_t0;
std::map<int, float> CAENDT5742RawEvent2StdEventConverter::_dt;
std::map<int, std::vector<int> > CAENDT5742RawEvent2StdEventConverter::_dut_channel_list;
std::map<int, std::map<int, PixelMap> > CAENDT5742RawEvent2StdEventConverter::_dut_channel_arrangement;
std::map<int, std::map<int, std::array<int,2>> > CAENDT5742RawEvent2StdEventConverter::_nrows_ncolumns;
std::map<int, std::map<int,int> > CAENDT5742RawEvent2StdEventConverter::_npixels;
std::map<int, std::map<std::string,int> > CAENDT5742RawEvent2StdEventConverter::_dut_names_id;
std::map<int, std::map<int, uint32_t> > CAENDT5742RawEvent2StdEventConverter::_channel_dc_offset_dac;
std::map<int, int> CAENDT5742RawEvent2StdEventConverter::_post_trigger_size;
std::map<int, std::string> CAENDT5742RawEvent2StdEventConverter::_x742_correction_table_basepath;
std::map<int, std::map<int, bool>> CAENDT5742RawEvent2StdEventConverter::_x742_correction_table_loaded;
std::map<int, std::map<int, CAEN_DGTZ_DRS4Correction_t>> CAENDT5742RawEvent2StdEventConverter::_x742_correction_tables;


namespace {
    std::map<int, uint32_t> ParseChannelDACMap(const std::string &text) {
        std::map<int, uint32_t> result;
        const std::regex re(R"(CH(\d+)\s*:\s*(\d+))");
        for(std::sregex_iterator it(text.begin(), text.end(), re); it != std::sregex_iterator(); ++it) {
            const std::smatch m = *it;
            result[std::stoi(m[1].str())] = static_cast<uint32_t>(std::stoul(m[2].str()));
        }
        return result;
    }

    int ParseIntTagOrDefault(eudaq::EventSPC bore, const std::string &tag_name, int default_value) {
        try {
            return std::stoi(bore->GetTag(tag_name), nullptr, 0);
        }
        catch (...) {
            return default_value;
        }
    }
}


void CAENDT5742RawEvent2StdEventConverter::Initialize(eudaq::EventSPC bore, eudaq::ConfigurationSPC /*conf*/) const {
    
    const int device_id = bore->GetDeviceN();
    
    _name.erase(device_id);
    _t0.erase(device_id);
    _dt.erase(device_id);
    _x742_correction_table_basepath.erase(device_id);
    _x742_correction_table_loaded.erase(device_id);
    _dut_names_id.erase(device_id);
    _dut_channel_list.erase(device_id);
    _dut_channel_arrangement.erase(device_id);
    _nrows_ncolumns.erase(device_id);
    _npixels.erase(device_id);

    // How many times are initializing = Digitizers present in the event
    // FIXME -- Use the DeviceN id?
    ++_n_digitizers;
    
    // Name of the producer
    _name[device_id] = bore->GetTag("producer_name");

    // XXX ==  OLD VERSION the name is giving the Serial Number
    std::string serial;
    if( bore->HasTag("serial_number") ) {
        serial = bore->GetTag("serial_number");
    } 
    else {
        serial = ResolveDigitizerSerialFromName(_name[device_id]);
    }

    if(serial.empty()) {
        EUDAQ_ERROR("Unable to resolve DT5742 serial number from producer name '" + _name[device_id] + "'.");
    }
    else {
        _x742_correction_table_basepath[device_id] = BuildCorrectionBasePath(serial);
    }

    // XXX -- Identify the DUTS with the Channels

    // The record length
    const size_t ns = std::stoi(bore->GetTag("n_samples_per_waveform"));
    // The sampling frequency
    const size_t fs = std::stoi(bore->GetTag("sampling_frequency_MHz"));

    if(!_name.empty() && _n_digitizers > 1) {
        if(_n_samples_per_waveform != ns) {
            EUDAQ_ERROR("Different n_samples_per_waveform across digitizers is not supported.");
        }
        if(_sampling_frequency_MHz != fs) {
            EUDAQ_ERROR("Different sampling_frequency_MHz across digitizers is not supported.");
        }
    }
    _n_samples_per_waveform = ns;
    _sampling_frequency_MHz = fs;

    if( _sampling_frequency_MHz != 5000 ) {
        EUDAQ_WARN("This converter is configured to use 5 GHz correction tables, but the BORE reports " +
                   std::to_string(_sampling_frequency_MHz) + " MHz.");
    }
    // The channel offset
    _channel_dc_offset_dac[device_id] = ParseChannelDACMap(bore->GetTag("channel_dc_offset_dac_map"));
    _post_trigger_size[device_id] = ParseIntTagOrDefault(bore, "post_trigger_size", 50);
    
    // Get the list of DUTs so it can be extracted all channels and row-col mapping:
    std::string s( bore->GetTag("dut_names") );
    // XXX - Provisional (ask MS to list() the dict keys)
    size_t start_rm = s.find("dict_keys");
    size_t end_rm = s.find("(");
    if(start_rm != std::string::npos && end_rm != std::string::npos) {
        s.erase(start_rm, end_rm - start_rm + 1);
    }    
    // XXX - END Provisional
    for (char c : {'[', ']', ')', '\'', '\"',}) {
        s.erase(remove(s.begin(), s.end(), c), s.end());
    }
    std::string delimiter = ", ";
    size_t pos = 0;
    int dut_internal_id = 0;
    while((pos = s.find(delimiter)) != std::string::npos) {
        std::string token = s.substr(0, pos);
        _dut_names_id[device_id][token] = dut_internal_id;
        s.erase(0, pos + delimiter.length());
        ++dut_internal_id;
    }
    // And the last one...
    _dut_names_id[device_id][s] = dut_internal_id;
    // The number of DUTS
    _n_duts[device_id] = _dut_names_id[device_id].size();

    // Extract the tags for each DUT:
    for(const auto & dutname_id: _dut_names_id[device_id]) {
        _dut_channel_arrangement[device_id][dutname_id.second] = GetDUTPixelMap(bore->GetTag(dutname_id.first));
        // Get the maximum nrows and ncolumns for the DUT
        // Loop over the channels:
        int nrow = -1;
        int ncol = -1;
        for(const auto & channel_listrowcol: _dut_channel_arrangement[device_id][dutname_id.second] ) {
            _dut_channel_list[device_id].push_back(channel_listrowcol.first);
            // And loop over all the pixels bounded to this channel
            for(const auto & rowcol: channel_listrowcol.second) {
                if(rowcol[0] > nrow) {
                    nrow = rowcol[0];
                }
                if(rowcol[1] > ncol) {
                    ncol = (rowcol)[1];
                }
            }
        }
        _nrows_ncolumns[device_id][dutname_id.second] = { nrow+1, ncol+1 };
        // Total number of pixels: Remember starting at 0, then 
       _npixels[device_id][dutname_id.second] = (nrow+1)*(ncol+1);
    }

    // Reconstruct a consistent time axis. This mirrors the producer-side Python
    // helper: the trigger position depends on the post-trigger percentage and the
    // fast-trigger mode adds the documented trigger latency.
    _dt[device_id] = 1.00 / (_sampling_frequency_MHz * 1.0e6);
    const double time_span = (_n_samples_per_waveform > 0 ? (_n_samples_per_waveform - 1) : 0) * _dt[device_id];
    const double trigger_latency = 42e-9;
    _t0[device_id] = -time_span * (100.0 - static_cast<double>(_post_trigger_size[device_id])) / 100.0 + trigger_latency;

    // Print-out the topology of the sensor and wire-bonding
    EUDAQ_INFO(" Defined DUTs in [" +_name[device_id]+ "] digitizer: ");
    for(auto & dn_id: _dut_names_id[device_id]) {
        EUDAQ_INFO(" [" + dn_id.first + "], ID:" + std::to_string(dn_id.second) +", "
                + "(rowsXcols): " + std::to_string(_nrows_ncolumns[device_id][dn_id.second][0])
                + "x" + std::to_string(_nrows_ncolumns[device_id][dn_id.second][1])
                + ", Total Pixels: " + std::to_string(_npixels[device_id][dn_id.second])
                + ", Total channels: "
                +std::to_string(_dut_channel_arrangement[device_id][dn_id.second].size()));
        for(const auto & ch_listpixels: _dut_channel_arrangement[device_id][dn_id.second]) {
            std::string list_pixels;
            for(const auto & pixels: ch_listpixels.second) {
                list_pixels += " ("+std::to_string(pixels[0]) + "," +std::to_string(pixels[1]) + ")";
            }
            EUDAQ_INFO("   ==: CH" + std::to_string(ch_listpixels.first) + " ["+ list_pixels + " ]");
        }
    }

    // Debugging print-out stuff
    EUDAQ_DEBUG(" Initialize:: nsamples_per_waveform: " + std::to_string(_n_samples_per_waveform) +
            ", sampling frequency: " + std::to_string(_sampling_frequency_MHz) + " MHz" +
            ", number of DUTs: " + std::to_string(_n_duts[device_id]));
    // Get the list of channels
    std::ostringstream oss;
    std::copy(_dut_channel_list[device_id].begin(), _dut_channel_list[device_id].end(), std::ostream_iterator<int>(oss, " "));
    EUDAQ_DEBUG(" Initialize:: Channel list (internal-ids): [ " + oss.str() +" ]");
}

float CAENDT5742RawEvent2StdEventConverter::AmplitudeWF(const std::vector<double>& waveform) const {
    if (waveform.empty()) {
        return 0.0f;
    }

    const double baseline = Median(waveform);
    std::vector<double> abs_dev;
    abs_dev.reserve(waveform.size());
    for (double v : waveform) {
        abs_dev.push_back(std::abs(v - baseline));
    }
    const double mad = Median(abs_dev);
    const double robust_sigma = (mad > 0.0) ? 1.4826 * mad : 0.0;

    const auto itminmax = std::minmax_element(waveform.begin(), waveform.end());
    const double min_dev = *itminmax.first - baseline;
    const double max_dev = *itminmax.second - baseline;
    const double peak = (std::abs(min_dev) > std::abs(max_dev)) ? min_dev : max_dev;

    if (robust_sigma > 0.0 && std::abs(peak) < 3.0 * robust_sigma) {
        return 0.0f;
    }
    return static_cast<float>(peak);
}

bool CAENDT5742RawEvent2StdEventConverter::EnsureCorrectionTablesLoaded(int dev_id) const {
    const std::string &basepath = _x742_correction_table_basepath[dev_id];
    if(basepath.empty()) {
        EUDAQ_ERROR("No DT5742 correction-table base path available for device " + std::to_string(dev_id) + ".");
        return false;
    }

    for(int group_id = 0; group_id < 2; ++group_id) {
        auto &loaded = _x742_correction_table_loaded[dev_id][group_id];
        if(loaded) {
            continue;
        }
        const std::string grouped_basepath = GroupedBasePath(basepath, group_id);
        std::memset(&_x742_correction_tables[dev_id][group_id], 0, sizeof(CAEN_DGTZ_DRS4Correction_t));
        const int rc = LoadCorrectionTable(const_cast<char *>(grouped_basepath.c_str()),
                                           &_x742_correction_tables[dev_id][group_id]);
        if(rc != 0) {
            EUDAQ_ERROR("Failed to load offline x742 correction table for dev " +
                        std::to_string(dev_id) + ", group " + std::to_string(group_id) +
                        " from base path '" + grouped_basepath + "' (LoadCorrectionTable rc=" +
                        std::to_string(rc) + ").");
            return false;
        }
        loaded = true;
    }
    return true;
}

bool CAENDT5742RawEvent2StdEventConverter::DecodeRawEvent(
        const int dev_id, const std::vector<uint8_t> & raw,
        std::map<size_t, std::vector<std::vector<float>>> & waveforms_group) const {
    // Apply all corrections
    const int kX742CorrectionLevelMask = 0x7;
    // Check the tables are properly loaded
    if(!EnsureCorrectionTablesLoaded(dev_id)) {
        return false;
    }

    uint32_t num_events = 0;
    int32_t rc = GetNumEvents(reinterpret_cast<char*>(const_cast<uint8_t*>(raw.data())), static_cast<uint32_t>(raw.size()), &num_events);
    if(rc != 0 || num_events == 0) {
        EUDAQ_ERROR("Offline GetNumEvents failed with rc=" + std::to_string(rc) + ", num_events=" + std::to_string(num_events));
        return false;
    }
    if(num_events != 1) {
        EUDAQ_WARN("Offline x742 decoder saw " + std::to_string(num_events) + " events in a single EUDAQ block. Only the first event will be used.");
    }

    char *evt_ptr = nullptr;
    rc = GetEventPtr(reinterpret_cast<char*>(const_cast<uint8_t*>(raw.data())), static_cast<uint32_t>(raw.size()), 0, &evt_ptr);
    if(rc != 0 || evt_ptr == nullptr) {
        EUDAQ_ERROR("Offline GetEventPtr failed with rc=" + std::to_string(rc));
        return false;
    }

    void *evt_void = nullptr;
    rc = X742_DecodeEvent(evt_ptr, &evt_void);
    if(rc != 0 || evt_void == nullptr) {
        EUDAQ_ERROR("Offline X742_DecodeEvent failed with rc=" + std::to_string(rc));
        return false;
    }

    auto *evt = reinterpret_cast<CAEN_DGTZ_X742_EVENT_t *>(evt_void);
    const CAEN_DGTZ_DRS4Frequency_t correction_frequency = CAEN_DGTZ_DRS4_5GHz;
    for(size_t group_id = 0; group_id < 2; ++group_id) {
        if(evt->GrPresent[group_id] != 1) {
            continue;
        }
        auto &group = evt->DataGroup[group_id];
        ApplyDataCorrection(&_x742_correction_tables[dev_id][static_cast<int>(group_id)], correction_frequency, kX742CorrectionLevelMask, &group);

        std::vector<std::vector<float>> waveforms(9);
        for(size_t ch = 0; ch < 9; ++ch) {
            const uint32_t n = group.ChSize[ch];
            if(group.DataChannel[ch] == nullptr || n == 0) {
                continue;
            }
            waveforms[ch].assign(group.DataChannel[ch], group.DataChannel[ch] + n);
        }
        waveforms_group[group_id] = std::move(waveforms);
    }

    FreeDecodedX742Event(evt);
    return !waveforms_group.empty();
}

bool CAENDT5742RawEvent2StdEventConverter::Converting(eudaq::EventSPC d1, eudaq::StdEventSP d2, eudaq::ConfigSPC conf) const {

    auto event = std::dynamic_pointer_cast<const eudaq::RawDataEvent>(d1);
    if(event == nullptr) {
        EUDAQ_ERROR("Received null event.");
        return false;
    }

    // Beginning Of Run Event, this is the header event.
    if(event->IsBORE()) { 
        EUDAQ_INFO("Starting initialization...");
        Initialize(event, conf);
    }

    const int dev_id = event->GetDeviceN();
    
    // Expecting only one block
    if(event->NumBlocks() != 1) {
        EUDAQ_ERROR(" Expected one block, got "+ std::to_string(event->NumBlocks()) );
        return false;
    }

    d2->SetDetectorType("CAENDT5742");

    if(!d2->IsFlagPacket()) {
        d2->SetFlag(d1->GetFlag());
        d2->SetRunN(d1->GetRunN());
        d2->SetEventN(d1->GetEventN());
        d2->SetStreamN(d1->GetStreamN());
        d2->SetTriggerN(d1->GetTriggerN(), d1->IsFlagTrigger());
        d2->SetTimestamp(d1->GetTimestampBegin(), d1->GetTimestampEnd(), d1->IsFlagTimestamp());
    }

    const std::string producer_name = _name[dev_id];
    
    // Extract the event and convert it back to 32b words
    // See data format in CAEN User Manual 9.7.2 
    std::vector<uint8_t> raw = event->GetBlock(0);
    if( raw.size() < 16 ) {
        EUDAQ_ERROR("Raw CAEN block too small: " + std::to_string(raw.size()) + " bytes");
        return false;
    }
    if( raw.size() % 4 != 0 ) {
        EUDAQ_ERROR("Raw CAEN block size is not 32-bit aligned: " + std::to_string(raw.size()) + " bytes");
        return false;
    }
    // The raw event in 32-bit words
    uint32_t first_word = 0;
    std::memcpy(&first_word, raw.data(), sizeof(uint32_t));
    // Get the size of the event --> To cross-check ?? 
    const size_t total_words = static_cast<size_t>(first_word & 0x0FFFFFFF);
    const size_t block_words = raw.size() / 4;
    if( total_words != block_words ) {
        EUDAQ_ERROR("Raw event size mismatch: header says " + std::to_string(total_words) +
                " words, but block contains " + std::to_string(block_words) + " words.");
        // -- XXX or return true for skipping this ? 
        return false;
    }
    
    std::map<size_t, std::vector<std::vector<float>>> waveforms_group;
    const bool decoded_offline = DecodeRawEvent(dev_id, raw, waveforms_group);
    if(!decoded_offline) {
        return false;
    }

    // Each DUT is a plane
    for(const auto & dutname_sensorid: _dut_names_id[dev_id]) {
        // XXX - Can we provide a dutname in the stdplane?? 
        const int sensor_id = dutname_sensorid.second;        
        // Each DUT defines a plane
        eudaq::StandardPlane plane(sensor_id, "CAENDT5742", producer_name);
        // Define the size of the DUT (in row and columns) --> Extracted from _nrows_ncolumns
        // Remember in here: first columns, then rows
        plane.SetSizeZS( (uint32_t)_nrows_ncolumns[dev_id][dutname_sensorid.second][1],
                (uint32_t)_nrows_ncolumns[dev_id][dutname_sensorid.second][0],
                0);

        // Extract waveforms per channel
        int pixid = 0;
        for(const auto & ch_rowcollist: _dut_channel_arrangement[dev_id][dutname_sensorid.second]) {
            const size_t channel = ch_rowcollist.first;
            // What group? 0-7 -> group 0, 8->15 group 1
            size_t gr = channel < 8 ? 0 : 1; 
            size_t channel_inside_group = channel < 8 ? channel : channel - 8;
            // PArticular case: channel 16 and 17 are TR0 for group-0 and TR0 for group-1
            if( channel > 15 ) {
                gr = channel == 16 ? 0 : 1;
                channel_inside_group = 8;
            }

            auto it_group = waveforms_group.find(gr);
            if( it_group == waveforms_group.end() || it_group->second.size() == 0 ) {
                // Channel belongs to a group that is not present in this event
                // XXX What about the TRIGGER groups??
                continue;
            }
            const std::vector<float> & waveform_float = it_group->second.at(channel_inside_group);
            if(waveform_float.empty()) {
                continue;
            }

            // Vpp is 1.0 V in DT5742
            const double Vpp = 1.0;
            uint32_t dc_offset_dac = 32768u;
            if (channel <= 15) {
                auto it_dac = _channel_dc_offset_dac[dev_id].find(static_cast<int>(channel));
                if (it_dac != _channel_dc_offset_dac[dev_id].end()) {
                    dc_offset_dac = it_dac->second;
                }
            }
            const double dc_offset_volts = DACToBaselineVolts(dc_offset_dac, Vpp);
            std::vector<double> wf_adc(waveform_float.begin(), waveform_float.end());
            // From ADC to Volts
            std::vector<double> wf = ADC12ToVolts(wf_adc, dc_offset_volts, Vpp);

            const float amplitude = AmplitudeWF(wf);
            float hit_value = std::abs(amplitude);
            
            for(const auto & pixel: ch_rowcollist.second) {
                // Note the signature introduce x,y -> col, row. Opposite to which we store
                // Amplitude as charge? It would be better a ToT or something similar
                plane.PushPixel(pixel[1], pixel[0], hit_value, uint32_t(0));
                plane.SetPixelAuxInfo(pixid, 
                        dutname_sensorid.first+":CH"+
                            std::to_string(ch_rowcollist.first)+
                            ":col"+std::to_string(pixel[1])+":row"+
                            std::to_string(pixel[0]));
                plane.SetWaveform(pixid, wf, _t0[dev_id], _dt[dev_id] );
                ++pixid;
            }
        }
        d2->AddPlane(plane);
    }
    return true;
}

PixelMap CAENDT5742RawEvent2StdEventConverter::GetDUTPixelMap(const std::string & dut_tag) const {
    
    // It must exist a tag with the name of the DUT
    // FIXME -- Error control: empyt string!!
    
    // Parsing something like:
    // "CH1: [(0,0),(0,1),(1,1),(2,0)], CH3: [(0,1)], CH6: [(1,2), (3,10)]"
    
    // ---- Split in blocks of CH
    std::regex token(R"(\])");
    std::vector<std::string> substrings(std::sregex_token_iterator(dut_tag.begin(), dut_tag.end(), token, -1), {});

    // For each substring: extract the channel and the list of col and rows
    std::regex re_ch(R"(CH(\d*):)");
    std::regex re_colrow(R"(\((\d+),(\d+)\))");

    PixelMap ch_dict;
    for(auto & chstr: substrings)
    {
        int current_channel = -1;
        for(std::sregex_iterator it = std::sregex_iterator(chstr.begin(),chstr.end(),re_ch); it != std::sregex_iterator();++it) {
            std::smatch m = *it;
            //std::cout << "[->> " << m[1].str() << std::endl;
            current_channel = std::stoi(m[1]);
        }

        if( current_channel == -1 ) {
            // there is no integer in channel, therefore trigger_group_0 or trigger_group_1
            // HARDCODED in the producer, hardcoded here
            // They are also hardcoded as CH16 being in the pixel (0,0) and CH17 in the pixel (0,1)
            // No matter what user introduces
            if( chstr.find("trigger_group") != std::string::npos) {
                if( chstr.find("group_0") != std::string::npos) {
                    current_channel = 16;
                    ch_dict[current_channel].push_back({0,0});
                }
                else if( chstr.find("group_1") != std::string::npos) {
                    current_channel = 17;
                    ch_dict[current_channel].push_back({0,1});
                }
                else {
                    EUDAQ_ERROR("Malformed Connections file. Expecting `trigger_group_0` or"
                            "`trigger_group_1`, but found `"+chstr+"`");
                }
                // pixel defined already
                continue;
            }
        }
        
        // Check for malformed mapping
        if(current_channel == -1) {
            if(!std::all_of(chstr.begin(), chstr.end(), [](unsigned char c){ return std::isspace(c); })) {
                EUDAQ_ERROR("Malformed DUT channel mapping fragment: `" + chstr + "`");
            }
            continue;
        }

        for(std::sregex_iterator cr = std::sregex_iterator(chstr.begin(),chstr.end(),re_colrow); cr != std::sregex_iterator();++cr) {
            std::smatch m = *cr;
            //std::cout << "[ colrow : " << m[1].str() << " " << m[2].str() <<  std::endl;
            ch_dict[current_channel].push_back({std::stoi(m[1].str()),std::stoi(m[2].str())});
        }
    }

    return ch_dict;
}


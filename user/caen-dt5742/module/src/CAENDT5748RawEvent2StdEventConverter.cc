// -- XXX DOC 
// -- 
#include "eudaq/StdEventConverter.hh"
#include "eudaq/RawEvent.hh"
#include "eudaq/Logger.hh"

#include <vector>
#include <map>
#include <array>
#include <cstdint>
#include <algorithm>
#include <regex>
#include <numeric>
#include <cmath>
#include <cstring>

// PROV -- dEBUGGING XXX
#include <ios>

// Digitizer: { channel : [ (row, col), (row, col), ... ], 
// Each channel can be bounded to several diodes/pixels
using PixelMap = std::map<int, std::vector<std::array<int,2>> >;

class CAENDT5748RawEvent2StdEventConverter: public eudaq::StdEventConverter {
    public:
        bool Converting(eudaq::EventSPC d1, eudaq::StdEventSP d2, eudaq::ConfigSPC conf) const override;
        static const uint32_t m_id_factory = eudaq::cstr2hash("CAENDT5748");

    private:
        void Initialize(eudaq::EventSPC bore, eudaq::ConfigurationSPC conf) const;
        PixelMap GetDUTPixelMap(const std::string & dut_tag) const; 
        // Helper functions
        void waveforms_reassemble(uint32_t w0, uint32_t w1, uint32_t w2, std::vector<std::vector<float> > & waveforms) const;
        int PolarityWF(const std::vector<float> & wf) const;
        float AmplitudeWF(const std::vector<float> & wf) const;

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
};

namespace {
    auto dummy0 = eudaq::Factory<eudaq::StdEventConverter>::Register<CAENDT5748RawEvent2StdEventConverter>(CAENDT5748RawEvent2StdEventConverter::m_id_factory);
}

// Static data members to avoid loosing info (after initialization of data members)
// [due to the re-creation of the instances each event?]
std::map<int,std::string> CAENDT5748RawEvent2StdEventConverter::_name;
size_t CAENDT5748RawEvent2StdEventConverter::_n_digitizers = 0;
size_t CAENDT5748RawEvent2StdEventConverter::_n_samples_per_waveform;
size_t CAENDT5748RawEvent2StdEventConverter::_sampling_frequency_MHz;
std::map<int, size_t> CAENDT5748RawEvent2StdEventConverter::_n_duts;
std::map<int, float> CAENDT5748RawEvent2StdEventConverter::_t0;
std::map<int, float> CAENDT5748RawEvent2StdEventConverter::_dt;
std::map<int, std::vector<int> > CAENDT5748RawEvent2StdEventConverter::_dut_channel_list;
std::map<int, std::map<int, PixelMap> > CAENDT5748RawEvent2StdEventConverter::_dut_channel_arrangement;
std::map<int, std::map<int, std::array<int,2>> > CAENDT5748RawEvent2StdEventConverter::_nrows_ncolumns;
std::map<int, std::map<int,int> > CAENDT5748RawEvent2StdEventConverter::_npixels;
std::map<int, std::map<std::string,int> > CAENDT5748RawEvent2StdEventConverter::_dut_names_id;

void CAENDT5748RawEvent2StdEventConverter::Initialize(eudaq::EventSPC bore, eudaq::ConfigurationSPC conf) const {
    
    const int device_id = bore->GetDeviceN();
    
    // How many times are initializing = Digitizers present in the event
    // FIXME -- Use the DeviceN id?
    ++_n_digitizers;
    
    // Name of the producer
    _name[device_id] = bore->GetTag("producer_name");

    // XXX -- Identify the DUTS with the Channels

    // The record length
    _n_samples_per_waveform = std::stoi(bore->GetTag("n_samples_per_waveform"));
    // The sampling frequency
    _sampling_frequency_MHz = std::stoi(bore->GetTag("sampling_frequency_MHz"));
    
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

    // Extract the initial (hardcoded to 0) and the temporal step value of the waveforms
    // --- in SECONDS
    _t0[device_id] = 0.0;
    _dt[device_id] = _n_samples_per_waveform/(_sampling_frequency_MHz);

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


// --- Función auxiliar para decodificar un bloque de 3 palabras ---
void CAENDT5748RawEvent2StdEventConverter::waveforms_reassemble(uint32_t w0, uint32_t w1, uint32_t w2, std::vector<std::vector<float> > & waveforms) const {
    // See CAEN User Manual data format. 
    // Each three words contains the one sample (RDS4 cell) 
    // for all enabled (?) channels. The info is store in 12bits
    waveforms[0].push_back( static_cast<float>( (w0 >>  0) & 0xFFF) );
    waveforms[1].push_back( static_cast<float>( (w0 >> 12) & 0xFFF) );
    waveforms[2].push_back( static_cast<float>( ((w0 >> 24) & 0xFF) | ((w1 & 0xF) << 8) ) );
    waveforms[3].push_back( static_cast<float>( (w1 >>  4) & 0xFFF ) );
    waveforms[4].push_back( static_cast<float>( (w1 >> 16) & 0xFFF ) );
    waveforms[5].push_back( static_cast<float>( ((w1 >> 28) & 0xF) | ((w2 & 0xFF) << 4) ) );
    waveforms[6].push_back( static_cast<float>( (w2 >>  8) & 0xFFF ) );
    waveforms[7].push_back( static_cast<float>( (w2 >> 20) & 0xFFF ) );
}


// FIXME -- Calculate it once: use a memoizer
int CAENDT5748RawEvent2StdEventConverter::PolarityWF(const std::vector<float> & wf) const {
    // Extract polarity -- XXX-- Just do it once ? -- then, TODO
    auto itminmax = std::minmax_element(wf.begin(), wf.end());
    const float min = *itminmax.first;
    const float max = *itminmax.second;
    if( std::abs(*itminmax.first) > std::abs(*itminmax.second) ) {
        return -1;
    }
    return 1;
}


float CAENDT5748RawEvent2StdEventConverter::AmplitudeWF(const std::vector<float>& waveform) const {
    // Rough estimation of the baseline using the median
    // But first use the right polarity to be sure we sort properly
    const int polarity = PolarityWF(waveform); 
    std::vector<float> wf_abs(waveform);
    for(float & v: wf_abs) {
        v * polarity;
    }
    //
    // All signals are now positives
    // -----------------------------

    // Sorted: smaller firts
    std::sort(wf_abs.begin(), wf_abs.end());
    const size_t wfsize = wf_abs.size();
    if(wfsize == 0) {
        return 0.0;
    }

    double baseline = 0;
    if(wfsize % 2 == 0) {
        // If even, we need to obtain the average of the two central values
        baseline = (wf_abs[wfsize/2 - 1]+wf_abs[wfsize/2])/2.0;
    } 
    else {
        baseline = wf_abs[wfsize/2];
    }
    // We need to evaluate a kind of sigma, to get an estimation
    // if there is a signal there
    const double wf_amplitude_max = wf_abs[wfsize-1];

    // --> Calculate the deviation standard -- XXX - ?
    const double mean = std::accumulate(wf_abs.begin(), wf_abs.end(),0.0)/wfsize;
    auto sum_term = [mean](double init, double value)-> double { return init + (value - mean)*(value - mean); };
    const double variance = std::accumulate(wf_abs.begin(), wf_abs.end(), 0.0, sum_term);
    const double stddev = std::sqrt(variance/wfsize);
    
/*std::cout << " Baseline: " << baseline << " -- " << wf_amplitude_max 
    << " 3sigma?" << 3.0*stddev << std::endl;*/
    // Assume 3 sigma to be signal
    if( wf_amplitude_max > 3.0*(baseline + stddev) ) {
        return wf_amplitude_max*polarity;
    }

    return 0.0;
}



bool CAENDT5748RawEvent2StdEventConverter::Converting(eudaq::EventSPC d1, eudaq::StdEventSP d2, eudaq::ConfigSPC conf) const {

    auto event = std::dynamic_pointer_cast<const eudaq::RawDataEvent>(d1);
    if (event == nullptr) {
        EUDAQ_ERROR("Received null event.");
        return false;
    }

    // Beginning Of Run Event, this is the header event.
    if(event->IsBORE()) { 
        EUDAQ_INFO("Starting initialization...");
        Initialize(event, conf);
    }

    const int dev_id = event->GetDeviceN();

/*std::cout << "Number of blocks: " << event->NumBlocks() << " , event number: " << event->GetEventN() 
    << ", event id: " << event->GetEventID()
    << ", stream N: " << event->GetStreamN()
    << ", Run: " << event->GetRunNumber() 
    << ", Type:" << event->GetType()
    << ", Version:" << event->GetVersion()
    << ", Flag:" << event->GetFlag()
    << ", DeviceN:" << event->GetDeviceN()
    << ", N-subEvents:" << event->GetNumSubEvent()
    << ", Trigger Number:" << event->GetTriggerN()
    << ", Extend word:" << event->GetExtendWord()
    << ", TS begin:" << event->GetTimestampBegin()
    << ", TS end:" << event->GetTimestampEnd()
    << ", Description:" << event->GetDescription()
    << std::endl;


std::cin.get();*/

    // Expecting one block per channel
    if(event->NumBlocks() != _dut_channel_list[dev_id].size()) {
        EUDAQ_ERROR(" Expected one block per channel (n-channel: "+ 
                std::to_string(_dut_channel_list[dev_id].size()) + "). Blocks: "+
                std::to_string(event->NumBlocks()) );
        return false;
    }

    d2->SetDetectorType("CAEN5748");
    
    if(!d2->IsFlagPacket()) {
        d2->SetFlag(d1->GetFlag());
        d2->SetRunN(d1->GetRunN());
        d2->SetEventN(d1->GetEventN());
        d2->SetStreamN(d1->GetStreamN());
        d2->SetTriggerN(d1->GetTriggerN(), d1->IsFlagTrigger());
        d2->SetTimestamp(d1->GetTimestampBegin(), d1->GetTimestampEnd(), d1->IsFlagTimestamp());
    }

    const std::string producer_name = _name[d1->GetDeviceN()];
    
    // Extract the event and convert it back to 32b words
    // See data format in CAEN User Manual 9.7.2 
    std::vector<uint8_t> raw = event->GetBlock(0);
    std::vector<uint32_t> raw_event(raw.size() / 4);
    std::memcpy(raw_event.data(), raw.data(), raw.size());
    // Get the size of the event --> To cross-check ?? 
    const size_t total_words = static_cast<size_t>(raw_event[0] & 0x0FFFFFFF);
    if( total_words != raw_event.size() ) {
        // XXX FIXME -- Some error message and break?
    }

    const uint32_t group_present  = raw_event[1] & 0x3; 
    const uint32_t event_counter  = raw_event[2] & 0xFFFFFF;
    const uint32_t event_time_tag = raw_event[3];

    // Loop over all channels
    // Processed event header (4 words)
    size_t offset = 4;
    size_t group_id  = 0;
    std::map<size_t, std::vector<std::vector<float> >> waveforms_group;
    while( offset < raw_event.size() ) {
        // Checking if the current group is present 
        if( ! (group_present >> group_id) & 0x1 ) {
            // check next group, this is not here
            ++group_id;
            continue;
        }

        // Group data extraction (next word)
        uint32_t group_header = raw_event[offset++];
        // Info from the header, the number of words to be read
        const uint32_t ch0_7_words = group_header & 0xFFF;

std::cout << "Group-" << (group_id - 1) 
    << " Group header: 0x" << std::hex << group_header << std::dec
    << " Words to be read to extract all samples (excluding trigger): " << ch0_7_words
    << std::endl;

        // Eech waveform is stored in three words 
        const size_t sample_steps = ch0_7_words / 3;

        // The waveforms for each of the channels (from a total of 8)
        //  each element corresponds to the channel number 
        std::vector<std::vector<float> > waveforms(8);
        for(size_t i = 0; i < sample_steps; ++i) {
            // Extract the three consecutive words to
            // reassemble the total waveform
            uint32_t w0 = raw_event[offset++];
            uint32_t w1 = raw_event[offset++];
            uint32_t w2 = raw_event[offset++];

            waveforms_reassemble(w0, w1, w2, waveforms);
        }
        // All channels of the group are stored
        waveforms_group[group_id] = waveforms;
    }
    // All channels are extracted (from all enabled groups)
    // --> XXX -- TRIGGER TR0 MISSING TO BE DONE
    
    // Each DUT is a plane
    for(const auto & dutname_sensorid: _dut_names_id[dev_id]) {
std::cout << " Dut: " << dutname_sensorid.first << " (ID: " << dutname_sensorid.second << ")" 
    << " Total words in Event: " << total_words 
    << " group present: " << group_present 
    << " event counter: " << event_counter
    << " event time tag: " << event_time_tag 
    << std::endl;
        // XXX - Can we provide a dutname in the stdplane?? 
        const int sensor_id = dutname_sensorid.second;        
        // Each DUT defines a plane
        eudaq::StandardPlane plane(sensor_id, "CAEN5748", producer_name);
        // Define the size of the DUT (in row and columns) --> Extracted from _nrows_ncolumns
        // Remember in here: first columns, then rows
        plane.SetSizeZS( (uint32_t)_nrows_ncolumns[dev_id][dutname_sensorid.second][1], 
            (uint32_t)_nrows_ncolumns[dev_id][dutname_sensorid.second][0],
            _npixels[dev_id][dutname_sensorid.second]);

        int pixid = 0;
        for(const auto & ch_colrowlist: _dut_channel_arrangement[dev_id][dutname_sensorid.second]) {
            const size_t channel = ch_colrowlist.first;
            // What group? 0-7 -> group 0, 8->15 group 1
            const size_t gr = channel < 8 ? 0 : 1; 
            const size_t channel_inside_group = channel < 8 ? channel : channel - 8;
            
            const std::vector<float> & waveform_float = waveforms_group[gr][channel_inside_group];
            // XXX -- Make this sense? Just to avoid crashing... [PROV]
            if(waveform_float.size() == 0)
            {
                ++pixid;
                continue;
            }
            
            // Each channel is wirebonded to the the list of pixels, assign
            // same amplitude and waveform for all the belonging pixels

            // XXX -- Is this what we want? Or maybe extract the integral? 
            //        for sure we'd like to get the rise time as well?
            float amplitude = AmplitudeWF(waveform_float);
/*std::cout << "DUT: " << dutname_sensorid.first << " Sensor: " << dutname_sensorid.second 
    << " Amplitude: " << amplitude << std::endl; 
std::cin.get();*/

            std::vector<double> wf(waveform_float.begin(), waveform_float.end());
            
            for(const auto & pixel: ch_colrowlist.second) {
                // Note the signature introduce x,y -> col, row. Opposite to which we store
                plane.SetPixel(pixid, pixel[1], pixel[0], amplitude);
                plane.SetPixelAuxInfo(pixid, dutname_sensorid.first+":CH"+std::to_string(ch_colrowlist.first));
                plane.SetWaveform(pixid, wf, _t0[dev_id], _dt[dev_id] );
                ++pixid;
            }
/*std::cout << " The Raw data for CH-" << ch_colrowlist.first << ": [size: " << raw_data.size() << "]: " ;
for(const auto & dt: raw_data)
{
    std::cout << " " << dt ;
}
std::cout << std::endl;*/
        }
        d2->AddPlane(plane);
    }
/*d2->Print(std::cout);
std::cin.get();*/

    return true;
}

PixelMap CAENDT5748RawEvent2StdEventConverter::GetDUTPixelMap(const std::string & dut_tag) const {
    
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

        for(std::sregex_iterator cr = std::sregex_iterator(chstr.begin(),chstr.end(),re_colrow); cr != std::sregex_iterator();++cr) {
            std::smatch m = *cr;
            //std::cout << "[ colrow : " << m[1].str() << " " << m[2].str() <<  std::endl;
            ch_dict[current_channel].push_back({std::stoi(m[1].str()),std::stoi(m[2].str())});
        }
    }

    return ch_dict;
}


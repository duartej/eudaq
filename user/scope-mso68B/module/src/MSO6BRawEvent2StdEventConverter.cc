// -- XXX DOC 
// -- 
#include "eudaq/StdEventConverter.hh"
#include "eudaq/RawEvent.hh"
#include "eudaq/Logger.hh"

#include <sstream>
#include <tuple>

#include <vector>
#include <map>
#include <array>
#include <cstdint>
#include <algorithm>
#include <regex>
#include <numeric>
#include <cmath>
#include <iterator>

using DutMap =  std::map<std::string, std::map<int, std::vector<std::pair<int,int>>>>;

// Helper functions to parse BORE tags
DutMap parse_dutinfo(const std::string& dutinfo_str) {
    std::stringstream ss(dutinto_str);
    std::string dut;
    std::string channel_str;
    std::string coords_str;
    
    // Divide by ';'
    std::getline(ss, dut, ";");
    std::getline(ss, channel_str, ";");
    std::getline(ss, coords_str, ";");

    int channel = std::stoi(channel_str);
    std::stringstream coordstream(coords_str);
    std::string value;
    std::vector<int> numbers;

    // Divide by ','
    while( std::getline(coordstream, value, ',') ) {
        numbers.push_back(std::stoi(value));
    }

    DutMap dut_map;

    // Convert into (col,row) pairs
    for(size_t i = 0; i + 1 < numbers.size(); i += 2) {
        dut_map[dut][channel].push_back({numbers[i], numbers[i+1]});
    }

    return dut_map;
}

// The pixels can be bounded to the same channel
// CH : [ (col,row), ... ]
using PixelMap = std::map<int, std::vector<std::array<int,2>> >;

class MSO6BRawEvent2StdEventConverter: public eudaq::StdEventConverter {
    public:
        bool Converting(eudaq::EventSPC d1, eudaq::StdEventSP d2, eudaq::ConfigSPC conf) const override;
        static const uint32_t m_id_factory = eudaq::cstr2hash("MSO6B");

    private:
        void Initialize(eudaq::EventSPC bore, eudaq::ConfigurationSPC conf) const;
        PixelMap GetDUTPixelMap(const std::string & dut_tag) const; 
        // Helper functions
        std::vector<double> payloadToWF(const std::vector<uint8_t> &payload, int channel) const;
        int PolarityWF(const std::vector<float> & wf) const;
        float AmplitudeWF(const std::vector<float> & wf) const;

        static std::map<int, std::string> _name;
        // XXX -- NEEDED?
        static size_t _n_digitizers;
        static size_t _n_samples_per_waveform;
        static std::map<int, size_t> _n_duts;
        // Waveform starting t0 and Dt
        static std::map<int, float> _t0;
        static std::map<int, float> _dt;
        // Scaling 
        static std::map<int, float> _ymult;
        static std::map<int, float> _yzero;
        static std::map<int, float> _yoff;
        // XXX - TBD?
        // XXX -- TBD?
        // { Digitizer: { DUT: { channel : [ (row, col), (row, col), ... ],  ... 
        static DutMap _dut_channel_coords;
        // { Digitizer: { DUT: (n-row,n-col), ... 
        static std::map<int, std::map<int, std::array<int,2>> > _nrows_ncolumns;
        // { Digitizer: { DUT: npixels, ...
        static std::map<int, std::map<int,int> > _npixels;
        // Human-readable name related with the internal DUT-id (sensor-id)
        static std::map<int, std::map<std::string,int> > _dut_names_id;
};

namespace {
    auto dummy0 = eudaq::Factory<eudaq::StdEventConverter>::Register<MSO6BRawEvent2StdEventConverter>(MSO6BRawEvent2StdEventConverter::m_id_factory);
}

// Static data members to avoid loosing info (after initialization of data members)
// [due to the re-creation of the instances each event?]
std::map<int,std::string> MSO6BRawEvent2StdEventConverter::_name;
size_t MSO6BRawEvent2StdEventConverter::_n_scopes = 0;
size_t MSO6BRawEvent2StdEventConverter::_n_samples_per_waveform;
std::map<int, size_t> MSO6BRawEvent2StdEventConverter::_n_duts;
std::map<int, float> MSO6BRawEvent2StdEventConverter::_t0;
std::map<int, float> MSO6BRawEvent2StdEventConverter::_dt;
DutMap _dut_channel_coords;
std::map<int, std::map<int, std::array<int,2>> > MSO6BRawEvent2StdEventConverter::_nrows_ncolumns;
std::map<int, std::map<int,int> > MSO6BRawEvent2StdEventConverter::_npixels;
std::map<int, std::map<std::string,int> > MSO6BRawEvent2StdEventConverter::_dut_names_id;

void MSO6BRawEvent2StdEventConverter::Initialize(eudaq::EventSPC bore, eudaq::ConfigurationSPC conf) const {
    
    const int device_id = bore->GetDeviceN();
    // How many times are initializing = scopes present in the event
    // FIXME -- Use the DeviceN id?
    ++_n_scopes;
    // Name of the producer
    _name[device_id] = bore->GetTag("producer_name");
    
    // Extract all relevant info: DUT -> channels -> list of pixels bounded
    //  { dutname: { channel: [(col,row), ... 
    // No check in sizes, that's was done at producer level
    _dut_channel_coords = parse_dutinfo( bore->GetTag("duts_info") );
    // Assign the sensor id
    int sensor_id = 0;
    for(const dutname_w: _dut_channel_coords) {
        _dut_names_id[dutname.first] = sensor_id;
        ++sensor_id;
    }
    
    // The record length and horizontal scales
    _n_samples_per_waveform = std::stoi(bore->GetTag("sampled_points"));
    // Extract the initial and the temporal step value of the waveforms
    // --- in SECONDS
    _t0[device_id] = std::stof(bore->GetTag("t0"));
    _dt[device_id] = std::stof(bore->GetTag("dt"));
    
    // Extract the tags for each DUT:
    for(const auto & dutname_chcoordvect: _dut_channel_coords[device_id]) {
        auto dut = dutname.first;
        // Get the maximum nrows and ncolumns for the DUT
        // Loop over the channels:
        int nrow = -1;
        int ncol = -1;
        for(const auto & channel_rowcol: dutname_chcoordvect.second) {
            // And loop over all the pixels bounded to this channel
            for(const auto & rowcol: channel_listrowcol.second) {
                if(rowcol[0] > nrow) {
                    nrow = rowcol[0];
                }
                if(rowcol[1] > ncol) {
                    ncol = (rowcol)[1];
                }
            }
            // And the vertical scale info
            std::string ch_str = "channel"+std::to_string(channel);
            _ymult[device_id][channel_listrowcol.first] = bore->GetTag( std::string(ch_str+"_dv").c_str() );
            _yzero[device_id][channel_listrowcol.first] = bore->GetTag( std::string(ch_str+"_v0").c_str() );
            _yoff[device_id][channel_listrowcol.first] =  bore->GetTag( std::string(ch_str+"_voffset").c_str() );
        }
        _nrows_ncolumns[device_id][dutname_id.second] = { nrow+1, ncol+1 };
        // Total number of pixels: Remember starting at 0, then 
       _npixels[device_id][dutname_id.second] = (nrow+1)*(ncol+1);
    }


    // Print-out the topology of the sensor and wire-bonding
    EUDAQ_INFO(" Defined DUTs in [" +_name[device_id]+ "] SCOPE: ");
    for(const auto & dutname_chcoords: _dut_channel_coords[device_id]) {
        EUDAQ_INFO(" [" + dutname_chcoords.first + "], ID:" + std::to_string(dn_id.second) +", "
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
    EUDAQ_DEBUG(" Initialize:: record-length: " + std::to_string(_n_samples_per_waveform) +
            ", number of DUTs: " + std::to_string(_n_duts[device_id]));
    // Get the list of channels
    std::ostringstream oss;
    std::copy(_dut_channel_list[device_id].begin(), _dut_channel_list[device_id].end(), std::ostream_iterator<int>(oss, " "));
    EUDAQ_DEBUG(" Initialize:: Channel list (internal-ids): [ " + oss.str() +" ]");
}

std::vector<double> MSO6BRawEvent2StdEventConverter::payloadToWF(const std::vector<uint8_t> &payload, int channel) const {
    
    // XXX Just assuming 16-bits, MSB-firts (big-endian).
    // It can be incorporated different extractions depending on the bytes_per_point
    std::vector<double> wf;
    // 16-bit signed, MSB-first) 
    for(std::size_t i = 0; i < npts; ++i) {
        uint8_t b0 = payload[2*i];
        uint8_t b1 = payload[2*i + 1];
        int16_t v = static_cast<int16_t>((b0 << 8) | b1);
        // scale to volts
        wf[i] = (static_cast<double>(v) - _yoff[channel]) * ymult[channel] + yzero[channel] ;
    }

    return wf;
}

// FIXME -- Calculate it once: use a memoizer
int MSO6BRawEvent2StdEventConverter::PolarityWF(const std::vector<float> & wf) const {
    // Extract polarity: This must be done from outside
    // using the configuration file --- XXX - TODO

    // XXX-- REMOVE THIS
    /*auto itminmax = std::minmax_element(wf.begin(), wf.end());
    const float min = *itminmax.first;
    const float max = *itminmax.second;
    if( std::abs(*itminmax.first) > std::abs(*itminmax.second) ) {
std::cout << "NEGATIVE - min: " << *itminmax.second << " max: " << *itminmax.first << std::endl;
        return -1;
    }
std::cout << "POSITIVE-min: " << *itminmax.first << " max: " << *itminmax.second << std::endl;
    return 1;*/
    return -1;
}


float MSO6BRawEvent2StdEventConverter::AmplitudeWF(const std::vector<float>& waveform) const {
    // Rough estimation of the baseline using the median
    // But first use the right polarity to be sure we sort properly
    const int polarity = PolarityWF(waveform); 
    std::vector<float> wf_abs(waveform);
    for(float & v: wf_abs) {
        v *= polarity;
    }

    //
    // All signals are now positives
    // -----------------------------

    // Sorted: smaller first --> XXX the wf_abs is changed?
    std::sort(wf_abs.begin(), wf_abs.end());
//std::cout <<  std::endl;
//std::cout << " WAVEFORM (polarity: " << polarity << ") SORTED:\n [" ;
//for(float &vv: wf_abs)
//{
//std::cout << vv<< " " ;
//}
//std::cout <<  std::endl;
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
    
//std::cout << " Baseline: " << baseline << " -- " << wf_amplitude_max 
//    << " stdDev: " << stddev << " --> max > 3_stddev? " 
//    << (std::fabs(wf_amplitude_max) > 3.*(std::fabs(baseline)+stddev)) << std::endl;
    // Assume 3 sigma to be signal XXX -- Algortihm fails when negative!!!
    if( std::fabs(wf_amplitude_max) > 3.0*(std::fabs(baseline) + stddev) ) {
    /* -- XXX - WIP : what I was doing?
std::cout <<  std::endl;
std::cout << " WAVEFORM (polarity: " << polarity << ") SORTED:\n [" ;
for(const float &vv: waveform)
{
std::cout << vv<< ", " ;
}
std::cout << "]" << std::endl;
std::cout << " Baseline: " << baseline << " -- " << wf_amplitude_max 
    << " stdDev: " << stddev << " --> max > 3_stddev? " 
    << (std::fabs(wf_amplitude_max) > 3.*(std::fabs(baseline)+stddev)) << std::endl;*/
        return wf_amplitude_max*polarity;
    }

    return 0.0;
}



bool MSO6BRawEvent2StdEventConverter::Converting(eudaq::EventSPC d1, eudaq::StdEventSP d2, eudaq::ConfigSPC conf) const {

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

    // Expecting one block per channel
    if(event->NumBlocks() != _dut_channel_list[dev_id].size()) {
        EUDAQ_ERROR(" Expected one block per channel (n-channel: "+ 
                std::to_string(_dut_channel_list[dev_id].size()) + "). Blocks: "+
                std::to_string(event->NumBlocks()) );
        return false;
    }

    d2->SetDetectorType("MSO68B");
    
    if(!d2->IsFlagPacket()) {
        d2->SetFlag(d1->GetFlag());
        d2->SetRunN(d1->GetRunN());
        d2->SetEventN(d1->GetEventN());
        d2->SetStreamN(d1->GetStreamN());
        d2->SetTriggerN(d1->GetTriggerN(), d1->IsFlagTrigger());
        d2->SetTimestamp(d1->GetTimestampBegin(), d1->GetTimestampEnd(), d1->IsFlagTimestamp());
    }

    const std::string producer_name = _name[d1->GetDeviceN()];
    // Each DUT is a plane
    for(const auto & dutname_sensorid: _dut_names_id[dev_id]) {
        // XXX - Can we provide a dutname in the stdplane?? 
        const int sensor_id = dutname_sensorid.second;        
        // Each DUT defines a plane
        eudaq::StandardPlane plane(sensor_id, "MSO6B", producer_name);
        // Define the size of the DUT (in row and columns) --> Extracted from _nrows_ncolumns
        // Remember in here: first columns, then rows
        plane.SetSizeZS( (uint32_t)_nrows_ncolumns[dev_id][dutname_sensorid.second][1], 
                (uint32_t)_nrows_ncolumns[dev_id][dutname_sensorid.second][0],
                0);
        
        // Each channel is stored in a block
        int pixid = 0;
        for(const auto & ch_rowcollist: _dut_channel_arrangement[dev_id][dutname_sensorid.second]) {
            const size_t n_block = ch_rowcollist.first;
            std::vector<double> wf = payloadToWF(event->GetBlock(n_block), n_block);
            
            // XXX -- Make this sense? Just to avoid crashing... [PROV]
            if(raw_data.size() == 0)
            {
                //++pixid;
                continue;
            }
/* -- XXX - WIP : what I was doing?
{
std::cout << "--------------------------------------- " << std::endl;
std::cout << "[" << producer_name << "] DUT: " << dutname_sensorid.first << " Sensor: " << dutname_sensorid.second  << " PIXID: " << pixid << std::endl;
 }*/
            
            // Each channel is wirebonded to the the list of pixels, assign
            // same amplitude and waveform for all the belonging pixels

            // XXX -- Is this what we want? Or maybe extract the integral? 
            //        for sure we'd like to get the rise time as well?
            float amplitude = AmplitudeWF(wf);

/*if(producer_name == "CAEN_IJS")
{
std::cout << "--------------------------------------- " << std::endl;
std::cout << "DUT: " << dutname_sensorid.first << " Sensor: " << dutname_sensorid.second  << " PIXID: " << pixid << std::endl;
}*/
            for(const auto & pixel: ch_rowcollist.second) {
/*if(producer_name == "CAEN_IJS")
{
std::cout << "Block id: " << ch_rowcollist.first << " pixid: " << pixid << ", pixel: col-" << pixel[1] << " ,row-" << pixel[0]
    << " A=" << amplitude << std::endl ;
}*/
                // Note the signature introduce x,y -> col, row. Opposite to which we store
                plane.PushPixel(pixel[1], pixel[0], amplitude, uint32_t(0));
                plane.SetPixelAuxInfo(pixid, dutname_sensorid.first+":CH"+std::to_string(ch_rowcollist.first)+":col"+std::to_string(pixel[1])+":row"+std::to_string(pixel[0]));
                plane.SetWaveform(pixid, wf, _t0[dev_id], _dt[dev_id] );
                ++pixid;
            }
        }
        d2->AddPlane(plane);
    }
/*d2->Print(std::cout);
std::cin.get();*/
/* -- XXX - WIP : what I was doing?
std::cout << "EEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEE " << std::endl;
std::cin.get();
*/
    return true;
}

PixelMap MSO6BRawEvent2StdEventConverter::GetDUTPixelMap(const std::string & dut_tag) const {
    
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


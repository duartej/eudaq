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

// XXX PROV
#include <iterator>
// XXX PROV
//


// Helper to facilitate legibility: 
// Digitizer: { DUT: { channel : [ (row, col), (row, col), ... ], 
using pixel_map = std::map<int, std:vector<std::array<int,2>> >;

class CAENDT5748RawEvent2StdEventConverter: public eudaq::StdEventConverter {
    public:
        bool Converting(eudaq::EventSPC d1, eudaq::StdEventSP d2, eudaq::ConfigSPC conf) const override;
        static const uint32_t m_id_factory = eudaq::cstr2hash("CAENDT5748");

    private:
        void Initialize(eudaq::EventSPC bore, eudaq::ConfigurationSPC conf) const;
        pixel_map _GetDUTPixelMap(const std::string & dut_tag, int device_id) const; 
        static std::map<int, std::string> _name;
        static size_t _n_digitizers;
        static size_t _n_samples_per_waveform;
        static size_t _sampling_frequency_MHz;
        static std::map<int, size_t> _n_duts;
        static std::map<int, std::vector<std::string> > _channel_names_list;
        static std::map<int, std::vector<int> > _dut_channel_list;
        // Digitizer: { DUT: { channel : [ (row, col), (row, col), ... ], 
        static std::map<int, std::map<int, pixel_map> > _dut_channel_arrangement;
        // Human-readable name related with the internal DUT-id
        static std::map<int, std::map<std::string,int> > _dut_names_id;
        static std::map<int, std::vector<std::string> > DUTs_names;
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
std::map<int, std::vector<std::string> > CAENDT5748RawEvent2StdEventConverter::_channel_names_list;
std::map<int, std::vector<int> > CAENDT5748RawEvent2StdEventConverter::_dut_channel_list;
pixel_map CAENDT5748RawEvent2StdEventConverter::_dut_channel_arrangement;
std::map<int, std::map<std::string,int> > CAENDT5748RawEvent2StdEventConverter::_dut_names_id;
std::map<int, std::vector<std::string> > CAENDT5748RawEvent2StdEventConverter::DUTs_names;

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
    // The number of DUTS
    _n_duts[device_id] = std::stoi(bore->GetTag("number_of_DUTs"));
    
    // XXX -- Is it needed this way? It could be implemented easily this info...
    // XXX -- TBC: into a function
    // Parse list with the names of the channels in the order they will appear in the raw data:
    std::string s( bore->GetTag("dut_names") );
    for (char c : {'[', ']', '\'', '\"'}) {
        s.erase(remove(s.begin(), s.end(), c), s.end());
    }
    std::string delimiter = ", ";
    size_t pos = 0;
    int dut_internal_id = 0
    while((pos = s.find(delimiter)) != std::string::npos) {
        std::string token = s.substr(0, pos);
        _dut_names_id[device_id][token] = dut_internal_id;
        s.erase(0, pos + delimiter.length());
        ++dut_internal_di;
    }
    // And the last one...
    _dut_names_id[device_id][s] = dut_internal_id;

    // Extract the tags for each DUT:
    for(const auto & dutname_id: _dut_names_id) {
        _dut_channel_arrangement[device_id][dutname_id.second] = _GetDUTPixelMap(bore->GetTag(dutname_id.first));
    }

    
    /*
    // For each DUT, build a matrix where the elements are integer numbers specifying the 
    // position where the respective waveform begins in the raw data:
    for (size_t n_DUT=0; n_DUT<_n_duts[device_id]; n_DUT++) {
        // XXX --- Needed?
        DUTs_names[device_id].push_back(bore->GetTag("DUT_"+std::to_string(n_DUT)+"_name"));
        // Human-readable to internal id
        _dut_names_id[device_id][*DUTs_names[deviced_id].back()] = 
        // Gets something like e.g. `"[['CH4', 'CH5'], ['CH6', 'CH7']]"`.
        s = bore->GetTag("DUT_"+std::to_string(n_DUT)+"_channels_matrix");
        if (s.empty()) {
            EUDAQ_THROW("Cannot get information about the channels to which the DUT named \""+DUTs_names[device_id][n_DUT]+"\" was connected.");
        }

        // Identify the channels for this DUT
        const std::regex re_channels("CH([0-9])*");
        for(std::sregex_iterator it = std::sregex_iterator(s.begin(), s.end(), re_channels);
                it != std::sregex_iterator();
                ++it)
        {
            std::smatch m = *it;
            _dut_channel_list[n_DUT].emplace_back( std::stoi(m[1].str().c_str()) );
        }

        // Get the arrangement
        s = bore->GetTag("DUT_"+std::to_string(n_DUT)+"_channels_arrangement");
        const std::regex re_arrangement("CH[0-9]*: ([0-9]*),([0-9]*)");
        for(std::sregex_iterator it = std::sregex_iterator(s.begin(), s.end(), re_arrangement);
                it != std::sregex_iterator();
                ++it)
        {
            std::smatch m = *it;
            // --- FIXME  error control?
            _dut_channel_arrangement[device_id][n_DUT].push_back( { std::stoi(m[1].str().c_str()), std::stoi(m[2].str().c_str()) } );
        }

    }

std::cout << "--> ";
for(auto & ndut_map: _dut_channel_arrangement[device_id])
{
    std::cout << "--> DUT-" << ndut_map.first << " [";
    int _i = 0;
    for(auto & chorder: ndut_map.second)
    {
        std::cout << "--> CH-" << _i << " " << chorder[0] << " " << chorder[1] << std::endl;
        ++_i;
    }
std::cout << "]" << std::endl;
}
std::cin.get();
//std::ostringstream ossa;
//std::copy(DUTs_names[device_id].begin(), DUTs_names[device_id].end(), std::ostream_iterator<std::string>(ossa, " "));
//std::cout << " Initialize: DUSTs list-[ " << ossa.str() << "]" << std::endl;*/

    // Debugging print-out stuff
    EUDAQ_DEBUG(" Initialize: nsamples_per_waveform: " + std::to_string(_n_samples_per_waveform) +
            ", sampling frequency: " + std::to_string(_sampling_frequency_MHz) + " MHz" +
            ", number of DUTs: " + std::to_string(_n_duts[device_id]));
    // Get the list of channels
    std::ostringstream oss;
    std::copy(_channel_names_list[device_id].begin(), _channel_names_list[device_id].end(), std::ostream_iterator<std::string>(oss, " "));
    EUDAQ_DEBUG(" Initialize: Channel names list-[ " + oss.str() +"]");
    // And the DUTs
    // clear first the oss
    oss.str("");
    oss.clear();
    std::copy(DUTs_names[device_id].begin(), DUTs_names[device_id].end(), std::ostream_iterator<std::string>(oss, " ")) ;
    EUDAQ_DEBUG(" Initialize: DUT names-["+oss.str()+"]");
    oss.str("");
    oss.clear();
    for(const auto & dutid_chlist: _dut_channel_list)
    {
        oss << " [DUT:" << dutid_chlist.first << "]-{";
        for(const auto & _ch: dutid_chlist.second)
        {
            oss << " " << _ch ;
        }
        oss << " } ";
    }
    EUDAQ_DEBUG(" Initialize:"+oss.str());
}

std::vector<float> uint8VectorToFloatVector(std::vector<uint8_t> data) {
    // Everything in this function, except for this single line, was provided to me by ChatGPT. Amazing.
    std::vector<float> result;
    // size the result vector appropriately
    result.resize(data.size() / sizeof(float)); 
    // cast the pointer to float
    float* resultPtr = reinterpret_cast<float*>(&result[0]);

    // get a pointer to the data in the uint8_t vector
    uint8_t* dataPtr = &data[0];

    // get the size of the data in the uint8_t vector
    size_t dataSize = data.size(); 

    for (size_t i = 0; i < dataSize; i += sizeof(float)) {
        // cast the value at dataPtr to float and store in result vector
        *resultPtr = *reinterpret_cast<float*>(dataPtr); 
        resultPtr++;
        dataPtr += sizeof(float);
    }
    
    return result;
}

float median(std::vector<float>& vec) {
    std::sort(vec.begin(), vec.end());
    int size = vec.size();
    if (size % 2 == 0) {
        return (vec[size/2 - 1] + vec[size/2]) / 2.0;
    } else {
        return vec[size/2];
    }
}

float max(const std::vector<float>& vec) {
    auto it = std::max_element(vec.begin(), vec.end());
    return *it;
}

float amplitude_from_waveform(std::vector<float>& waveform) {
    float base_line = median(waveform);
    return max(waveform) - base_line;
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

std::cout << " ESTOY-1" <<std::endl;
std::cout << "Data members: No-channels:" << _channel_names_list[dev_id].size() 
    << " _dut_channel_list: " << _dut_channel_list.size()
    << " Nsamples_per_wf: " << _n_samples_per_waveform
    << " DUTS names: " << DUTs_names[dev_id].size() << "[";
for(auto & kk: DUTs_names[dev_id])
{
    std::cout << " " << kk ;
}
std::cout << " ]" << std::endl;

std::cin.get();*/

    // Expecting one block per channel
    // XXX - Should be right, remember to uncomment
    if(event->NumBlocks() != _channel_names_list[dev_id].size()) {
        EUDAQ_ERROR("Expected one block per channel (n-channel: "+ 
                std::to_string(_channel_names_list.size()) + "). Blocks: "+
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
    // Each DUT is a plane
    for(const auto & dutname_sensorid: _dut_names_id) {
        // XXX - Can we use the dutname in the stdplane?? 
        const int sensor_id = dutname_sensorid.second;        
        // Each DUT defines a plane
        eudaq::StandardPlane plane(sensor_id, "CAEN5748", producer_name);
        // Define the size of the DUT (in row and columns) 
        // XXX  --- Check it is fine
        plane.SetSizeZS( (uint32_t)_dut_channel_arrangement[dev_id][dutname_sensorid.second].size(), 
                (uint32_t)_dut_channel_arrangement[dev_id][dutname_sensorid.second].size(),
                dut_chlist.second.size());
        
        // Each channel is stored in a block
        uint8_t pixid = 0;
        for(const auto & channel: dut_chlist.second) {
            const size_t n_block = channel;
            std::vector<float> raw_data = uint8VectorToFloatVector(event->GetBlock(n_block));

            // XXX -- Make this sense? Just to avoid crashing... [PROV]
            if(raw_data.size() == 0)
            {
                ++pixid;
                continue;
            }

            // XXX -- Is this what we want? Or maybe extract the integral? 
            //        for sure we'd like to get the rise time as well?
            double amplitude = amplitude_from_waveform(raw_data);
            
            // Note the signature introduce x,y -> col, row. Opposite which 
            // what we store
            plane.SetPixel(pixid, 
                    _dut_channel_arrangement[dev_id][dut_id][channel][1],
                    _dut_channel_arrangement[dev_id][dut_id][channel][0],
                    amplitude);
            
            // FIXME --- Hardcoded!
            std::vector<float> samples_times;
            for(size_t n_sample=0; n_sample<_n_samples_per_waveform; n_sample++) {
                // Convert back into seconds
                samples_times.push_back(n_sample*_sampling_frequency_MHz*1e6);
            }

            std::vector<double> wf(raw_data.begin(), raw_data.end());
            // Set The Waveform
            plane.SetWaveform(pixid, wf, samples_times[0], samples_times.back() );
            
            ++pixid;
/*std::cout << " The Raw data for CH-" << channel << ": [size: " << raw_data.size() << "]: " ;
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

void CAENDT5748RawEvent2StdEventConverter::_GetDUTPixelMap(const std::string & dut_tag) {
    
    // It must exist a tag with the name of the DUT
    std::string s( bore->GetTag(dut tag) );
    // FIXME -- Error control: empyt string!!
    
    // Parsing something like:
    // "CH1: [(0,0),(0,1),(1,1),(2,0)], CH3: [(0,1)], CH6: [(1,2), (3,10)]"
    
    // ---- Split in blocks of CH
    std::regex token(R"(\])");
    std::vector<std::string> substrings(std::sregex_token_iterator(s.begin(), s.end(), token, -1), {});

    // For each substring: extract the channel and the list of col and rows
    std::regex re_ch(R"(CH(\d*):)");
    std::regex re_colrow(R"(\((\d+),(\d+)\))");

    std::map<int,std::vector<std::array<int,2> > > ch;
    for(auto & chstr: substrings)
    {
        int current_channel = -1;
        for(std::sregex_iterator it = std::sregex_iterator(chstr.begin(),chstr.end(),re_ch); it != std::sregex_iterator();++it)
        {
            std::smatch m = *it;
            //std::cout << "[->> " << m[1].str() << std::endl;
            current_channel = std::stoi(m[1]);
        }

        for(std::sregex_iterator cr = std::sregex_iterator(chstr.begin(),chstr.end(),re_colrow); cr != std::sregex_iterator();++cr)
        {
               std::smatch m = *cr;
               //std::cout << "[ colrow : " << m[1].str() << " " << m[2].str() <<  std::endl;
               ch[current_channel].push_back({std::stoi(m[1].str()),std::stoi(m[2].str())});
        }
    }

    return ch;
}

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

class CAENDT5748RawEvent2StdEventConverter: public eudaq::StdEventConverter {
    public:
        bool Converting(eudaq::EventSPC d1, eudaq::StdEventSP d2, eudaq::ConfigSPC conf) const override;
        static const uint32_t m_id_factory = eudaq::cstr2hash("CAENDT5748");

    private:
        void Initialize(eudaq::EventSPC bore, eudaq::ConfigurationSPC conf) const;
        static size_t _n_samples_per_waveform;
        static size_t _sampling_frequency_MHz;
        static size_t _n_duts;
        static std::vector<std::string> _channel_names_list;
        static std::map<int, std::vector<int> > _dut_channel_list;
        // DUT: [ (ch0-col-id, ch0-row-id), (ch1-col-id, ch1-row-id] , ... 
        static std::map<int, std::vector<std::array<int,2> > > _dut_channel_arrangement;
        static std::vector<std::string> DUTs_names;
        // waveform_position[n_DUT][nxpixel][nypixel] = position where this waveform begins in the raw data.
        static std::vector<std::vector<std::vector<size_t>>> waveform_position; 
};

namespace {
    auto dummy0 = eudaq::Factory<eudaq::StdEventConverter>::Register<CAENDT5748RawEvent2StdEventConverter>(CAENDT5748RawEvent2StdEventConverter::m_id_factory);
}

// Static data members to avoid loosing info (after initialization of data members)
// [due to the re-creation of the instances each event?]
size_t CAENDT5748RawEvent2StdEventConverter::_n_samples_per_waveform;
size_t CAENDT5748RawEvent2StdEventConverter::_sampling_frequency_MHz;
size_t CAENDT5748RawEvent2StdEventConverter::_n_duts;
std::vector<std::string> CAENDT5748RawEvent2StdEventConverter::_channel_names_list;
std::map<int, std::vector<int> > CAENDT5748RawEvent2StdEventConverter::_dut_channel_list;
std::map<int, std::vector<std::array<int,2> > > CAENDT5748RawEvent2StdEventConverter::_dut_channel_arrangement;
std::vector<std::string> CAENDT5748RawEvent2StdEventConverter::DUTs_names;
std::vector<std::vector<std::vector<size_t>>> CAENDT5748RawEvent2StdEventConverter::waveform_position; 

void CAENDT5748RawEvent2StdEventConverter::Initialize(eudaq::EventSPC bore, eudaq::ConfigurationSPC conf) const {
    
    // XXX -- Identify the DUTS with the Channels

    // The record length
    _n_samples_per_waveform = std::stoi(bore->GetTag("n_samples_per_waveform"));
    // The sampling frequency
    _sampling_frequency_MHz = std::stoi(bore->GetTag("sampling_frequency_MHz"));
    // The number of DUTS
    _n_duts = std::stoi(bore->GetTag("number_of_DUTs"));
    
    // XXX -- Is it needed this way? It could be implemented easily this info...
    // XXX -- TBC: into a function
    // Parse list with the names of the channels in the order they will appear in the raw data:
    std::string s( bore->GetTag("channels_names_list") );
    for (char c : {'[', ']', '\'', '\"'}) {
        s.erase(remove(s.begin(), s.end(), c), s.end());
    }
    std::string delimiter = ", ";
    size_t pos = 0;
    while ((pos = s.find(delimiter)) != std::string::npos) {
        std::string token = s.substr(0, pos);
        _channel_names_list.push_back(token);
        s.erase(0, pos + delimiter.length());
    }
    // And the last one...
    _channel_names_list.push_back(s);

    // For each DUT, build a matrix where the elements are integer numbers specifying the 
    // position where the respective waveform begins in the raw data:
    for (size_t n_DUT=0; n_DUT<_n_duts; n_DUT++) {
        // XXX --- Needed?
        DUTs_names.push_back(bore->GetTag("DUT_"+std::to_string(n_DUT)+"_name"));
        // Gets something like e.g. `"[['CH4', 'CH5'], ['CH6', 'CH7']]"`.
        s = bore->GetTag("DUT_"+std::to_string(n_DUT)+"_channels_matrix");
        if (s.empty()) {
            EUDAQ_THROW("Cannot get information about the channels to which the DUT named \""+DUTs_names[n_DUT]+"\" was connected.");
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
            _dut_channel_arrangement[n_DUT].push_back( { std::stoi(m[1].str().c_str()), std::stoi(m[2].str().c_str()) } );
        }

    }

    // Debugging print-out stuff
    EUDAQ_DEBUG(" Initialize: nsamples_per_waveform: " + std::to_string(_n_samples_per_waveform) +
            ", sampling frequency: " + std::to_string(_sampling_frequency_MHz) + " MHz" +
            ", number of DUTs: " + std::to_string(_n_duts));
    // Get the list of channels
    std::ostringstream oss;
    std::copy(_channel_names_list.begin(), _channel_names_list.end(), std::ostream_iterator<std::string>(oss, " "));
    EUDAQ_DEBUG(" Initialize: Channel names list-[ " + oss.str() +"]");
    // And the DUTs
    // clear first the oss
    oss.str("");
    oss.clear();
    std::copy(DUTs_names.begin(), DUTs_names.end(), std::ostream_iterator<std::string>(oss, " ")) ;
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

    auto event = std::dynamic_pointer_cast<const eudaq::RawEvent>(d1);
    if (event == nullptr) {
        EUDAQ_ERROR("Received null event.");
        return false;
    }

    // Beginning Of Run Event, this is the header event.
    if(event->IsBORE()) { 
        EUDAQ_INFO("Starting initialization...");
        Initialize(event, conf);
    }

std::cout << "Number of blocks: " << event->NumBlocks() << " , event number: " << event->GetEventN() 
    << ", event id: " << event->GetEventID()
    << ", stream N: " << event->GetStreamN()
    << ", Run: " << event->GetRunNumber() 
    << ", Type:" << event->GetType()
    << ", Version:" << event->GetVersion()
    << ", Flag:" << event->GetFlag()
    << ", DeviceN:" << event->GetDeviceN()
    << ", Trigger Number:" << event->GetTriggerN()
    << ", Extend word:" << event->GetExtendWord()
    << ", TS begin:" << event->GetTimestampBegin()
    << ", TS end:" << event->GetTimestampEnd()
    << ", Description:" << event->GetDescription()
    << std::endl;
std::cin.get();

    /*if(event->NumBlocks() != 1) {
        EUDAQ_ERROR("One and only one data block per event is expected, but received an event with " + \
                std::to_string(event->NumBlocks()) + " blocks.");
        return false;
    }*/

    // Each block is a channel
    for(const auto & dut_chlist: _dut_channel_list) {
        // XXX -- Extract here the dut-id and do whatever you want to do
        const int dut_id = dut_chlist.first;        
        // Each DUT defines a plane
        eudaq::StandardPlane plane(dut_id, DUTs_names[dut_id]);
        // 
        plane.SetSizeZS( (uint32_t)_dut_channel_arrangement[dut_id].size(), 
                (uint32_t)_dut_channel_arrangement[dut_id].size(),
                dut_chlist.second.size());
        
        // Each channel is stored in a block
        uint8_t pixid = 0;
        for(const auto & channel: dut_chlist.second) {
            const size_t n_block = channel;
            std::vector<float> raw_data = uint8VectorToFloatVector(event->GetBlock(n_block));

            // XXX -- Is this what we want? Or maybe extract the integral? 
            //        for sure we'd like to get the rise time as well?
            double amplitude = amplitude_from_waveform(raw_data);
        
std::cout << _dut_channel_arrangement[dut_id].size() << " CHANNEL: " << channel << std::endl;
std::cout << _dut_channel_arrangement[dut_id][channel].size() << " CHANNEL: " << channel << std::endl;
            plane.SetPixel(pixid, 
                    _dut_channel_arrangement[dut_id][channel][0],
                    _dut_channel_arrangement[dut_id][channel][1],
                    amplitude);
            
            // FIXME --- Hardcoded!
            std::vector<float> samples_times;
            for(size_t n_sample=0; n_sample<n_samples_per_waveform; n_sample++) {
                // Convert back into seconds
                samples_times.push_back(n_sample*sampling_frequency_MHz*1e6);
            }

            std::vector<double> wf(raw_data.begin(), raw_data.end());
            // Set The Waveform
            plane.SetWaveform(pixid, wf, samples_times[0], samples_times.back() );

std::cout << " The Raw data for CH-" << channel << ": [size: " << raw_data.size() << "]: " ;
for(const auto & dt: raw_data)
{
    std::cout << " " << dt ;
}
            ++pixid ;
            
std::cout << std::endl;
        }
        d2->AddPlane(plane);
}
std::cin.get();
    return true;
}

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
        static std::map<int, std::string> _name;
        static size_t _n_digitizers;
        static size_t _n_samples_per_waveform;
        static size_t _sampling_frequency_MHz;
        static std::map<int, size_t> _n_duts;
        static std::map<int, std::vector<std::string> > _channel_names_list;
        static std::map<int, std::vector<int> > _dut_channel_list;
        // DUT: [ (ch0-col-id, ch0-row-id), (ch1-col-id, ch1-row-id] , ... 
        static std::map<int, std::vector<std::array<int,2> > > _dut_channel_arrangement;
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
std::map<int, std::vector<std::array<int,2> > > CAENDT5748RawEvent2StdEventConverter::_dut_channel_arrangement;
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
    std::string s( bore->GetTag("channels_names_list") );
    for (char c : {'[', ']', '\'', '\"'}) {
        s.erase(remove(s.begin(), s.end(), c), s.end());
    }
    std::string delimiter = ", ";
    size_t pos = 0;
    while((pos = s.find(delimiter)) != std::string::npos) {
        std::string token = s.substr(0, pos);
        _channel_names_list[device_id].push_back(token);
        s.erase(0, pos + delimiter.length());
    }
    // And the last one...
    _channel_names_list[device_id].push_back(s);

    // For each DUT, build a matrix where the elements are integer numbers specifying the 
    // position where the respective waveform begins in the raw data:
    for (size_t n_DUT=0; n_DUT<_n_duts[device_id]; n_DUT++) {
        // XXX --- Needed?
        DUTs_names[device_id].push_back(bore->GetTag("DUT_"+std::to_string(n_DUT)+"_name"));
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
            _dut_channel_arrangement[n_DUT].push_back( { std::stoi(m[1].str().c_str()), std::stoi(m[2].str().c_str()) } );
        }

    }
std::ostringstream ossa;
std::copy(DUTs_names[device_id].begin(), DUTs_names[device_id].end(), std::ostream_iterator<std::string>(ossa, " "));
std::cout << " Initialize: DUSTs list-[ " << ossa.str() << "]" << std::endl;

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

std::cout << "Number of blocks: " << event->NumBlocks() << " , event number: " << event->GetEventN() 
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

std::cin.get();

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
    for(const auto & dut_chlist: _dut_channel_list) {
        // XXX -- Extract here the dut-id and do whatever you want to do
        const int dut_id = dut_chlist.first;        
        // Each DUT defines a plane
        eudaq::StandardPlane plane(dut_id, "CAEN5748", producer_name);
        // 
        plane.SetSizeZS( (uint32_t)_dut_channel_arrangement[dut_id].size(), 
                (uint32_t)_dut_channel_arrangement[dut_id].size(),
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
        
            plane.SetPixel(pixid, 
                    _dut_channel_arrangement[dut_id][channel][0],
                    _dut_channel_arrangement[dut_id][channel][1],
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



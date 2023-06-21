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
// Digitizer: { channel : [ (row, col), (row, col), ... ], 
// Each channel can be bounded to several diodes/pixels
using PixelMap = std::map<int, std::vector<std::array<int,2>> >;

class CAENDT5748RawEvent2StdEventConverter: public eudaq::StdEventConverter {
    public:
        bool Converting(eudaq::EventSPC d1, eudaq::StdEventSP d2, eudaq::ConfigSPC conf) const override;
        static const uint32_t m_id_factory = eudaq::cstr2hash("CAENDT5748");

    private:
        void Initialize(eudaq::EventSPC bore, eudaq::ConfigurationSPC conf) const;
        PixelMap _GetDUTPixelMap(const std::string & dut_tag) const; 
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
        _dut_channel_arrangement[device_id][dutname_id.second] = _GetDUTPixelMap(bore->GetTag(dutname_id.first));
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
        _nrows_ncolumns[device_id][dutname_id.second] = { nrow, ncol };
        // Total number of pixels
       _npixels[device_id][dutname_id.second] = nrow*ncol;
    }

    // Extract the initial (hardcoded to 0) and the temporal step value of the waveforms
    // --- in SECONDS
    _t0[device_id] = 0.0;
    _dt[device_id] = (_sampling_frequency_MHz*1e6)/_n_samples_per_waveform;

    // Print-out some info about the config data
    EUDAQ_INFO(" Defined DUTs in [" +_name[device_id]+ "] digitizer: ");
    for(auto & dn_id: _dut_names_id[device_id]) {
        EUDAQ_INFO(" [" + dn_id.first + "], internal-id:" + std::to_string(dn_id.second) +", "
                + std::to_string(_dut_channel_arrangement[device_id][dn_id.second].size())
                + " channels");
        std::string list_pixels;
        for(const auto & ch_listpixels: _dut_channel_arrangement[device_id][dn_id.second]) {
            list_pixels += "  "+std::to_string(_npixels[device_id][dn_id.second]) 
                + "-CH" + std::to_string(ch_listpixels.first) + ": {";
            for(const auto & pixels: ch_listpixels.second) {
                list_pixels += " ("+std::to_string(pixels[0]) + "," +std::to_string(pixels[1]) + ")";
            }
            list_pixels += " }, ";
        }
        EUDAQ_INFO(list_pixels);
    }

std::cin.get();

    // Debugging print-out stuff
    EUDAQ_DEBUG(" Initialize:: nsamples_per_waveform: " + std::to_string(_n_samples_per_waveform) +
            ", sampling frequency: " + std::to_string(_sampling_frequency_MHz) + " MHz" +
            ", number of DUTs: " + std::to_string(_n_duts[device_id]));
    // Get the list of channels
    std::ostringstream oss;
    std::copy(_dut_channel_list[device_id].begin(), _dut_channel_list[device_id].end(), std::ostream_iterator<int>(oss, " "));
    EUDAQ_DEBUG(" Initialize:: Channel list (internal-ids): [ " + oss.str() +" ]");
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
    // Each DUT is a plane
    for(const auto & dutname_sensorid: _dut_names_id[dev_id]) {
        // XXX - Can we provide a dutname in the stdplane?? 
        const int sensor_id = dutname_sensorid.second;        
        // Each DUT defines a plane
        eudaq::StandardPlane plane(sensor_id, "CAEN5748", producer_name);
        // Define the size of the DUT (in row and columns) --> Extracted from _nrows_ncolumns
        // Remember in here: first columns, then rows
OJOOOOO -LA DEFINICION DEP IXELS
std::cout << " SetSizeZS " << _npixels[dev_id][dutname_sensorid.second] << std::endl;
        plane.SetSizeZS( (uint32_t)_nrows_ncolumns[dev_id][dutname_sensorid.second][1], 
                (uint32_t)_nrows_ncolumns[dev_id][dutname_sensorid.second][0],
                _npixels[dev_id][dutname_sensorid.second]);
        
        // Each channel is stored in a block
        uint8_t pixid = 0;
        for(const auto & chlist_colrow: _dut_channel_arrangement[dev_id][dutname_sensorid.second]) {
std::cout << " ch: " << chlist_colrow.first << std::endl;
            const size_t n_block = chlist_colrow.first;
std::cout << " ANTESDESPUES ch: " << chlist_colrow.first << std::endl;
            std::vector<float> raw_data = uint8VectorToFloatVector(event->GetBlock(n_block));
std::cout << " DESPUES ch: " << chlist_colrow.first << std::endl;

            // XXX -- Make this sense? Just to avoid crashing... [PROV]
            if(raw_data.size() == 0)
            {
                ++pixid;
                continue;
            }

            // XXX -- Is this what we want? Or maybe extract the integral? 
            //        for sure we'd like to get the rise time as well?
            double amplitude = amplitude_from_waveform(raw_data);
            
            // Each channel is wirebonded to the the list of pixels, assuming the amplitude
            // is shared between all the pixels (same for all of them)
            std::vector<double> wf(raw_data.begin(), raw_data.end());
            for(const auto & pixel: chlist_colrow.second) {
                // Note the signature introduce x,y -> col, row. Opposite to which we store
std::cout << " =====" << pixel.size() << std::endl;
//FALLA AQUI --> Probablmente tenga que reservar los pixeles (
                plane.SetPixel(pixid, pixel[1], pixel[0], amplitude);
std::cout << " ANTS: " << pixid << std::endl;
                plane.SetWaveform(pixid, wf, _t0[dev_id], _dt[dev_id] );
std::cout << " DEPSIUES: " << pixid << std::endl;
                // Set The Waveform
                ++pixid;
            }
/*std::cout << " The Raw data for CH-" << channel << ": [size: " << raw_data.size() << "]: " ;
for(const auto & dt: raw_data)
{
    std::cout << " " << dt ;
}
std::cout << std::endl;*/
        }
std::cout << " PLANE:" << std::endl;
        d2->AddPlane(plane);
std::cout << " PLANE DESPUES: " << std::endl;
    }
/*d2->Print(std::cout);
std::cin.get();*/

    return true;
}

PixelMap CAENDT5748RawEvent2StdEventConverter::_GetDUTPixelMap(const std::string & dut_tag) const {
    
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

std::cout << "-----> " << dut_tag << std::endl;
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
            if( chstr.find("trigger_group_0") != std::string::npos) {
                current_channel = 16;
            }
            else if( chstr.find("trigger_group_1") != std::string::npos) {
                current_channel = 17;
            }
            // -- TODO - Error control?
        }
std::cout << "-----> " << current_channel << " === " << chstr << std::endl;

        for(std::sregex_iterator cr = std::sregex_iterator(chstr.begin(),chstr.end(),re_colrow); cr != std::sregex_iterator();++cr) {
            std::smatch m = *cr;
            //std::cout << "[ colrow : " << m[1].str() << " " << m[2].str() <<  std::endl;
            ch_dict[current_channel].push_back({std::stoi(m[1].str()),std::stoi(m[2].str())});
        }

    }

    return ch_dict;
}


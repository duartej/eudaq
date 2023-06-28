// --
//   DOC ...

#include "eudaq/OptionParser.hh"
#include "eudaq/FileReader.hh"
#include "eudaq/StdEventConverter.hh"
#include "eudaq/Event.hh"
#include "eudaq/RawEvent.hh"

#include "TFile.h"
#include "TTree.h"

#include <iostream>
#include <memory>
#include <map>
#include <vector>
#include <regex>


using PixelMap = std::map<int, std::vector<std::array<int,2>> >;

PixelMap GetDUTPixelMap(const std::string & dut_tag) 
{
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

        if( current_channel == -1 ) 
        {
            // there is no integer in channel, therefore trigger_group_0 or trigger_group_1
            // HARDCODED in the producer, hardcoded here
            // They are also hardcoded as CH16 being in the pixel (0,0) and CH17 in the pixel (0,1)
            // No matter what user introduces
            if( chstr.find("trigger_group") != std::string::npos) 
            {
                if( chstr.find("group_0") != std::string::npos) 
                {
                    current_channel = 16;
                    ch_dict[current_channel].push_back({0,0});
                }
                else if( chstr.find("group_1") != std::string::npos) 
                {
                    current_channel = 17;
                    ch_dict[current_channel].push_back({0,1});
                }
                else 
                {
                    std::cerr << "Malformed Connections file. Expecting `trigger_group_0` or"
                            << "`trigger_group_1`, but found `" << chstr << "`"<< std::endl;
                }
                // pixel defined already
                continue;
            }
        }

        for(std::sregex_iterator cr = std::sregex_iterator(chstr.begin(),chstr.end(),re_colrow); cr != std::sregex_iterator();++cr) 
        {
            std::smatch m = *cr;
            //std::cout << "[ colrow : " << m[1].str() << " " << m[2].str() <<  std::endl;
            ch_dict[current_channel].push_back({std::stoi(m[1].str()),std::stoi(m[2].str())});
        }
    }

    return ch_dict;
}


class CAENDT5742Events 
{
    public: 
        void initialize(const std::shared_ptr<const eudaq::Event> & bore);
        const std::map<std::string,int>& get_dutnames_and_id(int device_id) const { return _dut_names_id.at(device_id); }
        // Get the channel list corresponding to the dut acquiered with the caen_board
        // In the proper order it was stored (blocks)
        std::vector<int> get_channel_list(int caen_board, const std::string & dutname);
        // Get the channel list in the order was stored
        // Get list of rows and columns for a given channel:
        std::vector<std::array<int,2>> get_rows_and_columns_list(int caen_board, int dut_id, int channel_id) const 
        { 
            return _dut_channel_arrangement.at(caen_board).at(dut_id).at(channel_id) ; 
        }
        // getters
        size_t get_samples_per_waveform() { return _n_samples_per_waveform; }
        size_t get_sampling_frequency_MHz() { return _sampling_frequency_MHz; }
        // 
        std::string get_producer_name(int device_id) const { return _name.at(device_id); }

    private:
        // Helper functions
        //std::vector<float> uint8VectorToFloatVector(std::vector<uint8_t> data) const;
        //int PolarityWF(const std::vector<float> & wf) const;
        //float AmplitudeWF(const std::vector<float> & wf) const;

        std::map<int, std::string> _name;
        size_t _n_samples_per_waveform;
        size_t _sampling_frequency_MHz;
        std::map<int, size_t> _n_duts;
        // Waveform starting t0 and Dt
        std::map<int, float> _t0;
        std::map<int, float> _dt;
        // XXX - TBD?
        // XXX -- TBD?
        std::map<int, std::vector<int> > _dut_channel_list;
        // { Digitizer: { DUT: { channel : [ (row, col), (row, col), ... ],  ... 
        std::map<int, std::map<int, PixelMap> > _dut_channel_arrangement;
        // { Digitizer: { DUT: (n-row,n-col), ... 
        std::map<int, std::map<int, std::array<int,2>> > _nrows_ncolumns;
        // { Digitizer: { DUT: npixels, ...
        std::map<int, std::map<int,int> > _npixels;
        // Human-readable name related with the internal DUT-id
        std::map<int, std::map<std::string,int> > _dut_names_id;
};


void CAENDT5742Events::initialize(const std::shared_ptr<const eudaq::Event> & bore)
{   
    const int device_id = bore->GetDeviceN();
    
    // Name of the producer
    _name[device_id] = bore->GetTag("producer_name");

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
    _dt[device_id] = (_sampling_frequency_MHz*1e6)/_n_samples_per_waveform;

    // Print-out the topology of the sensor and wire-bonding
    /*std::cout << " Defined DUTs in [" << _name[device_id] << "] digitizer: " << std::endl;
    for(auto & dn_id: _dut_names_id[device_id]) {
        std::cout << " [" << dn_id.first << "], ID:" << dn_id.second << ", "
                << "(rowsXcols): " << _nrows_ncolumns[device_id][dn_id.second][0]
                << "x" << _nrows_ncolumns[device_id][dn_id.second][1] 
                << ", Total Pixels: " << _npixels[device_id][dn_id.second]
                << ", Total channels: "
                << _dut_channel_arrangement[device_id][dn_id.second].size() << std::endl;
        for(const auto & ch_listpixels: _dut_channel_arrangement[device_id][dn_id.second]) {
            std::string list_pixels;
            for(const auto & pixels: ch_listpixels.second) {
                list_pixels += " ("+std::to_string(pixels[0]) + "," +std::to_string(pixels[1]) + ")";
            }
            std::cout << "   ==: CH" << ch_listpixels.first << " [" << list_pixels <<  " ]" << std::endl;
        }
    }

    // Debugging print-out stuff
    std::cout << " Initialize:: nsamples_per_waveform: " + std::to_string(_n_samples_per_waveform) +
            << ", sampling frequency: " << sampling_frequency_MHz << " MHz"
            << ", number of DUTs: " << _n_duts[device_id]) << std::endl;
    // Get the list of channels
    std::ostringstream oss;
    std::copy(_dut_channel_list[device_id].begin(), _dut_channel_list[device_id].end(), std::ostream_iterator<int>(oss, " "));
    EUDAQ_DEBUG(" Initialize:: Channel list (internal-ids): [ " + oss.str() +" ]");*/
}

// FIXME -- TBD
std::vector<int> CAENDT5742Events::get_channel_list(int caen_board, const std::string & dutname)
{
    if(_dut_channel_arrangement.find(caen_board) == _dut_channel_arrangement.end())
    {
        std::cout << "Not a valid caen_board identifier!! [" << caen_board << "]" << std::endl;
        return { -1 };
    }
    if(_dut_names_id[caen_board].find(dutname) == _dut_names_id[caen_board].end())
    {
        std::cout << "Not a valid dutname!! [" << dutname << "]" << std::endl;
        return { -1 };
    }
    int dut_id = _dut_names_id[caen_board][dutname];

    std::vector<int> chlist;
    for(const auto & ch_pxs: _dut_channel_arrangement[caen_board][dut_id]) 
    {
        chlist.push_back(ch_pxs.first);
    }
    
    return chlist;
}

int main(int /*argc*/, const char **argv) 
{
    eudaq::OptionParser op("EUDAQ Command Line DataConverter", "2.0", "The Data Converter launcher of EUDAQ");
    eudaq::Option<std::string> file_input(op, "i", "input", "", "string",
            "input file");
    eudaq::Option<std::string> file_output(op, "o", "output", "", "string",
            "output file [default: <input>.root]");
    eudaq::OptionFlag iprint(op, "ip", "iprint", "enable print of input Event");

    try
    {
        op.Parse(argv);
    }
    catch (...) 
    {
        return op.HandleMainException();
    }
    
    std::string infile_path = file_input.Value();
    if(infile_path.empty()) 
    {
        std::cout<<"option --help to get help"<<std::endl;
        return 1;
    }
    
    std::string outfile_path = file_output.Value();
    if( outfile_path.empty() )
    {
        outfile_path = infile_path.substr(0,infile_path.find_last_of("."))+".root";
    }

  
    eudaq::FileReaderUP reader;
    reader = eudaq::Factory<eudaq::FileReader>::MakeUnique(eudaq::str2hash("native"), infile_path);

    // Output
    TFile * f_root = new TFile(outfile_path.c_str(),"RECREATE");
    TTree * wf_tree = new TTree("Waveforms","Waveforms from eudaq raw file");

    // Variable definition for the tree filling
    int event_number = -1;
    std::string current_producer;
    std::vector<std::vector<Double_t>>* volt; 
    
    // Branches creation
    wf_tree->Branch("event", &event_number);
    wf_tree->Branch("producer", &current_producer);
    wf_tree->Branch("voltages", &volt);
    
    // Header tree with topology info
    TTree * header_tree = new TTree("Metadata","Useful info");
    // Runtime vectors
    // DUTs names
    std::vector<std::string>* dutnames;
    // channels map : the number of elements here defines the rest of the vectors
    std::vector<std::vector<Double_t>>* channels; 
    // Run number
    int run_number =-1;
    // Sampling frequency
    int sampling_frequency_MHz = -1;
    // Number of samples per wf
    int n_samples_per_waveform = -1;
    // Initial time and dt
    float t0 = -1;
    float dt = -1;
    
    header_tree->Branch("run_number",&run_number);
    header_tree->Branch("sampling_frequency_MHz",&sampling_frequency_MHz);
    header_tree->Branch("samples_per_waveform",&n_samples_per_waveform);
    header_tree->Branch("t0",&t0);
    header_tree->Branch("dt",&dt);
    header_tree->Branch("dut_name",&dutnames);
    header_tree->Branch("channel",&channels);
    
    int counter = 0;
    std::cout << "Extracting the wf from " << infile_path << " to " << outfile_path << std::endl;
    // Dummy configuration needed for the event standard conversion
    const eudaq::Configuration cfg("","");
    eudaq::ConfigurationSPC config_spc = std::make_shared<const eudaq::Configuration>(cfg);
                
    // The caen class reader to parse
    CAENDT5742Events caen;

    while(true)
    {    
        auto ev = reader->GetNextEvent();

        if(ev == nullptr) 
        {
            std::cerr << "Received null event" << std::endl;
            break;
        }

        for(const auto & subevt: ev->GetSubEvents())
        {
            if( subevt->GetDescription().find("CAEN") == std::string::npos )
            {
                continue;
            }
            
            auto raw_event = std::dynamic_pointer_cast<const eudaq::RawDataEvent>(subevt);
            // The producer id, to identify univocally
            int device_id = raw_event->GetDeviceN();

            if(raw_event->IsBORE()) 
            {
                // Initialize vectors
                dutnames = new std::vector<std::string>;
                channels = new std::vector<std::vector<Double_t>>;        
                
                caen.initialize(raw_event);

                // Fill metadata
                run_number = raw_event->GetRunNumber();
                sampling_frequency_MHz = caen.get_sampling_frequency_MHz();
                n_samples_per_waveform = caen.get_samples_per_waveform();
                // Creation of the DUTs, channels 
                // (*) Expecting to keep same order than (*)
                for(const auto & dutname_id: caen.get_dutnames_and_id(device_id))
                {
                    dutnames->push_back(dutname_id.first);
                    channels->push_back( {} );
                    // At a given caen, the block-id, and therefore the order we need to
                    // propagate here, is given by the 
                    // _dut_channel_arrangement[dev_id][dutname_sensorid.second]
                    // where ` dutname_sensorid.second is given by _dut_names_id[dev_id]
                    // The order of the sensor, i.e. of the plane
                    for(const auto & chid: caen.get_channel_list(device_id, dutname_id.first))
                    {
                        (channels->rbegin())->push_back( chid );
                    }
                }
                header_tree->Fill();
                // Clear metadata vectors
                if( dutnames != nullptr )
                {
                    delete dutnames;
                    dutnames = nullptr;
                }
                if( channels != nullptr )
                {
                    delete channels;
                    channels = nullptr;
                }
            }

            current_producer = caen.get_producer_name(device_id);
            // Extract the relevant things.. so waveforms
            auto evstd = eudaq::StandardEvent::MakeShared();
            eudaq::StdEventConverter::Convert(subevt, evstd, config_spc);

            // Vector init
            volt = new std::vector<std::vector<Double_t>>;
            // They should (MUST!!) be in the same order than established in (*)
            // The Plane number (sensor ID in the monitor) is defined at the 
            // converter, so following the exact loop
            for(const auto & dutname_sensorid: caen.get_dutnames_and_id(device_id))
            {
                auto dut_plane = evstd->GetPlane(dutname_sensorid.second);
                int pixid = 0;
                // The pixel number is given in the producer by following the Channel-ID provided
                // in the configuration file (so extracted in the `initialize` method)
                for(const auto & chid: caen.get_channel_list(device_id, dutname_sensorid.first))
                {
                    if( caen.get_rows_and_columns_list(device_id,dutname_sensorid.second,chid).size() > 1 )
                    {
                        std::cerr << "Unable to deal with more than one pixel per channel... " << std::endl;
                        return -1;
                    }
                    volt->push_back( dut_plane.GetWaveform(pixid) );
                    ++pixid;
                }
            }
            wf_tree->Fill();
            delete volt;
        }
        ++counter;
        event_number = ev->GetEventN();
        if(event_number % 1000 == 0) 
        {
            std::cout << "Processed event: " << event_number << std::endl;
        }
    }

    // Closing up root file
    header_tree->Write();
    wf_tree->Write();
    f_root->Close();

    // Freeing memory
    delete f_root;

    delete channels;
    delete dutnames;
    //delete volt;

    std::cout << "Read " << counter << " events" << std::endl;
    return 0;
}

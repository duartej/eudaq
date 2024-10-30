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
#include <functional>
#include <stdexcept>


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
        float get_t0(int device_id) const { return _t0.at(device_id) ; }
        float get_dt(int device_id) const { return _dt.at(device_id) ; }
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

// ROOTCreator -- Convert the relevant info into the root file
class ROOTCreator 
{
    public:
        ROOTCreator() = delete;
        ROOTCreator(const std::string & outputfile, const std::string & split_condition, int max_for_condition);
        ~ROOTCreator();
        
        void initialize_file();
        void close_file();

        void fill_BORE(CAENDT5742Events & caen, int run_number, int device_id);
        void fill_event_waveforms(CAENDT5742Events & caen, 
                const std::shared_ptr<eudaq::StandardEvent> & evstd,
                int event_number, 
                int device_id);
        void use_split_condition();

    private:
        void _initialize_trees();

        int _current_processed; 
        int _file_counter;
        
        TFile * _root;
        TTree * _wf_tree;
        TTree * _header_tree;

        std::string _outfile;
        // Variable definition for the tree waveform filling
        int _event_number;
        std::string _current_producer;
        std::vector<std::vector<Double_t>>* _volt;
        // Variable definition for the header tree
        bool _bore_filled;
        std::vector<std::string>* _dutnames;
        // channels map : the number of elements here defines the rest of the vectors
        std::vector<std::vector<Double_t>>* _channels; 
        int _run_number;
        int _sampling_frequency_MHz;
        int _n_samples_per_waveform;
        // Initial time and dt
        float _t0;
        float _dt;
        // -- Functional for splitting
        std::function<bool(void)> _split_condition;
        int _max_condition;
};

ROOTCreator::ROOTCreator(const std::string & outputfile, const std::string & split_condition, int max_condition) :
    _current_processed(-1),
    _file_counter(0),
    _root(nullptr), 
    _wf_tree(nullptr), 
    _header_tree(nullptr), 
    _outfile(outputfile),
    _event_number(-1),
    _current_producer(""),
    _volt(nullptr),
    _bore_filled(false),
    _dutnames(nullptr),
    _run_number(-1),
    _sampling_frequency_MHz(-1),
    _n_samples_per_waveform(-1),
    _t0(-1),
    _dt(-1),
    _max_condition(max_condition)
{ 
    // Definition of the condition to split
    if( split_condition == "" )
    {
        // No splitting at all
        _split_condition = []() -> bool { return false; };
    }
    else if( split_condition == "size" )
    {
        // Splitting by root size (in GB)
        _split_condition = [&]() { return (_root->GetSize() / (1024 * 1024 * 1024)) > _max_condition ; };
    }
    else if( split_condition == "events" )
    {
        // Splitting by number of events
        _split_condition = [&]() { return (_current_processed > _max_condition) ; };
    }
    else
    {
        std::string msg("Invalid split condition '"+split_condition+
                "'. Valid : \"\", \"size\", \"events\"");
        throw std::runtime_error(msg);
    }
}

ROOTCreator::~ROOTCreator()
{
    // Clear metadata vectors
    if( _dutnames != nullptr )
    {
        delete _dutnames;
        _dutnames = nullptr;
    }
    if( _channels != nullptr )
    {
        delete _channels;
        _channels = nullptr;
    }
}

void ROOTCreator::_initialize_trees()
{
    // Free memory
    if( _wf_tree != nullptr ) 
    {
        _wf_tree = nullptr;
    }
    
    if( _header_tree != nullptr )
    {
        _header_tree = nullptr;
    }

    // -- Initilaze trees
    // Waveform branch
    _wf_tree = new TTree("Waveforms","Waveforms from eudaq raw file");

    // Branches creation
    _wf_tree->Branch("event", &_event_number);
    _wf_tree->Branch("producer", &_current_producer);
    _wf_tree->Branch("voltages", &_volt);
    
    // Header tree with topology info
    _header_tree = new TTree("Metadata","Useful info");
    // Branches creation
    _header_tree->Branch("run_number",&_run_number);
    _header_tree->Branch("sampling_frequency_MHz",&_sampling_frequency_MHz);
    _header_tree->Branch("samples_per_waveform",&_n_samples_per_waveform);
    _header_tree->Branch("t0",&_t0);
    _header_tree->Branch("dt",&_dt);
    _header_tree->Branch("dut_name",&_dutnames);
    _header_tree->Branch("channel",&_channels);

    if( _bore_filled ) 
    {
        _header_tree->Fill();
    }
}
    
void ROOTCreator::initialize_file()
{
    if(_root != nullptr)
    {
        close_file();
    }

    if(_root == nullptr)
    {
        std::string filename = _outfile;
        // Splitting the files
        if( _max_condition != -1 ) 
        {
            // Asumed '.root' suffix: 9 characters
            filename = filename.substr(0, filename.size() - 5) + "_"+std::to_string(_file_counter)+".root";
        }
        _root = new TFile(filename.c_str(),"RECREATE");
        ++_file_counter;

        _initialize_trees();

        // associate the trees to the file
        _header_tree->SetDirectory(_root);
        _wf_tree->SetDirectory(_root);
    }
}

void ROOTCreator::close_file()
{
    // Store file content
    _header_tree->Write();
    _wf_tree->Write();

    _root->Close();

    // and the counter
    _current_processed = 0;

    delete _root;
    _root = nullptr;
}

void ROOTCreator::use_split_condition()
{
    // XXX -- here it can be choosen the file or the number of events
    if( _split_condition() )
    {
        close_file();
        initialize_file();
    }
    // Check the current processed events in this file
    ++_current_processed;
}

void ROOTCreator::fill_BORE(CAENDT5742Events & caen, int run_number, int device_id)
{
    // Initialize vectors
    _dutnames = new std::vector<std::string>;
    _channels = new std::vector<std::vector<Double_t>>;
    
    // Fill metadata
    _run_number = run_number;
    _sampling_frequency_MHz = caen.get_sampling_frequency_MHz();
    _n_samples_per_waveform = caen.get_samples_per_waveform();
    _t0 = caen.get_t0(device_id);
    _dt = caen.get_dt(device_id);
    // Creation of the DUTs, channels 
    // (*) Expecting to keep same order than (*)
    for(const auto & dutname_id: caen.get_dutnames_and_id(device_id))
    {
        _dutnames->push_back(dutname_id.first);
        _channels->push_back( {} );
        // At a given caen, the block-id, and therefore the order we need to
        // propagate here, is given by the 
        // _dut_channel_arrangement[dev_id][dutname_sensorid.second]
        // where ` dutname_sensorid.second is given by _dut_names_id[dev_id]
        // The order of the sensor, i.e. of the plane
        for(const auto & chid: caen.get_channel_list(device_id, dutname_id.first))
        {
            (_channels->rbegin())->push_back( chid );
        }
    }

    _header_tree->Fill();

    _bore_filled = true;
}

void ROOTCreator::fill_event_waveforms(CAENDT5742Events & caen, 
        const std::shared_ptr<eudaq::StandardEvent> & evstd,
        int event_number,
        int device_id)
{
    // The event and producer branches
    _event_number = event_number;
    _current_producer = caen.get_producer_name(device_id);

    // Vector init
    _volt = new std::vector<std::vector<Double_t>>;
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
                const std::string msg("Unable to deal with more than one pixel per channel... ");
                throw std::runtime_error(msg);
            }
            _volt->push_back( dut_plane.GetWaveform(pixid) );
            ++pixid;
        }
    }
    _wf_tree->Fill();
    delete _volt;
}
// ROOTCreator --- END


int main(int /*argc*/, const char **argv) 
{
    eudaq::OptionParser op("EUDAQ Command Line DataConverter", "2.0", "The Data Converter launcher of EUDAQ");
    eudaq::Option<std::string> file_input(op, "i", "input", "", "string",
            "input file");
    eudaq::Option<std::string> file_output(op, "o", "output", "<input>.root", "string",
            "output file");
    eudaq::Option<int> max_events(op, "e", "events", -1, "EVENTS TO PROCESS", "maxim number of events to process");
    eudaq::Option<int> max_size(op, "s", "size", -1, "MAX ROOT SIZE", "maxim size of the output root");
    eudaq::OptionFlag iprint(op, "ip", "iprint", "enable print of input Event");

    try
    {
        op.Parse(argv);
    }
    catch (...) 
    {
        return op.HandleMainException();
    }

    // Check options mutually exclusive
    if( max_events.Value() != -1 && max_size.Value() != -1) 
    {
        std::cerr << "Mutually exclusive options -e and -s" << std::endl;
        return 1;
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
    /// Check if splitting was conducted
    std::string split_condition("");
    int split_max_value = -1;
    if( max_events.Value() != -1 )
    {
        split_condition = "events";
        split_max_value = max_events.Value();
        std::cout << "Splitting ROOT files into chunks of " << split_max_value << " events" << std::endl;
    }
    else if( max_size.Value() != -1 )
    {
        split_condition = "size";
        split_max_value = max_size.Value();
        std::cout << "Splitting ROOT files into chunks of " << split_max_value << " GB" << std::endl;
    }
    
    ROOTCreator root_creator = ROOTCreator(outfile_path, split_condition, split_max_value);
    root_creator.initialize_file();
    
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
        
        const int event_number = ev->GetEventN();

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
                caen.initialize(raw_event);
                const int run_number = raw_event->GetRunNumber();
                
                // Fill metadata
                root_creator.fill_BORE(caen, run_number, device_id);
            }

            // Extract the relevant things.. so waveforms
            auto evstd = eudaq::StandardEvent::MakeShared();
            eudaq::StdEventConverter::Convert(subevt, evstd, config_spc);

            // Fill the waveforms
            root_creator.fill_event_waveforms(caen, evstd, event_number, device_id);
        }

        ++counter;
        if(event_number % 1000 == 0)
        {
            std::cout << "Processed event: " << event_number << std::endl;
        }

        root_creator.use_split_condition();
    }
    
    // Closing up root file 
    root_creator.close_file();

    // Closing up root file
    /*header_tree->Write();
    wf_tree->Write();
    f_root->Close();

    // Freeing memory
    delete f_root;

    delete channels;
    delete dutnames;
    //delete volt;*/

    std::cout << "Read " << counter << " events" << std::endl;
    return 0;
}

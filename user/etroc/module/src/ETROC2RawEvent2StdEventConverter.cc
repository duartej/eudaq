#include "eudaq/StdEventConverter.hh"
#include "eudaq/RawEvent.hh"

#include<map>
#include<bitset>
// --- XXX DEBUG --- REMOVE 
//#include<iomanip>

// --- XXX DEBUG --- REMOVE 

static const uint32_t EVENT_HEADER_MASK = 0xFFFFFFF0;
static const uint32_t EVENT_HEADER = 0xC3A3C3A;
static const uint32_t EVENT_TRAILER_MASK = 0x3F;
static const uint32_t EVENT_TRAILER = 0xB;

static const uint32_t CHIP_HEADER_MASK = 0xFFFF;
static const uint32_t CHIP_HEADER = 0x3C5C;

class ETROCRawEvent2StdEventConverter: public eudaq::StdEventConverter {
    public:
        bool Converting(eudaq::EventSPC d1, eudaq::StdEventSP d2, eudaq::ConfigSPC conf) const override;
        static const uint32_t m_id_factory = eudaq::cstr2hash("ETROC");
};

namespace {
    auto dummy0 = eudaq::Factory<eudaq::StdEventConverter>::Register<ETROCRawEvent2StdEventConverter>(ETROCRawEvent2StdEventConverter::m_id_factory);
}

// Convert back binary data into original uint32_t words
std::vector<uint32_t> GetBack_32bWord(const std::vector<uint8_t>& binary_data) {
    std::vector<uint32_t> converted_data;
    for(size_t i = 0; i < binary_data.size(); i += sizeof(uint32_t)) {
        uint32_t converted_word = 0;
        // Stored as Big endian
        for(size_t j = 0; j < sizeof(uint32_t); ++j) {
            converted_word |= static_cast<uint32_t>(binary_data[i + j]) << (8 * j);
        }
        converted_data.push_back(converted_word);
    }

    return converted_data;
}


bool ETROCRawEvent2StdEventConverter::Converting(eudaq::EventSPC d1, eudaq::StdEventSP d2, eudaq::ConfigSPC conf) const {

    // --- XXX DEBUG --- REMOVE 
    //std::cout << "+++++++++++++++++++++++++ " << std::endl;
    // --- XXX DEBUG --- REMOVE 
    auto ev = std::dynamic_pointer_cast<const eudaq::RawEvent>(d1);
    size_t nblocks= ev->NumBlocks();
    auto block_n_list = ev->GetBlockNumList();
    // Just one block, each etroc is identified in the header
    for(auto &block_n: block_n_list) {
        std::vector<uint8_t> data_block_uint8 = ev->GetBlock(block_n);
        // Convert back into 32bits words 
        std::vector<uint32_t> raw_data = GetBack_32bWord(data_block_uint8);
        // --- XXX DEBUG --- REMOVE 
        /*std::cout << "Event: " << ev->GetEventNumber() << std::endl;
        for(const uint32_t & kk: raw_data) 
        {
            std::cout <<  "[0x" << std::setw(8) << std::setfill('0') << std::hex << kk << "]" << std::endl; 
        }
        std::cout << std::dec;
        std::cout << "A -- Number of 32b data: " << raw_data.size() << std::endl;*/
        // --- XXX DEBUG --- REMOVE 

        // Event Header 32bx2 = 64b
        // 1. [28b: 3A3C3A ][4b: Data Mask]
        // Check is the header
        if(( (raw_data[0] & EVENT_HEADER_MASK) >> 4 ) != EVENT_HEADER) {
            // FIXME -- ERROOR
            std::cerr << "Potential corrupted data: not found the expected "
                << "Event Header" << std::endl;
            return false;
        }
        // --- XXX DEBUG --- REMOVE 
        //std::cout << "B" << std::endl;
        // --- XXX DEBUG --- REMOVE 
        // The active ETROCs: [4b:[ch3][ch2][ch1][ch0]]
        const uint16_t event_mask = raw_data[0] & 0xF;
        // The second 32b word;
        // 2 [4b:firmware version][16b:event number[10b:data words][2b:event type]
        const uint16_t firwmare_version = (raw_data[1] >> 28) & 0xF;
        const uint16_t event_number = (raw_data[1] >> 12) & 0xFFFF;
        const uint16_t data_words = (raw_data[1] >> 2) & 0x3FF;
        const uint16_t event_type = raw_data[1] & 0x3;

        // --- XXX DEBUG --- REMOVE 
        //*
	std::cout << "EH -- event number:" << event_number << ", event type:" << event_type 
            << ", version:" << firwmare_version << ", event_mask:" << event_mask << std::endl;
        // --- XXX DEBUG --- REMOVE 

        // The last word is the trailer 32b
        // [6b: b001011][12b:hits count][3b:Overflow count][3b:Hamming count][8b:CRC]
        const auto trailer = raw_data[raw_data.size()-1];
        // Malformed ?
        if( ((trailer >> 26) & EVENT_TRAILER_MASK) != EVENT_TRAILER ) {
            std::cerr << "Potential corrupted data: not found event trailer" << std::endl;
            return false;
        }        
        const uint16_t hits_count = (trailer >> 14) & 0xFFF;
        const uint16_t overflow_count = (trailer >> 11) & 0x7;
        const uint16_t hamming_count = (trailer >> 8) & 0x7;
        const uint16_t crc = trailer & 0xFF;
        // XXX -- FIXME -- Missing L1counter and BCID probably needed to monitor
        //                 Also, the ETROC (channel Id), is this info in the frame header? or
        //                 in the frame trailer?


        //const uint16_t etroc_id = trailer_event >> 17
        // --- XXX DEBUG --- REMOVE 
        //std::cout << "D" << std::endl;
        // --- XXX DEBUG --- REMOVE 
        
        // DATA 40 x data_words + padding bits to create 32bit words
        // 40 x data_words / 32 = number of words
        // 40 x data_words % 32 = (8, 16 or 24 bits to pad)a
        // Build the 40b data words
        std::vector<std::bitset<40>> etroc_data_words;
        etroc_data_words.reserve(data_words);
        // First element with all zeros
        etroc_data_words.push_back(std::bitset<40>());
        // --- XXX DEBUG --- REMOVE 
        // std::cout << "E, data words segun el header :" << data_words << std::endl;
        // --- XXX DEBUG --- REMOVE 
        
        // The counter for the 40-word bit
        int counter_40b = 39;
        // The i-esim 40 word 
        int element_40b = 0;
            
        // --- XXX DEBUG --- REMOVE 
        //std::cout << "F" << std::endl;
        // --- XXX DEBUG --- REMOVE 
        // Run over the remaining raw data to extract all the data words
        // except the event header (0,1) and the trailer (-1)
        for(int ir = 2; ir < raw_data.size()-2; ++ir) {
            // Auxiliary bitset for the 32b raw data
            std::bitset<32> aux_data(raw_data[ir]);
            for(int i = aux_data.size()-1; i >= 0; --i) {
                etroc_data_words[element_40b][counter_40b] = aux_data[i];
                // --- Reach the 40th bit in the word
                if( counter_40b == 0 ) {
                    // Reset the bit counter
                    counter_40b = 39;
                    // Start a new 40-bit word
                    etroc_data_words.push_back(std::bitset<40>());
                    // and increase the accesor to the vector element
                    element_40b += 1;
                    continue;
                }
                counter_40b -= 1;
            }
        }
	// ETROCs attached to a FPGA boards are going to send all the events in order 
        uint16_t chip_id = 0;

        // --- XXX DEBUG --- REMOVE 
        //std::cout << "G -- aux_data vector(40b, bitset) size: " << etroc_data_words.size() 
        //    << std::endl;
        // --- XXX DEBUG --- REMOVE 
        
        // FIXME ----> GET THE CHIP!! (channel) !!! FIXME -----
	std::map<uint16_t, std::vector<uint32_t> > eas;
	std::map<uint16_t, std::vector<uint32_t> > cols;
        std::map<uint16_t, std::vector<uint32_t> > rows;
        std::map<uint16_t, std::vector<uint64_t> > toas;
        std::map<uint16_t, std::vector<uint32_t> > tots;
        std::map<uint16_t, std::vector<uint32_t> > cals;
        std::map<uint16_t, std::vector<uint32_t> > l1counter;
        // --- XXX DEBUG --- REMOVE 
        //std::cout << "H, Data Words size: " << etroc_data_words.size() 
        //    <<  " , " << std::endl;
        // --- XXX DEBUG --- REMOVE 
        // Looking at the data words ot extract hits
        uint32_t current_l1counter = 0;
        for(auto & current_word_bitset: etroc_data_words) {
            // --- XXX DEBUG --- REMOVE 
            //std::cout << "      [0x" << current_word_bitset << "] " << std::endl;
            // --- XXX DEBUG --- REMOVE 
            uint64_t current_word = current_word_bitset.to_ulong();
            // XXX -- FRAME HEADER or FRAME FILLER
            // [1b:0][15b:0x3c5c][2bYY][XXXXXX]
            if( ((current_word >> 24) & CHIP_HEADER_MASK) == CHIP_HEADER )
            {
		// Either is a Frame header [2bYY = 2b00]  or a filler header [2bYY = 2b10]
		if( (current_word >> 22) & 0x3 == 0x0 ) 
		{
                    // FRAME HEADER 
		    // [1b:0][15b:0x3c5c][2b00][8b:L1Counter][2b:Type][12b:BCID]
                    current_l1counter = (((current_word) >> 20 ) & 0xFF) ;
                    //*
		    std::cout << "   ---| FH (CHIP HEADER) " 
                       << "L1Counter: " << current_l1counter << ", "  
                       << "Type: " << (((current_word) >> 12 ) & 0x3)  << ", "
                       << "BCID: " << ((current_word) & 0xFFF)  
                       << std::endl;
                    continue;
		}
		else
		{
                    // FRAME FILLER
		    // [1b:0][15b:0x3c5c][2b00][8b:RT_L1Counter][2b:EBS][12b:RT_BCID]
                    current_l1counter = (((current_word) >> 20 ) & 0xFF) ;
                    //*
		    std::cout << "   ---| Frame FILLER " 
                       << "L1Counter: " << current_l1counter << ", "  
                       << "Type: " << (((current_word) >> 12 ) & 0x3)  << ", "
                       << "BCID: " << ((current_word) & 0xFFF)  
                       << std::endl;
		    continue;
		}
            }
            else if( ((current_word >> 39) & 0x1) == 1 )
            {
                // Look for data XXX -- FIXME What about the other data? See above
                // [1b: 1][2b: EA][4b: COL][4b: ROW][10b: TOA][9b: TOT][10b: CAL]
                eas[chip_id].push_back( (current_word >> 37) & 0x3 );
                cols[chip_id].push_back( (current_word >> 33) & 0xf );
                rows[chip_id].push_back( (current_word >> 29) & 0xf );
                toas[chip_id].push_back( (current_word >> 19) & 0x3ff );
                tots[chip_id].push_back( (current_word >> 10) & 0x1ff );
                cals[chip_id].push_back( current_word & 0x3ff );
                l1counter[chip_id].push_back( current_l1counter );
		//*
		size_t ci = cols.size()-1;
            	std::cout << "   +++[DATA] COL: " << cols[chip_id][ci] << " ROW: "<< rows[chip_id][ci] 
			<< " TOT:" << tots[chip_id][ci] << " TOA:" << toas[chip_id][ci] << " CAL:" << cals[chip_id][ci] << std::endl;
            // --- XXX DEBUG --- REMOVE 
            }
            else 
            {
                // XXX DEBUG --- REMOVE?
                // XXX -- And FRAME TRAILER?  Any ohter else
                // [1b:0][17b:ChipId][6b:Status][8B:hits][8b:CRC]

                //*
		std::cout << "   ---| FT (CHIP TRAILER) " 
                    << "ChipID: " << (((current_word) >> 22 ) & 0x1FFFF)  << ", "
                    << "STATuS:" << (((current_word) >> 16 ) & 0x3F)  << ", "
                    << "Hits: " << (((current_word) >> 8) & 0xFF) << ", "
                    << "CRC: " << ((current_word) & 0xFF) << ", "
                    << std::endl;
		chip_id++;
                // XXX DEBUG --- REMOVE
                continue;
            }
        }
	
	for(const auto & chipid_tots: tots)
	{
	    const uint16_t etroc_id = chipid_tots.first;
            eudaq::StandardPlane plane(etroc_id, "ETROC", "ETROC");
            // col, rows, npixels?, frames (l1)
            plane.SetSizeZS(16, 16, 0, 256);
            for(size_t i = 0; i < eas[etroc_id].size(); ++i) {
                // --- XXX DEBUG --- REMOVE 
                // XXX --- IT doens't work using the frame... why??
	        // l1counter should be there because data is therea
                // plane.PushPixel(cols[i],rows[i] , tots[i], toas[i], false, l1counter[i]);
                plane.PushPixel(cols[etroc_id][i],rows[etroc_id][i] , tots[etroc_id][i], toas[etroc_id][i]);
            }
            d2->AddPlane(plane);
	}
        // --- XXX DEBUG --- REMOVE 
	//* 
        std::cout << "ET -- hits count:" << hits_count << ", overflow:" << overflow_count
            << ", hamming:" << hamming_count << ", crc:" << crc << std::endl;
        std::cout << "|=====================================================================" 
		<< "==========================|" << std::endl;
        // --- XXX DEBUG --- REMOVE 
        //
    }
    // --- XXX DEBUG --- REMOVE 
    //std::cout << "\nI" << std::endl;
    // --- XXX DEBUG --- REMOVE 

    return true;
}

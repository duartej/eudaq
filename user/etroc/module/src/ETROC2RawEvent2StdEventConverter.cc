#include "eudaq/StdEventConverter.hh"
#include "eudaq/RawEvent.hh"

#include<bitset>
// --- XXX DEBUG --- REMOVE 
#include<iomanip>

// --- XXX DEBUG --- REMOVE 

static const uint32_t EVENT_HEADER_MASK = 0xFFFFFFF0;
static const uint32_t EVENT_HEADER = 0xC3A3C3A;
static const uint32_t EVENT_TRAILER = 0x2C;

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

    auto ev = std::dynamic_pointer_cast<const eudaq::RawEvent>(d1);
    size_t nblocks= ev->NumBlocks();
    auto block_n_list = ev->GetBlockNumList();
    // Just one block, each etroc is identified in the header
    for(auto &block_n: block_n_list) {
        std::vector<uint8_t> data_block_uint8 = ev->GetBlock(block_n);
        // Convert back into 32bits words 
        std::vector<uint32_t> raw_data = GetBack_32bWord(data_block_uint8);
/*for(const uint32_t & kk: raw_data) 
{
    std::cout << "[0x" << std::setw(8) << std::setfill('0') << std::hex << kk << "]" << std::endl; 
}
std::cout << std::dec;*/

//std::cout << "DECODE" << std::endl;
        // Event Header 32bx2 = 64b
        // [ 28b: 3A3C3A ][4b: Data Mask][16b: Event Number][10b: Data Words][2b: Event type][4b: version]
        //
        // Check is the header
        if(( (raw_data[0] & EVENT_HEADER_MASK) >> 4 ) != EVENT_HEADER) {
            // FIXME -- ERROOR
            std::cerr << "Potential corrupted data: not found the expected "
                << "Event Header" << std::endl;
            return false;
        }
        const uint16_t data_mask = raw_data[0] & 0xF;
        // The second 32b word;
        const uint16_t event_number = (raw_data[1] >> 16) & 0xFFFF;
        const uint16_t data_words = (raw_data[1] >> 26) & 0x3FF;
        const uint16_t event_type = (raw_data[1] >> 28) & 0x3;
        const uint16_t version = raw_data[1] & 0xF;

        // FIXME --- XXX
        // And the last word is the trailer
        //int trailer_event = raw_data[raw_data.size()-1];
        /// --> XXX - FIXME prov para estos datos sin trailer ?? CHECK TRAILER?a
        //
        // NO IDEA WHERE THIS INFO IS???
        //if( (trailer_event >> 24) & 0xFF != EVENT_TRAILER ) {
        //    std::cerr << "Potential corrupted data: not found event trailer" << std::endl;
        //    return false
        //}
        //const uint16_t etroc_id = trailer_event >> 17
        // XXX -- PROV -- No es aqui!!! Es del frame trailer notdel evnt trailer!
        const uint16_t etroc_id = 0;
        
/*std::cout << "data mask:" << data_mask 
    << ", data_words: " << data_words
    << ", event-type: " << event_type
    << ", version: " << version
    << std::endl; */
        // DATA 40 x data_words + padding bits to create 32bit words
        // 40 x data_words / 32 = number of words
        // 40 x data_words % 32 = (8, 16 or 24 bits to pad)a
        // Build the 40b data words
        std::vector<std::bitset<40>> etroc_data_words;
        etroc_data_words.reserve(data_words);
        // First element with all zeros
        etroc_data_words.push_back(std::bitset<40>());
        
        // The counter for the 40 word bit
        int counter_40b = 39;
        // The i-esim 40 word 
        int element_40b = 0;
            
        // FIXME --- TO Check data consistency
        //const int data_words_32b = int((40 * etroc_data_words)/32);
        //const int padding_bits = (40 * data_words) % 32;
        // FIXME --- TO Check data consistency
        
        // Run over the remaining raw data to extract all the data words
        for(int ir = 2; ir < raw_data.size(); ++ir) {
            // Auxiliary bitset for the 32b raw data
            std::bitset<32> aux_data(raw_data[2]);
            for(int i = aux_data.size()-1; i >= 0; --i) {
                etroc_data_words[element_40b][counter_40b] = aux_data[i];
    //std::cout << aux_data[i] << " --" << etroc_data_words[element_40b][counter_40b] << std::endl;
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
        
/*for(const auto & data_chip: etroc_data_words)
{
std::cout << "0x" << std::setw(10) << std::hex << data_chip << std::endl;
    // Is DATA?
    if( ((data_chip.to_ulong() >> 39) & 0x1) == 1 )
{
std::cout << "EA:" << ((data_chip.to_ulong() >> 37) & 0x3)
    << ", ROW: " << ((data_chip.to_ulong() >> 29) & 0xf)
    << ", COL: " << ((data_chip.to_ulong() >> 33) & 0xf)
    << ", TOA: " << ((data_chip.to_ulong() >> 21) & 0x3ff)
    << ", CAL: " << (data_chip.to_ulong() & 0x1ff)
    << std::endl;
}
}*/
//std::cout << "----- END" << std::endl;
        // FIXME ----> GET THE CHIP!! (channel) !!! FIXME -----
        std::vector<uint32_t> eas;
        std::vector<uint32_t> cols;
        std::vector<uint32_t> rows;
        std::vector<uint32_t> toas;
        std::vector<uint32_t> tots;
        std::vector<uint32_t> cals;
        // Looking at the data words ot extract hits
        for(auto & current_word_bitset: etroc_data_words) {
            uint64_t current_word = current_word_bitset.to_ulong();
            // Look for data XXX -- FIXME What about the other data? 
            // [1b: 1][2b: EA][4b: COL][4b: ROW][10b: TOA][9b: TOT][10b: CAL]
            if( ((current_word >> 39) & 0x1) != 1 )
            {
                continue;
            }
            const uint32_t ea = (current_word >> 37) & 0x3;
            const uint32_t col = (current_word >> 33) & 0xf;
            const uint32_t row = (current_word >> 29) & 0xf;
            const uint32_t toa = (current_word >> 19) & 0x3ff;
            const uint32_t tot = (current_word >> 10) & 0x1ff;
            const uint32_t cal = current_word & 0x3ff;

            eas.push_back(ea);
            cols.push_back(col);
            rows.push_back(row);
            toas.push_back(toa);
            tots.push_back(tot);
            cals.push_back(tot);
        }
        /// FIXME ---MISSING Mechanism for the etroc-id !!~!
        eudaq::StandardPlane plane(etroc_id, "ETROC", "ETROC");
        plane.SetSizeZS(16, 16, 0);
        for(size_t i = 0; i < eas.size(); ++i) {
            plane.PushPixel(cols[i],rows[i] , tots[i], toas[i]);
        }
        d2->AddPlane(plane);
    }

    return true;
}

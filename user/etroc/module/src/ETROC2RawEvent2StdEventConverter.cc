#include "eudaq/StdEventConverter.hh"
#include "eudaq/RawEvent.hh"


static const uint32_t DATA_HEADER = 0xC3A3C3A;
static const uint32_t DATA_TRAILER = 0xB;

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
    // XXX --- Just one block ?? No, Probably the different chips will have different blocks
    for(auto &block_n: block_n_list) {
        std::vector<uint8_t> data_block_uint8 = ev->GetBlock(block_n);
        // Convert back into 32bits words 
        std::vector<uint32_t> raw_data = GetBack_32bWord(data_block_uint8);
std::cout << "HI!!! Converted data:";
for(const uint32_t & kk: raw_data) { std::cout << " " << kk << " ";}
std::cout << std::endl;
        // Parsing 
        //for(const & raw_data: data_block) {
        //    uint8_t x_pixel = block[0];
        //std::vector<uint16_t> hit(block.begin()+2, block.end());
        //if(hit.size() != x_pixel*y_pixel) EUDAQ_THROW("Unknown data");
        //eudaq::StandardPlane plane(block_n, "my_etroc_plane", "my_etroc_plane");
        //plane.SetSizeZS(hit.size(), 1, 0);
        //for(size_t i = 0; i < y_pixel; ++i) {
        //    for(size_t n = 0; n < x_pixel; ++n) {
        //        plane.PushPixel(n, i , hit[n+i*x_pixel]);
        //    }
        //}
        //d2->AddPlane(plane);
    }

    return true;
}

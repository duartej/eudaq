#include "eudaq/StdEventConverter.hh"
#include "eudaq/RawEvent.hh"
#include <vector>
#include <cstdint>
#include <algorithm>

#define DIGITIZER_RECORD_LENGTH 1024 // This setting is also hardcoded in the producer. It can be changed.

class CAEN_DT5748RawEvent2StdEventConverter: public eudaq::StdEventConverter {
	public:
		bool Converting(eudaq::EventSPC d1, eudaq::StdEventSP d2, eudaq::ConfigSPC conf) const override;
		static const uint32_t m_id_factory = eudaq::cstr2hash("CAEN_DT5748");
	private:
		void Initialize(eudaq::EventSPC bore, eudaq::ConfigurationSPC conf) const;
		mutable size_t n_samples_per_waveform;
		mutable size_t sampling_frequency_MHz;
		mutable size_t number_of_DUTs;
		mutable std::vector<std::string> channels_names_list;
		mutable std::vector<std::string> DUTs_names;
		mutable std::vector<std::vector<std::vector<size_t>>> waveform_position; // waveform_position[n_DUT][nxpixel][nypixel] = position where this waveform begins in the raw data.
};

namespace {
	auto dummy0 = eudaq::Factory<eudaq::StdEventConverter>::Register<CAEN_DT5748RawEvent2StdEventConverter>(CAEN_DT5748RawEvent2StdEventConverter::m_id_factory);
}

void CAEN_DT5748RawEvent2StdEventConverter::Initialize(eudaq::EventSPC bore, eudaq::ConfigurationSPC conf) const {
	std::string s;
	
	n_samples_per_waveform = std::stoi(bore->GetTag("n_samples_per_waveform"));
	sampling_frequency_MHz = std::stoi(bore->GetTag("sampling_frequency_MHz"));
	number_of_DUTs = std::stoi(bore->GetTag("number_of_DUTs"));
	
	// Parse list with the names of the channels in the order they will appear in the raw data:
	s = bore->GetTag("channels_names_list");
	for (char c : {'[', ']', '\'', '\"'}) {
		s.erase(remove(s.begin(), s.end(), c), s.end());
	}
	std::string delimiter = ", ";
	size_t pos = 0;
	while ((pos = s.find(delimiter)) != std::string::npos) {
		std::string token = s.substr(0, pos);
		channels_names_list.push_back(token);
		s.erase(0, pos + delimiter.length());
	}
	channels_names_list.push_back(s);
	
	// For each DUT, build a matrix where the elements are integer numbers specifying the position where the respective waveform begins in the raw data:
	for (size_t n_DUT=0; n_DUT<number_of_DUTs; n_DUT++) {
		DUTs_names.push_back(bore->GetTag("DUT_"+std::to_string(n_DUT)+"_name"));
		s = bore->GetTag("DUT_"+std::to_string(n_DUT)+"_channels_matrix"); // Gets something like e.g. `"[['CH4', 'CH5'], ['CH6', 'CH7']]"`.
		if (s.empty())
			EUDAQ_THROW("Cannot get information about the channels to which the DUT named \""+DUTs_names[n_DUT]+"\" was connected.");
		std::vector<std::string> delims = {"[[", "]]", "'", "\"", " "};
		for (std::string delim : delims) {
			size_t pos = s.find(delim);
			while (pos != std::string::npos) {
				s.erase(pos, delim.length());
				pos = s.find(delim, pos);
			}
		}
		// Here `s` looks like e.g. `"CH4,CH5],[CH6,CH7"`.
		std::vector<std::string> split_s;
		size_t start_pos = 0;
		size_t end_pos = s.find("],[");
		while (end_pos != std::string::npos) {
			split_s.push_back(s.substr(start_pos, end_pos - start_pos));
			start_pos = end_pos + 3;
			end_pos = s.find("],[", start_pos);
		}
		split_s.push_back(s.substr(start_pos));
		// Here `split_s` looks like e.g. `["CH4,CH5"],["CH6,CH7"]` (Python).
		std::vector<std::vector<std::string>> matrix_with_channels_arrangement_for_this_DUT;
		for (std::string elem : split_s) {
			std::vector<std::string> sub_result;
			size_t sub_start_pos = 0;
			size_t sub_end_pos = elem.find(",");
			while (sub_end_pos != std::string::npos) {
				sub_result.push_back(elem.substr(sub_start_pos, sub_end_pos - sub_start_pos));
				sub_start_pos = sub_end_pos + 1;
				sub_end_pos = elem.find(",", sub_start_pos);
			}
			sub_result.push_back(elem.substr(sub_start_pos));
			matrix_with_channels_arrangement_for_this_DUT.push_back(sub_result);
		}
		// Here `matrix_with_channels_arrangement_for_this_DUT` should be `[['CH4', 'CH5'], ['CH6', 'CH7']]`.
		// Now find where each of the channels waveforms begins in the raw data and arrange that into a matrix:
		std::vector<std::vector<size_t>> matrix2;
		for (size_t nx=0; nx<matrix_with_channels_arrangement_for_this_DUT.size(); nx++) {
			std::vector<size_t> row;
			for (size_t ny=0; ny<matrix_with_channels_arrangement_for_this_DUT[nx].size(); ny++) {
				auto iterator = std::find(channels_names_list.begin(), channels_names_list.end(), matrix_with_channels_arrangement_for_this_DUT[nx][ny]);
				if (iterator != channels_names_list.end()) {
					row.push_back((iterator - channels_names_list.begin())*n_samples_per_waveform);
				} else { // This should never happen.
					EUDAQ_THROW("Channel " + matrix_with_channels_arrangement_for_this_DUT[nx][ny] + " not present in the list of channels that were recorded.");
				}
			}
			matrix2.push_back(row);
		}
		waveform_position.push_back(matrix2); // Finally... In Python this is no more than 5 lines of code.
	}
}

std::vector<float> uint8VectorToFloatVector(std::vector<uint8_t> data) {
	// Everything in this function, except for this single line, was provided to me by ChatGPT. Amazing.
	std::vector<float> result;
	result.resize(data.size() / sizeof(float)); // size the result vector appropriately
	float* resultPtr = reinterpret_cast<float*>(&result[0]); // cast the pointer to float

	uint8_t* dataPtr = &data[0]; // get a pointer to the data in the uint8_t vector
	size_t dataSize = data.size(); // get the size of the data in the uint8_t vector

	for (size_t i = 0; i < dataSize; i += sizeof(float)) {
		*resultPtr = *reinterpret_cast<float*>(dataPtr); // cast the value at dataPtr to float and store in result vector
		resultPtr++;
		dataPtr += sizeof(float);
	}

	return result;
}

float median(std::vector<float>& vec) {
    std::sort(vec.begin(), vec.end());
    int size = vec.size();
    if (size % 2 == 0)
        return (vec[size/2 - 1] + vec[size/2]) / 2.0;
    else
        return vec[size/2];
}

float max(const std::vector<float>& vec) {
    auto it = std::max_element(vec.begin(), vec.end());
    return *it;
}

float amplitude_from_waveform(std::vector<float>& waveform) {
	float base_line = median(waveform);
	return max(waveform) - base_line;
}

bool CAEN_DT5748RawEvent2StdEventConverter::Converting(eudaq::EventSPC d1, eudaq::StdEventSP d2, eudaq::ConfigSPC conf) const {
	auto event = std::dynamic_pointer_cast<const eudaq::RawEvent>(d1);
	if (event == nullptr) {
		EUDAQ_ERROR("Received null event.");
		return false;
	}
	
	if (event->IsBORE()) { // Beginning Of Run Event, this is the header event.
		EUDAQ_INFO("Starting initialization...");
		Initialize(event, conf);
	}
	
	if (event->NumBlocks() != 1) {
		EUDAQ_ERROR("One and only one data block per event is expected, but received an event with " + std::to_string(event->NumBlocks()) + " blocks.");
		return false;
	}
	
	size_t n_block = 0; // The producer puts all the data in a single block, I guess we ony need to care about that one block. I define this `n_block` variable to make the code more readable.
	std::vector<float> raw_data = uint8VectorToFloatVector(event->GetBlock(n_block));
	
	std::vector<float> samples_times;
	for (size_t n_sample=0; n_sample<n_samples_per_waveform; n_sample++)
		samples_times.push_back(n_sample*sampling_frequency_MHz*1e6);
	
	for (size_t n_DUT=0; n_DUT<waveform_position.size(); n_DUT++) {
		eudaq::StandardPlane plane(n_block, DUTs_names[n_DUT]);
		plane.SetSizeZS(
			waveform_position[n_DUT].size(), // `w`, number of pixels in the horizontal direction.
			waveform_position[n_DUT][0].size(), // `h`, number of pixels in the vertical direction.
			0, // `npix`, no idea...
			1, // `frames`, no idea...
			eudaq::StandardPlane::FLAG_DIFFCOORDS | eudaq::StandardPlane::FLAG_ACCUMULATE // `flags`, no idea...
		);
		for (size_t nx=0; nx<waveform_position[n_DUT].size(); nx++) {
			for (size_t ny=0; ny<waveform_position[n_DUT][nx].size(); ny++) {
				std::vector<float> this_pixel_waveform(raw_data.begin()+waveform_position[n_DUT][nx][ny], raw_data.begin()+waveform_position[n_DUT][nx][ny]+DIGITIZER_RECORD_LENGTH);
				float amplitude = amplitude_from_waveform(this_pixel_waveform);
				if (amplitude > 500) // REMOVE THIS HARDCODED VALUE SOMEHOW!!
					plane.PushPixel(nx, ny, amplitude);
			}
		}
		d2->AddPlane(plane);
	}
	return true;
}

/*
 * TimingTimingHitmapCollection.hh
 *
 *  Created on: June 24, 2013
 *      Author: Jordi Duarte-Campderros (IFCA)
 */
#include "TimingHitmapHistos.hh"
#include "OnlineMon.hh"
#include <cstdlib>
#include <algorithm>
#include <numeric>

namespace {
    struct WaveformSummary {
        double baseline = 0.0;
        double sigma = 0.0;
        // signed: peak - baseline
        double amplitude = 0.0;
        // |amplitude| / sigma
        double snr = 0.0;
        int peak_index = -1;
        bool is_hit = false;
    };

    static double Median(std::vector<double> v) {
        if(v.empty()) {
            return 0.0;
        }
        
        const size_t n = v.size();
        const size_t m = n / 2;
        
        std::nth_element(v.begin(), v.begin() + m, v.end());
        double med = v[m];
        
        if(n % 2 == 0) {
            auto max_it = std::max_element(v.begin(), v.begin() + m);
            med = 0.5 * (med + *max_it);
        }
        
        return med;
    }

    static WaveformSummary SummarizeWaveform(const std::vector<double>& wf,
            double nsigma_threshold = 5.0,
            int exclusion_half_window = 40) {
        WaveformSummary out;
        
        if(wf.size() < 10) {
            return out;
        }
        
        // 1) Rough baseline from the full waveform
        const double rough_baseline = Median(std::vector<double>(wf.begin(), wf.end()));
        
        // 2) Find the sample with the largest excursion from the rough baseline
        double max_abs_dev = -1.0;
        int peak_idx = -1;
        
        for(size_t i = 0; i < wf.size(); ++i) {
            const double dev = wf[i] - rough_baseline;
            const double abs_dev = std::abs(dev);
            if(abs_dev > max_abs_dev) {
                max_abs_dev = abs_dev;
                peak_idx = static_cast<int>(i);
            }
        }
        
        if(peak_idx < 0) {
            return out;
        }

        // 3) Re-estimate baseline excluding the pulse neighborhood
        std::vector<double> baseline_samples;
        baseline_samples.reserve(wf.size());
        
        const int i0 = std::max(0, peak_idx - exclusion_half_window);
        const int i1 = std::min(static_cast<int>(wf.size()), peak_idx + exclusion_half_window + 1);
        
        for(int i = 0; i < static_cast<int>(wf.size()); ++i) {
            if(i >= i0 && i < i1) {
                continue;
            }
            baseline_samples.push_back(wf[i]);
        }

        // Fallback if too few samples remain
        if(baseline_samples.size() < 10) {
            baseline_samples.assign(wf.begin(), wf.end());
        }
        const double baseline = Median(baseline_samples);
        
        // 4) Robust sigma from MAD
        std::vector<double> abs_dev_from_baseline;
        abs_dev_from_baseline.reserve(baseline_samples.size());
        for(double v : baseline_samples) {
            abs_dev_from_baseline.push_back(std::abs(v - baseline));
        }

        const double mad = Median(abs_dev_from_baseline);
        const double sigma = 1.4826 * mad;
        
        // 5) Final signed amplitude with respect to the refined baseline
        double max_abs_amp = -1.0;
        double signed_amp = 0.0;
        peak_idx = -1;

        for(size_t i = 0; i < wf.size(); ++i) {
            const double dev = wf[i] - baseline;
            const double abs_dev = std::abs(dev);
            if(abs_dev > max_abs_amp) {
                max_abs_amp = abs_dev;
                signed_amp = dev;
                peak_idx = static_cast<int>(i);
            }
        }
        
        out.baseline = baseline;
        out.sigma = sigma;
        out.amplitude = signed_amp;
        out.peak_index = peak_idx;
        out.snr = (sigma > 0.0) ? std::abs(signed_amp) / sigma : 0.0;
        out.is_hit = (sigma > 0.0) && (std::abs(signed_amp) > nsigma_threshold * sigma);
        
        return out;
    }
} // unnamed namespace

TimingHitmapHistos::TimingHitmapHistos(SimpleStandardPlane p, RootMonitor *mon) 
    : _occupancy_map(nullptr), _channel_map(nullptr) {

    _wait = false;

    auto dutname_dummychannel_col_row = p.getDutnameChannelColRow(0);
    _dutname = dutname_dummychannel_col_row[0];
    _boardname = p.getName();
    
    _mon = mon;

    // Needed
    if( _boardname.find("CAEN") != std::string::npos ) {
        is_CAENDT5742 = true;
    }

    _maxX = p.getMaxX();
    _maxY = p.getMaxY();

    std::string title(_dutname+" ["+_boardname+"]  Occupancy Map");
    std::string hname("h_"+_boardname+"_"+_dutname+"_occupancymap");
    if(_maxX != -1 && _maxY != -1) {
        _occupancy_map = new TH2I(hname.c_str(), title.c_str(), _maxX, 0, _maxX, _maxY, 0, _maxY);
        _SetHistoAxisLabels(_occupancy_map, "X", "Y");
        
        title = _dutname+" ["+_boardname+"]  Channel Map";
        hname = "h_"+_boardname+"_"+_dutname+"_channel_map";
        _channel_map = new TH2I(hname.c_str(), title.c_str(), _maxX, 0, _maxX, _maxY, 0, _maxY);
    
    for(unsigned int pixid = 0; pixid < p.getBinsX()*p.getBinsY(); ++pixid) {
            auto d_ch_col_row = p.getDutnameChannelColRow(pixid);
            // Cross-check there is no a filled up pixels
            if( d_ch_col_row[0].empty() ) {
                continue;
            }                     
            // Extracting the pixel col and row
            unsigned int col = std::stoi(d_ch_col_row[2]);
            unsigned int row = std::stoi(d_ch_col_row[3]);
            //std::cout << "    TIMINGHITHISTOS::BOARD: [" 
            //    << _boardname << "] [" << _dutname << "]" << " CH:[" 
            //    << d_ch_col_row[1] << "] PIXEL: col: " << col << ", row: " << row << std::endl;

            // Map to convert from col,row to pixid and viceversa
            _into_pixid[{col,row}] = pixid;
            _into_colrow[pixid] = {col,row};
            
            // Unfiltered
            title = _dutname+" ["+_boardname+"] CH:"+d_ch_col_row[1]+" (not-filtered) Waveforms  (Col:"
                +std::to_string(col)+",Row:"+std::to_string(row)+");#Sample;V [mV];#Entries";
            hname = "h_unfiltered_waveform_"+_boardname+"_"+_dutname+"_"+std::to_string(pixid);
            // XXX -- HARDCODED!!  I need to extract it from config
            _waveforms_not_filtered[pixid] = new TH2F(hname.c_str(), title.c_str(), 1024, -0.5,1023.5, 200, -0.045, 0.045); 
            _waveforms_not_filtered[pixid]->SetCanExtend(TH1::kAllAxes);
            
            // Unfiltered
            title = _dutname+" ["+_boardname+"] CH:"+d_ch_col_row[1]+" (not-filtered) Waveforms  (Col:"
                +std::to_string(col)+",Row:"+std::to_string(row)+");#Sample;V [mV];#Entries";
            hname = "h_timevsminwfm_"+_boardname+"_"+_dutname+"_"+std::to_string(pixid);
            // XXX -- HARDCODED!!  I need to extract it from config
            _time_minwaveform[pixid] = new TH2F(hname.c_str(), title.c_str(), 1024, -0.5,1023.5, 200, -0.045, 0.045); 
            _time_minwaveform[pixid]->SetCanExtend(TH1::kAllAxes);
            
            // raw signal at each sample (in Volts)
            title = _dutname+" ["+_boardname+"] CH:"+d_ch_col_row[1]+" Samples-1D  (Col:"
                +std::to_string(col)+",Row:"+std::to_string(row)+";V [mV];Digitizer samples Entries";
            hname = "h_signal_"+_boardname+"_"+_dutname+"_"+std::to_string(pixid);
            _signal[pixid] = new TH1F(hname.c_str(), title.c_str(), 1000, -0.3, 0.3); 
            _signal[pixid]->SetCanExtend(TH1::kAllAxes);
            
            // just the minimum of each sample (in Volts)
            title = _dutname+" ["+_boardname+"] CH:"+d_ch_col_row[1]+" Min. Signal  (Col:"
                +std::to_string(col)+",Row:"+std::to_string(row)+";V [mV];#Events";
            hname = "h_minsignal_"+_boardname+"_"+_dutname+"_"+std::to_string(pixid);
            _minsignal[pixid] = new TH1F(hname.c_str(), title.c_str(), 1000, -0.3, 0.3); 
            _minsignal[pixid]->SetCanExtend(TH1::kAllAxes);
            
            // baseline per event (in Volts)
            title = _dutname+" ["+_boardname+"] CH:"+d_ch_col_row[1]+" Baseline  (Col:"
                +std::to_string(col)+",Row:"+std::to_string(row)+");V [mV];#Events";
            hname = "h_baseline_"+_boardname+"_"+_dutname+"_"+std::to_string(pixid);
            _baseline[pixid] = new TH1F(hname.c_str(), title.c_str(), 1000, -0.03, 0.03); 
            _baseline[pixid]->SetCanExtend(TH1::kAllAxes);
            
            // amplitude 
            title = _dutname+" ["+_boardname+"] CH:"+d_ch_col_row[1]+" Amplitude [Peak - baseline] (Col:"
                +std::to_string(col)+",Row:"+std::to_string(row)+");V [mV];#Events";
            hname = "h_amplitude_"+_boardname+"_"+_dutname+"_"+std::to_string(pixid);
            _amplitude[pixid] = new TH1F(hname.c_str(), title.c_str(), 1000, -0.3, 0.3); 
            _amplitude[pixid]->SetCanExtend(TH1::kAllAxes);
            
            // Noise
            title = _dutname+" ["+_boardname+"] CH:"+d_ch_col_row[1]+" Noise (Col:"
                +std::to_string(col)+",Row:"+std::to_string(row)+");V [mV];#Events";
            hname = "h_noise_"+_boardname+"_"+_dutname+"_"+std::to_string(pixid);
            _noise[pixid] = new TH1F(hname.c_str(), title.c_str(), 1000, -3e-5,3e-5); 
            _noise[pixid]->SetCanExtend(TH1::kAllAxes);
            
            // SNR
            title = _dutname+" ["+_boardname+"] CH:"+d_ch_col_row[1]+" SNR (Col:"
                +std::to_string(col)+",Row:"+std::to_string(row)+");V [mV];#Events";
            hname = "h_snr_"+_boardname+"_"+_dutname+"_"+std::to_string(pixid);
            _snr[pixid] = new TH1F(hname.c_str(), title.c_str(), 1000, 0, 1); 
            _snr[pixid]->SetCanExtend(TH1::kAllAxes);
            
            title = _dutname+" ["+_boardname+"] CH:"+d_ch_col_row[1]+" Waveforms  (Col:"
                +std::to_string(col)+",Row:"+std::to_string(row)+");#Sample;V [mV];#Entries";
            hname = "h_waveform_"+_boardname+"_"+_dutname+"_"+std::to_string(pixid);
            // XXX -- HARDCODED!!  I need to extract it from config
            //_waveforms[pixid] = new TH2F(hname.c_str(), title.c_str(), 1024, -0.5,1023.5, 200, -0.045, 0.045); 
            _waveforms[pixid] = new TProfile(hname.c_str(), title.c_str(), 1024, -0.5,1023.5); 
            _waveforms[pixid]->SetCanExtend(TH1::kAllAxes);

            
            std::string chstr = d_ch_col_row[1];
            // Be careful, probably contains CH
            if( ! std::isdigit(chstr[0]) ) {
                chstr.replace(0,2,"");
            }
            // FIXME -- CHeck it again! This is a problematic bug to spot otherwise
            if( ! std::isdigit(chstr[0]) ) {
                std::cerr << "Unexpected format for the CHANNEL: [" << chstr << "]" <<std::endl;
            }
            int channel = std::stoi(chstr);
            // The Channel map, a static map, we can fill it up now
            const int col_bin = _channel_map->GetXaxis()->FindBin(col);
            const int row_bin = _channel_map->GetYaxis()->FindBin(row);
            _channel_map->SetBinContent(col_bin,row_bin,channel);
        }
    } else {
        std::cerr << "No max sensor size known!" << std::endl;
    }
}


void TimingHitmapHistos::Fill(const SimpleStandardHit &hit) {
    // Be sure it is not a fake pixel (just to fill up the board)
    int pixel_x = hit.getX();
    int pixel_y = hit.getY();
    
    // FIXME  -- XXX -- PERFORM HERE some quick waveform analysis: 
    //                  baseline, amplitude estimations, timing of arrival?
    const std::vector<double> wf = hit.getWaveform();
    const float dt = hit.getWaveformDX();
    if( wf.empty() ) {
        return;
    }
    
    auto itpix = _into_pixid.find({pixel_x,pixel_y});
    if( itpix == _into_pixid.end() )  {
        return;
    } 

    const unsigned int pixid = itpix->second;
    
    // Hit definition from waveform 
    const WaveformSummary ws = SummarizeWaveform(wf, 3.0, 40);

    // Fill it always
    _baseline[pixid]->Fill(ws.baseline);
    _amplitude[pixid]->Fill(ws.amplitude);

    // XXX
    _noise[pixid]->Fill(ws.sigma);
    _snr[pixid]->Fill(ws.snr);
    
    //// Build the physical x-axis properly: x[i] = x0 + i*dx
    //std::vector<double> x(wf.size());
    //for(size_t i = 0; i < wf.size(); ++i) {
    //    x[i] = hit.getWaveformX0() + static_cast<double>(i) * hit.getWaveformDx();
    //}
    std::vector<double> _s;
    // Just samples
    for(size_t _k = 0; _k < wf.size(); ++_k) {
        _s.push_back(_k);
    }
    _waveforms_not_filtered[pixid]->FillN(wf.size(), &_s[0], &(hit.getWaveform()[0]), nullptr, 1);
    // fill each signal of the sample
    _signal[pixid]->FillN(wf.size(), &(hit.getWaveform())[0], nullptr);
    
    // Time vs. minimum value of the waveform
    double min_wf = 1e9;    
    int index_min = -1;
    for(size_t _i = 0; _i < wf.size(); ++_i)
    {
        if( wf[_i] < min_wf ) {
            index_min = _i;
            min_wf = wf[_i];
        }
    }
    _time_minwaveform[pixid]->Fill(index_min, min_wf);
    _minsignal[pixid]->Fill(min_wf);
    
    // Occupancy: physical hit
    if( ws.is_hit ) {
        _occupancy_map->Fill(pixel_x, pixel_y);

        _waveforms[pixid]->FillN(wf.size(), &_s[0], &(hit.getWaveform()[0]), nullptr, 1);
    }

}

void TimingHitmapHistos::Fill(const SimpleStandardPlane &plane) {
    // XXX - Recover this?
    //if(_nHits != nullptr) {
    //    _nHits->Fill(plane.getNHits());
    //}
}


void TimingHitmapHistos::Reset() {
    _occupancy_map->Reset();
    _channel_map->Reset();
    for(auto & id_hwf: _waveforms) {
        id_hwf.second->Reset();
    }
    for(auto & id_hwf: _time_minwaveform) {
        id_hwf.second->Reset();
    }
    for(auto & id_hwf: _waveforms_not_filtered) {
        id_hwf.second->Reset();
    }
    for(auto & id_hamp: _minsignal) {
        id_hamp.second->Reset();
    }
    for(auto & id_hamp: _amplitude) {
        id_hamp.second->Reset();
    }
    for(auto & id_hamp: _noise) {
        id_hamp.second->Reset();
    }
    for(auto & id_hamp: _snr) {
        id_hamp.second->Reset();
    }
    for(auto & id_hamp: _signal) {
        id_hamp.second->Reset();
    }
    for(auto & id_hamp: _baseline) {
        id_hamp.second->Reset();
    }
}

void TimingHitmapHistos::Calculate(const int currentEventNum) {
    // No need to do anything
    _wait = true;
    // ... 
    _wait = false;
}

void TimingHitmapHistos::Write() {
    _occupancy_map->Write();
    _channel_map->Write();
    for(auto & id_hwf: _waveforms) {
        id_hwf.second->Write();
    }
    for(auto & id_hwf: _time_minwaveform) {
        id_hwf.second->Write();
    }
    for(auto & id_hwf: _waveforms_not_filtered) {
        id_hwf.second->Write();
    }
    for(auto & id_hamp: _minsignal) {
        id_hamp.second->Write();
    }
    for(auto & id_hamp: _amplitude) {
        id_hamp.second->Write();
    }
    for(auto & id_hamp: _noise) {
        id_hamp.second->Write();
    }
    for(auto & id_hamp: _snr) {
        id_hamp.second->Write();
    }
    for(auto & id_hamp: _signal) {
        id_hamp.second->Write();
    }
    for(auto & id_hamp: _baseline) {
        id_hamp.second->Write();
    }
}

int TimingHitmapHistos::_SetHistoAxisLabelx(TH1 *histo, string xlabel) {
    if(histo != nullptr) { 
        histo->GetXaxis()->SetTitle(xlabel.c_str());
    }
    return 0;
}

int TimingHitmapHistos::_SetHistoAxisLabels(TH1 *histo, string xlabel, string ylabel) {
    _SetHistoAxisLabelx(histo, xlabel);
    _SetHistoAxisLabely(histo, ylabel);

    return 0;
}

int TimingHitmapHistos::_SetHistoAxisLabely(TH1 *histo, string ylabel) {
    if(histo != nullptr) {
        histo->GetYaxis()->SetTitle(ylabel.c_str());
    }
    return 0;
}

std::pair<double,double> TimingHitmapHistos::getBaselineAndAmplitude(const std::vector<double> & waveform) {
    // Polarity
    auto itminmax = std::minmax_element(waveform.begin(), waveform.end());
    const double min = *itminmax.first;
    const double max = *itminmax.second;
    int polarity = 1;
    if( std::abs(*itminmax.first) > std::abs(*itminmax.second) ) {
        polarity = -1;
    }
    
    std::vector<double> wf_abs(waveform);
    for(double & v: wf_abs) {
        v *= polarity;
    }
    //
    // All signals are now positives
    // -----------------------------

    // Sorted: smaller first
    std::sort(wf_abs.begin(), wf_abs.end());
    const size_t wfsize = wf_abs.size();
    if(wfsize == 0) {
        return { 0.0, 0.0} ;
    }

    double baseline = 0;
    if(wfsize % 2 == 0) {
        // If even, we need to obtain the average of the two central values
        baseline = (wf_abs[wfsize/2 - 1]+wf_abs[wfsize/2])/2.0;
    } 
    else {
        baseline = wf_abs[wfsize/2];
    }
    
    // We need to evaluate a kind of sigma, to get an estimation
    // if there is a signal there
    const double wf_amplitude_max = wf_abs[wfsize-1];

    // --> Calculate the deviation standard -- XXX - ?
    const double mean = std::accumulate(wf_abs.begin(), wf_abs.end(),0.0)/wfsize;
    auto sum_term = [mean](double init, double value)-> double { return init + (value - mean)*(value - mean); };
    const double variance = std::accumulate(wf_abs.begin(), wf_abs.end(), 0.0, sum_term);
    const double stddev = std::sqrt(variance/wfsize);
    
    // Assume 3 sigma to be signal
    if( wf_amplitude_max > 3.0*(baseline + stddev) ) {
        return {baseline*polarity, wf_amplitude_max*polarity};
    }

    return {baseline*polarity, 0.0};
}

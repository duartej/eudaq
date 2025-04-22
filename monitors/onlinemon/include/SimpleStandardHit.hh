/*
 * SimpleStandardHit.hh
 *
 *  Created on: Jun 9, 2011
 *      Author: stanitz
 */
#ifndef SIMPLESTANDARDHIT_HH_
#define SIMPLESTANDARDHIT_HH_

#include<string>

class SimpleStandardHit {
protected:
  int _x;
  int _y;
  int _tot;
  int _lvl1;
  int _reduceX;
  int _reduceY;
  double _amplitude;
  std::vector<double> _waveform;
  double _waveform_x0;
  double _waveform_dx;
  std::string _auxinfo;

public:
  SimpleStandardHit(const int x, const int y)
      : _x(x), _y(y), _tot(-1), _lvl1(-1), _reduceX(-1), _reduceY(-1) {}
  SimpleStandardHit(const int x, const int y, const int tot, const int lvl1)
      : _x(x), _y(y), _tot(tot), _lvl1(lvl1), _reduceX(-1), _reduceY(-1) {}
  SimpleStandardHit(const int x, const int y, const int tot, const int lvl1,
                    const int reduceX, const int reduceY)
      : _x(x), _y(y), _tot(tot), _lvl1(lvl1), _reduceX(reduceX),
        _reduceY(-reduceY) {}

  int getX() const { return _x; }
  int getY() const { return _y; }

  int getTOT() const { return _tot; }
  int getLVL1() const { return _lvl1; }
  double getAmplitude() const { return _amplitude; }
  std::vector<double> getWaveform() const { return _waveform; }
  double getWaveformX0() const { return _waveform_x0; }
  double getWaveformDX() const { return _waveform_dx; }
  void setLVL1(const int lvl1) { _lvl1 = lvl1; }
  void setTOT(const int tot) { _tot = tot; }
  void setAmplitude(const double amplitude) { _amplitude = amplitude; }
  void setWaveform(const std::vector<double> waveform) { _waveform = waveform; }
  void setWaveformX0(double x0) { _waveform_x0 = x0; }
  void setWaveformDX(double dx) { _waveform_dx = dx; }
  void setAuxInfo( const std::string auxinfo ) { _auxinfo = auxinfo ; }
  template <typename T> 
      T getAuxInfoAs(int index) const;
  // void reduce(const int reduceX, const int reduceY) {_reduceX = reduceX;
  // _reduceY = reduceY;}
  // does this pixel use analog information ?
};

// Implementation of the template: Only works for numeric types!
template <typename T>
    T SimpleStandardHit::getAuxInfoAs(int index) const {
    // See in the client converter how it was defined. 
    // The extra information must be obtained by spliting the string
    // and each element is defined between semicolon (:)
    // element0:element1:element2: ... 
    const char token = ':';
    if( _auxinfo.empty() ) {
        std::cerr << "SimpleStandarHit::getAuxInfoAs no auxiliary info present" << std::endl;
        throw;
    }
    const int total_number_of_tokens = std::count(_auxinfo.begin(), _auxinfo.end(),token);
    if( total_number_of_tokens < index ) {
        std::cerr << "[SimpleStandarHit::getAuxInfoAs] Number of auxiliry items: " 
            << total_number_of_tokens+1 << " , requested item: " << index << std::endl;
        throw;
    }


    int current_size = 0;
    std::string remainder(_auxinfo);
    
    // Just check a stupid maximum
    while( current_size < 100 ) {
        size_t current_pos = remainder.find(token);
        if( current_pos == std::string::npos ) {
            if( current_size == index ) {
                return static_cast<T>(std::stod(remainder));
            }
        }

        std::string value = remainder.substr(0, current_pos);
        remainder = remainder.substr(current_pos+1);
        if( current_size == index ) {
            return static_cast<T>(std::stod(value));
        }
        ++current_size;
    }
    std::cerr << "[SimpleStandarHit::getAuxInfoAs] Unexpected error!!" << std::endl;
    throw;
}

#endif /* SIMPLESTANDARDHIT_HH_ */

#pragma once
#include <algorithm>
#include <cstddef>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <type_traits>
#include <utility>
namespace container::geometry::vdb::exact_statistics_detail {
// Exact integer units prevent both cancellation and squared underflow. A Double
// is an integer multiple of2^-1074; its coefficient has at most2098 bits.
// With count<=UINT64_MAX, squared sum/count expressions use at most4324 bits.
struct Big {
  static constexpr size_t Limbs=144; //4608 bits, fixed storage and no heap.
  std::array<uint32_t,Limbs> data{};
  explicit Big(uint64_t value=0){data[0]=uint32_t(value);data[1]=uint32_t(value>>32);}
  size_t bits()const{for(size_t i=Limbs;i--;)if(data[i])return i*32+32-std::countl_zero(data[i]);return 0;}
  bool zero()const{return bits()==0;}
  int compare(const Big& b)const{for(size_t i=Limbs;i--;)if(data[i]!=b.data[i])return data[i]<b.data[i]?-1:1;return 0;}
  void add(const Big& b){uint64_t carry=0;for(size_t i=0;i<Limbs;++i){const auto x=uint64_t(data[i])+b.data[i]+carry;data[i]=uint32_t(x);carry=x>>32;}if(carry)throw std::runtime_error("native statistics integer capacity exceeded");}
  void subtract(const Big& b){if(compare(b)<0)throw std::runtime_error("negative unsigned native statistics intermediate");uint64_t borrow=0;for(size_t i=0;i<Limbs;++i){const auto sub=uint64_t(b.data[i])+borrow;const auto value=uint64_t(data[i]);data[i]=uint32_t(value-sub);borrow=value<sub;}}
  void shift(size_t n){if(!n || zero())return;if(bits()+n>Limbs*32)throw std::runtime_error("native statistics shift capacity exceeded");const auto whole=n/32,part=n%32;for(size_t i=Limbs;i--;){uint64_t value=0;if(i>=whole){value=uint64_t(data[i-whole])<<part;if(part && i>whole)value|=uint64_t(data[i-whole-1])>>(32-part);}data[i]=uint32_t(value);}}
  static Big multiply(const Big& a,const Big& b){Big result;const size_t an=(a.bits()+31)/32,bn=(b.bits()+31)/32;for(size_t i=0;i<an;++i){uint64_t carry=0;for(size_t j=0;j<bn;++j){if(i+j>=Limbs)throw std::runtime_error("native statistics product capacity exceeded");const auto product=uint64_t(a.data[i])*b.data[j]+result.data[i+j]+carry;result.data[i+j]=uint32_t(product);carry=product>>32;}if(carry){if(i+bn>=Limbs)throw std::runtime_error("native statistics product capacity exceeded");result.data[i+bn]=uint32_t(carry);}}return result;}
  unsigned pair(size_t bit)const {const size_t index=bit/32;if(index>=Limbs)return 0;return unsigned((data[index]>>(bit%32))&3u);}
};
struct SignedBig {
  Big magnitude;bool negative=false;
  void add(const Big& value,bool sign){if(sign==negative)magnitude.add(value);else{const int order=magnitude.compare(value);if(order>=0)magnitude.subtract(value);else{auto next=value;next.subtract(magnitude);magnitude=next;negative=sign;}}if(magnitude.zero())negative=false;}
};
inline double ratio(const Big& numerator,uint64_t denominator,int unit) {
  if(numerator.zero())return 0;
  const Big denominatorBig(denominator);int exponent=int(numerator.bits())-int(denominatorBig.bits());
  if(exponent>=0){auto comparison=denominatorBig;comparison.shift(size_t(exponent));if(numerator.compare(comparison)<0)--exponent;}
  else{auto comparison=numerator;comparison.shift(size_t(-exponent));if(comparison.compare(denominatorBig)<0)--exponent;}
  const int target=std::max(exponent+unit-52,-1074),shift=unit-target;
  auto dividend=numerator,divisor=denominatorBig;if(shift>=0)dividend.shift(size_t(shift));else divisor.shift(size_t(-shift));
  uint64_t quotient=0;for(int bit=53;bit>=0;--bit){auto candidate=divisor;candidate.shift(size_t(bit));if(dividend.compare(candidate)>=0){dividend.subtract(candidate);quotient|=uint64_t(1)<<bit;}}
  dividend.shift(1);const auto rounding=dividend.compare(divisor);if(rounding>0 || (rounding==0 && (quotient&1)))++quotient;
  const double result=std::scalbn(double(quotient),target);
  if(!std::isfinite(result) || result==0)throw std::runtime_error("native statistics sum/moment exceeds representable Double range");return result;
}
inline Big squareRoot(const Big& value) {
  Big root,remainder;const size_t pairs=(value.bits()+1)/2;
  for(size_t i=pairs;i--;){remainder.shift(2);remainder.add(Big(value.pair(2*i)));root.shift(1);auto candidate=root;candidate.shift(1);candidate.add(Big(1));if(remainder.compare(candidate)>=0){remainder.subtract(candidate);root.add(Big(1));}}
  return root;
}
struct Moment {
  SignedBig sum;Big squares;int unit=0;
  template<class T>void add(T value, uint64_t weight=1) {
    if (!std::isfinite(double(value))) throw std::invalid_argument("native statistics require finite values");
    if (!weight) return;
    Big integer,square;bool negative=false;
    if constexpr(std::is_floating_point_v<T>) {
      unit=-1074;const auto bits=std::bit_cast<uint64_t>(double(value));const auto exponent=(bits>>52)&2047;
      const uint64_t significand=(bits&0x000fffffffffffffull)|(exponent?0x0010000000000000ull:0);
      integer=Big(significand);square=Big::multiply(integer,integer);
      if(exponent){integer.shift(size_t(exponent-1));square.shift(size_t(2*(exponent-1)));}negative=bool(bits>>63);
    } else {const auto signedValue=int64_t(value);negative=signedValue<0;integer=Big(negative?uint64_t(-(signedValue+1))+1:uint64_t(signedValue));square=Big::multiply(integer,integer);}
    if (weight!=1) {
      integer=Big::multiply(integer,Big(weight));
      square=Big::multiply(square,Big(weight));
    }
    sum.add(integer,negative);squares.add(square);
  }
  std::pair<double,double> finish(uint64_t count)const {
    if(!count)return {0,0};auto variance=Big::multiply(squares,Big(count));variance.subtract(Big::multiply(sum.magnitude,sum.magnitude));
    const double mean=ratio(sum.magnitude,count,unit)*(sum.negative?-1:1);
    //128 square-root guard bits preserve64 fractional integer-unit bits before
    // final rational rounding. No squared Double value is ever materialized.
    variance.shift(128);const double deviation=ratio(squareRoot(variance),count,unit-64);return {mean,deviation};
  }
  double total()const { return ratio(sum.magnitude,1,unit)*(sum.negative?-1:1); }
  double rootMeanSquare(uint64_t count)const {
    if (!count) return 0;
    auto numerator=Big::multiply(squares,Big(count));
    numerator.shift(128);
    return ratio(squareRoot(numerator),count,unit-64);
  }
};

inline SignedBig binaryUnits(double value) {
  if (!std::isfinite(value)) throw std::invalid_argument("histogram bounds/value must be finite");
  const auto bits=std::bit_cast<uint64_t>(value);
  const auto exponent=(bits>>52)&2047;
  SignedBig result;
  result.magnitude=Big((bits&0x000fffffffffffffull)|(exponent?0x0010000000000000ull:0));
  if (exponent) result.magnitude.shift(size_t(exponent-1));
  result.negative=(bits>>63) && !result.magnitude.zero();
  return result;
}

// Exact binary-rational bin boundaries. Neither high-low overflow nor Double
// rounding near an interior edge changes which half-open bin owns a value.
class Histogram {
public:
  Histogram(double low,double high,uint32_t bins):low_(binaryUnits(low)),bins_(bins) {
    if (!bins || !std::isfinite(high) || high<low)
      throw std::invalid_argument("invalid exact histogram range/bin count");
    auto span=binaryUnits(high);
    span.add(low_.magnitude,!low_.negative);
    if (span.negative) throw std::invalid_argument("negative exact histogram span");
    span_=span.magnitude;
  }
  uint32_t index(double value)const {
    if (span_.zero()) return 0;
    auto distance=binaryUnits(value);
    distance.add(low_.magnitude,!low_.negative);
    if (distance.negative) throw std::invalid_argument("histogram value below its range");
    const auto numerator=Big::multiply(distance.magnitude,Big(bins_));
    uint32_t low=0,high=bins_;
    while (low+1<high) {
      const uint32_t middle=low+(high-low)/2;
      if (numerator.compare(Big::multiply(span_,Big(middle)))>=0) low=middle;
      else high=middle;
    }
    return low; // The inclusive upper endpoint belongs to the last bin.
  }
private:
  SignedBig low_;
  Big span_;
  uint32_t bins_;
};
} // namespace container::geometry::vdb::exact_statistics_detail

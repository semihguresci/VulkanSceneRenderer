#include <Container/utility/TextureImageData.h>

#include <stb_image.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#ifndef CONTAINER_HAS_KTX
#define CONTAINER_HAS_KTX 0
#endif
#ifndef CONTAINER_HAS_WEBP
#define CONTAINER_HAS_WEBP 0
#endif
#if CONTAINER_HAS_KTX
#include <ktx.h>
#endif
#if CONTAINER_HAS_WEBP
#include <webp/decode.h>
#endif

namespace container::material {
namespace {
using Bytes = std::span<const std::byte>;
[[noreturn]] void fail(std::string_view reason) {
  throw std::runtime_error("Texture decode: " + std::string(reason));
}
uint64_t add(uint64_t a, uint64_t b) {
  if (b > UINT64_MAX - a) fail("byte-count overflow");
  return a + b;
}
uint64_t mul(uint64_t a, uint64_t b) {
  if (a && b > UINT64_MAX / a) fail("byte-count overflow");
  return a * b;
}
size_t hostSize(uint64_t n) {
  if (n > SIZE_MAX) fail("byte count exceeds address space");
  return static_cast<size_t>(n);
}
Bytes slice(Bytes data, uint64_t at, uint64_t size) {
  if (at > data.size() || size > data.size() - at) fail("truncated container or invalid byte range");
  return data.subspan(hostSize(at), hostSize(size));
}
uint32_t u32(Bytes bytes, uint64_t at, bool swap = false) {
  auto p = slice(bytes, at, 4);
  uint32_t result{};
  for (uint32_t i = 0; i != 4; ++i)
    result |= std::to_integer<uint32_t>(p[i]) << (8 * (swap ? 3 - i : i));
  return result;
}
uint64_t u64(Bytes bytes, uint64_t at) {
  return u32(bytes, at) | (uint64_t(u32(bytes, at + 4)) << 32);
}
bool starts(Bytes bytes, std::span<const unsigned char> prefix) {
  return bytes.size() >= prefix.size() && std::memcmp(bytes.data(), prefix.data(), prefix.size()) == 0;
}
struct FormatInfo { uint32_t x{1}, y{1}, bytes{}; bool compressed{}; };
FormatInfo info(VkFormat f) {
  switch (f) {
  case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SNORM: return {1,1,1,false};
  case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_SNORM:
  case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_SFLOAT: return {1,1,2,false};
  case VK_FORMAT_R8G8B8_UNORM: case VK_FORMAT_R8G8B8_SRGB:
  case VK_FORMAT_B8G8R8_UNORM: case VK_FORMAT_B8G8R8_SRGB: return {1,1,3,false};
  case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB:
  case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB:
  case VK_FORMAT_R16G16_UNORM: case VK_FORMAT_R16G16_SFLOAT:
  case VK_FORMAT_R32_SFLOAT: return {1,1,4,false};
  case VK_FORMAT_R16G16B16_UNORM: case VK_FORMAT_R16G16B16_SFLOAT: return {1,1,6,false};
  case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_SFLOAT:
  case VK_FORMAT_R32G32_SFLOAT: return {1,1,8,false};
  case VK_FORMAT_R32G32B32_SFLOAT: return {1,1,12,false};
  case VK_FORMAT_R32G32B32A32_SFLOAT: return {1,1,16,false};
  case VK_FORMAT_BC1_RGB_UNORM_BLOCK: case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
  case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
  case VK_FORMAT_BC4_UNORM_BLOCK: case VK_FORMAT_BC4_SNORM_BLOCK:
  case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK: case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
  case VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK: case VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK:
  case VK_FORMAT_EAC_R11_UNORM_BLOCK: case VK_FORMAT_EAC_R11_SNORM_BLOCK: return {4,4,8,true};
  case VK_FORMAT_BC2_UNORM_BLOCK: case VK_FORMAT_BC2_SRGB_BLOCK:
  case VK_FORMAT_BC3_UNORM_BLOCK: case VK_FORMAT_BC3_SRGB_BLOCK:
  case VK_FORMAT_BC5_UNORM_BLOCK: case VK_FORMAT_BC5_SNORM_BLOCK:
  case VK_FORMAT_BC6H_UFLOAT_BLOCK: case VK_FORMAT_BC6H_SFLOAT_BLOCK:
  case VK_FORMAT_BC7_UNORM_BLOCK: case VK_FORMAT_BC7_SRGB_BLOCK:
  case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK: case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:
  case VK_FORMAT_EAC_R11G11_UNORM_BLOCK: case VK_FORMAT_EAC_R11G11_SNORM_BLOCK: return {4,4,16,true};
#define ASTC(X,Y) case VK_FORMAT_ASTC_##X##x##Y##_UNORM_BLOCK: case VK_FORMAT_ASTC_##X##x##Y##_SRGB_BLOCK: return {X,Y,16,true}
  ASTC(4,4); ASTC(5,4); ASTC(5,5); ASTC(6,5); ASTC(6,6); ASTC(8,5); ASTC(8,6);
  ASTC(8,8); ASTC(10,5); ASTC(10,6); ASTC(10,8); ASTC(10,10); ASTC(12,10); ASTC(12,12);
#undef ASTC
  default: fail("unsupported VkFormat " + std::to_string(f));
  }
}
VkFormat roleFormat(VkFormat f, bool srgb) {
#define PAIR(A,B) case A: case B: return srgb ? B : A
  switch (f) {
  PAIR(VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8_SRGB);
  PAIR(VK_FORMAT_B8G8R8_UNORM, VK_FORMAT_B8G8R8_SRGB);
  PAIR(VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB);
  PAIR(VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB);
  PAIR(VK_FORMAT_BC1_RGB_UNORM_BLOCK, VK_FORMAT_BC1_RGB_SRGB_BLOCK);
  PAIR(VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC1_RGBA_SRGB_BLOCK);
  PAIR(VK_FORMAT_BC2_UNORM_BLOCK, VK_FORMAT_BC2_SRGB_BLOCK);
  PAIR(VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_BC3_SRGB_BLOCK);
  PAIR(VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_BC7_SRGB_BLOCK);
  PAIR(VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK);
  PAIR(VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK, VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK);
  PAIR(VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK, VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK);
  default:
    if (f >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK)
      return static_cast<VkFormat>(VK_FORMAT_ASTC_4x4_UNORM_BLOCK +
          ((uint32_t(f)-VK_FORMAT_ASTC_4x4_UNORM_BLOCK)/2)*2 + (srgb?1:0));
    return f;
  }
#undef PAIR
}
bool allowed(VkFormat format, const TextureDecodeOptions& o) {
  if (o.allowedFormats.empty()) return !info(format).compressed;
  return std::find(o.allowedFormats.begin(), o.allowedFormats.end(), format) != o.allowedFormats.end();
}
std::string formatName(VkFormat format) {
#define NAME(F) case F:return #F
  switch(format) {
  NAME(VK_FORMAT_R8_UNORM);NAME(VK_FORMAT_R8G8_UNORM);NAME(VK_FORMAT_R8G8B8A8_UNORM);NAME(VK_FORMAT_R8G8B8A8_SRGB);
  NAME(VK_FORMAT_R16_UNORM);NAME(VK_FORMAT_R16G16_UNORM);NAME(VK_FORMAT_R16G16B16A16_UNORM);
  NAME(VK_FORMAT_R16_SFLOAT);NAME(VK_FORMAT_R16G16B16A16_SFLOAT);NAME(VK_FORMAT_R32_SFLOAT);NAME(VK_FORMAT_R32G32_SFLOAT);NAME(VK_FORMAT_R32G32B32A32_SFLOAT);
  NAME(VK_FORMAT_BC1_RGBA_UNORM_BLOCK);NAME(VK_FORMAT_BC1_RGBA_SRGB_BLOCK);NAME(VK_FORMAT_BC3_UNORM_BLOCK);NAME(VK_FORMAT_BC3_SRGB_BLOCK);
  NAME(VK_FORMAT_BC4_UNORM_BLOCK);NAME(VK_FORMAT_BC5_UNORM_BLOCK);NAME(VK_FORMAT_BC7_UNORM_BLOCK);NAME(VK_FORMAT_BC7_SRGB_BLOCK);
  NAME(VK_FORMAT_ASTC_4x4_UNORM_BLOCK);NAME(VK_FORMAT_ASTC_4x4_SRGB_BLOCK);
  default:return "VkFormat("+std::to_string(format)+")";
  }
#undef NAME
}
void requireAllowed(VkFormat f, const TextureDecodeOptions& o) {
  if (!allowed(f,o)) fail(formatName(f) + " is absent from the caller's filterable format profile; a precision-preserving result is required");
}
void dimensions(uint32_t w, uint32_t h, uint32_t levels, const TextureDecodeOptions& o) {
  if (!w || !h || w > o.maxDimension || h > o.maxDimension) fail("image dimensions exceed limit or are zero");
  const auto full = static_cast<uint32_t>(std::bit_width(std::max(w,h)));
  if (!levels || levels > o.maxMipLevels || levels > full) fail("invalid or excessive mip level count");
}
uint64_t chainBytes(VkFormat f, uint32_t w, uint32_t h, uint32_t levels, const TextureDecodeOptions& o) {
  dimensions(w,h,levels,o);
  uint64_t total{};
  for (uint32_t i=0; i<levels; ++i)
    total = add(total, TextureMipByteSize(f,std::max(1u,w>>i),std::max(1u,h>>i)));
  if (total > o.maxDecodedBytes) fail("decoded mip chain exceeds maxDecodedBytes");
  return total;
}
float linear(float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); }
VkComponentMapping grayMapping() {
  return {VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_ONE};
}
void validateFloat(Bytes bytes) {
  for (size_t i=0; i<bytes.size(); i+=sizeof(float)) {
    float v; std::memcpy(&v,bytes.data()+i,sizeof(v));
    if (!std::isfinite(v)) fail("nonfinite floating-point texel");
  }
}
enum class Scalar { Unorm8, Unorm16, Snorm8, Float16, Float32 };
struct TexelInfo {Scalar scalar;uint32_t channels;bool bgr{};};
TexelInfo texelInfo(VkFormat f) {
  switch(f) {
  case VK_FORMAT_R8_UNORM:return {Scalar::Unorm8,1};case VK_FORMAT_R8G8_UNORM:return {Scalar::Unorm8,2};
  case VK_FORMAT_R8G8B8_UNORM:case VK_FORMAT_R8G8B8_SRGB:return {Scalar::Unorm8,3};
  case VK_FORMAT_R8G8B8A8_UNORM:case VK_FORMAT_R8G8B8A8_SRGB:return {Scalar::Unorm8,4};
  case VK_FORMAT_B8G8R8_UNORM:case VK_FORMAT_B8G8R8_SRGB:return {Scalar::Unorm8,3,true};
  case VK_FORMAT_B8G8R8A8_UNORM:case VK_FORMAT_B8G8R8A8_SRGB:return {Scalar::Unorm8,4,true};
  case VK_FORMAT_R8_SNORM:return {Scalar::Snorm8,1};case VK_FORMAT_R8G8_SNORM:return {Scalar::Snorm8,2};
  case VK_FORMAT_R16_UNORM:return {Scalar::Unorm16,1};case VK_FORMAT_R16G16_UNORM:return {Scalar::Unorm16,2};
  case VK_FORMAT_R16G16B16_UNORM:return {Scalar::Unorm16,3};case VK_FORMAT_R16G16B16A16_UNORM:return {Scalar::Unorm16,4};
  case VK_FORMAT_R16_SFLOAT:return {Scalar::Float16,1};case VK_FORMAT_R16G16_SFLOAT:return {Scalar::Float16,2};
  case VK_FORMAT_R16G16B16_SFLOAT:return {Scalar::Float16,3};case VK_FORMAT_R16G16B16A16_SFLOAT:return {Scalar::Float16,4};
  case VK_FORMAT_R32_SFLOAT:return {Scalar::Float32,1};case VK_FORMAT_R32G32_SFLOAT:return {Scalar::Float32,2};
  case VK_FORMAT_R32G32B32_SFLOAT:return {Scalar::Float32,3};case VK_FORMAT_R32G32B32A32_SFLOAT:return {Scalar::Float32,4};
  default:fail("unsupported uncompressed texel conversion");
  }
}
float halfFloat(uint16_t bits) {
  const uint32_t sign=uint32_t(bits&0x8000)<<16,exponent=(bits>>10)&31,mantissa=bits&1023;
  if(exponent==31) fail("nonfinite half-float texel");
  if(exponent) return std::bit_cast<float>(sign|((exponent+112)<<23)|(mantissa<<13));
  const float value=std::ldexp(float(mantissa),-24);return sign?-value:value;
}
std::array<float,4> readTexel(Bytes bytes,VkFormat format,uint64_t pixel) {
  const auto t=texelInfo(format);const auto stride=info(format).bytes;
  const auto* p=bytes.data()+pixel*stride;std::array<float,4> rgba{0,0,0,1};
  for(uint32_t c=0;c<t.channels;++c) {
    float value{};
    switch(t.scalar) {
    case Scalar::Unorm8:value=std::to_integer<uint8_t>(p[c])/255.f;break;
    case Scalar::Snorm8:{int8_t v;std::memcpy(&v,p+c,1);value=std::max(-1.f,v/127.f);break;}
    case Scalar::Unorm16:{uint16_t v;std::memcpy(&v,p+c*2,2);value=v/65535.f;break;}
    case Scalar::Float16:{uint16_t v;std::memcpy(&v,p+c*2,2);value=halfFloat(v);break;}
    case Scalar::Float32:std::memcpy(&value,p+c*4,4);break;
    }
    if(!std::isfinite(value)) fail("nonfinite floating-point texel");
    rgba[t.bgr && c<3?2-c:c]=value;
  }
  return rgba;
}
VkFormat fallbackUncompressed(VkFormat source,const TextureDecodeOptions& o) {
  const auto t=texelInfo(source);
  if(t.scalar==Scalar::Unorm8) return roleFormat(VK_FORMAT_R8G8B8A8_UNORM,o.isSrgb);
  if(t.scalar==Scalar::Unorm16 && !o.isSrgb && allowed(VK_FORMAT_R16G16B16A16_UNORM,o)) return VK_FORMAT_R16G16B16A16_UNORM;
  return VK_FORMAT_R32G32B32A32_SFLOAT;
}
std::vector<std::byte> convertTexels(Bytes bytes,VkFormat source,VkFormat target,uint32_t w,uint32_t h,bool srgb16) {
  std::vector<std::byte> converted(hostSize(TextureMipByteSize(target,w,h)));
  for(uint64_t p=0;p<mul(w,h);++p) {
    auto rgba=readTexel(bytes,source,p);
    if(srgb16) for(uint32_t c=0;c<3;++c) rgba[c]=linear(rgba[c]);
    if(target==VK_FORMAT_R32G32B32A32_SFLOAT) std::memcpy(converted.data()+p*16,rgba.data(),16);
    else if(target==VK_FORMAT_R16G16B16A16_UNORM) {
      std::array<uint16_t,4> words{};for(uint32_t c=0;c<4;++c)words[c]=uint16_t(std::lround(rgba[c]*65535.f));
      std::memcpy(converted.data()+p*8,words.data(),8);
    } else {
      std::array<uint8_t,4> texel{};for(uint32_t c=0;c<4;++c)texel[c]=uint8_t(std::lround(rgba[c]*255.f));
      std::memcpy(converted.data()+p*4,texel.data(),4);
    }
  }
  return converted;
}
TextureImageData ordinary(Bytes bytes, const TextureDecodeOptions& o) {
  if (bytes.size() > INT_MAX) fail("ordinary image exceeds STB's input-size range");
  const auto* data=reinterpret_cast<const stbi_uc*>(bytes.data());
  const int length=static_cast<int>(bytes.size());
  int w{},h{},channels{};
  if (!stbi_info_from_memory(data,length,&w,&h,&channels) || w<=0 || h<=0) fail("unrecognized or malformed ordinary image");
  dimensions(uint32_t(w),uint32_t(h),1,o);
  TextureImageData result;
  const bool hdr=stbi_is_hdr_from_memory(data,length)!=0;
  const bool wide=!hdr && stbi_is_16_bit_from_memory(data,length)!=0;
  const bool gray=channels==1 && !o.isSrgb && !hdr;
  const auto decodeFormat=hdr ? VK_FORMAT_R32G32B32A32_SFLOAT:
      wide ? (gray ? VK_FORMAT_R16_UNORM : VK_FORMAT_R16G16B16A16_UNORM)
      : gray ? VK_FORMAT_R8_UNORM : roleFormat(VK_FORMAT_R8G8B8A8_UNORM,o.isSrgb);
  result.format=wide&&o.isSrgb?VK_FORMAT_R32G32B32A32_SFLOAT:decodeFormat;
  if(!allowed(result.format,o) && !hdr && !(wide&&o.isSrgb))result.format=fallbackUncompressed(decodeFormat,o);
  requireAllowed(result.format,o);
  const auto size=chainBytes(result.format,w,h,1,o);
  TextureMipLevel mip{uint32_t(w),uint32_t(h),{}};
  if (gray) result.componentMapping=grayMapping();
  const int requested=gray ? 1 : 4;
  using StbiPtr=std::unique_ptr<void,decltype(&stbi_image_free)>;
  StbiPtr decoded(nullptr,stbi_image_free);
  if (hdr) decoded.reset(stbi_loadf_from_memory(data,length,&w,&h,&channels,4));
  else if (wide) decoded.reset(stbi_load_16_from_memory(data,length,&w,&h,&channels,requested));
  else decoded.reset(stbi_load_from_memory(data,length,&w,&h,&channels,requested));
  if (!decoded) fail("ordinary image pixel decode failed");
  mip.bytes.resize(hostSize(size));
  if (wide && o.isSrgb) {
    const auto* source=static_cast<const stbi_us*>(decoded.get());
    for (uint64_t p=0;p<mul(uint32_t(w),uint32_t(h));++p) {
      std::array<float,4> rgba{};
      for (uint32_t c=0;c!=4;++c) {
        const float value=source[p*4+c]/65535.0f;
        rgba[c]=c==3 ? value : linear(value);
      }
      std::memcpy(mip.bytes.data()+p*16,rgba.data(),16);
    }
  } else if(result.format!=decodeFormat) {
    const auto source=Bytes(static_cast<const std::byte*>(decoded.get()),hostSize(TextureMipByteSize(decodeFormat,w,h)));
    mip.bytes=convertTexels(source,decodeFormat,result.format,w,h,false);
  } else std::memcpy(mip.bytes.data(),decoded.get(),mip.bytes.size());
  if (hdr) validateFloat(mip.bytes);
  result.mips.push_back(std::move(mip));
  return result;
}

// BC1-5 UNORM fallback keeps the encoded transfer function. sRGB conversion is
// performed by the selected SRGB sampled-image format, including after decode.
std::array<uint8_t,4> rgb565(uint32_t v) {
  const uint32_t r=(v>>11)&31,g=(v>>5)&63,b=v&31;
  return {uint8_t((r<<3)|(r>>2)),uint8_t((g<<2)|(g>>4)),uint8_t((b<<3)|(b>>2)),255};
}
void bcAlpha(const uint8_t* p, uint8_t* out) {
  std::array<uint8_t,8> table{p[0],p[1]};
  if (p[0]>p[1]) for (uint32_t i=1;i<=6;++i) table[i+1]=uint8_t(((7-i)*p[0]+i*p[1])/7);
  else { for (uint32_t i=1;i<=4;++i) table[i+1]=uint8_t(((5-i)*p[0]+i*p[1])/5); table[6]=0; table[7]=255; }
  uint64_t bits{}; for(uint32_t i=0;i<6;++i) bits|=uint64_t(p[i+2])<<(8*i);
  for(uint32_t i=0;i<16;++i) out[i]=table[(bits>>(3*i))&7];
}
bool canDecodeBc(VkFormat f) {
  return (f>=VK_FORMAT_BC1_RGB_UNORM_BLOCK && f<=VK_FORMAT_BC3_SRGB_BLOCK) ||
      f==VK_FORMAT_BC4_UNORM_BLOCK || f==VK_FORMAT_BC5_UNORM_BLOCK;
}
VkFormat outputFormat(VkFormat source,const TextureDecodeOptions& o,bool flipY=false) {
  const auto format=roleFormat(source,o.isSrgb);const auto fi=info(format);
  VkFormat output=format;
  const bool noSrgbVariant=o.isSrgb && fi.compressed && roleFormat(format,true)==roleFormat(format,false) &&
      format!=VK_FORMAT_BC6H_UFLOAT_BLOCK && format!=VK_FORMAT_BC6H_SFLOAT_BLOCK;
  if(fi.compressed && (!allowed(format,o)||flipY||noSrgbVariant)) {
    if(!canDecodeBc(format)) fail("native compressed "+formatName(format)+" cannot satisfy the caller's format, orientation or sRGB role and has no CPU fallback; provide a supported uncompressed image");
    output=roleFormat(VK_FORMAT_R8G8B8A8_UNORM,o.isSrgb);
  } else if(!fi.compressed) {
    const auto t=texelInfo(format);
    if(o.isSrgb && t.scalar==Scalar::Snorm8) fail("signed normalized texels cannot be used in an sRGB material role");
    if(!allowed(format,o)||t.channels==3||
        (o.isSrgb && (t.scalar==Scalar::Unorm16||((format==VK_FORMAT_R8_UNORM||format==VK_FORMAT_R8G8_UNORM)))))
      output=fallbackUncompressed(format,o);
  }
  requireAllowed(output,o);return output;
}
std::vector<std::byte> decodeBc(Bytes source,VkFormat format,uint32_t w,uint32_t h) {
  std::vector<std::byte> out(hostSize(mul(mul(w,h),4)));
  const auto fi=info(format);
  const auto* data=reinterpret_cast<const uint8_t*>(source.data());
  for(uint32_t by=0;by<(h+3)/4;++by) for(uint32_t bx=0;bx<(w+3)/4;++bx) {
    const auto* p=data+((uint64_t(by)*((w+3)/4)+bx)*fi.bytes);
    std::array<std::array<uint8_t,4>,16> colors{};
    const bool bc4=format==VK_FORMAT_BC4_UNORM_BLOCK, bc5=format==VK_FORMAT_BC5_UNORM_BLOCK;
    if (bc4 || bc5) {
      std::array<uint8_t,16> red{},green{}; bcAlpha(p,red.data()); if(bc5) bcAlpha(p+8,green.data());
      for(uint32_t i=0;i<16;++i) colors[i]={red[i],green[i],0,255};
    } else {
      const bool bc2=format==VK_FORMAT_BC2_UNORM_BLOCK||format==VK_FORMAT_BC2_SRGB_BLOCK;
      const bool bc3=format==VK_FORMAT_BC3_UNORM_BLOCK||format==VK_FORMAT_BC3_SRGB_BLOCK;
      const auto* c=p+(bc2||bc3 ? 8 : 0);
      const uint32_t a=c[0]|(uint32_t(c[1])<<8), b=c[2]|(uint32_t(c[3])<<8);
      std::array<std::array<uint8_t,4>,4> table{rgb565(a),rgb565(b)};
      if(a>b||bc2||bc3) {
        for(uint32_t k=0;k<3;++k) {table[2][k]=uint8_t((2*table[0][k]+table[1][k])/3);table[3][k]=uint8_t((table[0][k]+2*table[1][k])/3);}
        table[2][3]=table[3][3]=255;
      } else {
        for(uint32_t k=0;k<3;++k) table[2][k]=uint8_t((table[0][k]+table[1][k])/2);
        table[2][3]=255;
        // RGB BC1's fourth code is opaque black; RGBA BC1's is transparent.
        table[3][3]=(format==VK_FORMAT_BC1_RGB_UNORM_BLOCK||format==VK_FORMAT_BC1_RGB_SRGB_BLOCK)?255:0;
      }
      uint32_t bits=c[4]|(uint32_t(c[5])<<8)|(uint32_t(c[6])<<16)|(uint32_t(c[7])<<24);
      std::array<uint8_t,16> alpha{}; if(bc3) bcAlpha(p,alpha.data());
      for(uint32_t i=0;i<16;++i) { colors[i]=table[(bits>>(2*i))&3]; if(bc2) colors[i][3]=uint8_t(((p[i/2]>>(4*(i%2)))&15)*17); if(bc3) colors[i][3]=alpha[i]; }
    }
    for(uint32_t y=0;y<4 && by*4+y<h;++y) for(uint32_t x=0;x<4 && bx*4+x<w;++x)
      std::memcpy(out.data()+((uint64_t(by*4+y)*w+bx*4+x)*4),colors[y*4+x].data(),4);
  }
  return out;
}
struct ContainerImage {
  VkFormat format{};
  uint32_t w{},h{},levels{};
  VkComponentMapping mapping{VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY};
  std::vector<Bytes> payload;
  std::vector<uint64_t> rowPitch;
  bool flipY{},swapWords{};
  uint32_t wordBytes{1};
};
void metadata(Bytes kv, bool swap, bool ktx1, bool strict, ContainerImage& image) {
  bool sawOrientation{},sawSwizzle{};
  uint64_t at{};
  while(at<kv.size()) {
    const uint32_t size=u32(kv,at,swap); at=add(at,4);
    auto entry=slice(kv,at,size); at=add(at,(uint64_t(size)+3)&~uint64_t(3));
    if(at>kv.size()) fail("invalid KTX key/value padding");
    const auto* p=reinterpret_cast<const char*>(entry.data());
    const auto* end=p+entry.size(); const auto* zero=std::find(p,end,'\0');
    if(zero==end) fail("KTX metadata key has no terminator");
    const std::string_view key(p,size_t(zero-p));
    std::string_view value(zero+1,size_t(end-zero-1));
    if(!value.empty()&&value.back()=='\0') value.remove_suffix(1);
    if(key=="KTXorientation") {
      if(sawOrientation) fail("duplicate KTXorientation"); sawOrientation=true;
      if(strict && value!="rd") fail("KHR_texture_basisu requires KTXorientation=rd");
      if(value==(ktx1?"S=r,T=d":"rd")) image.flipY=false;
      else if(value==(ktx1?"S=r,T=u":"ru")) image.flipY=true;
      else fail("unsupported KTXorientation (only rightward, down/up 2D images are supported)");
    } else if(key=="KTXswizzle") {
      if(sawSwizzle) fail("duplicate KTXswizzle"); sawSwizzle=true;
      if(value.size()!=4 || (strict && value!="rgba")) fail("invalid KTXswizzle or nonidentity KHR_texture_basisu swizzle");
      std::array<VkComponentSwizzle,4> sw{};
      for(size_t i=0;i<4;++i) switch(value[i]) {
        case 'r':sw[i]=VK_COMPONENT_SWIZZLE_R;break; case 'g':sw[i]=VK_COMPONENT_SWIZZLE_G;break;
        case 'b':sw[i]=VK_COMPONENT_SWIZZLE_B;break; case 'a':sw[i]=VK_COMPONENT_SWIZZLE_A;break;
        case '0':sw[i]=VK_COMPONENT_SWIZZLE_ZERO;break; case '1':sw[i]=VK_COMPONENT_SWIZZLE_ONE;break;
        default:fail("invalid KTXswizzle component");
      }
      image.mapping={sw[0],sw[1],sw[2],sw[3]};
    }
  }
}
TextureImageData materialize(const ContainerImage& image,const TextureDecodeOptions& o) {
  auto format=roleFormat(image.format,o.isSrgb);
  const auto fi=info(format);
  const auto output=outputFormat(format,o,image.flipY);
  const bool unpack=fi.compressed && output!=format;
  const bool wideSrgb=o.isSrgb && (format==VK_FORMAT_R16_UNORM||format==VK_FORMAT_R16G16_UNORM||
      format==VK_FORMAT_R16G16B16_UNORM||format==VK_FORMAT_R16G16B16A16_UNORM);
  const bool convertUncompressed=!fi.compressed && output!=format;
  requireAllowed(output,o); chainBytes(output,image.w,image.h,image.levels,o);
  TextureImageData result{output,image.mapping,{},true};
  for(uint32_t level=0;level<image.levels;++level) {
    const uint32_t w=std::max(1u,image.w>>level),h=std::max(1u,image.h>>level);
    const auto bytes=TextureMipByteSize(format,w,h);
    const auto row=mul((uint64_t(w)+fi.x-1)/fi.x,fi.bytes), rows=(uint64_t(h)+fi.y-1)/fi.y;
    const auto pitch=image.rowPitch.empty()?row:image.rowPitch[level];
    if(pitch<row || image.payload[level].size()!=mul(pitch,rows)) fail("container mip byte length disagrees with dimensions/format");
    TextureMipLevel mip{w,h,{}};
    if(unpack) mip.bytes=decodeBc(image.payload[level],format,w,h);
    else {
      mip.bytes.resize(hostSize(bytes));
      for(uint64_t y=0;y<rows;++y) std::memcpy(mip.bytes.data()+y*row,image.payload[level].data()+y*pitch,hostSize(row));
      if(image.swapWords && image.wordBytes>1) for(size_t at=0;at<mip.bytes.size();at+=image.wordBytes)
        std::reverse(mip.bytes.begin()+at,mip.bytes.begin()+at+image.wordBytes);
    }
    if(image.flipY) {
      const uint64_t outputRow=unpack?mul(w,4):row;
      for(uint32_t y=0;y<h/2;++y) for(uint64_t x=0;x<outputRow;++x)
        std::swap(mip.bytes[hostSize(y*outputRow+x)],mip.bytes[hostSize((h-1-y)*outputRow+x)]);
    }
    if(convertUncompressed) mip.bytes=convertTexels(mip.bytes,format,output,w,h,wideSrgb);
    if(!fi.compressed && !convertUncompressed && texelInfo(format).scalar==Scalar::Float16)
      for(uint64_t p=0;p<mul(w,h);++p) (void)readTexel(mip.bytes,format,p);
    if(output==VK_FORMAT_R32_SFLOAT||output==VK_FORMAT_R32G32_SFLOAT||output==VK_FORMAT_R32G32B32_SFLOAT||output==VK_FORMAT_R32G32B32A32_SFLOAT) validateFloat(mip.bytes);
    result.mips.push_back(std::move(mip));
  }
  return result;
}
constexpr uint32_t fourcc(char a,char b,char c,char d) { return uint32_t(a)|(uint32_t(b)<<8)|(uint32_t(c)<<16)|(uint32_t(d)<<24); }
VkFormat dxgi(uint32_t f) {
  switch(f) {
    case 2:return VK_FORMAT_R32G32B32A32_SFLOAT;case 10:return VK_FORMAT_R16G16B16A16_SFLOAT;case 11:return VK_FORMAT_R16G16B16A16_UNORM;
    case 16:return VK_FORMAT_R32G32_SFLOAT;case 28:case 29:return VK_FORMAT_R8G8B8A8_UNORM;
    case 34:return VK_FORMAT_R16G16_SFLOAT;case 35:return VK_FORMAT_R16G16_UNORM;case 41:return VK_FORMAT_R32_SFLOAT;
    case 49:return VK_FORMAT_R8G8_UNORM;case 54:return VK_FORMAT_R16_SFLOAT;case 56:return VK_FORMAT_R16_UNORM;case 61:return VK_FORMAT_R8_UNORM;
    case 71:case 72:return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;case 74:case 75:return VK_FORMAT_BC2_UNORM_BLOCK;case 77:case 78:return VK_FORMAT_BC3_UNORM_BLOCK;
    case 80:return VK_FORMAT_BC4_UNORM_BLOCK;case 81:return VK_FORMAT_BC4_SNORM_BLOCK;case 83:return VK_FORMAT_BC5_UNORM_BLOCK;case 84:return VK_FORMAT_BC5_SNORM_BLOCK;
    case 87:case 88:case 91:case 93:return VK_FORMAT_B8G8R8A8_UNORM;case 95:return VK_FORMAT_BC6H_UFLOAT_BLOCK;case 96:return VK_FORMAT_BC6H_SFLOAT_BLOCK;
    case 98:case 99:return VK_FORMAT_BC7_UNORM_BLOCK;default:fail("unsupported DDS DXGI format "+std::to_string(f));
  }
}
TextureImageData dds(Bytes bytes,const TextureDecodeOptions& o) {
  if(u32(bytes,4)!=124 || u32(bytes,76)!=32) fail("invalid DDS header sizes");
  ContainerImage image; image.w=u32(bytes,16);image.h=u32(bytes,12);image.levels=std::max(1u,u32(bytes,28));
  dimensions(image.w,image.h,image.levels,o);
  if(u32(bytes,24)>1 || (u32(bytes,112)&0x0020fe00)) fail("DDS cubemaps, arrays and volumes are unsupported for a 2D material texture");
  const auto flags=u32(bytes,80), code=u32(bytes,84); uint64_t at=128;
  if(flags&4) {
    switch(code) {
      case fourcc('D','X','1','0'):
        image.format=dxgi(u32(bytes,128));
        if(u32(bytes,132)!=3 || u32(bytes,140)!=1 || (u32(bytes,136)&4)) fail("DDS DX10 image must be a single 2D layer");
        if(u32(bytes,144)>3 || (u32(bytes,144)&7)==2) fail("premultiplied/custom DDS alpha is unsupported; provide straight alpha");
        if((u32(bytes,144)&7)==3||u32(bytes,128)==88||u32(bytes,128)==93)image.mapping.a=VK_COMPONENT_SWIZZLE_ONE;
        at=148;break;
      case fourcc('D','X','T','1'):image.format=VK_FORMAT_BC1_RGBA_UNORM_BLOCK;break;
      case fourcc('D','X','T','3'):image.format=VK_FORMAT_BC2_UNORM_BLOCK;break;
      case fourcc('D','X','T','5'):image.format=VK_FORMAT_BC3_UNORM_BLOCK;break;
      case fourcc('A','T','I','1'):case fourcc('B','C','4','U'):image.format=VK_FORMAT_BC4_UNORM_BLOCK;break;
      case fourcc('A','T','I','2'):case fourcc('B','C','5','U'):image.format=VK_FORMAT_BC5_UNORM_BLOCK;break;
      default:fail("unsupported DDS FourCC (premultiplied DXT2/DXT4 are not straight-alpha textures)");
    }
  } else {
    const auto bits=u32(bytes,88),r=u32(bytes,92),g=u32(bytes,96),b=u32(bytes,100),a=u32(bytes,104);
    if(bits==32 && r==0xff && g==0xff00 && b==0xff0000 && (a==0xff000000||!a)) image.format=VK_FORMAT_R8G8B8A8_UNORM;
    else if(bits==32 && r==0xff0000 && g==0xff00 && b==0xff && (a==0xff000000||!a)) image.format=VK_FORMAT_B8G8R8A8_UNORM;
    else if(bits==24 && r==0xff && g==0xff00 && b==0xff0000 && !a)image.format=VK_FORMAT_R8G8B8_UNORM;
    else if(bits==24 && r==0xff0000 && g==0xff00 && b==0xff && !a)image.format=VK_FORMAT_B8G8R8_UNORM;
    else if((flags&0x20000) && bits==8 && r==0xff && !g&&!b&&!a) {image.format=VK_FORMAT_R8_UNORM;image.mapping=grayMapping();}
    else if((flags&0x20000) && bits==16 && r==0xffff && !g&&!b&&!a) {image.format=VK_FORMAT_R16_UNORM;image.mapping=grayMapping();}
    else fail("unsupported DDS pixel bit masks");
    if(bits==32&&!a)image.mapping.a=VK_COMPONENT_SWIZZLE_ONE;
  }
  const auto fi=info(image.format);
  for(uint32_t level=0;level<image.levels;++level) {
    const auto w=std::max(1u,image.w>>level),h=std::max(1u,image.h>>level);
    const auto row=mul((uint64_t(w)+fi.x-1)/fi.x,fi.bytes);
    // DDS explicitly stores only the top-level pitch; lower levels are tight.
    const auto pitch=level==0&&!fi.compressed&&(u32(bytes,8)&8)?uint64_t(u32(bytes,20)):row;
    if(pitch<row) fail("DDS row pitch is smaller than a texel row");
    const auto size=mul(pitch,(uint64_t(h)+fi.y-1)/fi.y);
    image.payload.push_back(slice(bytes,at,size));image.rowPitch.push_back(pitch);at=add(at,size);
  }
  if(at!=bytes.size()) fail("DDS has trailing data or unsupported extra surfaces");
  return materialize(image,o);
}
VkFormat glInternal(uint32_t value) {
  switch(value) {
    case 0x8229:return VK_FORMAT_R8_UNORM;case 0x822b:return VK_FORMAT_R8G8_UNORM;
    case 0x8051:return VK_FORMAT_R8G8B8_UNORM;case 0x8058:case 0x8c43:return VK_FORMAT_R8G8B8A8_UNORM;case 0x8c41:return VK_FORMAT_R8G8B8_UNORM;
    case 0x822a:return VK_FORMAT_R16_UNORM;case 0x822c:return VK_FORMAT_R16G16_UNORM;case 0x8054:return VK_FORMAT_R16G16B16_UNORM;case 0x805b:return VK_FORMAT_R16G16B16A16_UNORM;
    case 0x822d:return VK_FORMAT_R16_SFLOAT;case 0x822f:return VK_FORMAT_R16G16_SFLOAT;case 0x881b:return VK_FORMAT_R16G16B16_SFLOAT;case 0x881a:return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 0x822e:return VK_FORMAT_R32_SFLOAT;case 0x8230:return VK_FORMAT_R32G32_SFLOAT;case 0x8815:return VK_FORMAT_R32G32B32_SFLOAT;case 0x8814:return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 0x83f0:case 0x8c4c:return VK_FORMAT_BC1_RGB_UNORM_BLOCK;case 0x83f1:case 0x8c4d:return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case 0x83f2:case 0x8c4e:return VK_FORMAT_BC2_UNORM_BLOCK;case 0x83f3:case 0x8c4f:return VK_FORMAT_BC3_UNORM_BLOCK;
    case 0x8dbb:return VK_FORMAT_BC4_UNORM_BLOCK;case 0x8dbc:return VK_FORMAT_BC4_SNORM_BLOCK;case 0x8dbd:return VK_FORMAT_BC5_UNORM_BLOCK;case 0x8dbe:return VK_FORMAT_BC5_SNORM_BLOCK;
    case 0x8e8f:return VK_FORMAT_BC6H_UFLOAT_BLOCK;case 0x8e8e:return VK_FORMAT_BC6H_SFLOAT_BLOCK;case 0x8e8c:case 0x8e8d:return VK_FORMAT_BC7_UNORM_BLOCK;
    case 0x9274:case 0x9275:return VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK;case 0x9276:case 0x9277:return VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK;case 0x9278:case 0x9279:return VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK;
    case 0x9270:return VK_FORMAT_EAC_R11_UNORM_BLOCK;case 0x9271:return VK_FORMAT_EAC_R11_SNORM_BLOCK;case 0x9272:return VK_FORMAT_EAC_R11G11_UNORM_BLOCK;case 0x9273:return VK_FORMAT_EAC_R11G11_SNORM_BLOCK;
    default:
      if(value>=0x93b0 && value<=0x93bd) return static_cast<VkFormat>(VK_FORMAT_ASTC_4x4_UNORM_BLOCK+(value-0x93b0)*2);
      if(value>=0x93d0 && value<=0x93dd) return static_cast<VkFormat>(VK_FORMAT_ASTC_4x4_UNORM_BLOCK+(value-0x93d0)*2);
      fail("unsupported KTX1 GL internal format "+std::to_string(value));
  }
}
TextureImageData ktx1(Bytes bytes,const TextureDecodeOptions& o) {
  const uint32_t endian=u32(bytes,12);if(endian!=0x04030201 && endian!=0x01020304) fail("invalid KTX1 endian marker");
  const bool swap=endian==0x01020304;auto read=[&](uint64_t at){return u32(bytes,at,swap);};
  ContainerImage image;image.w=read(36);image.h=read(40);image.levels=std::max(1u,read(56));
  dimensions(image.w,image.h,image.levels,o);
  if(read(44)||read(48)||read(52)!=1) fail("KTX1 material image must be a single 2D layer");
  image.format=glInternal(read(28));const auto fi=info(image.format);
  const auto type=read(16),typeSize=read(20),format=read(24);
  if(fi.compressed) {if(type||format||typeSize!=1) fail("invalid compressed KTX1 type metadata");}
  else {
    if(typeSize!=1 && typeSize!=2 && typeSize!=4) fail("unsupported KTX1 scalar width");
    const uint32_t channels=fi.bytes/typeSize;
    const uint32_t glFormat=channels==1?0x1903:channels==2?0x8227:channels==3?0x1907:0x1908;
    const bool isFloat=image.format==VK_FORMAT_R16_SFLOAT||image.format==VK_FORMAT_R16G16_SFLOAT||image.format==VK_FORMAT_R16G16B16_SFLOAT||image.format==VK_FORMAT_R16G16B16A16_SFLOAT||image.format==VK_FORMAT_R32_SFLOAT||image.format==VK_FORMAT_R32G32_SFLOAT||image.format==VK_FORMAT_R32G32B32_SFLOAT||image.format==VK_FORMAT_R32G32B32A32_SFLOAT;
    const uint32_t expectedType=isFloat?(typeSize==2?0x140b:0x1406):(typeSize==1?0x1401:0x1403);
    if(channels<1||channels>4||type!=expectedType||format!=glFormat) fail("KTX1 glType/glFormat disagree with internal format");
    image.swapWords=swap;image.wordBytes=typeSize;
  }
  const auto kv=read(60);metadata(slice(bytes,64,kv),swap,true,false,image);uint64_t at=add(64,kv);
  for(uint32_t level=0;level<image.levels;++level) {
    const uint32_t size=read(at);at=add(at,4);
    const uint32_t w=std::max(1u,image.w>>level),h=std::max(1u,image.h>>level);
    const auto row=mul((uint64_t(w)+fi.x-1)/fi.x,fi.bytes),pitch=fi.compressed?row:(row+3)&~uint64_t(3);
    if(size!=mul(pitch,(uint64_t(h)+fi.y-1)/fi.y)) fail("KTX1 imageSize disagrees with the mip format");
    image.payload.push_back(slice(bytes,at,size));image.rowPitch.push_back(pitch);at=add(at,(uint64_t(size)+3)&~uint64_t(3));
  }
  if(at!=bytes.size()) fail("KTX1 trailing data or invalid mip padding");
  return materialize(image,o);
}

struct Ktx2Header {
  ContainerImage image;
  uint32_t super{},dfdModel{},transfer{},primaries{},flags{};
  std::vector<uint64_t> offsets,lengths,rawLengths;
};
void validateDfd(Bytes dfd,VkFormat format,uint32_t model,bool supercompressed) {
  const uint32_t block=u32(dfd,8)>>16;
  if(block<40 || (block-24)%16 || block+4!=dfd.size()) fail("unsupported or malformed KTX2 DFD descriptor/sample length");
  if(std::to_integer<uint32_t>(dfd[18])||std::to_integer<uint32_t>(dfd[19])) fail("KTX2 DFD describes a non-2D texel block");
  const auto byte=[](std::byte b){return std::to_integer<uint32_t>(b);};
  uint32_t blockBytes{};for(uint32_t p=0;p<8;++p)blockBytes+=byte(dfd[20+p]);
  // Older KTX2 revisions required zero bytesPlane[] for deflated payloads.
  // The current specification explicitly requires consumers to retain support.
  const bool unsized=supercompressed && !blockBytes;
  if(format!=VK_FORMAT_UNDEFINED) {
    const auto fi=info(format);
    uint32_t expectedModel=1;
    if(format>=VK_FORMAT_BC1_RGB_UNORM_BLOCK&&format<=VK_FORMAT_BC7_SRGB_BLOCK)
      expectedModel=format<=VK_FORMAT_BC1_RGBA_SRGB_BLOCK?128:format<=VK_FORMAT_BC2_SRGB_BLOCK?129:format<=VK_FORMAT_BC3_SRGB_BLOCK?130:format<=VK_FORMAT_BC4_SNORM_BLOCK?131:format<=VK_FORMAT_BC5_SNORM_BLOCK?132:format<=VK_FORMAT_BC6H_SFLOAT_BLOCK?133:134;
    else if(format>=VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK&&format<=VK_FORMAT_EAC_R11G11_SNORM_BLOCK)expectedModel=161;
    else if(format>=VK_FORMAT_ASTC_4x4_UNORM_BLOCK&&format<=VK_FORMAT_ASTC_12x12_SRGB_BLOCK)expectedModel=162;
    if(model!=expectedModel || byte(dfd[16])+1!=fi.x || byte(dfd[17])+1!=fi.y || (!unsized&&(byte(dfd[20])!=fi.bytes||blockBytes!=fi.bytes))) fail("KTX2 DFD texel block disagrees with VkFormat");
    if(unsized)blockBytes=fi.bytes;
    if(!fi.compressed) {
      const auto t=texelInfo(format);const uint32_t scalarBits=fi.bytes*8/t.channels;
      if((block-24)/16!=t.channels)fail("KTX2 DFD sample count disagrees with VkFormat");
      uint32_t seen{};
      for(uint32_t at=28;at<dfd.size();at+=16) {
        const auto packed=u32(dfd,at),offset=packed&0xffff,bits=((packed>>16)&255)+1,channel=(packed>>24)&15,qualifiers=(packed>>24)&0xc0;
        const uint32_t component=offset/scalarBits;
        if(offset%scalarBits || bits!=scalarBits || component>=t.channels || (seen&(1u<<component)))fail("KTX2 DFD scalar layout disagrees with VkFormat");
        const auto expectedChannel=component==3?15u:t.bgr?2-component:component;
        const auto expectedQualifiers=t.scalar==Scalar::Float16||t.scalar==Scalar::Float32?0xc0u:t.scalar==Scalar::Snorm8?0x40u:0u;
        if(channel!=expectedChannel || qualifiers!=expectedQualifiers)fail("KTX2 DFD scalar channel/type disagrees with VkFormat");
        seen|=1u<<component;
      }
    }
  } else {
    if((model!=163 && model!=166)||byte(dfd[16])!=3||byte(dfd[17])!=3)fail("unsupported Basis KTX2 DFD block");
    if(model==166 && !unsized && (byte(dfd[20])!=16||blockBytes!=16))fail("UASTC DFD must describe a 16-byte block");
    if(model==163 && !unsized && (byte(dfd[20])!=8 || (byte(dfd[21])!=0 && byte(dfd[21])!=8)||blockBytes!=8+byte(dfd[21])))fail("ETC1S DFD must describe one or two 8-byte slices");
    if(unsized)blockBytes=model==166?16:(block-24)/16*8;
    const uint32_t samples=(block-24)/16;
    if((model==166&&samples!=1)||(model==163&&samples>2))fail("invalid Basis KTX2 DFD sample count");
    for(uint32_t i=0;i<samples;++i) {
      const uint32_t packed=u32(dfd,28+i*16),offset=packed&0xffff,bits=((packed>>16)&255)+1,channel=(packed>>24)&15;
      const bool knownChannel=model==166?(channel==0||channel==3||channel==4||channel==5||channel==6):
          (channel==0||channel==3||channel==4||channel==15);
      if(!knownChannel||bits!=(model==166?128u:64u)||offset!=(model==166?0u:i*64u))fail("invalid Basis KTX2 DFD channel/bit layout");
    }
  }
  for(uint32_t at=28;at<dfd.size();at+=16) {
    const uint32_t packed=u32(dfd,at),offset=packed&0xffff,bits=((packed>>16)&255)+1;
    if(offset+bits>blockBytes*8)fail("KTX2 DFD sample exceeds its texel block");
    if(format!=VK_FORMAT_UNDEFINED && !info(format).compressed && u32(dfd,at+4))fail("unsupported KTX2 DFD scalar sample positions");
  }
}
Ktx2Header ktx2Header(Bytes bytes,const TextureDecodeOptions& o) {
  Ktx2Header header;auto& im=header.image;
  im.format=static_cast<VkFormat>(u32(bytes,12));im.w=u32(bytes,20);im.h=u32(bytes,24);im.levels=std::max(1u,u32(bytes,40));header.super=u32(bytes,44);
  dimensions(im.w,im.h,im.levels,o);
  if(u32(bytes,28)||u32(bytes,32)||u32(bytes,36)!=1) fail("KTX2 material image must be a single 2D layer");
  const uint64_t indexEnd=add(80,mul(im.levels,24));slice(bytes,0,indexEnd);
  struct Range{uint64_t offset,size;};std::vector<Range> ranges{{0,indexEnd}};
  auto range=[&](uint64_t offset,uint64_t size){if(size){slice(bytes,offset,size);if(offset<indexEnd)fail("KTX2 section overlaps header");ranges.push_back({offset,size});}};
  const auto dfdOffset=u32(bytes,48),dfdSize=u32(bytes,52),kvOffset=u32(bytes,56),kvSize=u32(bytes,60);
  const auto sgdOffset=u64(bytes,64),sgdSize=u64(bytes,72);
  if((kvSize?kvOffset%4:kvOffset!=0)||(sgdSize?sgdOffset%8:sgdOffset!=0))fail("invalid KTX2 metadata offset/alignment");
  if(header.super!=1 && (sgdSize||sgdOffset))fail("unexpected KTX2 supercompression global data");
  range(dfdOffset,dfdSize);range(kvOffset,kvSize);range(sgdOffset,sgdSize);
  if(dfdSize<28 || (dfdOffset%4)) fail("missing or unaligned KTX2 DFD");
  const auto dfd=slice(bytes,dfdOffset,dfdSize);
  if(u32(dfd,0)!=dfdSize || u32(dfd,4)!=0 || (u32(dfd,8)&0xffff)!=2 || (u32(dfd,8)>>16)<24 || (u32(dfd,8)>>16)>dfdSize-4) fail("invalid KTX2 basic DFD");
  header.dfdModel=std::to_integer<uint32_t>(dfd[12]);header.primaries=std::to_integer<uint32_t>(dfd[13]);header.transfer=std::to_integer<uint32_t>(dfd[14]);header.flags=std::to_integer<uint32_t>(dfd[15]);
  if(header.flags) fail("premultiplied alpha or unknown KTX2 DFD flags; provide straight alpha");
  if(header.primaries>1||header.transfer>2)fail("KTX2 uses unsupported color primaries/transfer; convert to linear or sRGB BT709");
  validateDfd(dfd,im.format,header.dfdModel,header.super!=0);
  metadata(slice(bytes,kvOffset,kvSize),false,false,o.strictKhrBasisu,im);
  for(uint32_t i=0;i<im.levels;++i) {
    const auto at=80+uint64_t(i)*24;const auto offset=u64(bytes,at),length=u64(bytes,at+8),raw=u64(bytes,at+16);
    const uint32_t alignment=header.super==0?std::lcm(im.format==VK_FORMAT_UNDEFINED?16u:info(im.format).bytes,4u):1;
    if(offset%alignment)fail("unaligned KTX2 mip payload");
    if(!length) fail("empty KTX2 mip payload");range(offset,length);header.offsets.push_back(offset);header.lengths.push_back(length);header.rawLengths.push_back(raw);
  }
  std::sort(ranges.begin(),ranges.end(),[](auto a,auto b){return a.offset<b.offset;});
  for(size_t i=1;i<ranges.size();++i) if(ranges[i].offset<add(ranges[i-1].offset,ranges[i-1].size)) fail("overlapping KTX2 sections");
  if(o.strictKhrBasisu) {
    if(im.format!=VK_FORMAT_UNDEFINED || im.w%4 || im.h%4) fail("KHR_texture_basisu requires undefined VkFormat and dimensions divisible by four");
    if(!((header.dfdModel==163 && header.super==1)||(header.dfdModel==166 && (header.super==0||header.super==2)))) fail("KHR_texture_basisu requires ETC1S/BasisLZ or UASTC/none/Zstd");
    if(header.transfer!=(o.isSrgb?2u:1u) || header.primaries!=(o.isSrgb?1u:0u)) fail("KHR_texture_basisu DFD transfer/primaries disagree with material usage");
  }
  return header;
}
TextureImageData ktx2(Bytes bytes,const TextureDecodeOptions& o) {
  auto header=ktx2Header(bytes,o);auto& image=header.image;
  const bool basis=image.format==VK_FORMAT_UNDEFINED;
  if(basis && u32(bytes,16)!=1) fail("Basis KTX2 typeSize must be one");
  if(!u32(bytes,40) && (basis||info(image.format).compressed)) fail("block-compressed KTX2 levelCount must be nonzero");
  if(!basis) {
    const auto expectedType=info(image.format).compressed?1u:
      (image.format>=VK_FORMAT_R16_UNORM && image.format<=VK_FORMAT_R16G16B16A16_SFLOAT)?2u:
      (image.format>=VK_FORMAT_R32_UINT && image.format<=VK_FORMAT_R32G32B32A32_SFLOAT)?4u:1u;
    if(u32(bytes,16)!=expectedType) fail("KTX2 typeSize disagrees with VkFormat");
    if(header.super==1)fail("BasisLZ KTX2 must have undefined VkFormat");
  } else {
    if((header.dfdModel==163 && header.super!=1)||(header.dfdModel==166 && header.super!=0&&header.super!=2&&header.super!=3))fail("Basis KTX2 color model and supercompression scheme disagree");
    if(header.dfdModel==163) for(auto n:header.rawLengths)if(n)fail("BasisLZ uncompressedByteLength must be zero");
  }
  if(!basis && header.super==0) {
    for(uint32_t i=0;i<image.levels;++i) {
      const auto expected=TextureMipByteSize(image.format,std::max(1u,image.w>>i),std::max(1u,image.h>>i));
      if(header.lengths[i]!=expected || header.rawLengths[i]!=expected) fail("KTX2 mip length disagrees with VkFormat");
      image.payload.push_back(slice(bytes,header.offsets[i],header.lengths[i]));
    }
    return materialize(image,o);
  }
#if CONTAINER_HAS_KTX
  if(header.super>3) fail("unsupported KTX2 supercompression scheme");
  if(basis && header.dfdModel!=163 && header.dfdModel!=166) fail("KTX2 undefined VkFormat is not ETC1S or UASTC");
  VkFormat target=image.format;
  ktx_transcode_fmt_e transcode=KTX_TTF_RGBA32;
  if(basis) {
    target=roleFormat(VK_FORMAT_R8G8B8A8_UNORM,o.isSrgb);
    // Avoid lossy BC1/BC3 recompression of UASTC. BC7/ASTC and ETC2 are
    // explicit device capability choices; absent those use RGBA32.
    const std::array<std::pair<VkFormat,ktx_transcode_fmt_e>,4> candidates{{
      {roleFormat(VK_FORMAT_BC7_UNORM_BLOCK,o.isSrgb),KTX_TTF_BC7_RGBA},
      {roleFormat(VK_FORMAT_ASTC_4x4_UNORM_BLOCK,o.isSrgb),KTX_TTF_ASTC_4x4_RGBA},
      {roleFormat(VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK,o.isSrgb),KTX_TTF_ETC2_RGBA},
      {roleFormat(VK_FORMAT_BC3_UNORM_BLOCK,o.isSrgb),KTX_TTF_BC3_RGBA}}};
    for(auto [format,kind]:candidates) if(!image.flipY && !o.allowedFormats.empty() && allowed(format,o) &&
        !(header.dfdModel==166 && (kind==KTX_TTF_ETC2_RGBA||kind==KTX_TTF_BC3_RGBA))) {target=format;transcode=kind;break;}
  }
  // Preflight the final allocation and the inflated authored image data before
  // passing the bitstream to libktx; its internal transcoder scratch is separate.
  const auto finalFormat=outputFormat(target,o,image.flipY);
  chainBytes(finalFormat,image.w,image.h,image.levels,o);requireAllowed(finalFormat,o);
  if(!basis) for(uint32_t i=0;i<image.levels;++i) {
    const auto expected=TextureMipByteSize(image.format,std::max(1u,image.w>>i),std::max(1u,image.h>>i));
    if(header.rawLengths[i]!=expected) fail("KTX2 inflated mip length disagrees with VkFormat");
  }
  if(basis && header.dfdModel==166) for(uint32_t i=0;i<image.levels;++i) {
    const auto expected=TextureMipByteSize(VK_FORMAT_ASTC_4x4_UNORM_BLOCK,std::max(1u,image.w>>i),std::max(1u,image.h>>i));
    if(header.rawLengths[i]!=expected || (header.super==0 && header.lengths[i]!=expected)) fail("UASTC mip length disagrees with its 4x4 blocks");
  }
  ktxTexture* raw{};
  auto status=ktxTexture_CreateFromMemory(reinterpret_cast<const ktx_uint8_t*>(bytes.data()),bytes.size(),KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,&raw);
  if(status!=KTX_SUCCESS) fail(std::string("libktx container decode failed: ")+ktxErrorString(status));
  std::unique_ptr<ktxTexture,void(*)(ktxTexture*)> texture(raw,[](ktxTexture* p){ktxTexture_Destroy(p);});
  if(raw->baseWidth!=image.w||raw->baseHeight!=image.h||raw->baseDepth!=1||raw->numDimensions!=2||raw->numLevels!=image.levels||raw->numLayers!=1||raw->numFaces!=1||raw->isArray) fail("libktx decoded shape disagrees with validated header");
  auto* tex2=reinterpret_cast<ktxTexture2*>(raw);
  if(basis) {
    if(!ktxTexture2_NeedsTranscoding(tex2)) fail("undefined KTX2 VkFormat does not need Basis transcoding");
    status=ktxTexture2_TranscodeBasis(tex2,transcode,0);
    if(status!=KTX_SUCCESS) fail(std::string("libktx Basis transcode failed: ")+ktxErrorString(status));
    image.format=target;
  }
  const auto data=Bytes(reinterpret_cast<const std::byte*>(ktxTexture_GetData(raw)),ktxTexture_GetDataSize(raw));
  for(uint32_t i=0;i<image.levels;++i) {
    ktx_size_t offset{};status=ktxTexture_GetImageOffset(raw,i,0,0,&offset);
    if(status!=KTX_SUCCESS) fail("libktx mip offset query failed");
    const auto size=TextureMipByteSize(image.format,std::max(1u,image.w>>i),std::max(1u,image.h>>i));
    if(ktxTexture_GetImageSize(raw,i)!=size) fail("libktx mip size disagrees with the target format");
    image.payload.push_back(slice(data,offset,size));
  }
  return materialize(image,o);
#else
  fail("KTX2 BasisLZ/UASTC or Zstd/Zlib requires ENABLE_TEXTURE_KTX; configure libktx or explicitly enable CONTAINER_FETCH_TEXTURE_CODECS");
#endif
}
TextureImageData webp(Bytes bytes,const TextureDecodeOptions& o) {
#if CONTAINER_HAS_WEBP
  WebPBitstreamFeatures features{};
  const auto* p=reinterpret_cast<const uint8_t*>(bytes.data());
  if(WebPGetFeatures(p,bytes.size(),&features)!=VP8_STATUS_OK) fail("malformed WebP header");
  if(features.has_animation) fail("animated WebP is not a 2D material image");
  const uint32_t w=uint32_t(features.width),h=uint32_t(features.height);
  const auto f=roleFormat(VK_FORMAT_R8G8B8A8_UNORM,o.isSrgb);requireAllowed(f,o);
  auto size=chainBytes(f,w,h,1,o);TextureMipLevel mip{w,h,std::vector<std::byte>(hostSize(size))};
  if(w>uint32_t(INT_MAX/4) || !WebPDecodeRGBAInto(p,bytes.size(),reinterpret_cast<uint8_t*>(mip.bytes.data()),mip.bytes.size(),int(w*4))) fail("WebP pixel decode failed");
  return {f,{VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY},{std::move(mip)},false};
#else
  (void)bytes;(void)o;fail("WebP requires ENABLE_TEXTURE_WEBP; configure libwebp or explicitly enable CONTAINER_FETCH_TEXTURE_CODECS");
#endif
}
} // namespace

bool HasKtxTextureCodec() noexcept {return CONTAINER_HAS_KTX!=0;}
bool HasWebpTextureCodec() noexcept {return CONTAINER_HAS_WEBP!=0;}
uint64_t TextureMipByteSize(VkFormat format,uint32_t width,uint32_t height) {
  if(!width||!height) fail("zero-sized mip");
  const auto f=info(format);
  return mul(mul((uint64_t(width)+f.x-1)/f.x,(uint64_t(height)+f.y-1)/f.y),f.bytes);
}
TextureImageData DecodeTextureImage(Bytes bytes,const TextureDecodeOptions& o) {
  if(!o.maxEncodedBytes||!o.maxDecodedBytes||!o.maxDimension||!o.maxMipLevels) fail("decode limits must all be finite and nonzero");
  if(bytes.empty()||bytes.size()>o.maxEncodedBytes) fail("encoded image exceeds maxEncodedBytes or is empty");
  constexpr std::array<unsigned char,12> k1{0xab,'K','T','X',' ','1','1',0xbb,13,10,26,10};
  constexpr std::array<unsigned char,12> k2{0xab,'K','T','X',' ','2','0',0xbb,13,10,26,10};
  const bool isKtx2=starts(bytes,k2);
  if(o.strictKhrBasisu && !isKtx2) fail("KHR_texture_basisu requires a KTX2 image");
  if(isKtx2) return ktx2(bytes,o);
  if(starts(bytes,k1)) return ktx1(bytes,o);
  constexpr std::array<unsigned char,4> ddsMagic{'D','D','S',' '};
  if(starts(bytes,ddsMagic)) return dds(bytes,o);
  if(bytes.size()>=12 && u32(bytes,0)==fourcc('R','I','F','F')&&u32(bytes,8)==fourcc('W','E','B','P')) return webp(bytes,o);
  return ordinary(bytes,o);
}
} // namespace container::material

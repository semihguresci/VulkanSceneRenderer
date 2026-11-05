#include <Container/utility/TextureImageData.h>
#include <gtest/gtest.h>
#include <stb_image_write.h>

#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

#ifndef CONTAINER_HAS_WEBP
#define CONTAINER_HAS_WEBP 0
#endif

namespace {
using namespace container::material;
using Buffer=std::vector<std::byte>;
void le32(Buffer& b,uint32_t value) { for(uint32_t i=0;i<4;++i)b.push_back(std::byte(value>>(8*i))); }
void be32(Buffer& b,uint32_t value) { for(uint32_t i=0;i<4;++i)b.push_back(std::byte(value>>(24-8*i))); }
void set32(Buffer& b,size_t at,uint32_t value) { for(uint32_t i=0;i<4;++i)b[at+i]=std::byte(value>>(8*i)); }
void set64(Buffer& b,size_t at,uint64_t value) {set32(b,at,uint32_t(value));set32(b,at+4,uint32_t(value>>32));}
void append(Buffer& a,const Buffer& b) {a.insert(a.end(),b.begin(),b.end());}
uint32_t crc(std::span<const std::byte> bytes) {
  uint32_t c=~0u;
  for(auto v:bytes){c^=std::to_integer<uint32_t>(v);for(uint32_t i=0;i<8;++i)c=(c>>1)^(0xedb88320u&uint32_t(-int32_t(c&1)));}
  return ~c;
}
void pngChunk(Buffer& png,const char* kind,const Buffer& data) {
  be32(png,uint32_t(data.size()));size_t at=png.size();
  for(uint32_t i=0;i<4;++i)png.push_back(std::byte(kind[i]));append(png,data);be32(png,crc(std::span(png).subspan(at)));
}
Buffer png16(uint32_t channels=1) {
  Buffer result{std::byte{137},std::byte{'P'},std::byte{'N'},std::byte{'G'},std::byte{13},std::byte{10},std::byte{26},std::byte{10}};
  Buffer ihdr;be32(ihdr,2);be32(ihdr,1);ihdr.push_back(std::byte{16});ihdr.push_back(std::byte(channels==1?0:6));ihdr.resize(13);
  pngChunk(result,"IHDR",ihdr);
  Buffer row{std::byte{0}};
  const std::array<uint16_t,8> pixels{0x1234,0x8001,0xfedc,0x4567,0x1235,0x8002,0xfedd,0x4568};
  if(channels==1) {for(auto v:{0x1234,0x1235}) {row.push_back(std::byte(v>>8));row.push_back(std::byte(v));}}
  else for(auto v:pixels){row.push_back(std::byte(v>>8));row.push_back(std::byte(v));}
  Buffer z{std::byte{0x78},std::byte{1},std::byte{1}};const auto n=uint32_t(row.size());
  z.push_back(std::byte(n));z.push_back(std::byte(n>>8));z.push_back(std::byte(~n));z.push_back(std::byte((~n)>>8));append(z,row);
  uint32_t s1=1,s2=0;for(auto b:row){s1=(s1+std::to_integer<uint32_t>(b))%65521;s2=(s2+s1)%65521;}be32(z,(s2<<16)|s1);
  pngChunk(result,"IDAT",z);pngChunk(result,"IEND",{});return result;
}
void stbWrite(void* context,void* data,int size) {
  auto& b=*static_cast<Buffer*>(context);auto* p=static_cast<std::byte*>(data);b.insert(b.end(),p,p+size);
}
Buffer png8() {Buffer b;const std::array<unsigned char,8> pixels{17,33,65,129,19,35,67,131};EXPECT_NE(stbi_write_png_to_func(stbWrite,&b,2,1,4,pixels.data(),8),0);return b;}
Buffer hdr() {Buffer b;const std::array<float,3> pixels{8,2,0.5};EXPECT_NE(stbi_write_hdr_to_func(stbWrite,&b,1,1,3,pixels.data()),0);return b;}
Buffer ddsBc1(bool mips=true) {
  Buffer b(128);set32(b,0,0x20534444);set32(b,4,124);set32(b,8,0x81007);set32(b,12,4);set32(b,16,4);set32(b,20,8);set32(b,28,mips?2:1);
  set32(b,76,32);set32(b,80,4);set32(b,84,0x31545844);set32(b,108,0x1000);
  // Entire first level red, second green. The partial pyramid ends at 2x2.
  for(uint32_t level=0;level<(mips?2u:1u);++level){const uint16_t c=level?0x07e0:0xf800;b.push_back(std::byte(c));b.push_back(std::byte(c>>8));b.insert(b.end(),6,std::byte{0});}
  return b;
}
Buffer ddsDx10(uint32_t format,uint32_t w,uint32_t h,Buffer payload) {
  Buffer b(148);set32(b,0,0x20534444);set32(b,4,124);set32(b,12,h);set32(b,16,w);set32(b,28,1);set32(b,76,32);set32(b,80,4);set32(b,84,0x30315844);set32(b,128,format);set32(b,132,3);set32(b,140,1);append(b,payload);return b;
}
Buffer kv(std::string key,std::string value) {
  Buffer b;le32(b,uint32_t(key.size()+value.size()+2));for(char c:key)b.push_back(std::byte(c));b.push_back(std::byte{0});for(char c:value)b.push_back(std::byte(c));b.push_back(std::byte{0});while(b.size()%4)b.push_back(std::byte{0});return b;
}
Buffer ktx1(std::string orientation="S=r,T=d",std::string swizzle="rgba") {
  Buffer b;for(unsigned char v:std::array<unsigned char,12>{0xab,'K','T','X',' ','1','1',0xbb,13,10,26,10})b.push_back(std::byte(v));
  for(uint32_t v:{0x04030201u,0x1401u,1u,0x1903u,0x8229u,0x1903u,3u,2u,0u,0u,1u,2u,0u})le32(b,v);
  auto metadata=kv("KTXorientation",orientation);append(metadata,kv("KTXswizzle",swizzle));set32(b,60,uint32_t(metadata.size()));append(b,metadata);
  le32(b,8);for(uint8_t v:{1,2,3,0,4,5,6,0})b.push_back(std::byte(v));le32(b,4);for(uint8_t v:{9,0,0,0})b.push_back(std::byte(v));return b;
}
Buffer ktx2(bool basis=false) {
  Buffer b;for(unsigned char v:std::array<unsigned char,12>{0xab,'K','T','X',' ','2','0',0xbb,13,10,26,10})b.push_back(std::byte(v));
  b.resize(80+24);set32(b,12,basis?0:VK_FORMAT_R8G8B8A8_UNORM);set32(b,16,1);set32(b,20,4);set32(b,24,4);set32(b,36,1);set32(b,40,1);
  const uint32_t samples=basis?1:4,dfdSize=28+16*samples;
  set32(b,48,104);set32(b,52,dfdSize);b.resize(104+dfdSize);set32(b,104,dfdSize);set32(b,112,((dfdSize-4)<<16)|2);b[116]=std::byte(basis?166:1);b[117]=std::byte{1};b[118]=std::byte{2};
  b[120]=b[121]=std::byte(basis?3:0);b[124]=std::byte(basis?16:4);
  for(uint32_t i=0;i<samples;++i) {const auto at=132+i*16;set32(b,at,(basis?0:i*8)|((basis?127:7)<<16)|((i==3?15:i)<<24));set32(b,at+12,basis?UINT32_MAX:255);}
  while(b.size()%(basis?16:4))b.push_back(std::byte{0});const auto offset=b.size();
  set64(b,80,offset);set64(b,88,basis?16:64);set64(b,96,basis?16:64);b.resize(offset+(basis?16:64),std::byte{127});return b;
}
Buffer ktx2Typed(VkFormat format,uint32_t scalarBytes,uint32_t channels,Buffer payload,bool floating=false) {
  auto b=ktx2();set32(b,12,format);set32(b,16,scalarBytes);set32(b,20,2);set32(b,24,1);
  const uint32_t dfdSize=28+16*channels;set32(b,52,dfdSize);b.resize(104+dfdSize);set32(b,104,dfdSize);set32(b,112,((dfdSize-4)<<16)|2);b[118]=std::byte{1};b[124]=std::byte(channels*scalarBytes);
  for(uint32_t i=0;i<channels;++i){const auto at=132+i*16;set32(b,at,(i*scalarBytes*8)|((scalarBytes*8-1)<<16)|(((i==3?15:i)|(floating?0xc0:0))<<24));set32(b,at+4,0);set32(b,at+8,0);set32(b,at+12,0);}
  const auto alignment=std::lcm(channels*scalarBytes,4u);while(b.size()%alignment)b.push_back(std::byte{0});set64(b,80,b.size());set64(b,88,payload.size());set64(b,96,payload.size());append(b,payload);return b;
}
Buffer ktx2Metadata(Buffer b,Buffer metadata) {
  uint64_t oldOffset{};std::memcpy(&oldOffset,b.data()+80,8);Buffer payload(b.begin()+oldOffset,b.end());b.resize(size_t(oldOffset));set32(b,56,uint32_t(b.size()));set32(b,60,uint32_t(metadata.size()));append(b,metadata);while(b.size()%16)b.push_back(std::byte{0});set64(b,80,b.size());append(b,payload);return b;
}
Buffer words(std::initializer_list<uint16_t> values) {Buffer b;for(auto v:values){b.push_back(std::byte(v));b.push_back(std::byte(v>>8));}return b;}
void alphaBlock(Buffer& block,uint8_t a,uint8_t b,uint32_t index) {
  block.push_back(std::byte(a));block.push_back(std::byte(b));uint64_t bits{};for(uint32_t i=0;i<16;++i)bits|=uint64_t(index)<<(i*3);for(uint32_t i=0;i<6;++i)block.push_back(std::byte(bits>>(i*8)));
}
float valueFloat(const Buffer& b,size_t index) {float v;std::memcpy(&v,b.data()+index*sizeof(v),sizeof(v));return v;}
uint16_t value16(const Buffer& b,size_t index) {uint16_t v;std::memcpy(&v,b.data()+index*sizeof(v),sizeof(v));return v;}
float eotf(float v) {return v<=.04045f?v/12.92f:std::pow((v+.055f)/1.055f,2.4f);}

TEST(TextureImageData, OrdinaryPngPreservesAlphaAndSelectsRole) {
  auto b=png8();TextureDecodeOptions o;o.isSrgb=true;auto image=DecodeTextureImage(b,o);
  EXPECT_EQ(image.format,VK_FORMAT_R8G8B8A8_SRGB);ASSERT_EQ(image.mips.size(),1u);EXPECT_FALSE(image.authoredMipChain);
  EXPECT_EQ(image.mips[0].bytes[3],std::byte{129});EXPECT_EQ(image.mips[0].bytes[7],std::byte{131});
  o.isSrgb=false;EXPECT_EQ(DecodeTextureImage(b,o).format,VK_FORMAT_R8G8B8A8_UNORM);
}
TEST(TextureImageData, Png16LinearGrayKeepsAdjacentSixteenBitValues) {
  auto image=DecodeTextureImage(png16());EXPECT_EQ(image.format,VK_FORMAT_R16_UNORM);EXPECT_EQ(image.componentMapping.g,VK_COMPONENT_SWIZZLE_R);EXPECT_EQ(image.componentMapping.a,VK_COMPONENT_SWIZZLE_ONE);
  ASSERT_EQ(image.mips[0].bytes.size(),4u);EXPECT_EQ(value16(image.mips[0].bytes,0),0x1234);EXPECT_EQ(value16(image.mips[0].bytes,1),0x1235);
}
TEST(TextureImageData, Png16SrgbUsesLinearFloatAndKeepsAlphaLinear) {
  TextureDecodeOptions o;o.isSrgb=true;auto image=DecodeTextureImage(png16(4),o);EXPECT_EQ(image.format,VK_FORMAT_R32G32B32A32_SFLOAT);
  const auto& bytes=image.mips[0].bytes;EXPECT_NEAR(valueFloat(bytes,0),eotf(0x1234/65535.f),1e-7f);EXPECT_NEAR(valueFloat(bytes,3),0x4567/65535.f,1e-7f);
  EXPECT_GT(valueFloat(bytes,4),valueFloat(bytes,0));EXPECT_GT(valueFloat(bytes,7),valueFloat(bytes,3));
  o.allowedFormats={VK_FORMAT_R8G8B8A8_SRGB};
  EXPECT_THROW((void)DecodeTextureImage(png16(4),o),std::runtime_error);
}
TEST(TextureImageData, HdrKeepsValuesAboveOne) {
  auto image=DecodeTextureImage(hdr());EXPECT_EQ(image.format,VK_FORMAT_R32G32B32A32_SFLOAT);EXPECT_FLOAT_EQ(valueFloat(image.mips[0].bytes,0),8.f);EXPECT_FLOAT_EQ(valueFloat(image.mips[0].bytes,3),1.f);
}
TEST(TextureImageData, DdsNativeRequiresExplicitFormatAndPreservesPartialMips) {
  auto b=ddsBc1();TextureDecodeOptions o;o.isSrgb=true;o.allowedFormats={VK_FORMAT_BC1_RGBA_SRGB_BLOCK};auto image=DecodeTextureImage(b,o);
  EXPECT_EQ(image.format,VK_FORMAT_BC1_RGBA_SRGB_BLOCK);EXPECT_TRUE(image.authoredMipChain);ASSERT_EQ(image.mips.size(),2u);EXPECT_EQ(image.mips[1].width,2u);EXPECT_EQ(image.mips[1].bytes.size(),8u);
  o.allowedFormats.clear();image=DecodeTextureImage(b,o);EXPECT_EQ(image.format,VK_FORMAT_R8G8B8A8_SRGB);EXPECT_EQ(image.mips[0].bytes[0],std::byte{255});EXPECT_EQ(image.mips[0].bytes[1],std::byte{0});EXPECT_EQ(image.mips[1].bytes[1],std::byte{255});EXPECT_EQ(image.mips[1].bytes[3],std::byte{255});
}
TEST(TextureImageData, Bc1TransparentCodeRetainsAlphaAndClipsPartialBlocks) {
  auto b=ddsBc1(false);set32(b,12,3);set32(b,16,2);b[128]=b[129]=std::byte{0};b[130]=std::byte{0xff};b[131]=std::byte{0xff};for(size_t i=132;i<136;++i)b[i]=std::byte{0xff};
  auto image=DecodeTextureImage(b);ASSERT_EQ(image.mips[0].bytes.size(),24u);for(size_t i=3;i<24;i+=4)EXPECT_EQ(image.mips[0].bytes[i],std::byte{0});
}
TEST(TextureImageData, Ktx1RemovesRowPaddingPreservesMipsAndAppliesUpwardOrientation) {
  auto image=DecodeTextureImage(ktx1("S=r,T=u","rrr1"));EXPECT_EQ(image.format,VK_FORMAT_R8_UNORM);ASSERT_EQ(image.mips.size(),2u);EXPECT_EQ(image.mips[0].bytes,(Buffer{std::byte{4},std::byte{5},std::byte{6},std::byte{1},std::byte{2},std::byte{3}}));EXPECT_EQ(image.componentMapping.b,VK_COMPONENT_SWIZZLE_R);EXPECT_EQ(image.mips[1].bytes,(Buffer{std::byte{9}}));
}
TEST(TextureImageData, Ktx2NativeRgbaAndLegalSwizzleDoNotChangeMipBytes) {
  auto b=ktx2Metadata(ktx2(),kv("KTXswizzle","bgra"));auto image=DecodeTextureImage(b);EXPECT_EQ(image.format,VK_FORMAT_R8G8B8A8_UNORM);EXPECT_TRUE(image.authoredMipChain);ASSERT_EQ(image.mips[0].bytes.size(),64u);EXPECT_EQ(image.mips[0].bytes[0],std::byte{127});EXPECT_EQ(image.componentMapping.r,VK_COMPONENT_SWIZZLE_B);EXPECT_EQ(image.componentMapping.b,VK_COMPONENT_SWIZZLE_R);
}
TEST(TextureImageData, Bc3AndBc5FallbackKeepsInterpolationAlphaAndDataChannels) {
  Buffer payload;alphaBlock(payload,255,0,2);append(payload,words({0xf800,0x07e0,0xffff,0xffff}));
  auto image=DecodeTextureImage(ddsDx10(77,4,4,payload));const auto& rgba=image.mips[0].bytes;
  EXPECT_EQ(rgba[0],std::byte{85});EXPECT_EQ(rgba[1],std::byte{170});EXPECT_EQ(rgba[2],std::byte{0});EXPECT_EQ(rgba[3],std::byte{218});
  payload.clear();alphaBlock(payload,0,255,2);alphaBlock(payload,255,0,3);image=DecodeTextureImage(ddsDx10(83,4,4,payload));
  EXPECT_EQ(image.mips[0].bytes[0],std::byte{51});EXPECT_EQ(image.mips[0].bytes[1],std::byte{182});EXPECT_EQ(image.mips[0].bytes[2],std::byte{0});EXPECT_EQ(image.mips[0].bytes[3],std::byte{255});
}
TEST(TextureImageData, DdsUnsupportedNativeBlocksHaveActionableFallbackError) {
  auto b=ddsDx10(98,4,4,Buffer(16));EXPECT_THROW((void)DecodeTextureImage(b),std::runtime_error);
  TextureDecodeOptions o;o.isSrgb=true;o.allowedFormats={VK_FORMAT_BC7_SRGB_BLOCK};auto image=DecodeTextureImage(b,o);EXPECT_EQ(image.format,VK_FORMAT_BC7_SRGB_BLOCK);EXPECT_EQ(image.mips[0].bytes.size(),16u);
  o.allowedFormats={VK_FORMAT_R8G8B8A8_SRGB};EXPECT_THROW((void)DecodeTextureImage(b,o),std::runtime_error);
}
TEST(TextureImageData, Ktx1BigEndianSixteenBitWordsAreSwappedExactly) {
  Buffer b;for(unsigned char v:std::array<unsigned char,12>{0xab,'K','T','X',' ','1','1',0xbb,13,10,26,10})b.push_back(std::byte(v));
  for(uint32_t v:{0x04030201u,0x1403u,2u,0x1903u,0x822au,0x1903u,2u,1u,0u,0u,1u,1u,0u})be32(b,v);be32(b,4);append(b,Buffer{std::byte{0x12},std::byte{0x34},std::byte{0x12},std::byte{0x35}});
  auto image=DecodeTextureImage(b);EXPECT_EQ(value16(image.mips[0].bytes,0),0x1234);EXPECT_EQ(value16(image.mips[0].bytes,1),0x1235);
}
TEST(TextureImageData, ContainerSrgb16AndProfileFallbackPreservePrecision) {
  const auto payload=words({0x1234,0x8001,0xfedc,0x4567,0x1235,0x8002,0xfedd,0x4568});auto b=ddsDx10(11,2,1,payload);
  TextureDecodeOptions o;o.isSrgb=true;auto image=DecodeTextureImage(b,o);EXPECT_EQ(image.format,VK_FORMAT_R32G32B32A32_SFLOAT);EXPECT_NEAR(valueFloat(image.mips[0].bytes,0),eotf(0x1234/65535.f),1e-7f);EXPECT_NEAR(valueFloat(image.mips[0].bytes,3),0x4567/65535.f,1e-7f);
  o.isSrgb=false;o.allowedFormats={VK_FORMAT_R32G32B32A32_SFLOAT};image=DecodeTextureImage(b,o);EXPECT_NEAR(valueFloat(image.mips[0].bytes,0),0x1234/65535.f,1e-7f);EXPECT_GT(valueFloat(image.mips[0].bytes,4),valueFloat(image.mips[0].bytes,0));
  o.maxDecodedBytes=31;EXPECT_THROW((void)DecodeTextureImage(b,o),std::runtime_error);
}
TEST(TextureImageData, KtxNarrowFloatExpandsToFilterableRgbaWithoutReducingPrecision) {
  Buffer pixels(8);const std::array<float,2> values{2.125f,2.125001f};std::memcpy(pixels.data(),values.data(),8);
  auto b=ktx2Typed(VK_FORMAT_R32_SFLOAT,4,1,pixels,true);TextureDecodeOptions o;o.allowedFormats={VK_FORMAT_R32G32B32A32_SFLOAT};auto image=DecodeTextureImage(b,o);
  EXPECT_EQ(image.format,VK_FORMAT_R32G32B32A32_SFLOAT);EXPECT_FLOAT_EQ(valueFloat(image.mips[0].bytes,0),values[0]);EXPECT_FLOAT_EQ(valueFloat(image.mips[0].bytes,4),values[1]);EXPECT_FLOAT_EQ(valueFloat(image.mips[0].bytes,3),1.f);
  pixels=words({0x4000,0x4001});b=ktx2Typed(VK_FORMAT_R16_SFLOAT,2,1,pixels,true);image=DecodeTextureImage(b,o);EXPECT_FLOAT_EQ(valueFloat(image.mips[0].bytes,0),2.f);EXPECT_FLOAT_EQ(valueFloat(image.mips[0].bytes,4),2.f+1.f/512.f);
  pixels=words({0x7c00,0x4001});b=ktx2Typed(VK_FORMAT_R16_SFLOAT,2,1,pixels,true);EXPECT_THROW((void)DecodeTextureImage(b,o),std::runtime_error);
}
TEST(TextureImageData, AstcNativeRolePairIsExplicitAndNeverSilentlyDecompressed) {
  Buffer b;for(unsigned char v:std::array<unsigned char,12>{0xab,'K','T','X',' ','1','1',0xbb,13,10,26,10})b.push_back(std::byte(v));
  for(uint32_t v:{0x04030201u,0u,1u,0u,0x93b0u,0x1908u,4u,4u,0u,0u,1u,1u,0u})le32(b,v);le32(b,16);b.resize(b.size()+16);
  TextureDecodeOptions o;o.isSrgb=true;o.allowedFormats={VK_FORMAT_ASTC_4x4_SRGB_BLOCK};EXPECT_EQ(DecodeTextureImage(b,o).format,VK_FORMAT_ASTC_4x4_SRGB_BLOCK);
  o.isSrgb=false;o.allowedFormats={VK_FORMAT_ASTC_4x4_UNORM_BLOCK};EXPECT_EQ(DecodeTextureImage(b,o).format,VK_FORMAT_ASTC_4x4_UNORM_BLOCK);
  EXPECT_THROW((void)DecodeTextureImage(b),std::runtime_error);
}
TEST(TextureImageData, Bc5SrgbRoleCannotUseUnormNativeSampling) {
  Buffer payload;alphaBlock(payload,0,255,2);alphaBlock(payload,255,0,3);auto b=ddsDx10(83,4,4,payload);
  TextureDecodeOptions o;o.isSrgb=true;o.allowedFormats={VK_FORMAT_BC5_UNORM_BLOCK,VK_FORMAT_R8G8B8A8_SRGB};auto image=DecodeTextureImage(b,o);EXPECT_EQ(image.format,VK_FORMAT_R8G8B8A8_SRGB);EXPECT_EQ(image.mips[0].bytes[0],std::byte{51});
  o.allowedFormats={VK_FORMAT_BC5_UNORM_BLOCK};EXPECT_THROW((void)DecodeTextureImage(b,o),std::runtime_error);
}
TEST(TextureImageData, DdsPitchIsRemovedAndBgrxAlphaRemainsOneThroughViewMapping) {
  auto b=ddsBc1(false);b.resize(128);set32(b,8,8);set32(b,12,2);set32(b,16,1);set32(b,20,8);set32(b,80,0x40);set32(b,84,0);set32(b,88,32);set32(b,92,0xff0000);set32(b,96,0xff00);set32(b,100,0xff);set32(b,104,0);
  append(b,Buffer{std::byte{1},std::byte{2},std::byte{3},std::byte{0},std::byte{99},std::byte{99},std::byte{99},std::byte{99},std::byte{4},std::byte{5},std::byte{6},std::byte{0},std::byte{99},std::byte{99},std::byte{99},std::byte{99}});
  TextureDecodeOptions o;o.allowedFormats={VK_FORMAT_R8G8B8A8_UNORM};auto image=DecodeTextureImage(b,o);ASSERT_EQ(image.mips[0].bytes.size(),8u);EXPECT_EQ(image.mips[0].bytes[0],std::byte{3});EXPECT_EQ(image.mips[0].bytes[4],std::byte{6});EXPECT_EQ(image.componentMapping.a,VK_COMPONENT_SWIZZLE_ONE);
}
TEST(TextureImageData, Ktx2RawPartialMipsRetainIndependentAuthoredColors) {
  auto original=ktx2();Buffer b(original.begin(),original.begin()+80);b.resize(128);set32(b,40,2);set32(b,48,128);b.insert(b.end(),original.begin()+104,original.begin()+196);
  set64(b,80,236);set64(b,88,64);set64(b,96,64);set64(b,104,220);set64(b,112,16);set64(b,120,16);b.insert(b.end(),16,std::byte{31});b.insert(b.end(),64,std::byte{17});
  auto image=DecodeTextureImage(b);ASSERT_EQ(image.mips.size(),2u);EXPECT_EQ(image.mips[0].bytes.front(),std::byte{17});EXPECT_EQ(image.mips[1].bytes.front(),std::byte{31});EXPECT_EQ(image.mips[1].width,2u);EXPECT_TRUE(image.authoredMipChain);
}
TEST(TextureImageData, ContradictoryDfdAndUnsupportedColorMetadataFail) {
  auto b=ktx2();b[124]=std::byte{8};EXPECT_THROW((void)DecodeTextureImage(b),std::runtime_error);
  b=ktx2();b[116]=std::byte{161};EXPECT_THROW((void)DecodeTextureImage(b),std::runtime_error);
  b=ktx2();b[135]=std::byte{2};EXPECT_THROW((void)DecodeTextureImage(b),std::runtime_error);
  b=ktx2();b[117]=std::byte{2};EXPECT_THROW((void)DecodeTextureImage(b),std::runtime_error);
  b=ktx2();b[118]=std::byte{16};EXPECT_THROW((void)DecodeTextureImage(b),std::runtime_error);
  b=ktx2(true);b[135]=std::byte{7};EXPECT_THROW((void)DecodeTextureImage(b),std::runtime_error);
}
TEST(TextureImageData, FiniteLimitsRejectBeforeOutputAllocation) {
  auto b=png8();TextureDecodeOptions o;o.maxEncodedBytes=b.size()-1;
  EXPECT_THROW((void)DecodeTextureImage(b,o),std::runtime_error);
  o={};o.maxDecodedBytes=7;
  EXPECT_THROW((void)DecodeTextureImage(b,o),std::runtime_error);
  o={};o.maxDimension=1;
  EXPECT_THROW((void)DecodeTextureImage(b,o),std::runtime_error);
  o={};o.maxMipLevels=1;
  EXPECT_THROW((void)DecodeTextureImage(ddsBc1(),o),std::runtime_error);
  o={};o.maxDecodedBytes=0;
  EXPECT_THROW((void)DecodeTextureImage(b,o),std::runtime_error);
  EXPECT_THROW((void)TextureMipByteSize(VK_FORMAT_R32G32B32A32_SFLOAT,UINT32_MAX,UINT32_MAX),std::runtime_error);
  EXPECT_EQ(TextureMipByteSize(VK_FORMAT_BC7_UNORM_BLOCK,5,7),64u);
}
TEST(TextureImageData, MalformedContainerRangesAndUnsupportedShapesFail) {
  auto d=ddsBc1();d.pop_back();
  EXPECT_THROW((void)DecodeTextureImage(d),std::runtime_error);d=ddsBc1();set32(d,112,0x200);
  EXPECT_THROW((void)DecodeTextureImage(d),std::runtime_error);
  auto k=ktx2();set64(k,80,104);
  EXPECT_THROW((void)DecodeTextureImage(k),std::runtime_error);k=ktx2();set64(k,88,UINT64_MAX);
  EXPECT_THROW((void)DecodeTextureImage(k),std::runtime_error);
  k=ktx2();set32(k,32,1);
  EXPECT_THROW((void)DecodeTextureImage(k),std::runtime_error);k=ktx2();set32(k,16,4);
  EXPECT_THROW((void)DecodeTextureImage(k),std::runtime_error);
  EXPECT_THROW((void)DecodeTextureImage(ktx1("S=l,T=d")),std::runtime_error);
  EXPECT_THROW((void)DecodeTextureImage(ktx1("S=r,T=d","rgbx")),std::runtime_error);
}
TEST(TextureImageData, StrictBasisuRejectsWrongContainerFormatDimensionsAndDfdRoles) {
  TextureDecodeOptions o;o.strictKhrBasisu=true;o.isSrgb=true;
  EXPECT_THROW((void)DecodeTextureImage(png8(),o),std::runtime_error);
  EXPECT_THROW((void)DecodeTextureImage(ktx2(),o),std::runtime_error);
  auto b=ktx2(true);b[117]=std::byte{0};
  EXPECT_THROW((void)DecodeTextureImage(b,o),std::runtime_error);
  b=ktx2(true);b[118]=std::byte{1};
  EXPECT_THROW((void)DecodeTextureImage(b,o),std::runtime_error);
  b=ktx2(true);set32(b,20,3);
  EXPECT_THROW((void)DecodeTextureImage(b,o),std::runtime_error);
  o.isSrgb=false;b=ktx2(true);b[118]=std::byte{1};
  EXPECT_THROW((void)DecodeTextureImage(b,o),std::runtime_error);
}

#if CONTAINER_HAS_WEBP
TEST(TextureImageData, WebpLosslessKeepsAlphaAndRoleWithDecodedLimit) {
  // Encoded once by official WebP1.6.0 from RGBA
  // {(17,29,53,0),(91,103,127,149)}. No encoder dependency in the test target.
  constexpr std::array<uint8_t,40> fixture{0x52,0x49,0x46,0x46,0x20,0x00,0x00,0x00,0x57,0x45,0x42,0x50,0x56,0x50,0x38,0x4c,0x13,0x00,0x00,0x00,0x2f,0x01,0x00,0x00,0x10,0x0f,0x30,0x67,0xb3,0x35,0x7f,0x53,0x19,0xf0,0xa8,0x40,0x44,0xff,0x03,0x00};
  Buffer b(fixture.size());std::memcpy(b.data(),fixture.data(),fixture.size());
  TextureDecodeOptions o;o.isSrgb=true;auto image=DecodeTextureImage(b,o);EXPECT_EQ(image.format,VK_FORMAT_R8G8B8A8_SRGB);EXPECT_EQ(image.mips[0].bytes[7],std::byte{149});EXPECT_EQ(image.mips[0].bytes[4],std::byte{91});
  o.maxDecodedBytes=7;
  EXPECT_THROW((void)DecodeTextureImage(b,o),std::runtime_error);
}
#endif
TEST(TextureImageData, AnimatedWebpOrDisabledCodecIsRejectedExplicitly) {
  Buffer b(30);set32(b,0,0x46464952);set32(b,4,22);set32(b,8,0x50424557);set32(b,12,0x58385056);set32(b,16,10);b[20]=std::byte{2};b[24]=std::byte{1};
  try {(void)DecodeTextureImage(b);FAIL()<<"animated material image unexpectedly decoded";}
  catch(const std::runtime_error& e){EXPECT_NE(std::string(e.what()).find(HasWebpTextureCodec()?"animated":"ENABLE_TEXTURE_WEBP"),std::string::npos);}
}
TEST(TextureImageData, DisabledKtxCodecReportsRequiredBuildOption) {
  if(!HasKtxTextureCodec()) {
    try{(void)DecodeTextureImage(ktx2(true));FAIL()<<"Basis decoded with codec disabled";}
    catch(const std::runtime_error& e){EXPECT_NE(std::string(e.what()).find("ENABLE_TEXTURE_KTX"),std::string::npos);}
  }
}

Buffer upstream(const char* name) {
  const auto directory=std::filesystem::path(__FILE__).parent_path().parent_path()/"fixtures"/"texture-codecs";
  std::ifstream stream(directory/name,std::ios::binary);if(!stream)throw std::runtime_error("missing bundled upstream test fixture");
  std::vector<char> chars{std::istreambuf_iterator<char>(stream),{}};Buffer bytes(chars.size());std::memcpy(bytes.data(),chars.data(),chars.size());return bytes;
}
TEST(TextureImageDataOptional, OfficialBasisEtc1sAndUastcZstdActuallyDecode) {
  if(!HasKtxTextureCodec())GTEST_SKIP();
  for(const auto* name:{"cyan_rgb_reference_basis.ktx2","cyan_rgb_reference_uastc.ktx2"}) {
    TextureDecodeOptions o;o.isSrgb=true;o.strictKhrBasisu=true;auto image=DecodeTextureImage(upstream(name),o);EXPECT_EQ(image.format,VK_FORMAT_R8G8B8A8_SRGB);ASSERT_FALSE(image.mips.empty());
    const auto& p=image.mips[0].bytes;EXPECT_LT(std::to_integer<int>(p[0]),8);EXPECT_GT(std::to_integer<int>(p[1]),247);EXPECT_GT(std::to_integer<int>(p[2]),247);EXPECT_EQ(p[3],std::byte{255});
  }
  auto image=DecodeTextureImage(upstream("color_grid_uastc_zstd.ktx2"));EXPECT_EQ(image.format,VK_FORMAT_R8G8B8A8_UNORM);ASSERT_GT(image.mips[0].bytes.size(),100u);
  auto old=upstream("color_grid_uastc_zstd.ktx2");uint32_t dfdOffset{};std::memcpy(&dfdOffset,old.data()+48,4);for(uint32_t i=20;i<28;++i)old[dfdOffset+i]=std::byte{0};
  EXPECT_EQ(DecodeTextureImage(old).mips[0].bytes,image.mips[0].bytes);
}
TEST(TextureImageDataOptional, OfficialBasisNativeBc7AndAuthoredZstdMips) {
  if(!HasKtxTextureCodec())GTEST_SKIP();
  TextureDecodeOptions o;o.isSrgb=true;o.allowedFormats={VK_FORMAT_BC7_SRGB_BLOCK};auto image=DecodeTextureImage(upstream("cyan_rgb_reference_uastc.ktx2"),o);EXPECT_EQ(image.format,VK_FORMAT_BC7_SRGB_BLOCK);EXPECT_EQ(image.mips[0].bytes.size(),TextureMipByteSize(image.format,image.mips[0].width,image.mips[0].height));
  o={};image=DecodeTextureImage(upstream("color_grid_zstd.ktx2"),o);EXPECT_EQ(image.format,VK_FORMAT_R8G8B8A8_UNORM);EXPECT_TRUE(image.authoredMipChain);
  image=DecodeTextureImage(upstream("rgba-mipmap-reference-basis.ktx2"),o);ASSERT_EQ(image.mips.size(),7u);EXPECT_EQ(image.mips[0].width,64u);EXPECT_EQ(image.mips[6].width,1u);EXPECT_EQ(image.mips[6].bytes.size(),4u);
  o.maxDecodedBytes=4;
  EXPECT_THROW((void)DecodeTextureImage(upstream("color_grid_uastc_zstd.ktx2"),o),std::runtime_error);
}
} // namespace

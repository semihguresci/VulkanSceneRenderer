#include <Container/utility/SourceMaterialMapping.h>

#include <gtest/gtest.h>

#include <array>
#include <limits>

namespace {
using namespace container::scene;

TEST(GltfMaterialMapping, NoncontiguousSourceIdsPreserveInterleavedRuntimeMaterials) {
  // Runtime 0/1 are default/diagnostic, 4 is manually created between imports.
  const std::array<uint32_t, 4> mapping{2, 3, 5, 7};
  EXPECT_NO_THROW(ValidateSourceMaterialMapping(mapping, 4, 8));
  EXPECT_EQ(ResolveSourceMaterialIndex(0, mapping, 4, 8, 0), 2u);
  EXPECT_EQ(ResolveSourceMaterialIndex(2, mapping, 4, 8, 0), 5u);
  EXPECT_EQ(ResolveSourceMaterialIndex(3, mapping, 4, 8, 0), 7u);
  EXPECT_EQ(ExportSourceMaterialMapping(mapping, 4, 8), (std::vector<int32_t>{2, 3, 5, 7}));
}

TEST(GltfMaterialMapping, ImplicitDefaultNeverAliasesSourceMaterialZero) {
  const std::array<uint32_t, 2> mapping{4, 6};
  EXPECT_EQ(ResolveSourceMaterialIndex(-1, mapping, 2, 7, 2), 2u);
  EXPECT_EQ(ResolveSourceMaterialIndex(0, mapping, 2, 7, 2), 4u);
  EXPECT_EQ(ResolveSourceMaterialIndex(-1, {}, 0, 3, 2), 2u);
}

TEST(GltfMaterialMapping, MissingOrInvalidBindingsAreRejectedInsteadOfFallingBack) {
  const std::array<uint32_t, 2> mapping{2, 5};
  EXPECT_THROW(ValidateSourceMaterialMapping(mapping, 3, 6), std::invalid_argument);
  EXPECT_THROW(ResolveSourceMaterialIndex(0, mapping, 3, 6, 0), std::invalid_argument);
  EXPECT_THROW(ResolveSourceMaterialIndex(-2, mapping, 2, 6, 0), std::invalid_argument);
  EXPECT_THROW(ResolveSourceMaterialIndex(2, mapping, 2, 6, 0), std::invalid_argument);
  EXPECT_THROW(ResolveSourceMaterialIndex(-1, mapping, 2, 6, 6), std::invalid_argument);
  EXPECT_THROW(ValidateSourceMaterialMapping(mapping, 2, 5), std::invalid_argument);
  EXPECT_THROW(ExportSourceMaterialMapping(mapping, 2, 5), std::invalid_argument);
}

TEST(GltfMaterialMapping, UnrepresentableRuntimeIdsAreRejectedBeforeExportNarrowing) {
  const std::array<uint32_t, 1> mapping{std::numeric_limits<uint32_t>::max()};
  const size_t runtimeCount = size_t(std::numeric_limits<uint32_t>::max()) + size_t{1};
  EXPECT_THROW(ValidateSourceMaterialMapping(mapping, 1, runtimeCount), std::invalid_argument);
  EXPECT_THROW(ResolveSourceMaterialIndex(0, mapping, 1, runtimeCount, 0), std::invalid_argument);
  EXPECT_THROW(ExportSourceMaterialMapping(mapping, 1, runtimeCount), std::invalid_argument);
}

TEST(GltfMaterialMapping, InitialAppendCapturesExistingIdsAndValidatesIntervalBeforeAllocation) {
  EXPECT_EQ(MakeContiguousSourceMaterialMapping(4, 3, 7), (std::vector<uint32_t>{4, 5, 6}));
  EXPECT_TRUE(MakeContiguousSourceMaterialMapping(7, 0, 7).empty());
  EXPECT_THROW((void)MakeContiguousSourceMaterialMapping(4, 4, 7), std::invalid_argument);
  EXPECT_THROW((void)MakeContiguousSourceMaterialMapping(8, 0, 7), std::invalid_argument);
  EXPECT_THROW((void)MakeContiguousSourceMaterialMapping(std::numeric_limits<size_t>::max(), 2,
      std::numeric_limits<size_t>::max()), std::invalid_argument);
}
} // namespace

#include "Container/renderer/effects/ScreenReflectionSettings.h"

#include <gtest/gtest.h>

namespace {

using namespace container::renderer;

TEST(ScreenReflectionSettingsTests, QualityModesBoundTraversalAndUseHistory) {
  EXPECT_EQ(screenReflectionStepLimit(ScreenReflectionMode::Off), 0u);
  EXPECT_EQ(screenReflectionStepLimit(ScreenReflectionMode::Low), 24u);
  EXPECT_EQ(screenReflectionStepLimit(ScreenReflectionMode::High), 72u);
  EXPECT_EQ(screenReflectionDebugView(ScreenReflectionMode::Low), 0u);
  EXPECT_EQ(screenReflectionDebugView(ScreenReflectionMode::High), 0u);
  EXPECT_FALSE(screenReflectionModeUsesHistory(ScreenReflectionMode::Off));
  EXPECT_TRUE(screenReflectionModeUsesHistory(ScreenReflectionMode::Low));
  EXPECT_TRUE(screenReflectionModeUsesHistory(ScreenReflectionMode::High));
}

TEST(ScreenReflectionSettingsTests, TraceDiagnosticsUseShaderModesWithoutHistory) {
  struct Diagnostic {
    ScreenReflectionMode mode;
    uint32_t shaderMode;
    bool usesHistory;
    bool tracesScreenOnly;
  };
  constexpr Diagnostic diagnostics[] = {
      {ScreenReflectionMode::Confidence, 1, false, false},
      {ScreenReflectionMode::Distance, 2, false, false},
      {ScreenReflectionMode::Source, 3, false, false},
      {ScreenReflectionMode::History, 4, true, false},
      {ScreenReflectionMode::HitUv, 5, false, true},
      {ScreenReflectionMode::MissReason, 6, false, true},
  };
  for (const auto& diagnostic : diagnostics) {
    SCOPED_TRACE(diagnostic.shaderMode);
    EXPECT_EQ(screenReflectionDebugView(diagnostic.mode), diagnostic.shaderMode);
    EXPECT_EQ(screenReflectionStepLimit(diagnostic.mode), 72u);
    EXPECT_EQ(screenReflectionModeUsesHistory(diagnostic.mode), diagnostic.usesHistory);
    EXPECT_EQ(screenReflectionModeIsTraceDiagnostic(diagnostic.mode), diagnostic.tracesScreenOnly);
  }
}

} // namespace

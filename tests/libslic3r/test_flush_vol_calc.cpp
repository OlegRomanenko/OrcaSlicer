// Закрепляет пороги материала поддержки во внутренней матрице до применения коэффициента.
#include <catch2/catch_test_macros.hpp>

#include "libslic3r/FlushVolCalc.hpp"

// Проверяет обе общие константы в мм³, используемые расчётами GUI и CLI.
TEST_CASE("Материал поддержки использует пороги продувки 800 мм³ в обоих направлениях",
          "[FlushVolCalc][Regression]")
{
    CHECK(Slic3r::g_flush_volume_to_support == 800);
    CHECK(Slic3r::g_min_flush_volume_from_support == 800);
}

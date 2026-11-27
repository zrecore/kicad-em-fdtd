// BDD scenarios for the viewer's CPU-side pieces: phasor accumulation, colour map and PNG output.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <fstream>
#include <numbers>

#include "viz/a11y.hpp"
#include "viz/colormap.hpp"
#include "viz/phasor.hpp"
#include "viz/png.hpp"
#include "viz/slice.hpp"
#include "viz/theme.hpp"
#include "viz/triangulate.hpp"

using Catch::Approx;

SCENARIO("A phasor accumulator recovers amplitude and phase and reconstructs the animation") {
  GIVEN("a field A cos(w t + p) sampled for a whole number of periods, accumulated at two frequencies") {
    constexpr double f0 = 2.4e9, dt = 1.0 / (f0 * 64), amp = 0.7, phase = 0.9;
    constexpr int periods = 40, steps = 64 * periods;
    viz::PhasorAccumulator acc({f0, 1.7 * f0}, 1);
    for (int n = 0; n < steps; ++n) {
      const double t = n * dt;
      const float v = static_cast<float>(amp * std::cos(2 * std::numbers::pi * f0 * t + phase));
      acc.add(t, dt, std::span<const float>(&v, 1));
    }
    const double total = steps * dt;
    WHEN("the phasor at the drive frequency is normalised by T/2") {
      const auto x = acc.phasor(0, 0) * static_cast<float>(2.0 / total);
      THEN("its magnitude is the amplitude and its angle is the phase, within 1e-3") {
        REQUIRE(std::abs(x) == Approx(amp).epsilon(1e-3));
        REQUIRE(std::arg(x) == Approx(phase).margin(1e-3));
      }
      AND_THEN("the other frequency holds almost nothing (under 1 percent of the amplitude)") {
        REQUIRE(std::abs(acc.phasor(1, 0)) * 2.0 / total < 0.01 * amp);
      }
    }
    WHEN("the steady-state animation is evaluated over one period") {
      THEN("it follows A cos(phi + p) within 1e-3") {
        for (int i = 0; i < 8; ++i) {
          const double phi = 2 * std::numbers::pi * i / 8;
          REQUIRE(acc.steady(0, 0, phi) * 2.0 / total == Approx(amp * std::cos(phi + phase)).margin(1e-3));
        }
      }
    }
  }
}

SCENARIO("The diverging colour map carries sign by hue and magnitude by distance from neutral") {
  THEN("zero is the dark neutral, positive is warm, negative is cool, and values clamp at +-1") {
    const auto z = viz::diverging(0.f), p = viz::diverging(1.f), n = viz::diverging(-1.f);
    REQUIRE(z.r == 20);
    REQUIRE(p.r > p.b);
    REQUIRE(n.b > n.r);
    REQUIRE(viz::diverging(5.f).r == p.r);
    REQUIRE(viz::diverging(0.5f).r > z.r);
    REQUIRE(viz::diverging(0.5f).r < p.r);
  }
}

SCENARIO("A PNG file is written with a valid signature, header and size") {
  GIVEN("a 3 x 2 image") {
    const std::vector<std::uint8_t> rgb(3 * 2 * 3, 128);
    const std::string path = "viz_test.png";
    viz::writePng(path, 3, 2, rgb);
    std::ifstream f(path, std::ios::binary);
    std::vector<unsigned char> head(33);
    f.read(reinterpret_cast<char*>(head.data()), 33);
    THEN("it starts with the PNG signature and an IHDR of 3 x 2") {
      REQUIRE(head[1] == 'P');
      REQUIRE(head[12] == 'I');
      REQUIRE(head[19] == 3);
      REQUIRE(head[23] == 2);
    }
    std::remove(path.c_str());
  }
}

SCENARIO("A field plane is extracted from the grid and rendered with the right orientation and colours") {
  GIVEN("a 4 x 3 x 2 cell grid whose field is i + 10 j + 100 k") {
    const auto grid = fdtd::Grid::uniform({4, 3, 2}, {4, 3, 2});
    std::vector<float> field(grid.nodeCount());
    for (std::size_t k = 0; k < grid.nodes(fdtd::Z); ++k)
      for (std::size_t j = 0; j < grid.nodes(fdtd::Y); ++j)
        for (std::size_t i = 0; i < grid.nodes(fdtd::X); ++i)
          field[grid.nodeIndex(i, j, k)] = static_cast<float>(i + 10 * j + 100 * k);
    const auto plane = viz::makePlane(grid, fdtd::Z, 1);
    THEN("the z = 1 plane holds i + 10 j + 100, u (x) fastest") {
      const auto values = viz::extractPlane(grid, plane, field);
      REQUIRE(values.size() == 5 * 4);
      REQUIRE(values[0] == 100.f);
      REQUIRE(values[3] == 103.f);
      REQUIRE(values[2 * 5 + 4] == 124.f);
    }
    WHEN("a field that is negative for x < 2 and positive for x > 2 is rendered, with one masked node") {
      std::vector<float> step(plane.nu * plane.nv);
      for (std::size_t j = 0; j < plane.nv; ++j)
        for (std::size_t i = 0; i < plane.nu; ++i)
          step[i + plane.nu * j] = i < 2 ? -1.f : 1.f;
      viz::PhasorAccumulator acc({1e9}, step.size());
      acc.add(0.0, 1.0, step);
      std::vector<std::uint8_t> mask(step.size(), 0);
      mask[4 + plane.nu * 3] = 1; // top-right node (x = 4, y = 3)
      std::uint32_t w = 0, h = 0;
      const auto rgb = viz::renderFrame(grid, plane, acc, 0, 0.0, {.pixelsPerMetre = 4}, mask, w, h);
      THEN("the image is 16 x 12 with cool colours on the left and warm on the right") {
        REQUIRE(w == 16);
        REQUIRE(h == 12);
        const auto px = [&](std::uint32_t x, std::uint32_t y) { return &rgb[(y * w + x) * 3]; };
        REQUIRE(px(1, 6)[2] > px(1, 6)[0]);
        REQUIRE(px(12, 6)[0] > px(12, 6)[2]);
      }
      AND_THEN("the masked node is drawn grey in the top-right corner (row 0 is the largest y)") {
        const auto* p = &rgb[(0 * w + (w - 1)) * 3];
        REQUIRE(p[0] == 110);
        REQUIRE(p[1] == 110);
      }
    }
  }
}

SCENARIO("A frame can be fitted to a window of any size") {
  GIVEN("a 4 x 3 plane and a 100 x 100 pixel canvas") {
    const auto grid = fdtd::Grid::uniform({4, 3, 2}, {4, 3, 2});
    const auto plane = viz::makePlane(grid, fdtd::Z, 1);
    std::vector<float> ones(plane.nu * plane.nv, 1.f);
    viz::PhasorAccumulator acc({1e9}, ones.size());
    acc.add(0.0, 1.0, ones);
    std::uint32_t w = 0, h = 0;
    const auto rgb =
        viz::renderFrame(grid, plane, acc, 0, 0.0, {.canvasWidth = 100, .canvasHeight = 100}, {}, w, h);
    THEN("the frame is exactly the canvas size, the plane is centred, and the letterbox is the background") {
      REQUIRE(w == 100);
      REQUIRE(h == 100);
      REQUIRE(rgb[(0 * 100 + 50) * 3] == 20);  // top-centre: above the 100 x 75 plane
      REQUIRE(rgb[(50 * 100 + 50) * 3] > 100); // centre: inside the plane, warm (+1)
    }
  }
}

#include "imgui.h"
#include "viz/imgui_soft.hpp"

SCENARIO("UI draw data is rasterised on the CPU, clipped and blended") {
  GIVEN("an ImGui context and a 40 x 30 frame filled with dark grey") {
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().DisplaySize = {40, 30};
    ImGui::GetIO().Fonts->Build();
    const viz::SoftImGui renderer;
    std::vector<std::uint8_t> frame(40 * 30 * 4, 40);
    WHEN("an opaque red rectangle (10,5)-(20,15) and a half-transparent blue one (25,5)-(35,15) are drawn") {
      ImGui::NewFrame();
      auto* dl = ImGui::GetBackgroundDrawList();
      dl->AddRectFilled({10, 5}, {20, 15}, IM_COL32(255, 0, 0, 255));
      dl->AddRectFilled({25, 5}, {35, 15}, IM_COL32(0, 0, 255, 128));
      ImGui::Render();
      renderer.render(ImGui::GetDrawData(), frame.data(), 40, 30);
      auto px = [&](int x, int y) { return &frame[(y * 40 + x) * 4]; };
      THEN("the opaque rectangle is pure red inside and untouched outside") {
        REQUIRE(px(15, 10)[0] == 255);
        REQUIRE(px(15, 10)[1] == 0);
        REQUIRE(px(5, 10)[0] == 40);
        REQUIRE(px(15, 20)[0] == 40);
      }
      THEN("the translucent rectangle blends with the background (blue up, red down)") {
        REQUIRE(px(30, 10)[2] > 100);
        REQUIRE(px(30, 10)[2] < 200);
        REQUIRE(px(30, 10)[0] < 40);
      }
    }
    ImGui::DestroyContext(ctx);
  }
}

SCENARIO("The dark diverging map is a colour-blind-safe blue/yellow pair, near-black at zero") {
  auto luma = [](viz::Rgb c) { return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b; };
  THEN("zero is near black, negative is blue-ish (blue well above red), positive yellow-ish (red and green "
       "well "
       "above blue)") {
    const auto z = viz::divergingDark(0.f), n = viz::divergingDark(-1.f), p = viz::divergingDark(1.f);
    REQUIRE(luma(z) < 20);
    REQUIRE(n.b > n.r + 60);
    REQUIRE(n.g > n.r + 40);
    REQUIRE(p.r > p.b + 80);
    REQUIRE(p.g > p.b + 80);
  }
  THEN("strong fields are brighter than weak ones on both sides") {
    REQUIRE(luma(viz::divergingDark(0.9f)) > luma(viz::divergingDark(0.3f)));
    REQUIRE(luma(viz::divergingDark(-0.9f)) > luma(viz::divergingDark(-0.3f)));
  }
  THEN("sign also differs in lightness (the colour-vision cue): positive yellow is at least 8 % lighter") {
    REQUIRE(luma(viz::divergingDark(1.f)) > 1.08 * luma(viz::divergingDark(-1.f)));
  }
}

SCENARIO("The theme follows the system in Auto mode and can be forced") {
  THEN("setting text maps to the right mode, unknown text falls back to Auto") {
    REQUIRE(viz::parseThemeMode("light") == viz::ThemeMode::Light);
    REQUIRE(viz::parseThemeMode("dark") == viz::ThemeMode::Dark);
    REQUIRE(viz::parseThemeMode("auto") == viz::ThemeMode::Auto);
    REQUIRE(viz::parseThemeMode("???") == viz::ThemeMode::Auto);
  }
  THEN("Auto uses whatever the system reports, Light and Dark ignore it") {
    REQUIRE(viz::useDark(viz::ThemeMode::Auto, true));
    REQUIRE_FALSE(viz::useDark(viz::ThemeMode::Auto, false));
    REQUIRE_FALSE(viz::useDark(viz::ThemeMode::Light, true));
    REQUIRE(viz::useDark(viz::ThemeMode::Dark, false));
  }
}

SCENARIO("Field colours can be configured and fall back to the theme defaults") {
  THEN("the light map reaches the configured colours at full strength and is white at zero") {
    const auto z = viz::divergingLight(0.f);
    REQUIRE(z.r > 245);
    REQUIRE(z.b > 245);
    const auto p = viz::divergingLight(1.f, 1.0f, {0, 0, 255}, {0, 255, 0});
    REQUIRE(p.g == 255);
    REQUIRE(p.r == 0);
    const auto n = viz::divergingLight(-1.f, 1.0f, {0, 0, 255}, {0, 255, 0});
    REQUIRE(n.b == 255);
  }
  THEN("the light defaults are blue for negative and red for positive") {
    const auto n = viz::divergingLight(-1.f, 1.0f), p = viz::divergingLight(1.f, 1.0f);
    REQUIRE(n.b > n.r + 60);
    REQUIRE(p.r > p.b + 60);
  }
  THEN("the dark map uses the configured colour on each side") {
    const auto n = viz::divergingDark(-1.f, 1.0f, {255, 140, 0}, {120, 255, 80});
    REQUIRE(n.r > n.b + 80); // orange-ish negative
    const auto p = viz::divergingDark(1.f, 1.0f, {255, 140, 0}, {120, 255, 80});
    REQUIRE(p.g > p.b + 80); // green-ish positive
  }
}

SCENARIO("A zoomed or panned slice view shows only the chosen window and leaves outside the domain blank") {
  const auto grid = fdtd::Grid::uniform({20, 20, 4}, {1e-3, 1e-3, 1e-3});
  viz::Plane plane = viz::makePlane(grid, fdtd::Z, 2);
  viz::PhasorAccumulator acc({1e9}, plane.nu * plane.nv);
  GIVEN("a view panned half a window beyond the domain edge") {
    viz::FrameSpec spec;
    spec.canvasWidth = spec.canvasHeight = 40;
    spec.cropU0 = -10e-3;
    spec.cropU1 = 10e-3; // left half outside the 20 mm domain
    spec.cropV0 = 0;
    spec.cropV1 = 20e-3;
    spec.colourScale = 1;
    std::uint32_t w = 0, h = 0;
    const auto rgb = viz::renderFrame(grid, plane, acc, 0, 0.0, spec, {}, w, h);
    THEN("the outside is the same flat background, and the image has the requested size") {
      REQUIRE(w == 40);
      REQUIRE(h == 40);
      REQUIRE(rgb[(20 * 40 + 2) * 3] == rgb[(20 * 40 + 5) * 3]);
      REQUIRE(rgb[(20 * 40 + 2) * 3 + 2] == rgb[(5 * 40 + 1) * 3 + 2]);
    }
  }
}

SCENARIO(
    "Accessibility preferences resolve from the user's choice and the system, and contrast is measurable") {
  THEN("Follow takes the system value; On and Off override it") {
    REQUIRE(viz::resolve(viz::parseTri("follow"), true));
    REQUIRE_FALSE(viz::resolve(viz::parseTri("follow"), false));
    REQUIRE(viz::resolve(viz::parseTri("on"), false));
    REQUIRE_FALSE(viz::resolve(viz::parseTri("off"), true));
    REQUIRE(viz::parseTri("nonsense") == viz::Tri::Follow);
  }
  THEN("the system query returns without crashing") {
    (void)viz::systemAccess();
  }
  THEN("WCAG contrast: black on white is 21, identical colours 1, and mid grey on white is below AA") {
    REQUIRE(viz::contrastRatio({0, 0, 0}, {255, 255, 255}) == Catch::Approx(21.0).epsilon(1e-3));
    REQUIRE(viz::contrastRatio({90, 90, 90}, {90, 90, 90}) == Catch::Approx(1.0));
    REQUIRE(viz::contrastRatio({170, 170, 170}, {255, 255, 255}) < 4.5);
  }
}

#include "viz/access.hpp"

SCENARIO("The accessibility tree names every control and keeps ids stable between frames") {
  using namespace viz::access;
  GIVEN("a frame with a button, a repeated label and a focused slider") {
    auto build = [](Tree& t) {
      t.clear();
      t.add({.role = Role::Button, .name = "Run simulation"});
      t.add({.role = Role::Label, .name = "Ready"});
      t.add({.role = Role::Label, .name = "Ready"});
      t.add({.role = Role::Slider, .name = "Brightness", .value = "0.5", .focused = true});
    };
    Tree a, b;
    build(a);
    build(b);
    THEN("the ids do not change from one frame to the next, and repeats get their own") {
      REQUIRE(a == b);
      REQUIRE(a.nodes()[1].id != a.nodes()[2].id);
      REQUIRE(a.problems().empty());
    }
    THEN("the focus is the slider and a changed value makes the trees differ") {
      REQUIRE(a.focus() == a.nodes()[3].id);
      b.clear();
      b.add({.role = Role::Slider, .name = "Brightness", .value = "0.9", .focused = true});
      REQUIRE_FALSE(a == b);
    }
  }
  GIVEN("a control without a name") {
    Tree t;
    t.add({.role = Role::Button, .x = 3, .y = 4});
    t.add({.role = Role::Label}); // text may be empty; a control may not
    THEN("it is reported, so no control is announced as just \"button\"") {
      REQUIRE(t.problems().size() == 1);
      REQUIRE(t.problems()[0].find("button") == 0);
    }
  }
}

#include "viz/a11y.hpp"

SCENARIO("The text size follows the system unless the user picks one, within safe limits") {
  using viz::resolveTextScale;
  GIVEN("a system text scale and a user choice") {
    THEN("follow keeps the system scale, a percentage replaces it, nonsense falls back, limits clamp") {
      REQUIRE(resolveTextScale("follow", 1.0) == Catch::Approx(1.0));
      REQUIRE(resolveTextScale("follow", 1.4) == Catch::Approx(1.4));
      REQUIRE(resolveTextScale("150%", 1.0) == Catch::Approx(1.5));
      REQUIRE(resolveTextScale("nonsense", 1.25) == Catch::Approx(1.25));
      REQUIRE(resolveTextScale("follow", 9.0) == Catch::Approx(3.0)); // clamped
      REQUIRE(resolveTextScale("10%", 1.0) == Catch::Approx(0.75));
    }
  }
}

SCENARIO("UI draw data respects the clip rectangle on a high-density (Retina) display") {
  GIVEN("a 2x framebuffer scale and a clip rectangle of (10,10)-(20,20) points") {
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().DisplaySize = {40, 30};
    ImGui::GetIO().DisplayFramebufferScale = {2, 2};
    ImGui::GetIO().Fonts->Build();
    const viz::SoftImGui renderer;
    std::vector<std::uint8_t> frame(80 * 60 * 4, 40);
    WHEN("a rectangle covering the whole display is drawn inside that clip") {
      ImGui::NewFrame();
      auto* dl = ImGui::GetBackgroundDrawList();
      dl->PushClipRect({10, 10}, {20, 20});
      dl->AddRectFilled({0, 0}, {40, 30}, IM_COL32(255, 0, 0, 255));
      dl->PopClipRect();
      ImGui::Render();
      renderer.render(ImGui::GetDrawData(), frame.data(), 80, 60);
      auto px = [&](int x, int y) { return frame[(static_cast<std::size_t>(y) * 80 + x) * 4]; };
      THEN("only the pixels inside the clip, in pixel units, are painted") {
        REQUIRE(px(30, 30) == 255);
        REQUIRE(px(15, 30) == 40); // left of 20 px
        REQUIRE(px(30, 15) == 40); // above 20 px
        REQUIRE(px(45, 30) == 40); // right of 40 px
        REQUIRE(px(30, 45) == 40); // below 40 px
      }
    }
    ImGui::DestroyContext(ctx);
  }
}

SCENARIO("Copper outlines are triangulated into fills that cover exactly their area") {
  using Pt = std::array<double, 2>;
  auto area = [](const std::vector<Pt>& p, const std::vector<std::array<std::size_t, 3>>& tris) {
    double a = 0;
    for (const auto& t : tris)
      a += 0.5 * std::abs((p[t[1]][0] - p[t[0]][0]) * (p[t[2]][1] - p[t[0]][1]) -
                          (p[t[2]][0] - p[t[0]][0]) * (p[t[1]][1] - p[t[0]][1]));
    return a;
  };
  GIVEN("a concave L shape of area 3") {
    const std::vector<Pt> l{{0, 0}, {2, 0}, {2, 1}, {1, 1}, {1, 2}, {0, 2}};
    THEN("the triangles cover exactly 3, in either winding") {
      const auto t = viz::triangulate(l);
      REQUIRE(t.size() == 4);
      REQUIRE(area(l, t) == Catch::Approx(3.0));
      std::vector<Pt> r(l.rbegin(), l.rend());
      REQUIRE(area(r, viz::triangulate(r)) == Catch::Approx(3.0));
    }
  }
  GIVEN("a ring with a repeated closing point, a duplicate point and a collinear point") {
    const std::vector<Pt> p{{0, 0}, {1, 0}, {2, 0}, {2, 2}, {2, 2}, {0, 2}, {0, 0}};
    THEN("it still covers its area of 4") {
      REQUIRE(area(p, viz::triangulate(p)) == Catch::Approx(4.0));
    }
  }
  GIVEN("a degenerate ring or a self-crossing bow tie") {
    THEN("no fill is produced rather than a wrong one") {
      REQUIRE(viz::triangulate({{0, 0}, {1, 1}, {2, 2}}).empty());
      REQUIRE(viz::triangulate({{0, 0}, {1, 1}}).empty());
      const std::vector<Pt> bow{{0, 0}, {2, 2}, {2, 0}, {0, 2}};
      const auto t = viz::triangulate(bow);
      REQUIRE((t.empty() || area(bow, t) <= 2.0 + 1e-9)); // never more than the bow tie's own area
    }
  }
}

SCENARIO("A draw list with more than 65535 vertices still draws every shape where it belongs") {
  GIVEN("a frame with 20000 small rectangles (80000 vertices) and one large green one drawn last") {
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().DisplaySize = {40, 30};
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset; // what the app sets
    ImGui::GetIO().Fonts->Build();
    const viz::SoftImGui renderer;
    std::vector<std::uint8_t> frame(40 * 30 * 4, 40);
    ImGui::NewFrame();
    auto* dl = ImGui::GetBackgroundDrawList();
    REQUIRE((dl->Flags & ImDrawListFlags_AllowVtxOffset) != 0);
    for (int i = 0; i < 20000; ++i)
      dl->AddRectFilled({1, 1}, {3, 3}, IM_COL32(40, 40, 40, 255));
    dl->AddRectFilled({10, 10}, {30, 20}, IM_COL32(0, 255, 0, 255));
    ImGui::Render();
    renderer.render(ImGui::GetDrawData(), frame.data(), 40, 30);
    THEN("the last rectangle is exactly where it was asked for and nothing else changed") {
      auto g = [&](int x, int y) { return frame[(static_cast<std::size_t>(y) * 40 + x) * 4 + 1]; };
      REQUIRE(g(20, 15) >= 250);
      REQUIRE(g(5, 25) == 40);
      REQUIRE(g(35, 5) == 40);
    }
    ImGui::DestroyContext(ctx);
  }
}

#include "viz/poynting.hpp"

SCENARIO("The time-averaged Poynting vector of known fields") {
  constexpr double kEta = 376.730313668; // free-space impedance, ohm
  using C = std::complex<double>;
  GIVEN("a plane wave travelling along +z: E = 1 V/m along x, H = 1/eta A/m along y") {
    const viz::Phasor3 e{C(1, 0), 0, 0}, h{0, C(1 / kEta, 0), 0};
    THEN("S points along +z with magnitude 1 / (2 eta), within 1e-12 relative") {
      const auto s = viz::poynting(e, h);
      REQUIRE(s[0] == 0.0);
      REQUIRE(s[1] == 0.0);
      REQUIRE(s[2] == Catch::Approx(1 / (2 * kEta)).epsilon(1e-12));
    }
    AND_THEN("a common phase on E and H changes nothing") {
      const C rot = std::polar(1.0, 0.9);
      const auto s = viz::poynting({e[0] * rot, 0, 0}, {0, h[1] * rot, 0});
      REQUIRE(s[2] == Catch::Approx(1 / (2 * kEta)).epsilon(1e-12));
    }
  }
  GIVEN("a standing wave: E and H a quarter period apart") {
    const viz::Phasor3 e{C(1, 0), 0, 0}, h{0, C(0, 1 / kEta), 0};
    THEN("no power flows on average: |S| below 1e-15 of the travelling-wave value") {
      const auto s = viz::poynting(e, h);
      REQUIRE(std::abs(s[2]) <= 1e-15 / (2 * kEta));
    }
  }
  GIVEN("a wave travelling along -z: H reversed") {
    THEN("S is negative along z") {
      const auto s = viz::poynting({C(1, 0), 0, 0}, {0, C(-1 / kEta, 0), 0});
      REQUIRE(s[2] == Catch::Approx(-1 / (2 * kEta)).epsilon(1e-12));
    }
  }
  GIVEN("a wave travelling along +x: E along y, H along z") {
    THEN("S points along +x") {
      const auto s = viz::poynting({0, C(2, 0), 0}, {0, 0, C(2 / kEta, 0)});
      REQUIRE(s[0] == Catch::Approx(4 / (2 * kEta)).epsilon(1e-12));
      REQUIRE(s[1] == 0.0);
      REQUIRE(s[2] == 0.0);
    }
  }
}

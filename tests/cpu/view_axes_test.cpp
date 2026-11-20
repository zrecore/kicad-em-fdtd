// BDD scenarios for the 3D view's standard views and axis triad.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "viz/view_axes.hpp"

using Catch::Approx;

SCENARIO("A standard view puts the named axis end towards the viewer") {
  GIVEN("each of the six standard views") {
    for (viz::StdView v : {viz::StdView::PlusX, viz::StdView::MinusX, viz::StdView::PlusY,
                           viz::StdView::MinusY, viz::StdView::PlusZ, viz::StdView::MinusZ}) {
      WHEN(std::string("the camera is placed at ") + viz::stdViewName(v)) {
        const viz::Pose p = viz::stdViewPose(v);
        const auto ends = viz::axisTriad(p.azimuth, p.elevation);
        THEN("that end points at the viewer, within 1e-3 of full depth, and sits at the triad's centre") {
          for (const viz::AxisEnd& e : ends)
            if (e.view == v) {
              CHECK(e.depth == Approx(1.0).margin(1e-3));
              CHECK(std::hypot(e.x, e.y) < 2e-3);
            }
        }
        AND_THEN("the opposite end points away and the other four lie on the unit circle, within 1e-3") {
          int away = 0, rim = 0;
          for (const viz::AxisEnd& e : ends) {
            if (e.depth < -0.999)
              ++away;
            else if (std::abs(e.depth) < 2e-3 && std::abs(std::hypot(e.x, e.y) - 1.0) < 2e-3)
              ++rim;
          }
          CHECK(away == 1);
          CHECK(rim == 4);
        }
      }
    }
  }
}

SCENARIO("In the +Z (top) view the screen axes are a right-handed pair") {
  const viz::Pose p = viz::stdViewPose(viz::StdView::PlusZ);
  const auto ends = viz::axisTriad(p.azimuth, p.elevation);
  // azimuth 0 looks from +x, so tilting to the top leaves +x pointing down; the screen right is +y
  THEN("the screen axes are a right-handed pair, within 1e-3") {
    const auto& px = ends[0];
    const auto& py = ends[2];
    CHECK(px.x * py.y - px.y * py.x == Approx(-1.0).margin(2e-3)); // screen y is down, so det = -1 for z up
  }
}

SCENARIO("The glide to a view takes the short way round and ends exactly there") {
  GIVEN("a camera at azimuth 3 rad going to the -X view (azimuth pi)") {
    viz::Pose cur{3.0, 0.2};
    const viz::Pose target = viz::stdViewPose(viz::StdView::MinusX);
    WHEN("the glide runs for 40 steps at an ease of 0.3") {
      for (int i = 0; i < 40; ++i)
        cur = viz::easePose(cur, target, 0.3);
      THEN("the pose is within 1e-6 of the target") {
        CHECK(cur.azimuth == Approx(target.azimuth).margin(1e-6));
        CHECK(cur.elevation == Approx(target.elevation).margin(1e-6));
      }
    }
  }
  GIVEN("a camera at azimuth -3 rad going to azimuth +3 rad") {
    const viz::Pose next = viz::easePose({-3.0, 0}, {3.0, 0}, 0.5);
    THEN("it moves through +-pi (about -3.14) rather than through 0") {
      CHECK(next.azimuth < -3.0);
    }
  }
  GIVEN("Reduce Motion (ease 1)") {
    const viz::Pose p = viz::easePose({0.5, 0.5}, {2.0, -0.3}, 1.0);
    THEN("the pose jumps straight to the target") {
      CHECK(p.azimuth == Approx(2.0));
      CHECK(p.elevation == Approx(-0.3));
    }
  }
}

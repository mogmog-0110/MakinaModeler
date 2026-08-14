// pingu.pov's FlipperUp, verbatim: the fixture for sweep-silhouette-check.bat. A b_spline
// sweep with per-point radii and a squash, tilted so the curve's bend faces the camera.
sphere_sweep {
  b_spline 6
  <-0.04, 1.02,  0.02>, 0.152
  <-0.30, 1.26,  0.00>, 0.148
  <-0.56, 1.66, -0.03>, 0.130
  <-0.63, 2.04, -0.05>, 0.100
  <-0.54, 2.26, -0.05>, 0.068
  <-0.43, 2.33, -0.04>, 0.028
  tolerance 0.00004
  scale <1, 1, 0.78>
  pigment { color rgb <0.25, 0.25, 0.3> }
  rotate <10, 30, 0>
}

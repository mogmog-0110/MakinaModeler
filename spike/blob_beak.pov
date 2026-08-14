// A beak-like blob, shaped after pingu.pov's BeakShape: one scaled sphere component and one
// cylinder component, blended. The fixture for blob-silhouette-check.bat.
blob {
  threshold 0.55
  sphere   { <0,  0.26, 0>, 1.00, 1.0  scale <1.00, 0.52, 0.52> }
  cylinder { <0, -0.12, 0>, <0, 0.06, 0>, 0.92, 0.9  scale <1.00, 0.40, 0.52> }
  pigment { color rgb <0.9, 0.35, 0.1> }
  rotate <25, 40, 0>
}
